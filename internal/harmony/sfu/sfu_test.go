// The media server proved from the outside, with real browsers' worth of
// WebRTC on the other end: two Pion peers connect over a real UDP socket, send
// real packets, and the test asserts that what one sent is what the other got.
//
// Two things are being proved here. That the forwarding works at all — a room
// where nobody hears anybody is the failure this whole sprint is about. And
// that a session identifier is not a capability: knowing somebody else's does
// not let you answer for them, mute them, or hang up on them.
package sfu

import (
	"errors"
	"io"
	"log/slog"
	"net"
	"os"
	"sort"
	"sync"
	"testing"
	"time"

	"github.com/pion/ice/v4"
	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"
	"github.com/pion/webrtc/v4/pkg/media"
)

func testLogger() *slog.Logger {
	if os.Getenv("SFU_DEBUG") != "" {
		return slog.New(slog.NewTextHandler(os.Stderr, nil))
	}
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

func testServer(t *testing.T, max int) *SFU {
	t.Helper()
	s, err := New(Config{
		// Port zero: the operating system picks a free one, so two tests can
		// run at the same time and neither has to know about the other.
		UDPPort:         0,
		PublicIP:        "127.0.0.1",
		IncludeLoopback: true,
		MaxPerRoom:      max,
		Logger:          testLogger(),
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	return s
}

// client is one person's browser, near enough. It does what the web client
// does: take the offer, put its microphone in the slot the server named,
// answer, and keep answering every later offer.
type client struct {
	t    *testing.T
	api  *webrtc.API
	pc   *webrtc.PeerConnection
	sfu  *SFU
	id   string
	user int64

	// neg is the browser's one event loop, as far as negotiation goes: a
	// browser never applies the server's offer and makes one of its own at
	// the same time, and neither may this stand-in. The server's offers are
	// answered on the server's goroutines while the test runs on its own, and
	// without this lock both drove the one connection at once.
	neg sync.Mutex

	mu       sync.Mutex
	incoming map[string]int // track id -> packets received
	byKind   map[string]int // "audio"/"video" -> packets received
	seen     map[string]TrackInfo
	track    *webrtc.TrackLocalStaticSample
	// waiting is what this browser has in hand and has not yet been given a
	// slot for: source -> track. The server names the slot in the offer, and
	// answer() puts the right track in it.
	waiting map[string]*webrtc.TrackLocalStaticSample
	// shared is the same thing for a screen or a camera, and it carries RTP
	// rather than frames. That is what a browser puts on the wire, and it is
	// what a server that never decodes anything should be tested against — the
	// D2 measuring tool sends the same way.
	shared map[string]*webrtc.TrackLocalStaticRTP
	// answerTransform models a browser's fmtp limits on the answer sent to
	// the SFU. Pion keeps its own generated SDP; PTs and transports are intact.
	answerTransform func(string) string
}

func newClient(t *testing.T, s *SFU, room string, userID int64, name string, configure ...func(*webrtc.MediaEngine)) *client {
	t.Helper()
	se := webrtc.SettingEngine{}
	se.SetIncludeLoopbackCandidate(true)
	// This browser stands on the loopback and nowhere else.
	//
	// Without the three lines below Pion gathers a candidate on every address
	// this machine has, and opens a socket on each — and on Windows a socket
	// that is not on the loopback is the firewall's "allow this app?" box,
	// asked again for every new path, which for `go test` is a new temporary
	// directory on every build. The server side is bound narrow for the same
	// reason (sfu.go, onThisMachine); this is the other end of the same
	// connection, and leaving it out left the box appearing exactly as often.
	se.SetNetworkTypes([]webrtc.NetworkType{webrtc.NetworkTypeUDP4})
	se.SetIPFilter(func(ip net.IP) bool { return ip.IsLoopback() })
	se.SetICEMulticastDNSMode(ice.MulticastDNSModeDisabled)
	m := &webrtc.MediaEngine{}
	if err := m.RegisterDefaultCodecs(); err != nil {
		t.Fatal(err)
	}
	for _, setup := range configure {
		setup(m)
	}
	reg, err := defaultInterceptors(m)
	if err != nil {
		t.Fatal(err)
	}
	api := webrtc.NewAPI(webrtc.WithSettingEngine(se), webrtc.WithMediaEngine(m), webrtc.WithInterceptorRegistry(reg))
	pc, err := api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { pc.Close() })

	c := &client{t: t, api: api, pc: pc, sfu: s, user: userID,
		incoming: map[string]int{}, byKind: map[string]int{},
		seen: map[string]TrackInfo{}, waiting: map[string]*webrtc.TrackLocalStaticSample{},
		shared: map[string]*webrtc.TrackLocalStaticRTP{}}

	// The microphone goes on before the offer arrives, exactly as the web
	// client does it: a slot the server offered as "you send here" needs a
	// track already waiting, or the answer comes back saying nothing is coming.
	track, err := webrtc.NewTrackLocalStaticSample(
		webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus}, "mic", name)
	if err != nil {
		t.Fatal(err)
	}
	c.track = track
	c.waiting[SourceMic] = track
	if _, err := pc.AddTrack(track); err != nil {
		t.Fatal(err)
	}

	pc.OnTrack(func(remote *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		kind := remote.Kind().String()
		buf := make([]byte, 1500)
		for {
			if _, _, err := remote.Read(buf); err != nil {
				return
			}
			c.mu.Lock()
			c.incoming[remote.StreamID()]++
			c.byKind[kind]++
			c.mu.Unlock()
		}
	})

	offer, err := s.Join(room, Joiner{UserID: userID, LoginSession: name + "-login", Username: name, DisplayName: name})
	if err != nil {
		t.Fatal(err)
	}
	c.id = offer.SessionID
	if err := c.answer(offer); err != nil {
		t.Fatal(err)
	}
	return c
}

// answer is the whole browser side of negotiation: set what the server said,
// make sure the microphone is in the named slot, reply. It returns the error
// rather than failing the test, because later offers arrive on the server's
// goroutines — including while the test is tearing everything down.
func (c *client) answer(offer Offer) error {
	sdp, err := c.answerLocked(offer)
	if err != nil {
		return err
	}
	// Handed over with the lock released: the server may push its next offer
	// from inside this very call (a skipped one, see SFU.Answer), and that
	// offer comes back here on this goroutine.
	return c.sfu.Answer(c.id, c.user, sdp)
}

func (c *client) answerLocked(offer Offer) (string, error) {
	c.neg.Lock()
	defer c.neg.Unlock()
	if err := c.pc.SetRemoteDescription(webrtc.SessionDescription{
		Type: webrtc.SDPTypeOffer, SDP: offer.SDP,
	}); err != nil {
		return "", err
	}
	c.mu.Lock()
	c.seen = map[string]TrackInfo{}
	for _, info := range offer.Tracks {
		c.seen[info.Mid] = info
	}
	waiting := make(map[string]*webrtc.TrackLocalStaticSample, len(c.waiting))
	for k, v := range c.waiting {
		waiting[k] = v
	}
	c.mu.Unlock()
	// Every slot the server named gets the track this browser is holding for
	// it. On the first offer the microphone is usually matched already; this
	// is what makes it certain, and it is the only way a screen switched on in
	// the middle of a call ever reaches its slot.
	sources := make([]string, 0, len(offer.SendMids))
	for source := range offer.SendMids {
		sources = append(sources, source)
	}
	sort.Slice(sources, func(i, j int) bool {
		return offer.SendMids[sources[i]] < offer.SendMids[sources[j]]
	})
	c.mu.Lock()
	shared := make(map[string]*webrtc.TrackLocalStaticRTP, len(c.shared))
	for k, v := range c.shared {
		shared[k] = v
	}
	c.mu.Unlock()
	for _, source := range sources {
		if raw := shared[source]; raw != nil {
			for _, tr := range c.pc.GetTransceivers() {
				if tr.Mid() == offer.SendMids[source] && tr.Sender() == nil {
					// Into this slot, as a browser does. AddTrack would take
					// the first empty video slot, which, with somebody else's
					// camera arriving, is the one bringing it in.
					snd, err := c.api.NewRTPSender(raw, c.pc.SCTP().Transport())
					if err != nil {
						return "", err
					}
					if err := tr.SetSender(snd, raw); err != nil {
						return "", err
					}
				}
			}
			continue
		}
		track := waiting[source]
		if track == nil {
			continue
		}
		for _, tr := range c.pc.GetTransceivers() {
			if tr.Mid() != offer.SendMids[source] {
				continue
			}
			switch {
			case tr.Sender() == nil:
				// Where this stand-in differs from a browser: a browser makes
				// a sender for a slot it was told to send into, and Pion does
				// not — it leaves the slot empty until a track is added, and
				// AddTrack fills the first empty slot of that kind. Handling
				// the slots in mid order is what keeps that deterministic.
				if _, err := c.pc.AddTrack(track); err != nil {
					return "", err
				}
			case tr.Sender().Track() == nil:
				_ = tr.Sender().ReplaceTrack(track)
			}
		}
	}
	answer, err := c.pc.CreateAnswer(nil)
	if err != nil {
		return "", err
	}
	if err := c.pc.SetLocalDescription(answer); err != nil {
		return "", err
	}
	<-webrtc.GatheringCompletePromise(c.pc)
	raw := c.pc.LocalDescription().SDP
	if c.answerTransform != nil {
		raw = c.answerTransform(raw)
	}
	return raw, nil
}

// offer is a browser sharing something by offering it itself, the way the web
// client shares a screen since D4r (media.ts, startScreen): the slot is opened
// once, and the offer waits for a settled connection and goes again when the
// server says one of its own is in flight. That refusal is the server working
// as designed, not a failure, so a test must not read it as one.
func (c *client) offer(sources []string, track webrtc.TrackLocal) (string, error) {
	c.neg.Lock()
	_, err := c.pc.AddTransceiverFromTrack(track,
		webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionSendonly})
	c.neg.Unlock()
	if err != nil {
		return "", err
	}
	deadline := time.Now().Add(5 * time.Second)
	for {
		answer, err := c.offerOnce(sources)
		if !errors.Is(err, ErrBusyNegotiating) || time.Now().After(deadline) {
			return answer, err
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func (c *client) offerOnce(sources []string) (string, error) {
	c.neg.Lock()
	defer c.neg.Unlock()
	if c.pc.SignalingState() != webrtc.SignalingStateStable {
		return "", ErrBusyNegotiating
	}
	offer, err := c.pc.CreateOffer(nil)
	if err != nil {
		return "", err
	}
	if err := c.pc.SetLocalDescription(offer); err != nil {
		return "", err
	}
	answer, err := c.sfu.ClientOffer(c.id, c.user, sources, offer.SDP)
	if err != nil {
		// Taken back, as the browser does, so the server's own offer can land.
		_ = c.pc.SetLocalDescription(webrtc.SessionDescription{Type: webrtc.SDPTypeRollback})
		return "", err
	}
	if err := c.pc.SetRemoteDescription(webrtc.SessionDescription{
		Type: webrtc.SDPTypeAnswer, SDP: answer,
	}); err != nil {
		return "", err
	}
	return answer, nil
}

// talk sends audio for as long as it takes, or until the test gives up.
func (c *client) talk(stop <-chan struct{}) {
	go func() {
		tick := time.NewTicker(20 * time.Millisecond)
		defer tick.Stop()
		payload := make([]byte, 80)
		for {
			select {
			case <-stop:
				return
			case <-tick.C:
				_ = c.track.WriteSample(media.Sample{Data: payload, Duration: 20 * time.Millisecond})
			}
		}
	}()
}

// share is what a person pressing "compartilhar a tela" does: get the picture
// first, tell the server, and let the offer that comes back name the slot.
//
// What it hands over is RTP, not frames. A browser encodes and packetises on
// its own and puts finished packets on the wire, and a media server that never
// opens a packet should be proved against exactly that.
func (c *client) share(source, mime, id string) *sharedTrack {
	c.t.Helper()
	cap := webrtc.RTPCodecCapability{MimeType: mime, ClockRate: 90000}
	pt := uint8(96) // VP8
	switch mime {
	case webrtc.MimeTypeVP9:
		cap.SDPFmtpLine = "profile-id=0"
		pt = 98
	case webrtc.MimeTypeOpus:
		cap = webrtc.RTPCodecCapability{MimeType: mime, ClockRate: 48000, Channels: 2}
		pt = 111
	}
	track, err := webrtc.NewTrackLocalStaticRTP(cap, id, id)
	if err != nil {
		c.t.Fatal(err)
	}
	c.mu.Lock()
	c.shared[source] = track
	c.mu.Unlock()
	if err := c.sfu.Publish(c.id, c.user, []string{source}); err != nil {
		c.t.Fatal(err)
	}
	return &sharedTrack{track: track, payloadType: pt}
}

// sharedTrack is a screen or a camera on the wire: a stream of RTP with its own
// sequence numbers, which is the shape the server actually forwards.
type sharedTrack struct {
	track       *webrtc.TrackLocalStaticRTP
	payloadType uint8
}

// feed writes packets into a shared track until the test is done with it.
func feed(t *sharedTrack, stop <-chan struct{}) {
	go func() {
		seq := uint16(0)
		stamp := uint32(0)
		tick := time.NewTicker(33 * time.Millisecond)
		defer tick.Stop()
		payload := make([]byte, 900)
		for i := range payload {
			payload[i] = byte(i * 31)
		}
		for {
			select {
			case <-stop:
				return
			case <-tick.C:
				seq++
				stamp += 3000
				_ = t.track.WriteRTP(&rtp.Packet{
					Header: rtp.Header{
						Version: 2, PayloadType: t.payloadType,
						SequenceNumber: seq, Timestamp: stamp,
					},
					Payload: payload,
				})
			}
		}
	}()
}

// watch is what somebody pressing "assistir" does. Since 18/09/2026 a device
// that has just joined receives nobody's screen until it asks, so any test
// about a screen arriving has to ask first — which is the behaviour, not a
// nuisance of the harness.
func (c *client) watch(t *testing.T, who string) {
	t.Helper()
	if err := c.sfu.Watch(c.id, c.user, who, true); err != nil {
		t.Fatalf("watch %s: %v", who, err)
	}
}

func (c *client) sources() map[string]TrackInfo {
	c.mu.Lock()
	defer c.mu.Unlock()
	out := map[string]TrackInfo{}
	for _, info := range c.seen {
		out[info.Source] = info
	}
	return out
}

func (c *client) receivedKind(kind string) int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.byKind[kind]
}

func (c *client) received() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	n := 0
	for _, v := range c.incoming {
		n += v
	}
	return n
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

// TestTwoPeopleHearEachOther is the sprint in one test: two connections, and
// what one sends comes out of the other. The renegotiation is the interesting
// part — the second person's arrival changes the first person's connection
// while it is already running, and the first person has to answer that.
func TestTwoPeopleHearEachOther(t *testing.T) {
	s := testServer(t, 8)

	var mu sync.Mutex
	clients := map[string]*client{}
	s.cfg.Signal = func(_ int64, _, kind string, data any) {
		if kind != "voice_offer" {
			return
		}
		offer := data.(Offer)
		mu.Lock()
		c := clients[offer.SessionID]
		mu.Unlock()
		if c != nil {
			// An offer that lands after the test has closed a connection is
			// the teardown, not a failure.
			_ = c.answer(offer)
		}
	}

	alice := newClient(t, s, "sala", 1, "alice")
	mu.Lock()
	clients[alice.id] = alice
	mu.Unlock()

	bruno := newClient(t, s, "sala", 2, "bruno")
	mu.Lock()
	clients[bruno.id] = bruno
	mu.Unlock()

	stop := make(chan struct{})
	defer close(stop)
	alice.talk(stop)
	bruno.talk(stop)

	waitFor(t, "alice to hear bruno", func() bool { return alice.received() > 10 })
	waitFor(t, "bruno to hear alice", func() bool { return bruno.received() > 10 })

	st := s.Stats()
	if st.Peers != 2 || st.Rooms != 1 {
		t.Fatalf("stats say %d peers in %d rooms, want 2 in 1", st.Peers, st.Rooms)
	}
	if st.BytesIn == 0 || st.BytesOut == 0 {
		t.Fatalf("the counters the measurement depends on are empty: %+v", st)
	}
	// Two people, one stream each way: what goes out is what comes in. The
	// moment this stops being true for a bigger room is the whole point of the
	// measurement, so the number has to be trustworthy here first.
	if st.BytesOut < st.BytesIn/2 {
		t.Fatalf("bytes out %d is not plausible against bytes in %d", st.BytesOut, st.BytesIn)
	}
}

// TestSessionIsNotACapability: knowing somebody's session id gets you nothing.
// Every call takes the asker's account alongside it and the two have to agree.
func TestSessionIsNotACapability(t *testing.T) {
	s := testServer(t, 8)
	alice := newClient(t, s, "sala", 1, "alice")

	const bruno = int64(2)
	if err := s.Answer(alice.id, bruno, "v=0"); !errors.Is(err, ErrNoSession) {
		t.Fatalf("bruno answered alice's session: %v", err)
	}
	if err := s.Candidate(alice.id, bruno, webrtc.ICECandidateInit{Candidate: "candidate:0 1 udp 1 127.0.0.1 1 typ host"}); !errors.Is(err, ErrNoSession) {
		t.Fatalf("bruno added a candidate to alice's session: %v", err)
	}
	if err := s.SetState(alice.id, bruno, true, true); !errors.Is(err, ErrNoSession) {
		t.Fatalf("bruno muted alice: %v", err)
	}
	if err := s.Leave(alice.id, bruno); !errors.Is(err, ErrNoSession) {
		t.Fatalf("bruno hung up on alice: %v", err)
	}
	// And the refusal is the same one an invented session gets, so the answer
	// does not say whether the session exists.
	if err := s.Leave("nao-existe", bruno); !errors.Is(err, ErrNoSession) {
		t.Fatalf("invented session answered differently: %v", err)
	}
	if peers := s.Rooms([]string{"sala"})["sala"]; len(peers) != 1 {
		t.Fatalf("alice was moved by somebody else's request: %d peers", len(peers))
	}
}

// TestOwnStateIsOwnBusiness: the flags people set on themselves show up for
// everybody, and only the person themselves can set them.
func TestMuteAndDeafenShowUp(t *testing.T) {
	s := testServer(t, 8)
	alice := newClient(t, s, "sala", 1, "alice")

	if err := s.SetState(alice.id, 1, true, false); err != nil {
		t.Fatal(err)
	}
	peers := s.Rooms([]string{"sala"})["sala"]
	if len(peers) != 1 || !peers[0].Muted || peers[0].Deafened {
		t.Fatalf("state did not travel: %+v", peers)
	}
	if peers[0].Username != "alice" {
		t.Fatalf("wrong person in the room: %+v", peers[0])
	}
}

// TestRoomEmptiesAndDisappears: a voice channel with nobody in it is not a
// room, it is a row in a table. Nothing about it should survive in memory.
func TestRoomEmptiesAndDisappears(t *testing.T) {
	s := testServer(t, 8)
	alice := newClient(t, s, "sala", 1, "alice")
	if err := s.Leave(alice.id, 1); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "the room to be forgotten", func() bool { return s.Stats().Rooms == 0 })
	if peers := s.Rooms([]string{"sala"})["sala"]; len(peers) != 0 {
		t.Fatalf("room still has %d people in it", len(peers))
	}
}

// TestSessionThatNeverConnectsIsLetGo: a join whose reply never reached the
// device leaves a session nobody can hand back. It must not sit in the room
// for good — and one that did connect must not be touched by the same clock.
func TestSessionThatNeverConnectsIsLetGo(t *testing.T) {
	s := testServer(t, 8)
	s.connectWithin = 3 * time.Second
	start := time.Now()
	alice := newClient(t, s, "sala", 1, "alice")
	waitFor(t, "alice to connect", func() bool {
		s.mu.RLock()
		defer s.mu.RUnlock()
		sess := s.sessions[alice.id]
		return sess != nil && sess.connected.Load()
	})
	if _, err := s.Join("sala", Joiner{UserID: 2, Username: "bruno", DisplayName: "bruno", LoginSession: "bruno-login"}); err != nil {
		t.Fatal(err)
	}
	if n := len(s.Rooms([]string{"sala"})["sala"]); n != 2 {
		t.Fatalf("expected alice and the unanswered bruno inside, got %d", n)
	}
	waitFor(t, "the session that never connected to be dropped", func() bool {
		return len(s.Rooms([]string{"sala"})["sala"]) == 1
	})
	// Past alice's own deadline, too.
	time.Sleep(time.Until(start.Add(3500 * time.Millisecond)))
	peers := s.Rooms([]string{"sala"})["sala"]
	if len(peers) != 1 || peers[0].SessionID != alice.id {
		t.Fatalf("the connected session went too: %+v", peers)
	}
}

// TestRoomFull: the cap the plan asks for, enforced where the connection is
// made and not merely drawn in the interface.
func TestRoomFull(t *testing.T) {
	s := testServer(t, 2)
	newClient(t, s, "sala", 1, "alice")
	newClient(t, s, "sala", 2, "bruno")
	_, err := s.Join("sala", Joiner{UserID: 3, Username: "carla", DisplayName: "carla"})
	if !errors.Is(err, ErrRoomFull) {
		t.Fatalf("a third person got into a room of two: %v", err)
	}
}

// TestLeaveLoginHangsUpThatDevice: revoking one browser's login has to stop
// that browser's microphone, and leave the person's other device alone.
func TestLeaveLoginHangsUpThatDevice(t *testing.T) {
	s := testServer(t, 8)
	newClient(t, s, "sala", 1, "alice")
	s.LeaveLogin("alice-login")
	waitFor(t, "alice's device to be dropped", func() bool {
		return len(s.Rooms([]string{"sala"})["sala"]) == 0
	})
}

// TestRoomsOnlyAnswersWhatItIsAsked: the media server never decides who may see
// a room. It reports on the ids it is handed and invents nothing.
func TestRoomsOnlyAnswersWhatItIsAsked(t *testing.T) {
	s := testServer(t, 8)
	newClient(t, s, "sala-da-alice", 1, "alice")

	got := s.Rooms([]string{"outra-sala"})
	if len(got) != 1 {
		t.Fatalf("asked about one room, got %d", len(got))
	}
	if len(got["outra-sala"]) != 0 {
		t.Fatalf("a room nobody is in came back with people: %+v", got)
	}
	if _, leaked := got["sala-da-alice"]; leaked {
		t.Fatal("a room that was not asked about came back anyway")
	}
}
