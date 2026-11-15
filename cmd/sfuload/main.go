// Command sfuload is the instrument for the D2 measurement: it fills a voice
// channel with as many real WebRTC connections as you ask for and reports what
// the media server spent on them.
//
// What is real and what is not, stated plainly, because a measurement is only
// worth the honesty of its instrument.
//
// Real: the connections. Each participant is a full WebRTC peer — ICE over a
// real UDP socket, a real DTLS handshake, real SRTP. It signs in through the
// ordinary API with an ordinary account and joins through the ordinary
// endpoint. The server treats it exactly as it treats a browser, because from
// the server's side there is nothing to tell apart.
//
// Real too, and it was not always: one ACCOUNT per participant. The server
// hangs up an account's other connection the moment it arrives anywhere —
// "one person is in one place" — so eight peers behind a single login evicted
// each other as they joined and the room never held more than one. That rule
// arrived after the D2 measurement and this program only noticed on
// 18/09/2026, by which time it could no longer measure anything at all.
//
// Not real: what is inside the packets. The payloads are filler. That is not a
// shortcut, it is the whole point of a selective forwarding unit: it never
// decodes anything (PLANO-DISCORD.md §4), so its cost is decided by how many
// packets arrive, how big they are, and how many people they have to be
// re-encrypted for — and by nothing else. Feeding it a real screen recording
// would cost the same and would only add the encoder's own cost to the machine,
// which is the client's problem and not the server's.
//
// The one thing this cannot measure is whether a conversation feels good. That
// is distance, and it needs people (PLANO-DISCORD.md §6, the D2 acceptance).
//
// Mint the invites first — the accounts are made through the ordinary
// invite-only signup, like everybody else's, and they are reused on the next
// run:
//
//	harmony -db <db> -invite -n 8 > convites.txt
//	sfuload -url http://127.0.0.1:8081 -user bench -invites convites.txt \
//	        -admin <uma conta de administrador> -room geral \
//	        -n 8 -video 1 -seconds 60
//
// -admin is separate because /api/voice/stats, where the numbers come from,
// may only be read by an administrator, and the participants are deliberately
// ordinary accounts. The load generator does not get privileges it would not
// have in real life.
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"net/http/cookiejar"
	"net/netip"
	"net/url"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
	"github.com/pion/interceptor"
	"github.com/pion/webrtc/v4"
	"github.com/pion/webrtc/v4/pkg/media"
)

func main() {
	base := flag.String("url", "http://127.0.0.1:8081", "the harmony being measured")
	user := flag.String("user", "bench", "account to sign in as")
	pass := flag.String("pass", "", "its password")
	room := flag.String("room", "geral", "name of the voice channel to fill")
	n := flag.Int("n", 8, "how many participants")
	seconds := flag.Int("seconds", 60, "how long to hold the room, after everybody is connected")
	warmup := flag.Int("warmup", 10, "seconds to let the room settle before the numbers start counting")
	video := flag.Int("video", 1, "how many of them also share a screen")
	videoKbps := flag.Int("video-kbps", 2700, "bitrate of each shared screen (1080p60 measured in sprint 8)")
	videoFPS := flag.Int("video-fps", 60, "frames a second of each shared screen")
	cameras := flag.Int("cameras", 0, "how many of them also turn on a camera")
	cameraKbps := flag.Int("camera-kbps", 1500, "bitrate of each camera (720p)")
	cameraFPS := flag.Int("camera-fps", 30, "frames a second of each camera")
	audioKbps := flag.Int("audio-kbps", 40, "bitrate of each microphone (opus voice)")
	label := flag.String("label", "", "a name for this run, printed with the result")
	invites := flag.String("invites", "", "file with one invite code per line, used only for accounts that do not exist yet")
	admin := flag.String("admin", "", "account allowed to read /api/voice/stats; empty means the first participant")
	watch := flag.String("watch", "", "every participant asks to watch this person's screen, the way a person presses \"assistir\"")
	spread := flag.Bool("spread", false, "with -watch: participant i keeps asking for layer alta, media, baixa in turn, so every layer has somebody on it")
	flag.Parse()

	if *pass == "" {
		*pass = os.Getenv("SFULOAD_PASS")
	}
	if *pass == "" {
		die("no password: pass -pass or set SFULOAD_PASS")
	}

	people, err := gather(*base, *user, *pass, *invites, *n)
	if err != nil {
		die("%v", err)
	}
	defer people.closeGateways()

	// The numbers come from /api/voice/stats, which only an administrator may
	// read. The participants are ordinary accounts made from invites, so on a
	// database that already had a first user none of them can read it: say so
	// with -admin rather than making the load generator privileged.
	first := people.h[0]
	if *admin != "" {
		h, err := newHarness(*base)
		if err != nil {
			die("%v", err)
		}
		if err := h.login(*admin, *pass); err != nil {
			die("entrar como %q para ler as estatisticas: %v", *admin, err)
		}
		first = h
	}

	roomID, err := first.voiceChannel(*room)
	if err != nil {
		die("find the voice channel: %v", err)
	}
	fmt.Printf("sala %q (%s) em %s\n", *room, roomID, *base)

	for i := 0; i < *n; i++ {
		c := people.h[i]
		p := &peer{h: c, index: i}
		p.audio = trackSpec{kbps: *audioKbps, fps: 50, mime: webrtc.MimeTypeOpus, id: fmt.Sprintf("mic-%d", i)}
		if i < *video {
			p.video = &trackSpec{kbps: *videoKbps, fps: *videoFPS, mime: webrtc.MimeTypeVP8, id: fmt.Sprintf("tela-%d", i)}
		} else if i < *video+*cameras {
			p.video = &trackSpec{kbps: *cameraKbps, fps: *cameraFPS, mime: webrtc.MimeTypeVP8, id: fmt.Sprintf("camera-%d", i)}
		}
		if err := p.join(roomID); err != nil {
			die("participant %d: %v", i, err)
		}
		c.add(p)
		// Since 18/09/2026 a screen reaches only the people who asked for it,
		// and a browser whose screen nobody watches is told to stop encoding
		// it. Without this a bench measuring a screen measures an idle
		// encoder (scripts/bancada).
		if *watch != "" {
			if err := c.post("/api/voice/watch", map[string]any{
				"session_id": p.session, "username": *watch, "watching": true,
			}, nil); err != nil {
				die("participant %d watching %q: %v", i, *watch, err)
			}
		}
		fmt.Printf("  %d/%d dentro\n", i+1, *n)
		// A short gap, so eight connections are not eight simultaneous DTLS
		// handshakes. People do not all press the button in the same
		// millisecond, and measuring a thundering herd would flatter nothing.
		time.Sleep(300 * time.Millisecond)
	}

	if err := people.waitConnected(30 * time.Second); err != nil {
		die("%v", err)
	}
	if *watch != "" && *spread {
		go people.spreadLayers(*watch)
	}
	fmt.Printf("todos conectados; %d s de aquecimento\n", *warmup)
	time.Sleep(time.Duration(*warmup) * time.Second)

	before, err := first.stats()
	if err != nil {
		die("stats: %v", err)
	}
	start := time.Now()
	fmt.Printf("medindo por %d s\n", *seconds)
	time.Sleep(time.Duration(*seconds) * time.Second)
	after, err := first.stats()
	if err != nil {
		die("stats: %v", err)
	}
	elapsed := time.Since(start).Seconds()

	people.stopAll()
	report(*label, before, after, elapsed, *n, *video, *cameras)
}

// ---------------------------------------------------------------- reporting

type stats struct {
	Rooms         int     `json:"rooms"`
	Peers         int     `json:"peers"`
	Publications  int     `json:"publications"`
	Subscriptions int     `json:"subscriptions"`
	BytesIn       int64   `json:"bytes_in"`
	BytesOut      int64   `json:"bytes_out"`
	PacketsIn     int64   `json:"packets_in"`
	PacketsOut    int64   `json:"packets_out"`
	CPUSeconds    float64 `json:"cpu_seconds"`
	RSSBytes      int64   `json:"rss_bytes"`
	HeapBytes     int64   `json:"heap_bytes"`
	Goroutines    int     `json:"goroutines"`
	NumCPU        int     `json:"num_cpu"`
}

func report(label string, a, b stats, elapsed float64, n, video, cameras int) {
	mbps := func(bytes int64) float64 { return float64(bytes) * 8 / elapsed / 1e6 }
	cpu := (b.CPUSeconds - a.CPUSeconds) / elapsed

	fmt.Println()
	if label != "" {
		fmt.Printf("== %s ==\n", label)
	}
	fmt.Printf("cenario          %d participantes, %d tela(s), %d camera(s)\n", n, video, cameras)
	fmt.Printf("janela           %.0f s\n", elapsed)
	fmt.Printf("dentro da sala   %d pessoas, %d transmissoes, %d assinaturas\n", b.Peers, b.Publications, b.Subscriptions)
	fmt.Println()
	fmt.Printf("CPU              %.1f%% de um nucleo  (%.1f%% da maquina de %d nucleos)\n",
		cpu*100, cpu*100/float64(max(b.NumCPU, 1)), b.NumCPU)
	fmt.Printf("banda entrada    %.2f Mbit/s   (%d pacotes/s)\n", mbps(b.BytesIn-a.BytesIn), int(float64(b.PacketsIn-a.PacketsIn)/elapsed))
	fmt.Printf("banda saida      %.2f Mbit/s   (%d pacotes/s)\n", mbps(b.BytesOut-a.BytesOut), int(float64(b.PacketsOut-a.PacketsOut)/elapsed))
	fmt.Printf("memoria (RSS)    %.0f MB\n", float64(b.RSSBytes)/1e6)
	fmt.Printf("memoria (heap)   %.0f MB\n", float64(b.HeapBytes)/1e6)
	fmt.Printf("goroutines       %d\n", b.Goroutines)
	fmt.Println()
	// The number that decides the sprint: how many rooms like this one the
	// machine could hold before the processor is the limit.
	if cpu > 0 {
		fmt.Printf("cabem            ~%.0f salas iguais a esta antes da CPU acabar\n",
			float64(b.NumCPU)/cpu)
	}
}

// ---------------------------------------------------------------- the client

type harness struct {
	base string
	http *http.Client
	api  *webrtc.API

	ws     *websocket.Conn
	wsOnce sync.Once
	// stopping silences the offers that arrive while the room is emptying:
	// every departure renegotiates everybody, and answering on a connection
	// this program has just closed is teardown, not a fault.
	stopping atomic.Bool

	mu    sync.Mutex
	peers map[string]*peer
}

// loopbackJar hands the Secure session cookie back to a harmony on this
// machine over plain http, the way browsers and Go 1.26's cookiejar do. Up to
// Go 1.25 the jar keeps that cookie and never sends it, so against the default
// -url every request after signing in was anonymous and the run failed with a
// 401, unless the server had been started with HARMONY_INSECURE_COOKIE=1.
// Any other http address still gets no token: it would travel in the clear.
type loopbackJar struct{ http.CookieJar }

func (j loopbackJar) SetCookies(u *url.URL, cookies []*http.Cookie) {
	j.CookieJar.SetCookies(secureIfLoopback(u), cookies)
}

func (j loopbackJar) Cookies(u *url.URL) []*http.Cookie {
	return j.CookieJar.Cookies(secureIfLoopback(u))
}

func secureIfLoopback(u *url.URL) *url.URL {
	if u.Scheme != "http" || !isLoopback(u.Hostname()) {
		return u
	}
	c := *u
	c.Scheme = "https"
	return &c
}

func isLoopback(host string) bool {
	host = strings.TrimSuffix(strings.ToLower(host), ".")
	if host == "localhost" || strings.HasSuffix(host, ".localhost") {
		return true
	}
	ip, err := netip.ParseAddr(host)
	return err == nil && ip.IsLoopback()
}

func newHarness(base string) (*harness, error) {
	jar, err := cookiejar.New(nil)
	if err != nil {
		return nil, err
	}
	se := webrtc.SettingEngine{}
	se.SetIncludeLoopbackCandidate(true)
	m := &webrtc.MediaEngine{}
	if err := m.RegisterDefaultCodecs(); err != nil {
		return nil, err
	}
	registry := &interceptor.Registry{}
	if err := webrtc.RegisterDefaultInterceptors(m, registry); err != nil {
		return nil, err
	}
	h := &harness{
		base:  strings.TrimSuffix(base, "/"),
		http:  &http.Client{Jar: loopbackJar{jar}, Timeout: 30 * time.Second},
		api:   webrtc.NewAPI(webrtc.WithSettingEngine(se), webrtc.WithMediaEngine(m), webrtc.WithInterceptorRegistry(registry)),
		peers: map[string]*peer{},
	}
	return h, nil
}

func (h *harness) login(user, pass string) error {
	return h.post("/api/login", map[string]string{"username": user, "password": pass}, nil)
}

func (h *harness) signUp(code, user, pass string) error {
	// An address is required since D10; example.org is reserved and never
	// delivers, so the test harmony's confirmation mail goes nowhere.
	return h.post("/api/signup", map[string]string{
		"code": code, "username": user, "display_name": user, "password": pass,
		"email": user + "@example.org",
	}, nil)
}

// ---------------------------------------------------------------- the crowd

// crowd is the people in the room, and it is one ACCOUNT per person rather
// than several connections behind a single login.
//
// The server hangs up an account's other connection the moment that account
// arrives anywhere — "one person is in one place", sfu.Join calling
// LeaveAllFor. That rule was written after the D2 measurement, and until
// 18/09/2026 this program had not noticed: eight participants behind one login
// evicted each other as they joined, so the room never held more than one and
// the run ended saying nobody had connected in time. It was measuring one
// participant, eight times over.
type crowd struct {
	h []*harness
}

// gather signs everybody in, creating whatever accounts are missing.
//
// It tries to sign in before it spends an invite, so a second run costs
// nothing: the accounts the first run created are still there. Invites are
// single use by design — they are the whole access control of this service —
// and minting them stays the operator's job:
//
//	harmony -db <db> -invite -n 8 > convites.txt
func gather(base, prefix, pass, invitesPath string, n int) (*crowd, error) {
	codes, err := readInvites(invitesPath)
	if err != nil {
		return nil, err
	}
	spent := 0
	c := &crowd{}
	for i := 0; i < n; i++ {
		name := fmt.Sprintf("%s-%d", prefix, i)
		h, err := newHarness(base)
		if err != nil {
			return nil, err
		}
		if err := h.login(name, pass); err != nil {
			// An invite is single use, and a file of them will usually hold
			// some already spent: by an earlier run, or by the administrator
			// account that reads the numbers. A spent code is not a failure,
			// it is the next line down. Anything else is a real refusal and
			// stops here.
			created := false
			for spent < len(codes) && !created {
				code := codes[spent]
				spent++
				switch errUp := h.signUp(code, name, pass); {
				case errUp == nil:
					created = true
				case strings.Contains(errUp.Error(), "invite_unusable"):
					continue
				default:
					return nil, fmt.Errorf("criar %q: %v", name, errUp)
				}
			}
			if !created {
				return nil, fmt.Errorf(
					"a conta %q nao existe e nao sobrou convite utilizavel; gere com "+
						"'harmony -invite -n %d' e passe -invites <arquivo> (%v)",
					name, n, err)
			}
			if err := h.login(name, pass); err != nil {
				return nil, fmt.Errorf("entrar como %q: %v", name, err)
			}
		}
		if err := h.openGateway(); err != nil {
			return nil, fmt.Errorf("conexao viva de %q: %v", name, err)
		}
		c.h = append(c.h, h)
	}
	if spent > 0 {
		fmt.Printf("contas criadas   %d de %d\n", spent, n)
	}
	return c, nil
}

// readInvites accepts both a bare code per line and the output of
// "harmony -invite" as it is printed, so the codes can be piped straight from
// one command to the other without anybody editing a file by hand.
func readInvites(path string) ([]string, error) {
	if path == "" {
		return nil, nil
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var codes []string
	for _, line := range strings.Split(string(raw), "\n") {
		line = strings.TrimSpace(line)
		if i := strings.LastIndex(line, "="); i >= 0 {
			line = line[i+1:]
		}
		if line != "" && !strings.ContainsAny(line, " \t/") {
			codes = append(codes, line)
		}
	}
	return codes, nil
}

// waitConnected holds everybody to one deadline rather than giving each person
// the full timeout, so eight slow arrivals cannot add up to eight timeouts.
func (c *crowd) waitConnected(timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for _, h := range c.h {
		if err := h.waitConnected(time.Until(deadline)); err != nil {
			return err
		}
	}
	return nil
}

func (c *crowd) stopAll() {
	for _, h := range c.h {
		h.stopAll()
	}
}

func (c *crowd) closeGateways() {
	for _, h := range c.h {
		h.closeGateway()
	}
}

func (h *harness) add(p *peer) {
	h.mu.Lock()
	h.peers[p.session] = p
	h.mu.Unlock()
}

func (h *harness) post(path string, body, out any) error {
	encoded, err := json.Marshal(body)
	if err != nil {
		return err
	}
	req, err := http.NewRequest(http.MethodPost, h.base+path, bytes.NewReader(encoded))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	// The browser sends it and the server checks it.
	req.Header.Set("Origin", h.base)
	return h.send(req, out)
}

func (h *harness) get(path string, out any) error {
	req, err := http.NewRequest(http.MethodGet, h.base+path, nil)
	if err != nil {
		return err
	}
	return h.send(req, out)
}

func (h *harness) send(req *http.Request, out any) error {
	res, err := h.http.Do(req)
	if err != nil {
		return err
	}
	defer res.Body.Close()
	raw, _ := io.ReadAll(res.Body)
	if res.StatusCode >= 300 {
		return fmt.Errorf("%s %s: %d %s", req.Method, req.URL.Path, res.StatusCode, strings.TrimSpace(string(raw)))
	}
	if out != nil && len(raw) > 0 {
		return json.Unmarshal(raw, out)
	}
	return nil
}

func (h *harness) voiceChannel(name string) (string, error) {
	var res struct {
		Channels []struct {
			ID   string `json:"id"`
			Kind string `json:"kind"`
			Name string `json:"name"`
		} `json:"channels"`
	}
	if err := h.get("/api/channels", &res); err != nil {
		return "", err
	}
	for _, c := range res.Channels {
		if c.Kind == "voice" && (c.Name == name || c.ID == name) {
			return c.ID, nil
		}
	}
	return "", fmt.Errorf("no voice channel called %q", name)
}

func (h *harness) stats() (stats, error) {
	var s stats
	err := h.get("/api/voice/stats", &s)
	return s, err
}

// openGateway holds the one live socket. Every participant here signs in as the
// same account on the same login, so every renegotiation offer arrives on this
// one connection and is routed by the session id inside it.
func (h *harness) openGateway() error {
	u, err := url.Parse(h.base)
	if err != nil {
		return err
	}
	scheme := "ws"
	if u.Scheme == "https" {
		scheme = "wss"
	}
	header := http.Header{}
	header.Set("Origin", h.base)
	for _, c := range h.http.Jar.Cookies(u) {
		header.Add("Cookie", c.Name+"="+c.Value)
	}
	ws, _, err := websocket.DefaultDialer.Dial(scheme+"://"+u.Host+"/ws", header)
	if err != nil {
		return err
	}
	h.ws = ws
	go h.readGateway()
	return nil
}

func (h *harness) closeGateway() {
	h.wsOnce.Do(func() {
		if h.ws != nil {
			h.ws.Close()
		}
	})
}

func (h *harness) readGateway() {
	for {
		_, raw, err := h.ws.ReadMessage()
		if err != nil {
			return
		}
		var ev struct {
			Type string          `json:"type"`
			Data json.RawMessage `json:"data"`
		}
		if json.Unmarshal(raw, &ev) != nil || ev.Type != "voice_offer" {
			continue
		}
		var offer offerMessage
		if json.Unmarshal(ev.Data, &offer) != nil {
			continue
		}
		h.mu.Lock()
		p := h.peers[offer.SessionID]
		h.mu.Unlock()
		if p == nil {
			continue
		}
		if err := p.answer(offer); err != nil {
			fmt.Fprintf(os.Stderr, "renegociacao do participante %d: %v\n", p.index, err)
		}
	}
}

func (h *harness) waitConnected(timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		h.mu.Lock()
		bad := 0
		for _, p := range h.peers {
			if p.pc.ConnectionState() != webrtc.PeerConnectionStateConnected {
				bad++
			}
		}
		h.mu.Unlock()
		if bad == 0 {
			return nil
		}
		time.Sleep(200 * time.Millisecond)
	}
	return fmt.Errorf("not everybody connected in time")
}

func (h *harness) stopAll() {
	h.stopping.Store(true)
	h.mu.Lock()
	list := make([]*peer, 0, len(h.peers))
	for _, p := range h.peers {
		list = append(list, p)
	}
	h.mu.Unlock()
	for _, p := range list {
		close(p.stop)
		_ = h.post("/api/voice/leave", map[string]string{"session_id": p.session}, nil)
		_ = p.pc.Close()
	}
}

// spreadLayers puts participant i on layer i mod 3 of the watched screen, and
// keeps saying so.
//
// The server only asks the sharer for the layers somebody is receiving, so a
// room of watchers on a fast loopback all take the top one and the two smaller
// encoders are switched off — which would make "three layers" measure the same
// as "one". A real room has a telephone in it.
//
// It is said again every two seconds because a ceiling applies to the screen
// that exists when it is set, and the bench shares and stops a new screen for
// every combination it measures. Saying it to a room with no screen is a
// no-op, and the errors are dropped for the same reason.
func (c *crowd) spreadLayers(who string) {
	for {
		for _, h := range c.h {
			if h.stopping.Load() {
				return
			}
			h.mu.Lock()
			list := make([]*peer, 0, len(h.peers))
			for _, p := range h.peers {
				list = append(list, p)
			}
			h.mu.Unlock()
			for _, p := range list {
				_ = h.post("/api/voice/layer", map[string]any{
					"session_id": p.session, "username": who,
					"layer": []string{"alta", "media", "baixa"}[p.index%3],
				}, nil)
			}
		}
		time.Sleep(2 * time.Second)
	}
}

// ---------------------------------------------------------------- one person

type trackSpec struct {
	kbps int
	fps  int
	mime string
	id   string
}

type offerMessage struct {
	SessionID string            `json:"session_id"`
	SDP       string            `json:"sdp"`
	SendMids  map[string]string `json:"send_mids"`
	Tracks    []struct {
		Mid      string `json:"mid"`
		Username string `json:"username"`
		Kind     string `json:"kind"`
	} `json:"tracks"`
}

type peer struct {
	h       *harness
	index   int
	session string
	pc      *webrtc.PeerConnection
	audio   trackSpec
	video   *trackSpec

	mic  *webrtc.TrackLocalStaticSample
	cam  *webrtc.TrackLocalStaticSample
	stop chan struct{}

	negotiating sync.Mutex
}

func (p *peer) join(roomID string) error {
	pc, err := p.h.api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		return err
	}
	p.pc = pc
	p.stop = make(chan struct{})

	// Everything this participant sends goes on before the offer arrives, the
	// same order the web client uses: a slot the server offers as "you send
	// here" needs a track already waiting for it.
	p.mic, err = webrtc.NewTrackLocalStaticSample(
		webrtc.RTPCodecCapability{MimeType: p.audio.mime}, p.audio.id, fmt.Sprintf("bench-%d", p.index))
	if err != nil {
		return err
	}
	if _, err := pc.AddTrack(p.mic); err != nil {
		return err
	}
	if p.video != nil {
		p.cam, err = webrtc.NewTrackLocalStaticSample(
			webrtc.RTPCodecCapability{MimeType: p.video.mime}, p.video.id, fmt.Sprintf("bench-%d", p.index))
		if err != nil {
			return err
		}
		if _, err := pc.AddTrack(p.cam); err != nil {
			return err
		}
	}
	// Everything arriving is read and thrown away. It still has to be read:
	// what is not read is not decrypted, and a measurement of a server nobody
	// is listening to would be a measurement of half the work.
	pc.OnTrack(func(remote *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		buf := make([]byte, 1600)
		for {
			if _, _, err := remote.Read(buf); err != nil {
				return
			}
		}
	})
	pc.OnICECandidate(func(c *webrtc.ICECandidate) {
		if c == nil || p.session == "" {
			return
		}
		init := c.ToJSON()
		_ = p.h.post("/api/voice/candidate", map[string]any{
			"session_id": p.session, "candidate": init.Candidate,
			"sdp_mid": init.SDPMid, "sdp_mline_index": init.SDPMLineIndex,
		}, nil)
	})

	publish := []string{"audio"}
	if p.video != nil {
		publish = append(publish, "video")
	}
	var offer offerMessage
	if err := p.h.post("/api/voice/"+roomID+"/join", map[string]any{"publish": publish}, &offer); err != nil {
		return err
	}
	p.session = offer.SessionID
	if err := p.answer(offer); err != nil {
		return err
	}
	p.send(p.mic, p.audio)
	if p.cam != nil {
		p.send(p.cam, *p.video)
	}
	return nil
}

func (p *peer) answer(offer offerMessage) error {
	p.negotiating.Lock()
	defer p.negotiating.Unlock()

	if err := p.pc.SetRemoteDescription(webrtc.SessionDescription{
		Type: webrtc.SDPTypeOffer, SDP: offer.SDP,
	}); err != nil {
		return err
	}
	for _, tr := range p.pc.GetTransceivers() {
		if tr.Sender() == nil || tr.Sender().Track() != nil {
			continue
		}
		switch tr.Mid() {
		case offer.SendMids["audio"]:
			_ = tr.Sender().ReplaceTrack(p.mic)
		case offer.SendMids["video"]:
			if p.cam != nil {
				_ = tr.Sender().ReplaceTrack(p.cam)
			}
		}
	}
	answer, err := p.pc.CreateAnswer(nil)
	if err != nil {
		return err
	}
	if err := p.pc.SetLocalDescription(answer); err != nil {
		return err
	}
	return p.h.post("/api/voice/answer", map[string]any{
		"session_id": p.session, "sdp": p.pc.LocalDescription().SDP,
	}, nil)
}

// send writes filler at the shape of the real thing: the right number of frames
// a second, each the right number of bytes, which is what turns into the right
// number of packets of the right size on the wire.
func (p *peer) send(track *webrtc.TrackLocalStaticSample, spec trackSpec) {
	frame := spec.kbps * 1000 / 8 / spec.fps
	if frame < 1 {
		frame = 1
	}
	payload := make([]byte, frame)
	// Not zeroes: a run of zeroes is exactly the thing a compressor would eat,
	// and although nothing here compresses, filler that looks like data costs
	// nothing extra and removes the question.
	for i := range payload {
		payload[i] = byte(i * 31)
	}
	interval := time.Second / time.Duration(spec.fps)
	go func() {
		tick := time.NewTicker(interval)
		defer tick.Stop()
		for {
			select {
			case <-p.stop:
				return
			case <-tick.C:
				if err := track.WriteSample(media.Sample{Data: payload, Duration: interval}); err != nil {
					return
				}
			}
		}
	}()
}

func die(format string, args ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", args...)
	os.Exit(1)
}
