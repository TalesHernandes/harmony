// The D3 half of the media server, proved the same way the D2 half was: real
// Pion connections over a real UDP socket, and the assertion is that what one
// side sent came out of the other.
//
// Three things are being proved here, and the first is the one the sprint
// exists for. That a screen can be switched on in the middle of a call — on
// the connection that is already carrying the conversation, without anybody
// leaving the room and coming back. That switching it off again really takes
// it away, instead of leaving a frozen last picture on everybody's wall. And
// that the two new doors are doors: knowing somebody's session identifier does
// not let you turn their camera on.
package sfu

import (
	"errors"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/pion/webrtc/v4"
)

// party is a room with the signalling wired up: every offer the server pushes
// is answered by the browser it was addressed to, which is what the web client
// does with the events on its socket.
type party struct {
	s  *SFU
	mu sync.Mutex

	clients map[string]*client
	offers  map[string][]Offer
}

func newParty(t *testing.T, max int) *party {
	t.Helper()
	p := &party{
		s:       testServer(t, max),
		clients: map[string]*client{},
		offers:  map[string][]Offer{},
	}
	p.s.cfg.Signal = func(_ int64, _, kind string, data any) {
		if kind != "voice_offer" {
			return
		}
		offer := data.(Offer)
		p.mu.Lock()
		c := p.clients[offer.SessionID]
		p.offers[offer.SessionID] = append(p.offers[offer.SessionID], offer)
		p.mu.Unlock()
		if c != nil {
			// An offer that lands after the test has closed a connection is
			// the teardown, not a failure. SFU_DEBUG=1 shows the rest.
			if err := c.answer(offer); err != nil && os.Getenv("SFU_DEBUG") != "" {
				println("answer failed:", err.Error())
			}
		}
	}
	return p
}

func (p *party) join(t *testing.T, room string, userID int64, name string) *client {
	t.Helper()
	c := newClient(t, p.s, room, userID, name)
	p.mu.Lock()
	p.clients[c.id] = c
	p.mu.Unlock()
	return c
}

// lastOffer is the most recent description the server pushed at one device. The
// tests read it to check what was offered, which is the only place a rule about
// codecs can actually be enforced: a browser cannot send what it was not asked
// for.
func (p *party) lastOffer(session string) (Offer, bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	list := p.offers[session]
	if len(list) == 0 {
		return Offer{}, false
	}
	return list[len(list)-1], true
}

func (p *party) allSDP() []string {
	p.mu.Lock()
	defer p.mu.Unlock()
	var out []string
	for _, list := range p.offers {
		for _, o := range list {
			out = append(out, o.SDP)
		}
	}
	return out
}

// mediaSection pulls one m-line's block out of a description, by the label the
// slot was given. Reading the whole description would prove nothing: the
// question is what *this* slot was offered.
func mediaSection(sdp, mid string) string {
	for _, block := range strings.Split(sdp, "\nm=") {
		if strings.Contains(block, "a=mid:"+mid+"\r") || strings.HasSuffix(strings.TrimSpace(block), "a=mid:"+mid) {
			return block
		}
	}
	return ""
}

// firstCodec is the codec a media section leads with, which is the one a
// browser answering the offer will choose.
func firstCodec(section string) string {
	for _, line := range strings.Split(section, "\n") {
		line = strings.TrimSpace(line)
		if !strings.HasPrefix(line, "a=rtpmap:") {
			continue
		}
		name := line[strings.Index(line, " ")+1:]
		name = name[:strings.Index(name, "/")]
		if name == "rtx" {
			continue
		}
		return name
	}
	return ""
}

func publishingOf(s *SFU, room, session string) []string {
	for _, peer := range s.Rooms([]string{room})[room] {
		if peer.SessionID == session {
			return peer.Publishing
		}
	}
	return nil
}

// TestScreenTurnsOnMidCall is this sprint in one test. Two people are already
// talking; one starts sharing a screen with its sound; the other sees both
// arrive on the connection that was already up, and the voices never stop.
func TestScreenTurnsOnMidCall(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")
	bruno := p.join(t, "sala", 2, "bruno")

	stop := make(chan struct{})
	defer close(stop)
	alice.talk(stop)
	bruno.talk(stop)
	waitFor(t, "the conversation to start", func() bool { return bruno.received() > 10 })

	sessionBefore := alice.id
	heardBefore := bruno.receivedKind("audio")

	screen := alice.share(SourceScreen, webrtc.MimeTypeVP9, "tela")
	sound := alice.share(SourceScreenAudio, webrtc.MimeTypeOpus, "som-da-tela")
	feed(screen, stop)
	feed(sound, stop)

	// Nothing of a screen arrives until it is asked for.
	bruno.watch(t, "alice")
	waitFor(t, "bruno to see the screen", func() bool { return bruno.receivedKind("video") > 10 })
	waitFor(t, "bruno to be told whose screen it is", func() bool {
		return bruno.sources()[SourceScreen].Username == "alice"
	})

	info := bruno.sources()[SourceScreen]
	if info.Kind != "video" || info.DisplayName != "alice" {
		t.Fatalf("the screen arrived unlabelled: %+v", info)
	}
	if _, ok := bruno.sources()[SourceScreenAudio]; !ok {
		t.Fatalf("the screen's own sound did not arrive: %+v", bruno.sources())
	}
	// The microphone and the screen's sound are two different things on two
	// different slots, or a per-source volume is not possible at all.
	if bruno.sources()[SourceScreenAudio].Mid == bruno.sources()[SourceMic].Mid {
		t.Fatal("the screen's sound landed on the microphone's slot")
	}

	// Nobody left and came back: it is the same session, and the conversation
	// kept going the whole time.
	if alice.id != sessionBefore {
		t.Fatal("sharing a screen rebuilt the connection")
	}
	waitFor(t, "the conversation to carry on", func() bool {
		return bruno.receivedKind("audio") > heardBefore+10
	})

	// And the room says who is sharing, without anybody having to subscribe to
	// find out.
	waitFor(t, "the room to say alice is sharing", func() bool {
		return len(publishingOf(p.s, "sala", alice.id)) == 2
	})
	got := strings.Join(publishingOf(p.s, "sala", alice.id), ",")
	if got != SourceScreen+","+SourceScreenAudio {
		t.Fatalf("the room says alice is publishing %q", got)
	}
	if len(publishingOf(p.s, "sala", bruno.id)) != 0 {
		t.Fatalf("bruno is not sharing anything but the room says he is")
	}
}

// TestScreenIsH264AndAV1IsNowhere: the owner picked H.264 for the screen on
// 19/09/2026 and forbade AV1 (CLAUDE.md). The place either decision can be
// enforced is the offer, because a browser can only send what it was offered.
//
// This test led with VP9 until that date. It is the same test: what it guards
// is that the screen's codec is a decision somebody took, not whatever Pion
// happened to register first.
func TestScreenIsH264AndAV1IsNowhere(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")
	alice.share(SourceScreen, webrtc.MimeTypeH264, "tela")
	alice.share(SourceCamera, webrtc.MimeTypeVP8, "camera")

	var offer Offer
	waitFor(t, "the offer with both slots", func() bool {
		o, ok := p.lastOffer(alice.id)
		if !ok || o.SendMids[SourceScreen] == "" || o.SendMids[SourceCamera] == "" {
			return false
		}
		offer = o
		return true
	})

	screen := mediaSection(offer.SDP, offer.SendMids[SourceScreen])
	if screen == "" {
		t.Fatal("no media section for the screen's slot")
	}
	// The first codec offered is the one a browser picks, so this is where the
	// owner's "the screen is H.264" is actually decided.
	if first := firstCodec(screen); first != "H264" {
		t.Fatalf("a screen's slot leads with %s, not H264:\n%s", first, screen)
	}
	// A camera still leads with VP8: a telephone that has a chip for VP8 and
	// none for VP9 should be able to turn its camera on without cooking.
	camera := mediaSection(offer.SDP, offer.SendMids[SourceCamera])
	if first := firstCodec(camera); first != "VP8" {
		t.Fatalf("a camera's slot leads with %s, not VP8:\n%s", first, camera)
	}

	// And AV1 is not a preference at all — it is absent, because this build
	// never registers it. No ordering can bring back what is not there.
	for _, sdp := range p.allSDP() {
		if strings.Contains(sdp, "AV1") || strings.Contains(sdp, "H265") {
			t.Fatal("a codec this service does not carry appears in a description it wrote")
		}
	}
}

// TestScreenTurnsOffAndComesBack: stopping a share really takes the picture
// away — a screen that was switched off must not sit on everybody's wall as a
// frozen last frame — and the same person can start again on the same
// connection afterwards.
func TestScreenTurnsOffAndComesBack(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")
	bruno := p.join(t, "sala", 2, "bruno")

	stop := make(chan struct{})
	defer close(stop)
	alice.talk(stop)
	bruno.talk(stop)

	screen := alice.share(SourceScreen, webrtc.MimeTypeVP9, "tela")
	feed(screen, stop)
	bruno.watch(t, "alice")
	waitFor(t, "bruno to see the screen", func() bool { return bruno.receivedKind("video") > 10 })

	if err := alice.sfu.Unpublish(alice.id, 1, []string{SourceScreen}); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "the room to forget the screen", func() bool {
		return len(publishingOf(p.s, "sala", alice.id)) == 0
	})
	waitFor(t, "bruno to be told the screen is gone", func() bool {
		_, still := bruno.sources()[SourceScreen]
		return !still
	})

	// The conversation did not go with it.
	heard := bruno.receivedKind("audio")
	waitFor(t, "the conversation to carry on", func() bool {
		return bruno.receivedKind("audio") > heard+10
	})

	// And it can be turned on again, on the same connection — without bruno
	// having to press "assistir" a second time. Choosing to watch somebody is
	// about the person, not about the picture that happened to be up.
	seen := bruno.receivedKind("video")
	again := alice.share(SourceScreen, webrtc.MimeTypeVP9, "tela-de-novo")
	feed(again, stop)
	waitFor(t, "bruno to see the second screen", func() bool {
		return bruno.receivedKind("video") > seen+10
	})
	waitFor(t, "the room to say alice is sharing again", func() bool {
		return len(publishingOf(p.s, "sala", alice.id)) == 1
	})
}

// TestUnpublishDoesNotSilenceAnybody: a microphone is silenced by mute, which
// is a label the room draws, and never by taking its slot away — a connection
// with no microphone slot cannot be spoken into until it is rebuilt.
func TestUnpublishDoesNotSilenceAnybody(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")
	bruno := p.join(t, "sala", 2, "bruno")

	stop := make(chan struct{})
	defer close(stop)
	alice.talk(stop)
	waitFor(t, "bruno to hear alice", func() bool { return bruno.received() > 10 })

	if err := p.s.Unpublish(alice.id, 1, []string{SourceMic}); err != nil {
		t.Fatal(err)
	}
	heard := bruno.received()
	waitFor(t, "alice to still be heard", func() bool { return bruno.received() > heard+10 })
}

// TestPublishIsNotACapability: the two new doors are doors. A session id is the
// strongest thing somebody could get hold of, and it still buys nothing — and
// the refusal is the same one an invented session gets, so the answer does not
// say whether the session exists.
func TestPublishIsNotACapability(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")

	const bruno = int64(2)
	if err := p.s.Publish(alice.id, bruno, []string{SourceCamera}); !errors.Is(err, ErrNoSession) {
		t.Fatalf("bruno turned alice's camera on: %v", err)
	}
	if err := p.s.Unpublish(alice.id, bruno, []string{SourceScreen}); !errors.Is(err, ErrNoSession) {
		t.Fatalf("bruno turned alice's screen off: %v", err)
	}
	if err := p.s.Publish("nao-existe", bruno, []string{SourceCamera}); !errors.Is(err, ErrNoSession) {
		t.Fatalf("an invented session answered differently: %v", err)
	}
	if err := p.s.Unpublish("nao-existe", bruno, []string{SourceScreen}); !errors.Is(err, ErrNoSession) {
		t.Fatalf("an invented session answered differently: %v", err)
	}

	// Alice is untouched, and the same calls as herself work — a refusal that
	// refuses everybody would prove nothing.
	if len(publishingOf(p.s, "sala", alice.id)) != 0 {
		t.Fatal("somebody else's request changed what alice is sharing")
	}
	if err := p.s.Publish(alice.id, 1, []string{SourceCamera}); err != nil {
		t.Fatalf("alice could not turn her own camera on: %v", err)
	}
}

// TestUnknownSourcesAreDropped: a newer client asking for something this server
// has never heard of gets a working voice channel, not an error.
func TestUnknownSourcesAreDropped(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")
	if err := p.s.Publish(alice.id, 1, []string{"holograma"}); err != nil {
		t.Fatalf("an unknown source was an error: %v", err)
	}
	if err := p.s.Unpublish(alice.id, 1, []string{"holograma"}); err != nil {
		t.Fatalf("an unknown source was an error: %v", err)
	}
	offer, ok := p.lastOffer(alice.id)
	if ok && offer.SendMids["holograma"] != "" {
		t.Fatal("a slot was opened for a source that does not exist")
	}
}

// TestSlotsAreBounded: a slot that has been closed is never reused, so a person
// who turns a screen on and off all evening would grow the description of their
// connection without end. The limit is reached long after any real evening, and
// what it produces is a request to reconnect rather than a description too
// large to send.
func TestSlotsAreBounded(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")

	var err error
	for i := 0; i < maxSlotsPerSession+2; i++ {
		if err = p.s.Publish(alice.id, 1, []string{SourceScreen}); err != nil {
			break
		}
		if err = p.s.Unpublish(alice.id, 1, []string{SourceScreen}); err != nil {
			break
		}
		time.Sleep(time.Millisecond)
	}
	if !errors.Is(err, ErrTooManySlots) {
		t.Fatalf("a connection grew without a limit: %v", err)
	}
	// And the room is still standing: the person is still in it, still heard.
	if len(p.s.Rooms([]string{"sala"})["sala"]) != 1 {
		t.Fatal("hitting the limit threw the person out of the room")
	}
}

// A whole room asking for a fresh picture at the same moment asks the sender
// once, which is the difference between recovering from a bad second and
// spending the next ten seconds in one.
//
// The owner's words were "ta dando uns stutterings durante a live". A full
// picture is ten to fifty times an ordinary one; one bad second on a long route
// makes everybody watching ask at once, and one enormous frame per person fills
// the sender's upload, loses more packets, and starts the loop again.
func TestARoomAskingTogetherAsksTheSenderOnce(t *testing.T) {
	var mu sync.Mutex
	asked := 0
	p := &publication{askAgain: func() {
		mu.Lock()
		asked++
		mu.Unlock()
	}}

	if !p.keyframe() {
		t.Fatal("the first request has to go through, or a newcomer sits looking at grey")
	}
	var wg sync.WaitGroup
	for i := 0; i < 32; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			p.keyframe()
		}()
	}
	wg.Wait()

	mu.Lock()
	got := asked
	mu.Unlock()
	if got != 1 {
		t.Fatalf("thirty-three people asking at once woke the sender %d times, want 1", got)
	}

	// A second later it is a new bad second, and the sender is asked again:
	// this holds the storm back, it does not swallow the request.
	p.kfMu.Lock()
	p.kfLast = time.Now().Add(-2 * keyframeEvery)
	p.kfMu.Unlock()
	if !p.keyframe() {
		t.Fatal("after the window the sender has to be asked again")
	}
	mu.Lock()
	defer mu.Unlock()
	if asked != 2 {
		t.Fatalf("asked %d times in all, want 2", asked)
	}
}

// A route that arrives before the answer is kept, not thrown on the floor.
//
// This was a real 500 in production, in the owner's own diagnostic log: the
// browser starts telling us how to reach it the moment it has written its
// answer down, and that message races the answer itself across the network.
// Whichever routes won the race were refused, because there was nothing on the
// other end to attach them to yet. Here it cost nothing — another route worked.
// On a stricter network the refused one is the only one there is.
func TestARouteThatArrivesBeforeTheAnswerIsKept(t *testing.T) {
	s := testServer(t, 4)
	offer, err := s.Join("sala", Joiner{
		UserID: 1, LoginSession: "ana-login", Username: "ana", DisplayName: "Ana",
	})
	if err != nil {
		t.Fatal(err)
	}
	sess, err := s.session(offer.SessionID, 1)
	if err != nil {
		t.Fatal(err)
	}

	mid := "0"
	route := webrtc.ICECandidateInit{
		Candidate: "candidate:1 1 udp 2130706431 192.0.2.1 4242 typ host",
		SDPMid:    &mid,
	}
	if err := s.Candidate(offer.SessionID, 1, route); err != nil {
		t.Fatalf("a route that arrived before the answer was refused: %v", err)
	}
	sess.iceMu.Lock()
	waiting := len(sess.iceEarly)
	sess.iceMu.Unlock()
	if waiting != 1 {
		t.Fatalf("%d routes waiting, want 1", waiting)
	}

	// The browser's answer, from a real connection on the other side.
	pc, err := webrtc.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		t.Fatal(err)
	}
	defer pc.Close()
	if err := pc.SetRemoteDescription(webrtc.SessionDescription{
		Type: webrtc.SDPTypeOffer, SDP: offer.SDP,
	}); err != nil {
		t.Fatal(err)
	}
	answer, err := pc.CreateAnswer(nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := pc.SetLocalDescription(answer); err != nil {
		t.Fatal(err)
	}
	if err := s.Answer(offer.SessionID, 1, answer.SDP); err != nil {
		t.Fatal(err)
	}

	sess.iceMu.Lock()
	waiting, ready := len(sess.iceEarly), sess.iceReady
	sess.iceMu.Unlock()
	if waiting != 0 || !ready {
		t.Fatalf("after the answer: %d routes still waiting, ready=%v", waiting, ready)
	}
	// And from here they go straight in.
	if err := s.Candidate(offer.SessionID, 1, route); err != nil {
		t.Fatalf("a route after the answer was refused: %v", err)
	}
}

// TestJoiningToTalkDoesNotCostAFilm: entrar num canal de voz não começa a
// assistir a tela de ninguém. Pedido do dono em 18/09/2026, depois da primeira
// live na máquina nova — "assistir é uma escolha, não algo que acontece com
// você" — e não é só interface: um fluxo de 1080p60 que ninguém pediu custa
// banda de quem transmite, decodificação de quem recebe e bateria no celular.
//
// A recusa é de verdade: os pacotes não atravessam a rede. Uma imagem
// escondida não devolveria nada disso.
func TestJoiningToTalkDoesNotCostAFilm(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")
	bruno := p.join(t, "sala", 2, "bruno")

	stop := make(chan struct{})
	defer close(stop)
	alice.talk(stop)
	bruno.talk(stop)

	screen := alice.share(SourceScreen, webrtc.MimeTypeVP9, "tela")
	sound := alice.share(SourceScreenAudio, webrtc.MimeTypeOpus, "som-da-tela")
	feed(screen, stop)
	feed(sound, stop)

	// A sala diz que a alice está compartilhando — isso todo mundo sabe, é o
	// que desenha o botão de assistir.
	waitFor(t, "the room to say alice is sharing", func() bool {
		return len(publishingOf(p.s, "sala", alice.id)) == 2
	})
	// E a conversa chega, que é o que ele veio buscar.
	waitFor(t, "the conversation to reach bruno", func() bool {
		return bruno.receivedKind("audio") > 10
	})

	// Mas a tela não. Nem a imagem, nem o som dela.
	if n := bruno.receivedKind("video"); n != 0 {
		t.Fatalf("a tela chegou sem ninguém pedir: %d pacotes", n)
	}
	if _, ok := bruno.sources()[SourceScreen]; ok {
		t.Fatal("houve uma vaga aberta para uma tela que ninguém pediu")
	}
	if _, ok := bruno.sources()[SourceScreenAudio]; ok {
		t.Fatal("o som da tela chegou sem ninguém pedir")
	}

	// Aí ele pede, e aí sim.
	bruno.watch(t, "alice")
	waitFor(t, "bruno to see the screen he asked for", func() bool {
		return bruno.receivedKind("video") > 10
	})
	if _, ok := bruno.sources()[SourceScreenAudio]; !ok {
		t.Fatalf("o som da tela não veio junto: %+v", bruno.sources())
	}

	// E dá para voltar atrás: parar de assistir também é de verdade.
	seen := bruno.receivedKind("video")
	if err := p.s.Watch(bruno.id, 2, "alice", false); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "the picture to stop crossing the network", func() bool {
		a := bruno.receivedKind("video")
		time.Sleep(300 * time.Millisecond)
		return bruno.receivedKind("video") == a
	})
	if bruno.receivedKind("video") < seen {
		t.Fatal("contagem andou para trás")
	}

	// E a conversa nunca esteve em jogo, nas duas direções.
	heard := bruno.receivedKind("audio")
	waitFor(t, "the conversation to carry on", func() bool {
		return bruno.receivedKind("audio") > heard+10
	})
}

// TestAScreenDoesNotLandOnTheWatchersOwnCamera: a device sending its camera
// has a video slot open towards the server, and Pion's AddTrack reuses any
// video slot with nothing going out on it — that one included. The screen
// then arrives on a slot whose codecs were agreed for the device's camera,
// and a phone with no H.264 encoder agreed on VP8 and VP9 only there: the
// screen's H.264 came in as packets nobody could read (A4, 25/09/2026, seen
// on the emulator as "codec=null", 0 frames). A screen gets a slot of its own.
func TestAScreenDoesNotLandOnTheWatchersOwnCamera(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")
	bruno := p.join(t, "sala", 2, "bruno")

	stop := make(chan struct{})
	defer close(stop)
	alice.talk(stop)
	bruno.talk(stop)

	cam := bruno.share(SourceCamera, webrtc.MimeTypeVP8, "camera")
	feed(cam, stop)
	waitFor(t, "the room to say bruno's camera is on", func() bool {
		return len(publishingOf(p.s, "sala", bruno.id)) == 1
	})
	offer, _ := p.lastOffer(bruno.id)
	camMid := offer.SendMids[SourceCamera]
	if camMid == "" {
		t.Fatalf("bruno's camera was given no slot: %+v", offer.SendMids)
	}

	screen := alice.share(SourceScreen, webrtc.MimeTypeVP9, "tela")
	feed(screen, stop)
	bruno.watch(t, "alice")
	waitFor(t, "bruno to be told about the screen", func() bool {
		return bruno.sources()[SourceScreen].Mid != ""
	})
	if got := bruno.sources()[SourceScreen].Mid; got == camMid {
		t.Fatalf("alice's screen arrived on bruno's own camera slot (mid %s)", got)
	}
	waitFor(t, "bruno to see the screen", func() bool { return bruno.receivedKind("video") > 10 })
	// And his camera still goes out: the slot was not taken from him.
	waitFor(t, "alice to see bruno's camera", func() bool {
		return alice.sources()[SourceCamera].Username == "bruno"
	})
}
