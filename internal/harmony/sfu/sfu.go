// Package sfu is harmony's own media server: the thing that lets more than two
// people talk at once.
//
// What it does, in one paragraph. Every person in a voice channel opens one
// connection to this server and sends their microphone up it. The server takes
// each arriving packet and writes it, byte for byte, to everybody else in the
// room. It never opens a packet: it does not decode audio, does not mix, does
// not re-encode, does not even parse the payload. That is the whole reason a
// small machine can hold a room — the cost per packet is a decrypt, a copy and
// an encrypt, and nothing that grows with picture size or sample rate.
//
// Two rules shape the code.
//
// The server is always the one who offers. A WebRTC connection breaks if both
// ends offer at the same moment, and the usual cure ("perfect negotiation",
// PROTOCOL.md) exists because in a 1:1 call either side may legitimately go
// first. Here there is an obvious authority, so there is no race to resolve:
// the browser never creates an offer, it only ever answers one. That removes
// the whole class of glare bugs from the group path and leaves perfect
// negotiation where it belongs, in the direct 1:1 call this does not touch.
//
// A session belongs to one person and one device. Every call below takes the
// asker's user id alongside the session id and refuses if they disagree, so
// knowing somebody else's session identifier buys nothing: not their audio,
// not the power to hang up on them.
package sfu

import (
	"crypto/rand"
	"encoding/base64"
	"errors"
	"io"
	"log/slog"
	"net"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/pion/ice/v4"
	"github.com/pion/rtcp"
	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"
)

// Errors the API turns into status codes. They are as coarse as the store's:
// "not yours" and "no such thing" must not be told apart from the outside.
var (
	// ErrNoSession covers an unknown session and a session belonging to
	// somebody else, deliberately as one error.
	ErrNoSession = errors.New("sfu: no such session")
	// ErrRoomFull is the per-room cap in Config.MaxPerRoom.
	ErrRoomFull = errors.New("sfu: room full")
	// ErrClosed is a call after Close.
	ErrClosed = errors.New("sfu: closed")
)

// readBuffer is one MTU's worth. Nothing here inspects the bytes; the buffer
// exists only to carry them from the decrypting reader to the encrypting
// writer.
const readBuffer = 1500

// How often each forwarded track reports what its own loop is costing, and the
// line above which one turn of that loop counts as slow.
//
// This is not curiosity. Pion's transport-cc interceptor stamps a packet's
// arrival time inside the Read below — at the moment *this* loop asks for the
// packet, not when it landed on the machine — and then hands it to its own
// goroutine over a channel with no buffer
// (interceptor@v0.1.48/pkg/twcc/sender_interceptor.go). Every millisecond this
// loop spends forwarding, or waiting for that goroutine to finish writing a
// feedback report, is therefore told to the sender as if the network had been
// slow. A sender answers delay by sending less, and it does that without
// losing a single packet — which is the exact shape of what the owner measured
// on 18/09/2026: a screen that carried 3,9 Mbit/s falling to 36 kbit/s with
// perda=0% and never coming back.
//
// So the question is whether this loop stalls. If it never does, the cause is
// somewhere else and this rules out the server. If it stalls for tens of
// milliseconds, it is the answer.
const (
	readReportEvery = 10 * time.Second
	slowWork        = 5 * time.Millisecond
	// A microphone sends a packet every twenty milliseconds, like a clock. A
	// read that takes longer than this on a microphone did not wait for the
	// network: something held it. On video the same number means little, since
	// a screen that is not changing simply makes no packets — so this is read
	// with the microphone in mind and the screen ignored.
	slowRead = 50 * time.Millisecond
)

// Peer is one person inside a room, as everybody else sees them.
type Peer struct {
	SessionID   string `json:"session_id"`
	Username    string `json:"username"`
	DisplayName string `json:"display_name"`
	Avatar      string `json:"avatar"`
	Muted       bool   `json:"muted"`
	Deafened    bool   `json:"deafened"`
	JoinedAt    int64  `json:"joined_at"`
	// Publishing is what this person is sending right now, by source. It is
	// how the room can say "fulano está com a tela ligada" without anybody
	// having to be subscribed to find out, and it is derived from what is
	// actually arriving rather than from what somebody said they would send.
	Publishing []string `json:"publishing"`
}

// TrackInfo tells the browser which incoming stream belongs to whom, and what
// it is. The mid is WebRTC's own label for a slot in the connection, and it is
// the only handle both ends agree on that survives renegotiation. Source is
// what makes a wall of squares readable: a screen is drawn large, a camera
// small, and a screen's own sound gets a volume separate from the voice of the
// person sharing it.
type TrackInfo struct {
	Mid         string `json:"mid"`
	Username    string `json:"username"`
	DisplayName string `json:"display_name"`
	Avatar      string `json:"avatar"`
	Kind        string `json:"kind"`
	Source      string `json:"source"`
}

// Offer is what the browser has to answer. SendMids names the slots the browser
// is expected to send into, by source: which m-line is the microphone, which is
// the screen, which is the screen's sound. The browser is told which slot is
// which rather than counting m-lines, because the count changes every time
// somebody joins or somebody starts sharing.
type Offer struct {
	ICERestart bool              `json:"ice_restart,omitempty"`
	SessionID  string            `json:"session_id"`
	SDP        string            `json:"sdp"`
	Type       string            `json:"type"`
	SendMids   map[string]string `json:"send_mids"`
	Tracks     []TrackInfo       `json:"tracks"`
}

// The sources a device may send. Nothing outside this list is accepted: the
// list decides how many slots the server opens, and an unbounded list would be
// an unbounded connection.
//
// Why sources and not just "audio" and "video". A person sharing a film sends
// two sounds — their voice and the film's — and a wall of video sends pictures
// that mean completely different things. Naming them here means the browser
// never has to guess from a picture's shape what it is looking at, and it is
// what lets a screen be pinned to VP9 while a camera is not (codecs.go).
const (
	SourceMic         = "mic"
	SourceCamera      = "cam"
	SourceScreen      = "screen"
	SourceScreenAudio = "screen_audio"
)

// maxSlotsPerSession bounds how many times one connection may open and close a
// screen or a camera before it has to be rebuilt. A slot that is closed is not
// reused — the browser picks a new synchronisation source every time it starts
// a fresh track, and a slot that has been told to expect the old one is a slot
// that receives nothing. Thirty-two is far past a whole evening of sharing and
// stopping, and the description of a connection that grew without a limit
// would eventually be a description too large to send.
const maxSlotsPerSession = 32

// ErrTooManySlots is that limit. The cure is to leave the room and come back,
// which builds a new connection.
var ErrTooManySlots = errors.New("sfu: too many slots")

// The three refusals a client-offered negotiation can meet. See ClientOffer.
var (
	// A source this server does not carry. Refused rather than guessed at, for
	// the same reason addSlot refuses one.
	ErrBadSource = errors.New("sfu: unknown source")
	// One of this server's own offers is already in flight. The browser is
	// asked to come back rather than handed an answer to a question the
	// connection has already moved past — and coming back is cheap, because
	// the offer in flight is about to settle.
	ErrBusyNegotiating = errors.New("sfu: renegotiation in flight")
	// The offer arrived and brought no new picture slot with it, so there is
	// nothing to name. A browser that asks to share a screen and then does not
	// offer one gets a refusal instead of a slot pointing at nothing.
	ErrNoNewSlot = errors.New("sfu: offer opened no slot")
)

// sourceKind says whether a source is carried on an audio or a video slot.
// An unknown source is refused rather than guessed at.
func sourceKind(source string) (webrtc.RTPCodecType, bool) {
	switch source {
	case SourceMic, SourceScreenAudio:
		return webrtc.RTPCodecTypeAudio, true
	case SourceCamera, SourceScreen:
		return webrtc.RTPCodecTypeVideo, true
	}
	return 0, false
}

// Config is everything the media server needs from its environment.
type Config struct {
	// UDPPort is the one port every participant's media arrives on. One port,
	// not a range: the ICE layer tells connections apart by the credentials
	// inside each packet, so a range would only be more holes in the firewall
	// for exactly the same result.
	UDPPort int
	// PublicIP is the address to advertise. Inside a container the server only
	// sees a private address, and a candidate nobody can route to is a call
	// that never connects.
	PublicIP string
	// MaxPerRoom is the cap the plan asks for: voice channels of four to eight.
	MaxPerRoom int
	// Signal delivers a renegotiation offer to one device — the device that
	// joined, named by its login session, never "every device this person has".
	Signal func(userID int64, loginSession, kind string, data any)
	// RoomChanged is called whenever who-is-inside changes. The SFU does not
	// know who may see a room; it hands the room id up and the store answers.
	RoomChanged func(roomID string)
	// IncludeLoopback offers 127.0.0.1 as an address. Only the tests want it:
	// in production it is a candidate no browser can ever reach.
	IncludeLoopback bool
	Logger          *slog.Logger
}

// SFU owns the UDP socket and every room on it.
type SFU struct {
	cfg     Config
	log     *slog.Logger
	api     *webrtc.API
	mux     ice.UDPMux
	conn    *net.UDPConn
	arrival *arrivalTrace
	closed  atomic.Bool

	mu       sync.RWMutex
	rooms    map[string]*room
	sessions map[string]*session

	// Counters for the measurement. Bytes as they cross the media server, not
	// as the operating system saw them, so a number here means "this is what
	// forwarding cost" with no TLS, backup or ARP traffic mixed in.
	bytesIn    atomic.Int64
	bytesOut   atomic.Int64
	packetsIn  atomic.Int64
	packetsOut atomic.Int64
	startedAt  time.Time
	// stop ends the heartbeat goroutine when the SFU is closed; without it
	// every New left one behind, waking fifty times a second for ever.
	stop chan struct{}
	// joinedRoom is a test seam: it runs in Join right after the room is
	// looked up, which is the window a leaving last person used to empty it
	// in. Nil in production.
	joinedRoom func(roomID string)
	// connectWithin is how long a session may take to connect for the first
	// time before it is let go of (connectDeadline). A variable only so the
	// test does not have to wait half a minute.
	connectWithin time.Duration
}

// connectDeadline is the time a new session gets to connect. Until it does,
// the connection timeouts never start — ICE only begins once the answer is in
// — so a device whose join reply was lost on the way (a network switch mid
// request, found by the Codex audit of 25/09/2026) left a session nobody
// could hand back, sitting in the room for everybody to see until the same
// account joined again. Thirty seconds is far past any real connect (one to
// two seconds, a relayed one a few more).
const connectDeadline = 30 * time.Second

// room is one voice channel while people are in it. Rooms are not stored: a
// room exists while somebody is inside and vanishes when the last person
// leaves, which is exactly what "who is in the voice channel" means.
type room struct {
	id  string
	mu  sync.RWMutex
	sfu *SFU
	// peers is by session id: the same person may be in from two devices, and
	// the connection, not the account, is what has audio on it.
	peers map[string]*session
	pubs  map[string]*publication
	// gone is set, under both s.mu and r.mu, when the last person leaves and
	// the room is taken out of the index. A Join that fetched this room just
	// before that must not move into it: nobody would ever find it there, and
	// the next person to arrive would get a second room for the same channel
	// and hear nobody. Codex, 23/09/2026.
	gone bool
}

// publication is one track somebody is sending. One local track is shared by
// every subscriber: Pion gives each subscriber's sender its own synchronisation
// source, so a single write reaches all of them and the copy happens once.
type publication struct {
	clockMu  sync.Mutex
	clocks   map[string]captureReport
	id       string
	owner    *session
	kind     string // "audio" or "video"
	source   string // "mic", "cam", "screen", "screen_audio"
	local    *webrtc.TrackLocalStaticRTP
	subs     atomic.Int64
	askAgain func() // ask the sender for a fresh full picture

	// The last time this publication's sender was asked to start over, so that
	// a room asking together is asked upstream once. See keyframe().
	kfMu   sync.Mutex
	kfLast time.Time
	// The same, per layer, when there are layers: three layers are three
	// encoders, and asking one of them to start over says nothing to the other
	// two. See askLayer().
	kfPer map[string]time.Time

	// Simulcast (D4r). A screen arrives as up to three tracks on one
	// transceiver; see simulcast.go. When simul is set, local above is nil and
	// every subscriber has its own track in outs, because two people on two
	// layers are two different streams and a stream cannot fork.
	simul    bool
	refs     atomic.Int64 // layers still arriving; the last one out closes it
	codec    webrtc.RTPCodecCapability
	trackID  string
	streamID string
	layMu    sync.Mutex
	layers   map[string]*layer
	outMu    sync.Mutex
	outs     map[string]*forwarder // by subscriber session id

	// Who to tell when a sender report has to be recomputed. See wakeSync().
	syncMu    sync.Mutex
	syncWakes map[chan struct{}]struct{}
}

// keyframeEvery is how often one sender can be asked for a fresh full picture,
// however many people ask.
//
// A full picture is ten to fifty times the size of an ordinary one. Everybody
// in the room is watching the same stream over their own line, so one bad
// second on a long route makes every one of them ask at the same moment, and
// asking the sender once per person turns a stutter into a pile of enormous
// frames — which fills the sender's upload, which loses more packets, which
// makes everybody ask again. That is the loop, and this is the thing that
// breaks it: the first ask goes through immediately and the rest of that second
// are answered by the picture the first one produced, because it is the same
// picture they were all going to get.
// Two seconds and not one. One was the first guess and it was still too
// generous: a full 1080p picture is a couple of hundred kilobytes leaving in
// one burst, with no shaping between here and the far end, and a burst like
// that is itself a cause of loss — which produces the next request. Once every
// two seconds is still fast enough that nobody sits looking at a broken
// picture, and it halves the biggest thing this server puts on the wire.
const keyframeEvery = 2 * time.Second

// keyframe asks the sender to start over, at most once every keyframeEvery.
// It returns whether the request actually went upstream, which is what the test
// looks at.
func (p *publication) keyframe() bool {
	now := time.Now()
	p.kfMu.Lock()
	if !p.kfLast.IsZero() && now.Sub(p.kfLast) < keyframeEvery {
		p.kfMu.Unlock()
		return false
	}
	p.kfLast = now
	p.kfMu.Unlock()
	p.askAgain()
	return true
}

// slot is one m-line this device sends into: the microphone, a camera, a
// screen, a screen's sound. It exists so that the offer can tell the browser
// "put your screen in mid 4" instead of "put your video somewhere", which
// matters as soon as one connection carries two sounds and two pictures.
//
// A slot is never reused after it is closed. Stopping it is what makes the
// browser's own sender go away, and it is also what makes the read on our side
// end — which is how a publication disappears from the room without anybody
// having to be told twice.
type slot struct {
	source string
	tr     *webrtc.RTPTransceiver
	live   bool
}

// isOwnSlot says whether snd went out on a slot this device opened to send
// something of its own.
func (sess *session) isOwnSlot(snd *webrtc.RTPSender) bool {
	sess.slotMu.Lock()
	defer sess.slotMu.Unlock()
	for _, sl := range sess.slots {
		if sl.tr.Sender() == snd {
			return true
		}
	}
	return false
}

// session is one device's connection.
type session struct {
	id     string
	userID int64
	// login is the id of the browser's cookie session, so a renegotiation
	// offer reaches the device that joined and not the person's other phone.
	login       string
	username    string
	displayName string
	room        *room
	pc          *webrtc.PeerConnection
	joinedAt    time.Time
	// connected is set the first time the connection stands up.
	connected atomic.Bool
	// One bounded grace per outage; repeated requests cannot keep a ghost alive.
	lostAt        atomic.Int64
	outageSeq     atomic.Int64
	outputDelayMs atomic.Int64 // Receiver-only screen audio clock compensation.

	stateMu  sync.RWMutex
	muted    bool
	deafened bool

	// negMu serialises renegotiation. Two people joining in the same second
	// would otherwise produce two offers racing down one connection, and the
	// second would be answered against a state the first had already moved.
	negMu sync.Mutex
	// Payload assignments survive retired m-lines for this PeerConnection.
	// Pion also retains negotiated codecs; a stopped camera does not free PTs.
	reservedPayloads map[webrtc.PayloadType]string
	pending          atomic.Bool
	lastOffer        Offer  // protected by negMu, replayed when signalling was lost
	answerSDP        string // exact accepted HTTP answer, before trickled candidates
	restartOffer     bool   // protected by negMu

	// Whose screens this device has asked TO receive.
	//
	// A list of who is wanted, and not of who was refused, which is the whole
	// difference: a device that has just joined wants nobody's screen yet.
	// Watching is a decision somebody makes, not something that happens to
	// them — the owner's words on 18/09/2026, after a live where joining to
	// talk cost him a film he had not asked for.
	//
	// It is a real refusal rather than a hidden picture, so a telephone stops
	// paying for megabits nobody is looking at. Voices are never in here:
	// being in a room means hearing it.
	watchMu  sync.Mutex
	watching map[string]struct{}
	// Whether anybody in the room is receiving this device's screen. It is
	// told to the device, because a browser that nobody is watching should
	// stop encoding three layers for nobody. See watchersChanged.
	screenWatched atomic.Bool
	// The layers last asked for, joined by commas, so the same answer is not
	// sent twice. Empty means nobody is receiving anything.
	screenLayers atomic.Value

	// Routes the browser found before its answer arrived.
	//
	// The browser starts telling us how to reach it the moment it has written
	// its own answer down, and that message and the answer itself are two
	// separate requests racing each other over the network. A route that
	// arrives first cannot be given to the connection — there is nothing on the
	// other end of it yet — and refusing it means throwing away a way in. Here
	// it usually cost nothing, because one of the other routes worked; on a
	// stricter network the one that was thrown away is the only one there was,
	// and the call simply never connects.
	//
	// So they wait here and go in the moment the answer lands.
	iceMu    sync.Mutex
	iceEarly []webrtc.ICECandidateInit
	iceReady bool

	// slots are this device's own m-lines, in the order they were opened, and
	// they are what turns "here is a picture" into "here is Ana's screen".
	// opened counts every slot ever opened, including closed ones, because
	// that is the number maxSlotsPerSession is about.
	slotMu sync.Mutex
	slots  []*slot
	opened int
	// abandoned are transceivers a refused browser offer created. Nothing can
	// remove them, so they are never adopted as a slot. See abandonOffer.
	abandoned map[*webrtc.RTPTransceiver]bool
	// seq numbers publications so that starting a screen again, before the
	// previous one has finished being cleaned up, cannot have the old one
	// delete the new one.
	seq int

	// subs maps a publication id to the sender carrying it, so a publication
	// that goes away can be removed without walking every transceiver.
	subMu sync.Mutex
	subs  map[string]*webrtc.RTPSender
	// subOpMu holds one subscribe or unsubscribe of this device from start to
	// finish. subMu alone only guarded the map, so two subscriptions to the
	// same publication — the person showing starts a screen while this device
	// presses "assistir" — both saw nothing there, both built a sender, and
	// the map kept one of them: the other could never be removed. left is set
	// under it when the device is dropped, so a subscription that was
	// half-way through cannot land on a connection that is closing.
	subOpMu sync.Mutex
	left    bool
	// watchersMu makes "work out who watches this device's screen" and "tell
	// the device" one step. Two of them interleaved could send "switch on"
	// and then a stale "switch off", and the cache would already hold the new
	// answer, so nothing ever repeated it: a screen that froze with somebody
	// watching it.
	watchersMu sync.Mutex

	closeOnce sync.Once
}

// addSlot opens one m-line for a source, with the codecs that source is
// allowed to be (codecs.go). It reports false if a live slot for that source
// already exists, which is what makes "start sharing" safe to press twice.
func (s *SFU) addSlot(sess *session, source string) (bool, error) {
	kind, ok := sourceKind(source)
	if !ok {
		return false, nil
	}
	sess.slotMu.Lock()
	defer sess.slotMu.Unlock()
	for _, sl := range sess.slots {
		if sl.source == source && sl.live {
			return false, nil
		}
	}
	if sess.opened >= maxSlotsPerSession {
		return false, ErrTooManySlots
	}
	tr, err := sess.pc.AddTransceiverFromKind(kind,
		webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionRecvonly})
	if err != nil {
		return false, err
	}
	if kind == webrtc.RTPCodecTypeVideo {
		prefs := cameraCodecs()
		if source == SourceScreen {
			prefs = screenCodecs()
		}
		// A refusal here would mean this build offers fewer codecs than
		// codecs.go says it does. It is worth a line in the log, but a slot
		// carrying the default list still carries a picture, so it is not
		// worth refusing to share a screen over.
		if err := tr.SetCodecPreferences(prefs); err != nil {
			s.log.Error("codec preferences", "source", source, "err", err)
		}
	}
	sess.slots = append(sess.slots, &slot{source: source, tr: tr, live: true})
	sess.opened++
	return true, nil
}

// closeSlot shuts one source's m-line. Stopping the transceiver is deliberate
// rather than merely marking it inactive: it ends the read on our side, which
// is what removes the publication from the room, and it makes the browser's
// sender go away instead of lingering with a track nobody is looking at.
func (sess *session) closeSlot(source string) bool {
	sess.slotMu.Lock()
	var found *slot
	for _, sl := range sess.slots {
		if sl.source == source && sl.live {
			sl.live = false
			found = sl
		}
	}
	sess.slotMu.Unlock()
	if found == nil {
		return false
	}
	_ = found.tr.Stop()
	return true
}

// sourceOf says which source a slot's arriving track belongs to, found by the
// receiver the track came in on. An empty answer means a track arrived on a
// slot this server never opened, which cannot happen and is treated as one.
func (sess *session) sourceOf(recv *webrtc.RTPReceiver) string {
	sess.slotMu.Lock()
	defer sess.slotMu.Unlock()
	for _, sl := range sess.slots {
		if sl.tr.Receiver() == recv {
			return sl.source
		}
	}
	return ""
}

// nextSeq hands out the number that makes a publication id unique for the life
// of the connection.
func (sess *session) nextSeq() int {
	sess.slotMu.Lock()
	defer sess.slotMu.Unlock()
	sess.seq++
	return sess.seq
}

// onThisMachine says whether the address this server is going to advertise is
// the loopback — which is true for the Go tests and the browser suite, and for
// nothing else.
//
// It decides two things together, and they have to agree. The socket binds the
// loopback alone rather than every interface, and the loopback is then offered
// as a candidate, which Pion otherwise drops as unroutable. Bind narrow without
// offering it and the server gathers nothing at all: no candidates, no pairs,
// no call.
//
// The reason it is worth doing. On Windows, a program that binds 0.0.0.0
// raises the firewall's "allow this app?" box, and the box is asked again for
// every new path — which for `go test` is a new temporary directory on every
// build. The owner had 426 accumulated rules for three binaries of ours and,
// quite fairly, told us to stop. A socket bound to the loopback is never asked
// about. In production nothing changes: the public address is not loopback, so
// the socket takes every interface exactly as it always has.
func onThisMachine(advertised string) bool {
	ip := net.ParseIP(advertised)
	return ip != nil && ip.IsLoopback()
}

// New opens the media server's single UDP socket and prepares the WebRTC
// machinery. It does not accept anybody yet: a room only exists once somebody
// joins it.
// heartbeat says whether the whole process is stopping, which nothing else
// here can tell.
//
// It sleeps and looks at the clock, and that is all it does: it never reads,
// never writes, never takes a lock of ours. So when the forwarding loop reports
// a 160 ms write and this reports nothing, the pause belongs to the writing —
// and when both report it, the pause belongs to everybody and the writing is
// only where it was noticed.
//
// The garbage collector's own total comes along for the ride, because it is the
// third answer and it is free to read: a stall with the collector's clock
// moving is the collector, and a stall with it still is the process being taken
// off the machine by something underneath us.
func heartbeat(log *slog.Logger, stop <-chan struct{}) {
	const beat = 20 * time.Millisecond
	const late = 100 * time.Millisecond
	var last runtime.MemStats
	runtime.ReadMemStats(&last)
	pausedBefore := last.PauseTotalNs
	worst := time.Duration(0)
	var lates int64
	report := time.Now()
	for {
		at := time.Now()
		select {
		case <-stop:
			return
		case <-time.After(beat):
		}
		slept := time.Since(at)
		if over := slept - beat; over > worst {
			worst = over
		}
		if slept-beat > late {
			lates++
		}
		if time.Since(report) < readReportEvery {
			continue
		}
		var now runtime.MemStats
		runtime.ReadMemStats(&now)
		// Only worth a line when something actually happened: a heartbeat that
		// prints "everything is fine" every ten seconds forever is a log nobody
		// reads by the time it matters.
		if lates > 0 || worst > late/2 {
			log.Info("voice pulso",
				"atrasos", lates, "pior_us", worst.Microseconds(),
				"gc_pausa_us", (now.PauseTotalNs-pausedBefore)/1000,
				"gc_ciclos", now.NumGC-last.NumGC)
		}
		last, pausedBefore = now, now.PauseTotalNs
		worst, lates, report = 0, 0, time.Now()
	}
}

func New(cfg Config) (*SFU, error) {
	if cfg.Logger == nil {
		cfg.Logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	}
	if cfg.MaxPerRoom <= 0 {
		cfg.MaxPerRoom = 8
	}

	local := onThisMachine(cfg.PublicIP)
	bind := net.IPv4zero
	if local {
		bind = net.IPv4(127, 0, 0, 1)
	}
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: bind, Port: cfg.UDPPort})
	if err != nil {
		return nil, err
	}
	// A receive buffer big enough that a scheduling hiccup is not a lost
	// packet. Eight people in a room is a few thousand packets a second and the
	// default on some kernels drops under a burst.
	_ = conn.SetReadBuffer(1 << 20)
	_ = conn.SetWriteBuffer(1 << 20)

	se := webrtc.SettingEngine{}
	var diagnostic *arrivalTrace
	var packetConn net.PacketConn = conn
	if arrivalDiagnosticEnabled() {
		diagnostic = newArrivalTrace(cfg.Logger)
		packetConn = &arrivalConn{PacketConn: conn, trace: diagnostic}
	}
	diagnosticOwned := false
	defer func() {
		if diagnostic != nil && !diagnosticOwned {
			diagnostic.close()
		}
	}()
	mux := webrtc.NewICEUDPMux(nil, packetConn)
	se.SetICEUDPMux(mux)
	// Only the one socket, and only IPv4. Without this Pion also gathers every
	// local address it can find, and inside a container those are addresses no
	// browser on the internet can reach.
	se.SetNetworkTypes([]webrtc.NetworkType{webrtc.NetworkTypeUDP4})
	if cfg.PublicIP != "" {
		se.SetNAT1To1IPs([]string{cfg.PublicIP}, webrtc.ICECandidateTypeHost)
	}
	if cfg.IncludeLoopback || local {
		se.SetIncludeLoopbackCandidate(true)
	}
	if local {
		// And no multicast DNS, which is the second socket nobody thinks about.
		// Pion asks .local names on the network by default, and to ask it opens
		// a socket on 0.0.0.0:5353 — which is a second visit from the Windows
		// firewall box, from a binary that is otherwise bound to the loopback.
		// Nothing here needs it: on this machine both ends are addresses, not
		// names.
		se.SetICEMulticastDNSMode(ice.MulticastDNSModeDisabled)
	}
	// If the browser stops sending for this long the person shut the laptop or
	// lost the network, and the room should stop showing them.
	se.SetICETimeouts(5*time.Second, 15*time.Second, 2*time.Second)
	// This server offers, so the browser answers and picks itself as the DTLS
	// client — which leaves this end passive for the life of the connection.
	// The screen is the one place the browser offers instead (D4r), and a role
	// may not change on a renegotiation: without this Pion answers as the DTLS
	// client, the two ends disagree about who speaks first, and the browser
	// refuses the answer with "Failed to set SSL role for the transport".
	// Measured on 18/09/2026, in the e2e, before this line existed — and the
	// library says so itself: defaultDtlsRoleAnswer = DTLSRoleClient
	// (webrtc v4.2.20, dtlsrole.go). DTLSRoleServer is the passive one.
	if err := se.SetAnsweringDTLSRole(webrtc.DTLSRoleServer); err != nil {
		return nil, err
	}

	m := &webrtc.MediaEngine{}
	// Our own list, not Pion's default one: the default brings AV1, which
	// CLAUDE.md forbids, and this server is the side that offers, so a codec
	// that is not here is a codec no browser can choose (codecs.go).
	if err := registerCodecs(m); err != nil {
		conn.Close()
		return nil, err
	}
	// The two header extensions that carry the simulcast layer's name.
	//
	// Without these the layers still arrive — three streams, three
	// synchronisation sources — but nothing says which is which, and a layer
	// nobody can name is a layer nobody can choose. Pion answers the question
	// "did OnTrack fire three times, each with a RID?" with a flat no when this
	// is missing: measured with scripts/simulcast-probe on 18/09/2026.
	if err := webrtc.ConfigureSimulcastExtensionHeaders(m); err != nil {
		conn.Close()
		return nil, err
	}
	reg, err := arrivalInterceptors(m, diagnostic)
	if err != nil {
		conn.Close()
		return nil, err
	}

	s := &SFU{
		cfg: cfg,
		log: cfg.Logger,
		api: webrtc.NewAPI(
			webrtc.WithSettingEngine(se),
			webrtc.WithMediaEngine(m),
			webrtc.WithInterceptorRegistry(reg),
		),
		mux:       mux,
		conn:      conn,
		arrival:   diagnostic,
		rooms:     map[string]*room{},
		sessions:  map[string]*session{},
		startedAt: time.Now(),
		stop:      make(chan struct{}),
	}
	cfg.Logger.Info("sfu listening",
		"udp", s.UDPPort(), "public_ip", cfg.PublicIP, "max_per_room", cfg.MaxPerRoom)
	diagnosticOwned = true
	go heartbeat(cfg.Logger, s.stop)
	return s, nil
}

// Close hangs up on everybody and releases the socket.
func (s *SFU) Close() error {
	if !s.closed.CompareAndSwap(false, true) {
		return nil
	}
	close(s.stop)
	s.mu.Lock()
	list := make([]*session, 0, len(s.sessions))
	for _, sess := range s.sessions {
		list = append(list, sess)
	}
	s.sessions = map[string]*session{}
	s.rooms = map[string]*room{}
	s.mu.Unlock()
	for _, sess := range list {
		_ = sess.pc.Close()
	}
	if s.mux != nil {
		_ = s.mux.Close()
	}
	if s.arrival != nil {
		s.arrival.close()
	}
	return s.conn.Close()
}

// UDPPort reports the port actually bound, which matters when the caller asked
// for port 0 — the tests do, so that two of them can run at once.
func (s *SFU) UDPPort() int {
	if a, ok := s.conn.LocalAddr().(*net.UDPAddr); ok {
		return a.Port
	}
	return s.cfg.UDPPort
}

// ---------------------------------------------------------------- joining

// Joiner is who is asking. The caller has already decided that this person may
// be in this room; the SFU takes that as settled and never guesses at it.
type Joiner struct {
	UserID       int64
	LoginSession string
	Username     string
	DisplayName  string
	// Publish is what this device intends to send on the way in: normally
	// just the microphone. Empty means the microphone alone, which is what a
	// voice channel is. A screen or a camera switched on later does not come
	// through here — it comes through Publish, on the connection that is
	// already up.
	Publish []string
}

// Join builds this device's connection and returns the offer it must answer.
//
// The offer already contains everything: one slot pointing the other way for
// the newcomer's microphone, and one slot for each person already talking.
// There is no second round trip to subscribe to anybody — being in the room is
// the subscription.
func (s *SFU) Join(roomID string, who Joiner) (Offer, error) {
	if s.closed.Load() {
		return Offer{}, ErrClosed
	}
	// One person is in one place. Arriving anywhere hangs up whatever that
	// account had open somewhere else — the other tab, the telephone in the
	// pocket, the connection a reload left behind and that would sit in the
	// room for fifteen seconds until the network noticed. Seeing the same name
	// twice in a room is not a feature anybody wanted, and it is what every
	// service like this one does: the newest device wins.
	s.LeaveAllFor(who.UserID)

	id, err := newID()
	if err != nil {
		return Offer{}, err
	}

	r := s.roomFor(roomID)
	if s.joinedRoom != nil {
		s.joinedRoom(roomID)
	}
	pc, err := s.api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		return Offer{}, err
	}
	sess := &session{
		id: id, userID: who.UserID, login: who.LoginSession,
		username: who.Username, displayName: who.DisplayName,
		room: r, pc: pc, joinedAt: time.Now(),
		subs: map[string]*webrtc.RTPSender{},
	}

	pc.OnTrack(func(remote *webrtc.TrackRemote, recv *webrtc.RTPReceiver) {
		s.forward(sess, remote, recv)
	})
	pc.OnConnectionStateChange(func(st webrtc.PeerConnectionState) {
		s.log.Info("voice connection", "session", sess.id, "state", st.String())
		switch st {
		case webrtc.PeerConnectionStateConnected:
			sess.connected.Store(true)
			sess.lostAt.Store(0)
		case webrtc.PeerConnectionStateClosed:
			s.drop(sess)
		case webrtc.PeerConnectionStateFailed, webrtc.PeerConnectionStateDisconnected:
			s.holdForRestart(sess)
		}
	})

	// The newcomer's own slots. Recvonly is from the server's side: the server
	// receives, so the browser is the one that sends. One slot per source and
	// no more — a device that asks for the microphone twice gets one
	// microphone.
	for _, source := range publishSources(who.Publish) {
		if _, err := s.addSlot(sess, source); err != nil {
			pc.Close()
			return Offer{}, err
		}
	}

	// Everybody already talking, in one go.
	r.mu.Lock()
	if r.gone {
		// The last person left this room while the connection was being
		// built, and it is no longer in the index. Start again: the next
		// roomFor finds or makes the one everybody else will find.
		r.mu.Unlock()
		pc.Close()
		return s.Join(roomID, who)
	}
	if len(r.peers) >= s.cfg.MaxPerRoom {
		r.mu.Unlock()
		pc.Close()
		return Offer{}, ErrRoomFull
	}
	for _, p := range r.pubs {
		if err := sess.subscribe(p); err != nil {
			s.log.Error("subscribe", "err", err)
		}
	}
	r.peers[sess.id] = sess
	r.mu.Unlock()

	s.mu.Lock()
	s.sessions[sess.id] = sess
	s.mu.Unlock()

	offer, err := s.offer(sess)
	if err != nil {
		s.drop(sess)
		return Offer{}, err
	}
	s.log.Info("voice join", "room", roomID, "session", sess.id)
	s.roomChanged(roomID)
	within := s.connectWithin
	if within <= 0 {
		within = connectDeadline
	}
	time.AfterFunc(within, func() {
		if sess.connected.Load() {
			return
		}
		s.log.Info("voice session never connected", "room", roomID, "session", sess.id)
		s.drop(sess) // a no-op if it already left
	})
	return offer, nil
}

// offer builds and installs a fresh offer for one device, and reads back the
// slot labels WebRTC assigned. Those labels only exist once the offer is the
// local description, which is why the mapping is read here rather than built by
// hand from what we think we asked for.
func (s *SFU) offer(sess *session) (Offer, error) {
	sess.negMu.Lock()
	defer sess.negMu.Unlock()
	return s.offerLocked(sess)
}

// offerLocked is the same thing with the lock already held. It exists because
// deciding "is this connection settled enough to be offered to?" and building
// the offer have to happen without letting go in between: two people arriving
// in the same second would otherwise both find a settled connection, and the
// second would install its offer on top of the first one's.
func (s *SFU) offerLocked(sess *session) (Offer, error) {
	restart := sess.restartOffer
	desc, err := sess.pc.CreateOffer(&webrtc.OfferOptions{ICERestart: restart})
	if err != nil {
		return Offer{}, err
	}
	if err := sess.alignPayloads(desc); err != nil {
		return Offer{}, err
	}
	desc, err = sess.pc.CreateOffer(nil)
	if err != nil {
		return Offer{}, err
	}
	if err := sess.pc.SetLocalDescription(desc); err != nil {
		return Offer{}, err
	}
	// Gather to the end before answering. It costs nothing here and it saves a
	// whole message type: this server has exactly one candidate to offer — one
	// public address, one port — so there is nothing for trickle to overlap
	// with. The browser still trickles its own candidates up, which is the
	// direction where somebody behind a slow home router actually gains.
	<-webrtc.GatheringCompletePromise(sess.pc)
	local := sess.pc.LocalDescription()

	out := Offer{ICERestart: restart, SessionID: sess.id, SDP: local.SDP, Type: local.Type.String(), SendMids: map[string]string{}}

	// The slots this device sends into, by name. Reading the labels back from
	// the transceivers rather than remembering what we think we asked for is
	// what keeps this true across every renegotiation.
	sess.slotMu.Lock()
	for _, sl := range sess.slots {
		if !sl.live {
			continue
		}
		if mid := sl.tr.Mid(); mid != "" {
			out.SendMids[sl.source] = mid
		}
	}
	sess.slotMu.Unlock()

	sess.subMu.Lock()
	bySender := make(map[*webrtc.RTPSender]string, len(sess.subs))
	for pubID, snd := range sess.subs {
		bySender[snd] = pubID
	}
	sess.subMu.Unlock()

	for _, tr := range sess.pc.GetTransceivers() {
		mid := tr.Mid()
		if mid == "" {
			continue
		}
		snd := tr.Sender()
		if snd == nil {
			continue
		}
		pubID, ok := bySender[snd]
		if !ok {
			continue
		}
		if pub := sess.room.publication(pubID); pub != nil {
			out.Tracks = append(out.Tracks, TrackInfo{
				Mid: mid, Username: pub.owner.username, DisplayName: pub.owner.displayName,
				Kind: pub.kind, Source: pub.source,
			})
		}
	}
	sort.Slice(out.Tracks, func(i, j int) bool { return out.Tracks[i].Mid < out.Tracks[j].Mid })
	if out.Tracks == nil {
		out.Tracks = []TrackInfo{}
	}
	sess.lastOffer = out
	sess.restartOffer = false
	return out, nil
}

const restartGrace = 20 * time.Second

func (s *SFU) holdForRestart(sess *session) {
	at := sess.outageSeq.Add(1)
	if !sess.lostAt.CompareAndSwap(0, at) {
		return
	}
	s.log.Info("voice restart grace", "session", sess.id, "ms", restartGrace.Milliseconds())
	time.AfterFunc(restartGrace, func() { s.expireOutage(sess, at) })
}

func (s *SFU) expireOutage(sess *session, at int64) {
	if sess.lostAt.Load() == at {
		s.log.Info("voice restart expired", "session", sess.id)
		s.drop(sess)
	}
}

// RestartICE offers fresh credentials on the same transport. An unanswered
// offer is replayed first, so a lost gateway event never wedges negotiation.
// The caller answers that offer and asks again if it was not a restart.
func (s *SFU) RestartICE(sessionID string, userID int64) (Offer, error) {
	sess, err := s.session(sessionID, userID)
	if err != nil {
		return Offer{}, err
	}
	sess.negMu.Lock()
	defer sess.negMu.Unlock()
	if sess.pc.SignalingState() == webrtc.SignalingStateHaveLocalOffer {
		s.log.Info("voice offer replay", "session", sess.id)
		return sess.lastOffer, nil
	}
	if sess.pc.SignalingState() != webrtc.SignalingStateStable {
		return Offer{}, ErrBusyNegotiating
	}
	sess.restartOffer = true
	sess.iceMu.Lock()
	sess.iceReady = false
	sess.iceEarly = nil
	sess.iceMu.Unlock()
	s.log.Info("voice ICE restart", "session", sess.id)
	return s.offerLocked(sess)
}

// Answer takes the browser's reply. userID is checked against the session's
// owner: a session id is not a capability.
func (s *SFU) Answer(sessionID string, userID int64, sdp string) error {
	sess, err := s.session(sessionID, userID)
	if err != nil {
		return err
	}
	sess.negMu.Lock()
	// An HTTP response can be lost after the answer was accepted. Replaying
	// that exact answer is safe and must not force a healthy call to rejoin.
	if sess.pc.SignalingState() == webrtc.SignalingStateStable {
		if sess.answerSDP == sdp && sdp != "" {
			sess.negMu.Unlock()
			return nil
		}
	}
	err = sess.pc.SetRemoteDescription(webrtc.SessionDescription{
		Type: webrtc.SDPTypeAnswer, SDP: sdp,
	})
	if err == nil {
		sess.answerSDP = sdp
	}
	sess.negMu.Unlock()
	if err != nil {
		return err
	}
	// There is now an other end, so the routes that arrived before it can go in.
	s.flushCandidates(sess)
	// Somebody joined or left while this answer was in flight; the offer they
	// caused was skipped rather than queued, and now is when it goes out.
	if sess.pending.CompareAndSwap(true, false) {
		s.renegotiate(sess)
	}
	return nil
}

// Candidate adds one of the browser's addresses.
func (s *SFU) Candidate(sessionID string, userID int64, c webrtc.ICECandidateInit) error {
	sess, err := s.session(sessionID, userID)
	if err != nil {
		return err
	}
	// Before the answer has landed there is no other end to attach a route to,
	// and handing one over now is an error rather than a candidate. It waits
	// instead. See iceEarly.
	sess.iceMu.Lock()
	if !sess.iceReady {
		if len(sess.iceEarly) < maxEarlyCandidates {
			sess.iceEarly = append(sess.iceEarly, c)
		}
		sess.iceMu.Unlock()
		return nil
	}
	sess.iceMu.Unlock()
	return sess.pc.AddICECandidate(c)
}

// maxEarlyCandidates bounds the wait. A browser offers a handful of routes; a
// thousand of them is not a browser and is not worth holding on to.
const maxEarlyCandidates = 64

// flushCandidates is called once the answer is in, and hands over everything
// that arrived too early. A route that turns out to be malformed is dropped
// with a line in the log rather than failing the answer that carried it: the
// connection has other routes and the person is mid-join.
func (s *SFU) flushCandidates(sess *session) {
	sess.iceMu.Lock()
	early := sess.iceEarly
	sess.iceEarly = nil
	sess.iceReady = true
	sess.iceMu.Unlock()
	for _, c := range early {
		if err := sess.pc.AddICECandidate(c); err != nil {
			s.log.Warn("early candidate refused", "session", sess.id, "err", err)
		}
	}
	if len(early) > 0 {
		s.log.Info("early candidates", "session", sess.id, "n", len(early))
	}
}

// Layer is the subscriber saying how big somebody's screen needs to be on
// THIS device — which is the one thing this server cannot work out for itself.
//
// A screen drawn in a thumbnail does not need 1080p, and sending it there is
// paid for twice: in the upload of the person showing and in the decoder and
// battery of the person watching. The browser measures the element and says
// so; this only ever lowers what that browser receives, never what anybody
// else does.
//
// An empty rid takes the ceiling off. An unknown one is ignored rather than
// refused: a browser from a future version asking for a layer this build does
// not have should keep its picture, not lose it.
func (s *SFU) Layer(sessionID string, userID int64, username, rid string) error {
	sess, err := s.session(sessionID, userID)
	if err != nil {
		return err
	}
	if username == "" {
		return ErrBadSource
	}
	room := sess.room
	room.mu.RLock()
	var theirs []*publication
	for _, p := range room.pubs {
		if p.simul && p.owner.username == username && p.source == SourceScreen {
			theirs = append(theirs, p)
		}
	}
	room.mu.RUnlock()
	for _, p := range theirs {
		if f := p.forwarderFor(sess); f != nil {
			f.setCeiling(rid)
		}
	}
	return nil
}

// Watch turns one person's screen on or off for one device, and it is a real
// unsubscription rather than a hidden picture: the packets stop crossing the
// network. That is the whole point — somebody on a telephone plan who is in the
// room for the conversation should not be paying for a film.
//
// It never touches voices. Not wanting to hear somebody is a different thing,
// with a different answer (the volume in their profile), and a room where
// people cannot hear each other is not a room.
func (s *SFU) Watch(sessionID string, userID int64, username string, on bool) error {
	sess, err := s.session(sessionID, userID)
	if err != nil {
		return err
	}
	sess.watchMu.Lock()
	if on {
		if sess.watching == nil {
			sess.watching = map[string]struct{}{}
		}
		sess.watching[username] = struct{}{}
	} else {
		delete(sess.watching, username)
	}
	sess.watchMu.Unlock()

	// And what is already flowing changes now, not at the next renegotiation.
	room := sess.room
	room.mu.Lock()
	var theirs []*publication
	for _, p := range room.pubs {
		if p.owner.username != username {
			continue
		}
		if p.source == SourceScreen || p.source == SourceScreenAudio {
			theirs = append(theirs, p)
		}
	}
	room.mu.Unlock()

	for _, p := range theirs {
		if on {
			if err := sess.subscribe(p); err != nil {
				s.log.Error("watch subscribe", "session", sess.id, "err", err)
			}
		} else {
			sess.unsubscribe(p)
		}
	}
	if len(theirs) > 0 {
		s.renegotiate(sess)
	}
	s.log.Info("watch", "session", sess.id, "who", username, "on", on, "tracks", len(theirs))
	return nil
}

// SetState records mute and deafen so the room can draw them. Both are enforced
// on the person's own device — the microphone is switched off there, and a
// deafened person's speakers are silent there. This is the label, not the lock.
func (s *SFU) SetState(sessionID string, userID int64, muted, deafened bool) error {
	sess, err := s.session(sessionID, userID)
	if err != nil {
		return err
	}
	sess.stateMu.Lock()
	sess.muted, sess.deafened = muted, deafened
	sess.stateMu.Unlock()
	s.roomChanged(sess.room.id)
	return nil
}

// Publish opens slots for sources this device was not sending yet — a camera,
// a screen, a screen's own sound — on the connection that is already up, and
// pushes a fresh offer at it.
//
// The alternative was to have the browser leave the room and come back with
// the new list, and it was rejected: it would tear down a working connection,
// take everybody else's connection down with it for a moment (each of them
// would see the person leave and arrive again), and drop the audio of a
// conversation that is very likely the reason somebody is sharing a screen in
// the first place. Adding a slot is the same rule the whole service already
// follows — the connection is never rebuilt, only renegotiated (PROTOCOL.md,
// replaceTrack).
//
// Nothing is subscribed here. The picture reaches the room when it arrives,
// through the same path a microphone takes.
func (s *SFU) Publish(sessionID string, userID int64, sources []string) error {
	sess, err := s.session(sessionID, userID)
	if err != nil {
		return err
	}
	changed := false
	for _, source := range publishSources(sources) {
		added, err := s.addSlot(sess, source)
		if err != nil {
			return err
		}
		changed = changed || added
	}
	if !changed {
		return nil
	}
	s.renegotiate(sess)
	return nil
}

// ClientOffer is the one negotiation this server does not start, and it exists
// for exactly one reason: simulcast.
//
// Everywhere else the rule holds — the server opens the slots and writes the
// offer, and the browser only ever answers. That rule is what keeps two offers
// from racing down one connection. But a browser can only be asked for several
// layers of a picture if the **offer** names them, with `a=rid` and
// `a=simulcast`, and Pion does not write those lines when it offers: it writes
// them only when answering, reading them out of the remote description. Editing
// its offer afterwards is refused outright —
// `InvalidModificationError: new sdp does not match previous offer` — and
// editing only what goes on the wire leaves Pion unable to sort the layers it
// then receives ("Incoming unhandled RTP, OnTrack will not be fired"). Both
// measured on 18/09/2026, with scripts/simulcast-probe.
//
// So for a screen, and only for a screen, the browser offers once. It is what
// ion-sfu, Galene and LiveKit all do on the publishing side, for the same
// reason.
//
// The lock is the whole safety of it: negMu is already what serialises this
// server's own offers, and taking it here puts the browser's one offer in the
// same queue as ours instead of alongside it.
func (s *SFU) ClientOffer(sessionID string, userID int64, sources []string, sdp string) (string, error) {
	sess, err := s.session(sessionID, userID)
	if err != nil {
		return "", err
	}
	// A screen and the film's own sound arrive in ONE offer, not two. Two
	// offers in a row is the thing negMu exists to prevent, and doing it to
	// ourselves on purpose would be worse than any race it protects against.
	wanted := publishSources(sources)
	if len(wanted) == 0 {
		return "", ErrBadSource
	}

	sess.negMu.Lock()
	defer sess.negMu.Unlock()

	// What can be refused without reading the offer is refused before it is
	// installed. Afterwards a refusal has to settle the offer (abandonOffer).
	if err := sess.canAdopt(wanted); err != nil {
		return "", err
	}

	// Answering an offer is only legal from a settled connection. If one of
	// ours is in flight the browser is told to come back, rather than being
	// given an answer to a question the connection has already moved past.
	if sess.pc.SignalingState() != webrtc.SignalingStateStable {
		return "", ErrBusyNegotiating
	}
	// Every transceiver the connection has before the offer goes in — this
	// device's own slots, and just as much the ones carrying everybody else's
	// microphone and camera TO it. negMu is held, so nothing else adds one
	// between here and the offer.
	before := map[*webrtc.RTPTransceiver]bool{}
	for _, tr := range sess.pc.GetTransceivers() {
		before[tr] = true
	}
	if err := sess.pc.SetRemoteDescription(webrtc.SessionDescription{
		Type: webrtc.SDPTypeOffer, SDP: sdp,
	}); err != nil {
		return "", err
	}

	// Whatever transceiver that offer brought into being is this source. It is
	// found by elimination rather than by trusting a number from the browser:
	// the new one is the one that was not there before the offer.
	//
	// "Not there before" and not "not one of my slots", which is what this was
	// until 23/09/2026. The transceiver carrying somebody's camera to this
	// device is not one of its slots either, it comes first, and it is video:
	// a screen shared while anybody in the room had a camera on was recorded
	// on that transceiver, and the screen itself arrived on a slot nobody had
	// named and was dropped. Nobody saw it.
	if err := sess.adoptNewSlots(wanted, before); err != nil {
		sess.abandonOffer()
		return "", err
	}

	answer, err := sess.pc.CreateAnswer(nil)
	if err != nil {
		sess.abandonOffer()
		return "", err
	}
	if err := sess.pc.SetLocalDescription(answer); err != nil {
		sess.abandonOffer()
		return "", err
	}
	s.log.Info("client offer", "session", sess.id, "sources", len(wanted))
	return sess.pc.LocalDescription().SDP, nil
}

// adoptNewSlot records the transceiver a client offer created, so that the
// picture arriving on it can be named. Without this the track lands in
// forward() with no source and is dropped, which is the honest behaviour for a
// slot this server never opened — and would be a screen that silently never
// appears.
func (sess *session) adoptNewSlots(sources []string, before map[*webrtc.RTPTransceiver]bool) error {
	sess.slotMu.Lock()
	defer sess.slotMu.Unlock()

	known := map[*webrtc.RTPTransceiver]bool{}
	for tr := range before {
		known[tr] = true
	}
	live := map[string]bool{}
	for _, sl := range sess.slots {
		known[sl.tr] = true
		if sl.live {
			live[sl.source] = true
		}
	}
	for tr := range sess.abandoned {
		known[tr] = true
	}
	// Collected first and recorded only if every source found its slot: a
	// refusal half-way used to leave the first one recorded, pointing at a
	// transceiver from an offer that was then taken back.
	var found []*slot
	for _, source := range sources {
		if live[source] {
			// Pressing "share" twice, or the same offer arriving twice. The
			// slot is already there and already named.
			continue
		}
		kind, ok := sourceKind(source)
		if !ok {
			return ErrBadSource
		}
		match := false
		for _, tr := range sess.pc.GetTransceivers() {
			if known[tr] || tr.Receiver() == nil || tr.Kind() != kind {
				continue
			}
			// Matched by kind and by not being spoken for, in the order the
			// sources were asked for. The browser puts them in that order, and
			// trusting a mid from the browser instead would be trusting the
			// one party with a reason to lie about which slot is which.
			known[tr] = true
			found = append(found, &slot{source: source, tr: tr, live: true})
			match = true
			break
		}
		if !match {
			return ErrNoNewSlot
		}
	}
	if sess.opened+len(found) > maxSlotsPerSession {
		return ErrTooManySlots
	}
	sess.slots = append(sess.slots, found...)
	sess.opened += len(found)
	return nil
}

// canAdopt is the part of adoptNewSlots that does not need the offer: an
// unknown source, and the slot limit. Checked before the offer is installed,
// because the limit is the refusal a real person reaches — sharing and
// stopping thirty-two times in one sitting — and it should not cost them the
// connection.
func (sess *session) canAdopt(sources []string) error {
	sess.slotMu.Lock()
	defer sess.slotMu.Unlock()
	live := map[string]bool{}
	for _, sl := range sess.slots {
		if sl.live {
			live[sl.source] = true
		}
	}
	need := 0
	for _, source := range sources {
		if _, ok := sourceKind(source); !ok {
			return ErrBadSource
		}
		if !live[source] {
			need++
		}
	}
	if sess.opened+need > maxSlotsPerSession {
		return ErrTooManySlots
	}
	return nil
}

// abandonOffer settles a browser offer that was installed and then refused.
//
// Returning the error was not enough: the connection stayed in
// have-remote-offer, every later offer from the browser was told a
// negotiation was in flight, and every offer of ours waited for an answer that
// would never come — so nobody else's screen reached this device again either.
//
// Taking the offer back would be the textbook move, and Pion does not have it:
// its table has no rollback out of have-remote-offer (measured on 23/09/2026,
// "invalid proposed signaling state transition"). So the exchange is finished
// instead — answered and set, which is what returns the connection to stable —
// and nothing it brought is adopted: the transceivers it created are set aside
// and never named as a slot. The browser, told no, rolls its own side back, and
// the next offer from either end starts from a settled connection.
//
// If even that fails, the connection is in a state nothing here can reason
// about, and dropping it is better than leaving it half-open: the person hears
// the room go quiet and rejoins, instead of sitting in a room where nothing
// new ever arrives.
func (sess *session) abandonOffer() {
	if sess.pc.SignalingState() != webrtc.SignalingStateHaveRemoteOffer {
		return
	}
	answer, err := sess.pc.CreateAnswer(nil)
	if err == nil {
		err = sess.pc.SetLocalDescription(answer)
	}
	if err != nil {
		sess.room.sfu.log.Error("abandon offer", "session", sess.id, "err", err)
		go sess.room.sfu.drop(sess)
		return
	}
	sess.slotMu.Lock()
	defer sess.slotMu.Unlock()
	mine := map[*webrtc.RTPTransceiver]bool{}
	for _, sl := range sess.slots {
		mine[sl.tr] = true
	}
	if sess.abandoned == nil {
		sess.abandoned = map[*webrtc.RTPTransceiver]bool{}
	}
	for _, tr := range sess.pc.GetTransceivers() {
		if !mine[tr] {
			sess.abandoned[tr] = true
		}
	}
}

// Unpublish closes those slots again. Everybody subscribed to what was on them
// stops being subscribed as soon as the read ends, which is what stops a
// screen that was turned off from sitting on everybody's wall as a frozen last
// picture.
func (s *SFU) Unpublish(sessionID string, userID int64, sources []string) error {
	sess, err := s.session(sessionID, userID)
	if err != nil {
		return err
	}
	changed := false
	for _, source := range sources {
		if _, ok := sourceKind(source); !ok {
			continue
		}
		if source == SourceMic {
			// The microphone is not something a person turns off by closing
			// its slot: that is what mute is, and mute is a label the room
			// draws. Closing it would leave a connection that cannot be
			// spoken into until it is rebuilt.
			continue
		}
		changed = sess.closeSlot(source) || changed
	}
	if !changed {
		return nil
	}
	s.renegotiate(sess)
	return nil
}

// Leave hangs up one device.
func (s *SFU) Leave(sessionID string, userID int64) error {
	sess, err := s.session(sessionID, userID)
	if err != nil {
		return err
	}
	s.drop(sess)
	return nil
}

// LeaveAllFor hangs up every session a person has. It runs when their login is
// revoked: silence is the right answer to "this device may no longer be here".
func (s *SFU) LeaveAllFor(userID int64) {
	s.mu.RLock()
	var list []*session
	for _, sess := range s.sessions {
		if sess.userID == userID {
			list = append(list, sess)
		}
	}
	s.mu.RUnlock()
	for _, sess := range list {
		s.drop(sess)
	}
}

// LeaveLogin hangs up whatever one browser session had open. It runs when that
// session is revoked or logs out, so "disconnect this device" also stops its
// microphone instead of leaving it in the room until the network notices.
func (s *SFU) LeaveLogin(login string) {
	if login == "" {
		return
	}
	s.mu.RLock()
	var list []*session
	for _, sess := range s.sessions {
		if sess.login == login {
			list = append(list, sess)
		}
	}
	s.mu.RUnlock()
	for _, sess := range list {
		s.drop(sess)
	}
}

// LeaveAllExceptLogin hangs up every session of one person except the one
// browser session named, which is what "disconnect my other devices" means.
func (s *SFU) LeaveAllExceptLogin(userID int64, keepLogin string) {
	s.mu.RLock()
	var list []*session
	for _, sess := range s.sessions {
		if sess.userID == userID && sess.login != keepLogin {
			list = append(list, sess)
		}
	}
	s.mu.RUnlock()
	for _, sess := range list {
		s.drop(sess)
	}
}

// Logins lists the browser sessions that have something open here, once each,
// so the caller can ask the store which of them are still signed in.
func (s *SFU) Logins() []string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	seen := make(map[string]bool, len(s.sessions))
	var out []string
	for _, sess := range s.sessions {
		if sess.login != "" && !seen[sess.login] {
			seen[sess.login] = true
			out = append(out, sess.login)
		}
	}
	return out
}

// Rooms reports who is inside each of the given rooms. The caller passes the
// rooms this person may see; the SFU never decides that.
func (s *SFU) Rooms(ids []string) map[string][]Peer {
	out := make(map[string][]Peer, len(ids))
	s.mu.RLock()
	rooms := make(map[string]*room, len(ids))
	for _, id := range ids {
		if r, ok := s.rooms[id]; ok {
			rooms[id] = r
		}
	}
	s.mu.RUnlock()
	for _, id := range ids {
		if r, ok := rooms[id]; ok {
			out[id] = r.snapshot()
		} else {
			out[id] = []Peer{}
		}
	}
	return out
}

// ---------------------------------------------------------------- forwarding

// forward is the media path, and it is meant to be boring: read a packet, write
// a packet. Nothing between the two looks at what is inside.
func (s *SFU) forward(sess *session, remote *webrtc.TrackRemote, recv *webrtc.RTPReceiver) {
	kind := remote.Kind().String()
	source := sess.sourceOf(recv)
	if source == "" {
		// A track on a slot this server never opened. It cannot happen, and if
		// it did the honest thing is to drop it rather than forward something
		// nobody can label.
		s.log.Error("track on unknown slot", "session", sess.id, "kind", kind)
		return
	}
	ssrc := uint32(remote.SSRC())
	mime := remote.Codec().MimeType
	// rid names a simulcast layer. It is empty for everything this server has
	// ever carried except a screen offered by a browser that knows how to send
	// three of them (D4r) — and when it is set, this is the second or third
	// arrival on a publication that already exists.
	rid := remote.RID()
	var pub *publication
	var local *webrtc.TrackLocalStaticRTP
	// The layer this read loop belongs to, so every packet can say the layer
	// is still arriving. Nil for anything that is not simulcast.
	var lay *layer
	if rid == "" {
		var err error
		local, err = webrtc.NewTrackLocalStaticRTP(
			remote.Codec().RTPCodecCapability, remote.ID(), remote.StreamID())
		if err != nil {
			s.log.Error("local track", "err", err)
			return
		}
		pub = &publication{
			id:    sess.id + ":" + source + ":" + strconv.Itoa(sess.nextSeq()),
			owner: sess, kind: kind, source: source, local: local,
			askAgain: func() {
				_ = sess.pc.WriteRTCP([]rtcp.Packet{&rtcp.PictureLossIndication{MediaSSRC: ssrc}})
			},
		}
		sess.room.addPublication(pub)
		defer sess.room.removePublication(pub)
		if source == SourceScreen {
			s.watchersChanged(sess, true)
		}
	} else {
		pub = sess.room.layered(sess, source, kind, remote)
		lay = pub.addLayer(rid, ssrc)
		defer func() {
			pub.removeLayer(rid, ssrc)
			if pub.refs.Add(-1) == 0 {
				sess.room.removePublication(pub)
			}
		}()
	}
	go pub.readSenderReports(recv, rid, ssrc)
	s.log.Info("voice track", "session", sess.id, "source", source,
		"kind", kind, "codec", mime, "camada", rid)

	buf := make([]byte, readBuffer)
	var pkt rtp.Packet
	// Where this loop's time goes, split in two, because the two halves accuse
	// different things and want different repairs.
	//
	// **Writing** is the fan-out: TrackLocalStaticRTP.WriteRTP walks every
	// subscriber in turn under one lock (webrtc v4.2.20,
	// track_local_static.go), so one subscriber that is slow to accept a
	// packet holds up everybody behind it — and holds up this loop, which is
	// the part that matters.
	//
	// **Reading** is not only waiting for the network. The transport-cc
	// interceptor hands the packet it just read to its own goroutine over a
	// channel with **no buffer**, and that same goroutine also builds and
	// writes the feedback reports (interceptor v0.1.48,
	// pkg/twcc/sender_interceptor.go). While it is writing one, nobody drains
	// the channel and the read waits.
	//
	// Both end the same way — the arrival time stamped inside that Read is
	// late, the sender is told the network is slow, and it answers by sending
	// less without losing anything. But one is fixed with a queue per
	// subscriber and the other by configuring an interceptor, so knowing which
	// is worth measuring before touching either. On 19/09/2026 only the
	// writing half was being measured, which is how the question stayed open.
	// Reused every turn so the fan-out does not allocate per packet.
	var outs []*forwarder
	// Whether the next packet begins a picture. True to start with: the first
	// packet of a stream begins one by definition.
	frameStart := true
	// Packets in a row that nobody took. See askIfWanted.
	var idle int
	var turns, slowW, slowR int64
	var worstWrite, worstRead time.Duration
	report := time.Now()
	for {
		askedAt := time.Now()
		n, _, err := remote.Read(buf)
		waited := time.Since(askedAt)
		if err != nil {
			s.log.Info("voice track ended", "session", sess.id, "kind", kind)
			return
		}
		if waited > worstRead {
			worstRead = waited
		}
		if waited > slowRead {
			slowR++
		}
		began := time.Now()
		s.bytesIn.Add(int64(n))
		s.packetsIn.Add(1)
		fanout := pub.subs.Load()
		if fanout == 0 {
			continue
		}
		if err := pkt.Unmarshal(buf[:n]); err != nil {
			continue
		}
		// The header extensions come off, and this is not an optimisation —
		// it is the difference between a picture arriving and a picture
		// vanishing. One of them, "mid", says which slot of *the sender's*
		// connection the packet belongs to, and the slots are numbered
		// separately on every connection: Ana's screen is her slot 1, and on
		// Bento's connection slot 1 is somebody's microphone. A browser that
		// believes the extension binds the stream to the wrong slot, decides
		// the payload makes no sense there, and throws every packet away for
		// the rest of the call — silently, with no error anywhere. The two
		// alongside it (rid, repaired-rid) name a simulcast layer and belong
		// to the sender's connection for exactly the same reason, and the
		// bandwidth-estimate counter is per connection too, so forwarding it
		// would be a lie about a transport it never crossed.
		//
		// Pion puts back whatever *this* connection negotiated on the way out.
		// Nothing here looks at the payload: the header is read, the
		// extensions are dropped, and the bytes that mean anything are copied
		// untouched.
		pkt.Header.Extension = false
		pkt.Header.ExtensionProfile = 0
		pkt.Header.Extensions = nil
		wroteAt := time.Now()
		sent := fanout
		if pub.simul {
			// One layer, only the people who are on it. The list is copied out
			// from under the lock before a single packet is written: writing
			// while holding it would put every subscriber behind every other
			// one, which is the thing the note above this loop is about.
			outs = outs[:0]
			pub.outMu.Lock()
			for _, f := range pub.outs {
				outs = append(outs, f)
			}
			pub.outMu.Unlock()
			// A picture can be sent as several slices, and the second slice of
			// a keyframe also looks like a keyframe from one byte in. Changing
			// layer there lands in the middle of the picture. The marker bit
			// is what closes a picture, so the packet after one is the only
			// place a new picture can begin — and that is in the RTP header,
			// which was already being read.
			key := frameStart && startsKeyframe(mime, pkt.Payload)
			frameStart = pkt.Marker
			// This layer is arriving. It is the only place that can say so,
			// and without it a layer switched off with `active: false` looks
			// alive for ever — see layerSilent.
			if lay != nil {
				lay.lastAt.Store(time.Now().UnixNano())
			}
			sent = 0
			for _, f := range outs {
				if f.write(rid, &pkt, key) {
					sent++
				}
			}
			// A settled viewer taking this layer does not mean a newcomer
			// got its first keyframe. Retry for every waiting forwarder even
			// while the other viewers receive packets. askLayer rate limits it.
			if idle++; idle%50 == 0 {
				pub.askIfWanted(rid)
			}
			if sent == 0 {
				continue
			}
		} else if err := local.WriteRTP(&pkt); err != nil && !errors.Is(err, io.ErrClosedPipe) {
			return
		}
		wrote := time.Since(wroteAt)
		s.bytesOut.Add(int64(n) * sent)
		s.packetsOut.Add(sent)

		// The whole turn, so that time spent neither reading nor writing —
		// unmarshalling, stripping extensions, the counters — shows up as the
		// difference between the two numbers instead of hiding.
		work := time.Since(began)
		turns++
		if work > worstWrite {
			worstWrite = work
		}
		if wrote > slowWork {
			slowW++
		}
		if time.Since(report) >= readReportEvery {
			s.log.Info("voice read", "session", sess.id, "source", source,
				"packets", turns,
				"escrita_lenta", slowW, "escrita_pior_us", worstWrite.Microseconds(),
				"leitura_lenta", slowR, "leitura_pior_us", worstRead.Microseconds(),
				"subs", fanout)
			turns, slowW, slowR = 0, 0, 0
			worstWrite, worstRead, report = 0, 0, time.Now()
		}
	}
}

// subscribe hangs one publication on one device's connection.
func (sess *session) subscribe(p *publication) error {
	notify, err := sess.subscribeOnce(p)
	// Outside subOpMu, so that telling the owner never waits on another
	// device's subscription.
	if notify {
		p.owner.room.sfu.watchersChanged(p.owner, false)
	}
	return err
}

// subscribeOnce is subscribe under subOpMu, and says whether the owner of a
// screen has to be told.
func (sess *session) subscribeOnce(p *publication) (bool, error) {
	sess.subOpMu.Lock()
	defer sess.subOpMu.Unlock()
	if sess.left {
		return false, nil
	}
	if p.owner == sess {
		return false, nil // nobody hears themselves
	}
	// A screen nobody on this device has asked for. Silence is the answer
	// here: somebody who joined to talk gets the conversation and nothing
	// else, and the screen waits behind a button under the person showing it.
	//
	// Voices are never refused. Leaving the conversation is what the leave
	// button is for.
	if p.source == SourceScreen || p.source == SourceScreenAudio {
		sess.watchMu.Lock()
		_, yes := sess.watching[p.owner.username]
		sess.watchMu.Unlock()
		if !yes {
			return false, nil
		}
	}
	sess.subMu.Lock()
	if _, ok := sess.subs[p.id]; ok {
		sess.subMu.Unlock()
		return false, nil
	}
	sess.subMu.Unlock()

	// A publication with layers has no one track to hand out: this subscriber
	// gets its own, and which layer feeds it is decided from its own reports.
	var f *forwarder
	track := webrtc.TrackLocal(p.local)
	if p.simul {
		var err error
		if f, err = p.newForwarder(sess); err != nil {
			return false, err
		}
		track = &forwarderTrack{TrackLocalStaticRTP: f.local, f: f}
	}
	track = &syncTrack{TrackLocal: track, pub: p, f: f, viewer: sess, send: sess.pc.WriteRTCP}
	snd, err := sess.pc.AddTrack(track)
	if err == nil && p.kind == "video" && sess.isOwnSlot(snd) {
		// AddTrack fills any video slot with nothing going out on it, and
		// the one this device opened to send its own camera is such a slot.
		// Its codecs were agreed for that camera, though: a phone without an
		// H.264 encoder agreed VP8 and VP9 only there, and an H.264 screen
		// arrived as packets it could not read. Somebody else's picture gets
		// a slot of its own. (A voice on the microphone's slot is fine: both
		// are Opus, and it is how the web has always been answered.)
		_ = sess.pc.RemoveTrack(snd)
		var tr *webrtc.RTPTransceiver
		tr, err = sess.pc.AddTransceiverFromTrack(track,
			webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionSendonly})
		if err == nil {
			snd = tr.Sender()
		}
	}
	if err != nil {
		if f != nil {
			p.dropForwarder(sess)
		}
		return false, err
	}
	sess.subMu.Lock()
	sess.subs[p.id] = snd
	sess.subMu.Unlock()
	p.subs.Add(1)

	// A sender's incoming reports have to be drained or the buffer behind them
	// fills and the connection stalls. It is also where a subscriber asks for a
	// fresh picture, which is the one message this server passes back upstream:
	// it still decodes nothing, it only says "start again" to the sender.
	go func() {
		buf := make([]byte, readBuffer)
		for {
			n, _, err := snd.Read(buf)
			if err != nil {
				return
			}
			if p.kind != "video" {
				continue
			}
			pkts, err := rtcp.Unmarshal(buf[:n])
			if err != nil {
				continue
			}
			p.handleSubscriberRTCP(snd.GetParameters(), f, pkts)
		}
	}()
	if p.kind == "video" {
		// The newcomer joined mid-picture; without this they wait for the
		// sender's next scheduled keyframe, which on a screen share can be
		// several seconds of grey.
		if f != nil {
			p.askLayer(f.watching())
		} else {
			p.keyframe()
		}
	}
	return p.source == SourceScreen, nil
}

// handleSubscriberRTCP handles feedback drained from one outgoing sender.
func (p *publication) handleSubscriberRTCP(params webrtc.RTPSendParameters, f *forwarder, pkts []rtcp.Packet) {
	if p.kind != "video" {
		return
	}
	// Encodings describe the subscriber's outgoing stream, not the publisher's
	// incoming layer SSRC. RTX/FEC SSRCs are repair streams, never media loss.
	mediaSSRC := func(ssrc uint32) bool {
		if ssrc == 0 {
			return false
		}
		for _, encoding := range params.Encodings {
			if uint32(encoding.SSRC) == ssrc {
				return true
			}
		}
		return false
	}
	for _, pkt := range pkts {
		requestKeyframe := false
		switch m := pkt.(type) {
		case *rtcp.PictureLossIndication:
			requestKeyframe = mediaSSRC(m.MediaSSRC)
		case *rtcp.FullIntraRequest:
			// FIR addresses media in its FCI entries; MediaSSRC is zero
			// in a standards-compliant FIR, not a wildcard for this sender.
			for _, entry := range m.FIR {
				if mediaSSRC(entry.SSRC) {
					requestKeyframe = true
					break
				}
			}
		case *rtcp.ReceiverReport:
			// What this device says about its own line. It is the only
			// measurement here that was taken at the far end, which is
			// the only end that knows whether the picture arrived.
			if f == nil {
				continue
			}
			for _, rr := range m.Reports {
				if mediaSSRC(rr.SSRC) {
					f.report(float64(rr.FractionLost) / 256)
				}
			}
		}
		if requestKeyframe {
			if f != nil {
				p.askLayer(f.watching())
			} else {
				p.keyframe()
			}
		}
	}
}

// subscribed says whether this device's connection carries the publication.
func (sess *session) subscribed(p *publication) bool {
	sess.subMu.Lock()
	_, ok := sess.subs[p.id]
	sess.subMu.Unlock()
	return ok
}

// unsubscribe takes one publication off one device's connection, and says
// whether there was anything to take.
func (sess *session) unsubscribe(p *publication) bool {
	sess.subOpMu.Lock()
	sess.subMu.Lock()
	snd, ok := sess.subs[p.id]
	delete(sess.subs, p.id)
	sess.subMu.Unlock()
	if ok {
		p.subs.Add(-1)
		p.dropForwarder(sess)
		_ = sess.pc.RemoveTrack(snd)
	}
	sess.subOpMu.Unlock()
	if ok && p.source == SourceScreen {
		p.owner.room.sfu.watchersChanged(p.owner, false)
	}
	return ok
}

// ---------------------------------------------------------------- rooms

func (s *SFU) roomFor(id string) *room {
	s.mu.Lock()
	defer s.mu.Unlock()
	if r, ok := s.rooms[id]; ok {
		return r
	}
	r := &room{id: id, sfu: s, peers: map[string]*session{}, pubs: map[string]*publication{}}
	s.rooms[id] = r
	return r
}

func (r *room) publication(id string) *publication {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.pubs[id]
}

func (r *room) snapshot() []Peer {
	r.mu.RLock()
	// What each device is actually sending, taken from what is arriving rather
	// than from what anybody declared.
	sending := map[string][]string{}
	for _, pub := range r.pubs {
		if pub.source == SourceMic {
			continue // everybody has a microphone; saying so says nothing
		}
		sending[pub.owner.id] = append(sending[pub.owner.id], pub.source)
	}
	out := make([]Peer, 0, len(r.peers))
	for _, p := range r.peers {
		srcs := sending[p.id]
		if srcs == nil {
			srcs = []string{}
		}
		sort.Strings(srcs)
		p.stateMu.RLock()
		out = append(out, Peer{
			SessionID: p.id, Username: p.username, DisplayName: p.displayName,
			Muted: p.muted, Deafened: p.deafened, JoinedAt: p.joinedAt.UnixMilli(),
			Publishing: srcs,
		})
		p.stateMu.RUnlock()
	}
	r.mu.RUnlock()
	sort.Slice(out, func(i, j int) bool {
		if out[i].JoinedAt != out[j].JoinedAt {
			return out[i].JoinedAt < out[j].JoinedAt
		}
		return out[i].SessionID < out[j].SessionID
	})
	return out
}

func (r *room) addPublication(p *publication) {
	r.mu.Lock()
	r.pubs[p.id] = p
	others := make([]*session, 0, len(r.peers))
	for _, sess := range r.peers {
		if sess != p.owner {
			others = append(others, sess)
		}
	}
	r.mu.Unlock()
	for _, sess := range others {
		if err := sess.subscribe(p); err != nil {
			r.sfu.log.Error("subscribe", "err", err)
			continue
		}
		// Only a connection that gained a track has anything new to be told.
		// A screen is handed to nobody until they ask to watch it, and every
		// device in the room used to be sent a fresh offer for it all the
		// same: a full offer and answer, on each of them, that changed nothing.
		if sess.subscribed(p) {
			r.sfu.renegotiate(sess)
		}
	}
	// Who is sharing what is part of what the room shows, so it moves when a
	// publication appears and not only when somebody joins or mutes.
	r.sfu.roomChanged(r.id)
}

func (r *room) removePublication(p *publication) {
	r.mu.Lock()
	if r.pubs[p.id] != p {
		r.mu.Unlock()
		return
	}
	delete(r.pubs, p.id)
	others := make([]*session, 0, len(r.peers))
	for _, sess := range r.peers {
		if sess != p.owner {
			others = append(others, sess)
		}
	}
	r.mu.Unlock()
	for _, sess := range others {
		// The same in reverse: a device that never had it loses nothing.
		if sess.unsubscribe(p) {
			r.sfu.renegotiate(sess)
		}
	}
	r.sfu.roomChanged(r.id)
}

// ---------------------------------------------------------------- plumbing

// renegotiate pushes a fresh offer at one device. If that device has not yet
// answered the previous one the new offer is not queued: it is marked pending
// and rebuilt after the answer arrives, because an offer describes the whole
// connection and the newest one already says everything the skipped one would
// have said.
func (s *SFU) renegotiate(sess *session) {
	if s.closed.Load() || s.cfg.Signal == nil {
		return
	}
	sess.negMu.Lock()
	if sess.pc.SignalingState() != webrtc.SignalingStateStable {
		sess.pending.Store(true)
		sess.negMu.Unlock()
		return
	}
	offer, err := s.offerLocked(sess)
	sess.negMu.Unlock()
	if err != nil {
		s.log.Error("renegotiate", "err", err, "session", sess.id)
		return
	}
	s.cfg.Signal(sess.userID, sess.login, "voice_offer", offer)
}

// watchersChanged tells a device whether anybody is receiving its screen.
//
// Since watching became a decision instead of a default (18/09/2026), a screen
// can be shared into a room where nobody has asked for it — and the browser
// would go on uploading three layers to a server that drops every packet. The
// megabits saved on the watching side have to be saved on the showing side
// too, or the change just moves the waste.
//
// It says WHICH layers, not how many people. How many is not this device's
// business and would be a message per person joining; which layers is the
// only thing it can act on — a layer nobody is receiving is an encoder running
// for nobody, and on the machine of somebody who is usually playing a game
// that is the most expensive kind of nothing.
//
// force sends the answer even when it has not changed, which is what a screen
// that has just appeared needs: nobody is watching it yet, that is not a
// crossing from anything, and without this the browser would never be told.
func (s *SFU) watchersChanged(owner *session, force bool) {
	if s.cfg.Signal == nil || s.closed.Load() {
		return
	}
	owner.watchersMu.Lock()
	defer owner.watchersMu.Unlock()
	var n int64
	var wanted []string
	room := owner.room
	room.mu.RLock()
	var screens []*publication
	for _, p := range room.pubs {
		if p.owner == owner && p.source == SourceScreen {
			n += p.subs.Load()
			screens = append(screens, p)
		}
	}
	room.mu.RUnlock()
	// Outside the room lock: layersWanted takes the publication's own locks.
	for _, p := range screens {
		if p.simul {
			wanted = append(wanted, p.layersWanted()...)
		}
	}
	watched := n > 0
	key := strings.Join(wanted, ",")
	was, _ := owner.screenLayers.Load().(string)
	owner.screenLayers.Store(key)
	if owner.screenWatched.Swap(watched) == watched && was == key && !force {
		return
	}
	s.log.Info("screen watchers", "session", owner.id, "assistido", watched,
		"camadas", key)
	s.cfg.Signal(owner.userID, owner.login, "voice_watchers", map[string]any{
		"screen": watched, "layers": wanted,
	})
}

func (s *SFU) roomChanged(roomID string) {
	if s.cfg.RoomChanged != nil {
		s.cfg.RoomChanged(roomID)
	}
}

// session looks up a session and refuses if it is not this person's. The two
// failures are one error on purpose: the answer to "is this a live session?"
// must not depend on whose it is.
func (s *SFU) session(id string, userID int64) (*session, error) {
	s.mu.RLock()
	sess, ok := s.sessions[id]
	s.mu.RUnlock()
	if !ok || sess.userID != userID {
		return nil, ErrNoSession
	}
	return sess, nil
}

// drop removes a device from everything and closes its connection. It is safe
// to call twice, which matters because both the browser hanging up and the
// connection failing lead here.
func (s *SFU) drop(sess *session) {
	sess.closeOnce.Do(func() {
		// Both locks at once, s.mu first as everywhere else: deciding that the
		// room is empty and taking it out of the index have to be one step, or
		// a Join can move in between them.
		s.mu.Lock()
		delete(s.sessions, sess.id)
		r := sess.room
		r.mu.Lock()
		delete(r.peers, sess.id)
		empty := len(r.peers) == 0
		if empty {
			r.gone = true
			if cur, ok := s.rooms[r.id]; ok && cur == r {
				delete(s.rooms, r.id)
			}
		}
		others := make([]*publication, 0, len(r.pubs))
		for _, p := range r.pubs {
			if p.owner != sess {
				others = append(others, p)
			}
		}
		r.mu.Unlock()
		s.mu.Unlock()

		// And whatever this device was receiving. Closing the connection ends
		// the reads, but every publication still counted it as a subscriber
		// and still carried a forwarder for it — a screen went on being
		// encoded, in every layer, for somebody who had gone.
		sess.forgetSubscriptions(others)

		go func() { _ = sess.pc.Close() }()
		s.log.Info("voice leave", "room", r.id, "session", sess.id)
		s.roomChanged(r.id)
	})
}

// forgetSubscriptions undoes every subscription of a device that is leaving,
// without touching its connection, which is being closed anyway.
func (sess *session) forgetSubscriptions(pubs []*publication) {
	sess.subOpMu.Lock()
	sess.left = true
	sess.subMu.Lock()
	held := sess.subs
	sess.subs = map[string]*webrtc.RTPSender{}
	sess.subMu.Unlock()
	var screens []*session
	for _, p := range pubs {
		if _, ok := held[p.id]; !ok {
			continue
		}
		p.subs.Add(-1)
		p.dropForwarder(sess)
		if p.source == SourceScreen {
			screens = append(screens, p.owner)
		}
	}
	sess.subOpMu.Unlock()
	for _, owner := range screens {
		owner.room.sfu.watchersChanged(owner, false)
	}
}

// publishSources cleans up what a device asked to send. Anything unrecognised
// is dropped rather than refused: a newer client asking for something this
// server has never heard of should get a working voice channel, not an error.
//
// "audio" and "video" are accepted as the old names for the microphone and a
// camera, because the D2 measuring tool speaks them and there is no reason to
// go and change a tool whose numbers are already in the runbook.
func publishSources(asked []string) []string {
	out := make([]string, 0, 4)
	seen := map[string]bool{}
	for _, k := range asked {
		switch k {
		case "audio":
			k = SourceMic
		case "video":
			k = SourceCamera
		}
		if _, ok := sourceKind(k); !ok || seen[k] {
			continue
		}
		seen[k] = true
		out = append(out, k)
	}
	if len(out) == 0 {
		return []string{SourceMic}
	}
	return out
}

func newID() (string, error) {
	var b [32]byte
	if _, err := rand.Read(b[:]); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(b[:]), nil
}
