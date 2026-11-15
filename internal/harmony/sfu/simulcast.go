package sfu

import (
	"encoding/binary"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/pion/rtcp"
	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"
)

// Simulcast: one screen, three sizes, one chosen per person watching.
//
// Everything in this file exists because of one sentence the owner wrote after
// a night of testing: the room receives a single stream, dimensioned by the
// best connection in it, and the phone in somebody's hand pays for the fibre in
// somebody else's house. Worse than that, it runs the other way too — one
// subscriber on a bad line makes the *sender's* encoder back off, and everybody
// loses the picture because of one of them.
//
// The browser sends the same screen three times (web/harmony/src/media.ts,
// SCREEN_LAYERS). This server picks one per subscriber and never touches the
// pixels: it reads RTP headers, rewrites two numbers in them, and copies the
// payload through.

// layerOrder is biggest first. It is the order the browser offers them in and
// the order every loop here walks, so "the next one down" is just +1.
var layerOrder = []string{"alta", "media", "baixa"}

func layerIndex(rid string) int {
	for i, r := range layerOrder {
		if r == rid {
			return i
		}
	}
	return -1
}

// layerSilent is how long a layer may deliver nothing before it stops counting
// as arriving.
//
// A layer does not only end by ending. The browser switches one off with
// `active: false`, and that stops the packets without touching the track — so
// the read loop simply blocks, removeLayer never runs, and the layer stays
// marked live for ever. On 20/09/2026 that left the owner's second screen dead
// for forty seconds: he was watching the big layer, the sender was told to
// stop sending it, and nothing in here could tell the difference between "this
// layer is quiet" and "this layer is gone". Silence is the only signal there
// is, so silence has to be the signal.
//
// Three seconds is long enough that the small layer's fifteen frames a second
// never trips it, and short enough that nobody sits in front of a frozen
// picture wondering.
const layerSilent = 3 * time.Second

// layer is one of the three tracks arriving on the screen's transceiver.
type layer struct {
	rid  string
	ssrc uint32
	live atomic.Bool
	// When the most recent packet of this layer arrived, in Unix nanoseconds.
	lastAt atomic.Int64
}

// arriving is whether this layer is still delivering, which is not the same as
// whether it was ever registered.
func (l *layer) arriving(now time.Time) bool {
	if !l.live.Load() {
		return false
	}
	return now.UnixNano()-l.lastAt.Load() < int64(layerSilent)
}

// forwarder is one subscriber's own copy of a simulcast publication.
//
// It exists because the shared track that serves everybody else cannot serve
// this: two people on two layers are two different streams of packets, and a
// stream of packets is a sequence — it cannot fork. So each subscriber gets its
// own local track and its own pair of running offsets.
//
// The offsets are the whole trick. When somebody moves from the big layer to
// the small one, the packets they start receiving carry the small layer's
// sequence numbers and timestamps, which have nothing to do with the ones they
// were just getting. A receiver that sees the numbers jump decides it has lost
// a great deal and asks for everything again, or freezes. So the numbers are
// made continuous on the way out: this subscriber sees one unbroken stream and
// never learns that anything changed.
type forwarder struct {
	pub   *publication
	local *webrtc.TrackLocalStaticRTP
	sess  *session

	mu      sync.Mutex
	current string // the rid being written right now; "" until the first packet
	want    string // the rid asked for; the swap waits for a fresh full picture
	// The two things that decide `want`, and they are decided by different
	// people. lossWant is what this subscriber's own line can carry, moved by
	// the ladder in report(). ceiling is what the subscriber ASKED for, and it
	// comes from how big the picture is actually being drawn over there — a
	// screen in a thumbnail does not need 1080p, and sending it is paid for
	// twice: once in the sender's upload and once in this person's decoder.
	//
	// The effective answer is the SMALLER picture of the two. A ceiling is a
	// ceiling, and a bad line is a bad line; neither one overrules the other.
	lossWant string
	ceiling  string
	// Rewriting state. outSeq and outTS are what the subscriber last saw;
	// inSeq and inTS are what the current layer last sent.
	inputSSRC uint32 // source identity for the current timestamp translation
	started   bool
	outSeq    uint16
	outTS     uint32
	inSeq     uint16
	inTS      uint32
	// The furthest this subscriber has ever been sent, which is not the same
	// as the last thing it was sent: a packet that arrives late is given its
	// own old number, correctly, and that walks outSeq backwards. Starting the
	// next layer from there would hand out a number already used, and the
	// receiver would treat the new picture as an old duplicate and drop it.
	// Found by Codex auditing this file on 18/09/2026, with the sequence
	// 100, 102, 101, switch — which produces 102 twice.
	outHi   uint16
	outTSHi uint32
	// When the last packet actually went out, so a layer change can be given
	// the time that really passed instead of one 90 kHz tick. A tick is 11
	// microseconds; if the two layers' pictures are 100 ms apart, claiming 11
	// microseconds moves the whole timeline and lands in the receiver's jitter
	// estimate as a 100 ms error. ion-sfu and Janus both use elapsed time here.
	lastOut time.Time
	// Serialises the decision and the write. Two layers can be inside write()
	// at the same moment on either side of a change, and Pion's local track
	// takes only a read lock — so without this the packet decided second can
	// reach the wire first.
	wmu sync.Mutex
	// Protected by wmu. StaticRTP writes succeed with no bindings, so those
	// writes must not consume the keyframe that starts this subscriber.
	bound bool

	// Set when a layer change actually happened, so the person showing can be
	// told that the layer being left behind is now free. The notification
	// cannot happen inside next(): it walks every forwarder of this
	// publication and would take this same lock again.
	justSwitched atomic.Bool

	// Choosing. See report().
	lastMove time.Time
	clean    int  // consecutive clean reports since the last loss
	moved    bool // whether this subscriber's own line has ever moved it

	// The memory that keeps the ladder from oscillating.
	//
	// A line that cannot carry the big layer produces exactly one shape of
	// trouble, and it repeats forever: the layer loses packets, the ladder
	// falls a step, the small layer fits so the reports go clean, the hold
	// expires, the ladder climbs back into the same wall. The owner's log of
	// 19/09/2026 has twenty of those cycles in eight minutes, at a period of
	// exactly holdAfterMove, with one person watching whose window never
	// changed size. It cost that person 74 freezes.
	//
	// Falling is free; climbing is not. Each climb is a layer change, which
	// waits for a fresh full picture — ten to fifty times the size of an
	// ordinary one — and shows as a freeze. So the ladder has to remember
	// which layer just refused to be carried, and wait longer each time
	// before believing in it again.
	failures map[string]int       // consecutive times this layer broke the line
	failedAt map[string]time.Time // when it last broke it
	promoted map[string]time.Time // when this subscriber last climbed onto it
}

// newForwarder gives this subscriber its own output.
func (p *publication) newForwarder(sess *session) (*forwarder, error) {
	local, err := webrtc.NewTrackLocalStaticRTP(p.codec, p.trackID, p.streamID)
	if err != nil {
		return nil, err
	}
	f := &forwarder{pub: p, local: local, sess: sess, lastMove: time.Now()}
	// Everybody starts on the biggest layer that is actually arriving, and
	// falls from there if their line says so. Starting small and climbing
	// would show the whole room a blurry screen for the first seconds of every
	// share, which is close to the thing being fixed here.
	f.lossWant = p.bestLive()
	f.want = f.lossWant
	p.outMu.Lock()
	p.outs[sess.id] = f
	p.outMu.Unlock()
	// Asked again after being registered. Between the two lines above, a layer
	// can end — and removeLayer only moves forwarders it can see in the map,
	// so this one would have been born wanting something that is already gone.
	if live := p.bestLive(); live != "" && f.watching() != live {
		f.mu.Lock()
		if !f.moved {
			f.lossWant = live
			f.settleLocked()
		}
		f.mu.Unlock()
	}
	return f, nil
}

// forwarderFor is how a caller finds one subscriber's output again.
func (p *publication) forwarderFor(sess *session) *forwarder {
	p.outMu.Lock()
	defer p.outMu.Unlock()
	return p.outs[sess.id]
}

func (p *publication) dropForwarder(sess *session) {
	p.outMu.Lock()
	delete(p.outs, sess.id)
	p.outMu.Unlock()
}

// layered finds the publication a layer belongs to, creating it on the first
// layer to arrive. Three tracks on one transceiver are one thing somebody is
// showing, not three: the room must never learn that a screen came in pieces,
// or it would draw the same screen three times.
//
// The key is the person's session and the source, because that is exactly what
// "one screen from one device" means.
func (r *room) layered(sess *session, source, kind string, remote *webrtc.TrackRemote) *publication {
	id := sess.id + ":" + source + ":camadas"
	r.mu.Lock()
	pub, ok := r.pubs[id]
	if !ok {
		pub = &publication{
			id: id, owner: sess, kind: kind, source: source,
			simul:    true,
			codec:    remote.Codec().RTPCodecCapability,
			trackID:  remote.ID(),
			streamID: remote.StreamID(),
			layers:   map[string]*layer{},
			outs:     map[string]*forwarder{},
		}
		// askAgain is what a subscriber's "start over" request lands on. With
		// layers it means the layer that subscriber is actually watching, and
		// subscribe() calls the forwarder's own version; this one is the
		// fallback for anything that still holds a publication and asks.
		pub.askAgain = func() { pub.askLayer(pub.bestLive()) }
		// Stored here, under the same lock as the lookup: three layers arrive
		// on three goroutines within a few milliseconds of each other, and two
		// of them finding nothing and both creating one would give the room
		// two screens from one person.
		r.pubs[id] = pub
	}
	r.mu.Unlock()
	pub.refs.Add(1)
	if !ok {
		// Added after refs, so a layer ending the instant it began cannot take
		// the publication away from a layer that is still arriving.
		r.addPublication(pub)
		// And the person showing is told, straight away, whether anybody is
		// receiving it — which at this moment is usually nobody.
		r.sfu.watchersChanged(sess, true)
	}
	return pub
}

// layersWanted is the set of layers that still have to be on the wire, so the
// person showing can stop encoding the rest.
//
// It reports a RANGE, not the layers in use, and that distinction is the whole
// of this function. It was written the other way first, and the owner's Path
// of Exile log of 20/09/2026 is what a range exists to prevent: twenty-one
// times in seventeen minutes the middle layer was switched off and switched
// back on, and every single one of the sender's seven "0 fps" seconds falls
// within five seconds of one of those. The watcher paid 70 freezes, 24.7
// seconds of them, 4.3% of the call.
//
// The mechanism, once seen, is obvious. Turning an encoding off and on again
// reconfigures the browser's SimulcastEncoderAdapter, which stalls capture and
// costs a full picture. It is therefore expensive, and it was being driven by
// the cheapest, most transient thing in the system: where this subscriber's
// LOSS LADDER happens to be standing this second. A line that dips for two
// seconds is not a statement that a layer is unwanted.
//
// So the range runs from the biggest picture anybody may need — their ceiling,
// which is the size the video is actually drawn at over there and only changes
// when they resize or their decoder gives up — all the way down to the
// smallest layer.
//
// All the way down, and not "one step below where they stand", which is what
// this was first written as and what the oscillation test caught within a
// minute: a bottom edge that follows the ladder is a bottom edge that flaps,
// so the defect would have moved from the middle layer to the small one
// instead of going away. The smallest layer is where a failing line always
// ends up, it is 800 kbit against the big layer's fifteen megabits, and a
// price of five percent to never stall is not a decision worth making twice.
//
// **Light the runway before it is needed, because lighting it costs exactly
// when you can least afford it** — the moment a line is already failing is the
// worst possible time to make the sender rebuild its encoder.
//
// What this does NOT give up is the saving the range was introduced for: when
// everybody is watching in a small tile, every ceiling is the small layer, top
// and bottom meet there, and the two big encoders go dark. That is the grid,
// and the grid is the common case. What it gives up is the saving on a good
// line watching full screen, where the middle layer now idles along at 4 Mbit
// against a measured 32 Mbit of uplink. That is the price, it is named, and it
// buys back 4.3% of the picture.
func (p *publication) layersWanted() []string {
	last := len(layerOrder) - 1
	top, bottom := -1, -1
	p.outMu.Lock()
	for _, f := range p.outs {
		f.mu.Lock()
		// The biggest picture this subscriber may need.
		//
		// No ceiling at all is not "does not count" — it is "no limit", so it
		// anchors the top at the biggest layer. Reading it as nothing was the
		// first defect Codex found in this function on 20/09/2026: the top
		// would then follow current/want, which the ladder moves, and the
		// flapping this whole range exists to stop would come straight back
		// for anybody who has not said a size yet. That also covers the
		// moment a forwarder is born, before the first layer has arrived and
		// before the watcher has measured its own tile.
		if f.ceiling == "" {
			top = 0
		} else if i := layerIndex(f.ceiling); i >= 0 && (top < 0 || i < top) {
			top = i
		}
		// current is in here because a layer change waits for a full picture,
		// and until it arrives the old layer is what is on their screen.
		for _, rid := range []string{f.current, f.want} {
			if i := layerIndex(rid); i >= 0 && (top < 0 || i < top) {
				top = i
			}
		}
		// And the floor, because a line that keeps failing ends up there and
		// the fall must not be the thing that lights it.
		bottom = last
		f.mu.Unlock()
	}
	p.outMu.Unlock()
	if top < 0 {
		return nil
	}
	if bottom < top {
		bottom = top
	}
	out := make([]string, 0, len(layerOrder))
	for i := top; i <= bottom; i++ {
		out = append(out, layerOrder[i])
	}
	return out
}

// wantedChanged tells the person showing which layers are still worth sending.
func (p *publication) wantedChanged() {
	if p.owner == nil || p.owner.room == nil || p.owner.room.sfu == nil {
		return
	}
	p.owner.room.sfu.watchersChanged(p.owner, false)
}

// bestLiveUnder is the biggest layer arriving that is not bigger than the
// ceiling. An empty ceiling is no ceiling, so it is just bestLive.
func (p *publication) bestLiveUnder(ceiling string) string {
	floor := layerIndex(ceiling)
	if floor < 0 {
		floor = 0
	}
	now := time.Now()
	p.layMu.Lock()
	defer p.layMu.Unlock()
	for i := floor; i < len(layerOrder); i++ {
		if l, ok := p.layers[layerOrder[i]]; ok && l.arriving(now) {
			return layerOrder[i]
		}
	}
	return ""
}

// bestLive is the biggest layer currently arriving. A browser may not have all
// three up yet — or may have stopped one because its own upload is small — and
// asking for a layer that is not there is asking for a black screen.
func (p *publication) bestLive() string {
	now := time.Now()
	p.layMu.Lock()
	defer p.layMu.Unlock()
	for _, rid := range layerOrder {
		if l, ok := p.layers[rid]; ok && l.arriving(now) {
			return rid
		}
	}
	return ""
}

// askIfWanted asks for a fresh full picture when somebody is waiting for this
// layer and the packets going past are not the kind they can start from.
//
// It is the answer to the one way this whole design can stall: the swap only
// happens on a picture that stands on its own, the request for one is rate
// limited, and a request that is swallowed is never repeated. A person who
// stopped watching and started again lands exactly there — and waits on a
// screen that is not changing, so the encoder never produces one by itself.
// Found on 18/09/2026 by the e2e that clicks "parar de assistir" and then
// "assistir à tela", which timed out; Codex had described the shape of it in
// the same hour, from the code alone.
func (p *publication) askIfWanted(rid string) {
	p.outMu.Lock()
	var waiting bool
	for _, f := range p.outs {
		f.mu.Lock()
		if (!f.started || f.current != rid) && (f.want == rid || f.want == "") {
			waiting = true
		}
		f.mu.Unlock()
		if waiting {
			break
		}
	}
	p.outMu.Unlock()
	if waiting {
		p.askLayer(rid)
	}
}

// alive says whether a layer is still arriving.
func (p *publication) alive(rid string) bool {
	now := time.Now()
	p.layMu.Lock()
	defer p.layMu.Unlock()
	l, ok := p.layers[rid]
	return ok && l.arriving(now)
}

// addLayer registers one arriving layer, and lifts anybody who is only on a
// small one because the big one had not arrived yet.
//
// The three layers arrive on three goroutines within a few milliseconds of each
// other, and nothing says which wins: the probe that proved this path saw them
// come baixa, alta, media. Somebody who subscribed in that gap started on the
// smallest picture there was, and without this would have climbed out of it
// only through the slow ladder in report() — eight seconds of held blur for a
// line with nothing wrong with it.
//
// It only lifts subscribers whose own line has never moved them. Somebody the
// reports pushed down stays down; that decision was made about them.
func (p *publication) addLayer(rid string, ssrc uint32) *layer {
	p.layMu.Lock()
	l, ok := p.layers[rid]
	if !ok || l.ssrc != ssrc {
		l = &layer{rid: rid}
		p.layers[rid] = l
	}
	l.ssrc = ssrc
	l.live.Store(true)
	// Registered counts as arriving, so that the first packet has time to
	// turn up before anybody calls the layer silent.
	l.lastAt.Store(time.Now().UnixNano())
	p.layMu.Unlock()
	// Which stream a layer is decides whether its cached report still counts.
	p.wakeSync()

	idx := layerIndex(rid)
	if idx < 0 {
		return l
	}
	// Through lossWant and settleLocked, like every other move: writing want
	// directly skipped the ceiling, so a telephone that had asked for the
	// small picture was handed the big one the moment it arrived late.
	asks := map[string]bool{}
	p.outMu.Lock()
	for _, f := range p.outs {
		f.mu.Lock()
		if !f.moved && (f.lossWant == "" || layerIndex(f.lossWant) > idx) {
			f.lossWant = rid
			if f.settleLocked() {
				asks[f.want] = true
			}
		}
		f.mu.Unlock()
	}
	p.outMu.Unlock()
	for target := range asks {
		p.askLayer(target)
	}
	return l
}

// removeLayer marks a layer as gone and moves anybody who was on it.
func (p *publication) removeLayer(rid string, retiredSSRC ...uint32) {
	p.layMu.Lock()
	if l, ok := p.layers[rid]; ok {
		if len(retiredSSRC) != 0 && l.ssrc != retiredSSRC[0] {
			p.layMu.Unlock()
			return
		}
		l.live.Store(false)
	}
	p.layMu.Unlock()
	fallback := p.bestLive()
	if fallback == "" {
		return
	}
	// The biggest layer still arriving under each possible limit, read
	// before taking the forwarders' locks. Falling to the biggest layer there
	// is would hand a subscriber with a small ceiling the big picture.
	under := map[string]string{}
	for _, cap := range layerOrder {
		under[cap] = p.bestLiveUnder(cap)
	}
	asks := map[string]bool{}
	p.outMu.Lock()
	for _, f := range p.outs {
		f.mu.Lock()
		if f.want == rid || f.current == rid {
			target := fallback
			// The tighter of the two limits: what the line carries and what
			// the screen draws.
			limit := layerIndex(f.lossWant)
			if c := layerIndex(f.ceiling); c > limit {
				limit = c
			}
			if limit >= 0 {
				if t := under[layerOrder[limit]]; t != "" {
					target = t
				}
			}
			f.want = target
			asks[target] = true
		}
		f.mu.Unlock()
	}
	p.outMu.Unlock()
	for target := range asks {
		p.askLayer(target)
	}
}

// askLayer asks the sender for a fresh full picture on one layer, at most once
// every keyframeEvery — the same aggregation as keyframe(), for the same
// reason, but counted per layer because they are three separate encoders.
func (p *publication) askLayer(rid string) {
	// Copied out under the lock, not read afterwards: the same layer object is
	// reused when a layer comes back, and addLayer writes its ssrc. Reading it
	// out here is a data race, and the packet it would send asks a stranger for
	// a picture.
	p.layMu.Lock()
	l, ok := p.layers[rid]
	var ssrc uint32
	if ok {
		ssrc = l.ssrc
		ok = l.live.Load()
	}
	p.layMu.Unlock()
	if !ok {
		return
	}
	now := time.Now()
	p.kfMu.Lock()
	if p.kfPer == nil {
		p.kfPer = map[string]time.Time{}
	}
	if last, seen := p.kfPer[rid]; seen && now.Sub(last) < keyframeEvery {
		p.kfMu.Unlock()
		return
	}
	p.kfPer[rid] = now
	p.kfMu.Unlock()
	_ = p.owner.pc.WriteRTCP([]rtcp.Packet{&rtcp.PictureLossIndication{MediaSSRC: ssrc}})
}

// write hands one packet of one layer to this subscriber, if this is the layer
// it is on. It returns whether the packet was written.
//
// pkt is shared with every other forwarder on this publication, so the header
// is copied before anything in it is changed. Changing it in place would hand
// the next subscriber somebody else's numbers.
func (f *forwarder) write(rid string, pkt *rtp.Packet, keyframe bool) bool {
	// Every packet of every layer is offered to every subscriber, and each
	// subscriber is on one layer: two offers in three are to somebody who is
	// neither on this layer nor waiting for it. next() would refuse those all
	// the same, after the write lock and the layer table; saying no here costs
	// one lock. It is the answer next() gives for this state, taken an instant
	// earlier, so no packet is dropped that would have been sent.
	f.mu.Lock()
	apart := f.current != rid && f.want != rid
	f.mu.Unlock()
	if apart {
		return false
	}
	f.wmu.Lock()
	defer f.wmu.Unlock()
	if !f.bound {
		return false
	}
	out, ok := f.next(rid, pkt, keyframe)
	if !ok {
		return false
	}
	wrote := f.local.WriteRTP(&out) == nil
	// The layer this subscriber was on may now be free. Told outside next(),
	// and outside its lock.
	if f.justSwitched.Swap(false) {
		f.pub.wantedChanged()
		// The timestamp translation just changed: this subscriber's sender
		// report has to follow it now, not at the next publisher report.
		f.pub.wakeSync()
	}
	return wrote
}

// forwarderTrack observes the negotiated output binding, rather than treating
// a successful write on an empty StaticRTP track as delivery to the viewer.
type forwarderTrack struct {
	*webrtc.TrackLocalStaticRTP
	f *forwarder
	// Retained while unbound; a renegotiation of the same output must not
	// discard the decoder history or the RTP sequence/timestamp mapping.
	bindID    string
	bindSSRC  webrtc.SSRC
	bindPT    webrtc.PayloadType
	bindCodec string
}

func (t *forwarderTrack) Bind(ctx webrtc.TrackLocalContext) (webrtc.RTPCodecParameters, error) {
	t.f.wmu.Lock()
	defer t.f.wmu.Unlock()
	codec, err := t.TrackLocalStaticRTP.Bind(ctx)
	if err == nil {
		if t.bindID != ctx.ID() || t.bindSSRC != ctx.SSRC() ||
			t.bindPT != codec.PayloadType || t.bindCodec != codecKey(codec) {
			t.f.mu.Lock()
			t.f.started, t.f.current = false, ""
			t.f.mu.Unlock()
		}
		t.bindID, t.bindSSRC = ctx.ID(), ctx.SSRC()
		t.bindPT, t.bindCodec = codec.PayloadType, codecKey(codec)
		t.f.bound = true
	}
	return codec, err
}

func (t *forwarderTrack) Unbind(ctx webrtc.TrackLocalContext) error {
	t.f.wmu.Lock()
	defer t.f.wmu.Unlock()
	err := t.TrackLocalStaticRTP.Unbind(ctx)
	if err == nil {
		t.f.bound = false
	}
	return err
}

// next is the arithmetic on its own, so it can be read — and tested — without a
// connection on the end of it.
func (f *forwarder) next(rid string, pkt *rtp.Packet, keyframe bool) (rtp.Packet, bool) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.pub != nil && pkt.SSRC != 0 {
		f.pub.layMu.Lock()
		layer := f.pub.layers[rid]
		current := layer != nil && layer.ssrc == pkt.SSRC
		f.pub.layMu.Unlock()
		if !current {
			return rtp.Packet{}, false
		}
	}
	if f.current != rid || (pkt.SSRC != 0 && f.inputSSRC != pkt.SSRC) {
		// A layer change only happens on a fresh full picture. Starting in the
		// middle of one would give the subscriber frames that refer back to
		// pictures it never received: a smear, for as long as the encoder
		// takes to start over by itself.
		if f.want != rid || !keyframe {
			return rtp.Packet{}, false
		}
		if f.started {
			// Carry on from the furthest this subscriber has been sent — one
			// past it — rather than from the last packet handed over, which
			// may have been a late one from further back.
			// When both publisher clocks are known, stay on the same capture
			// timeline. Arrival time differs by encoder/pacer delay per layer.
			mapped, aligned := f.captureTimestamp(rid, pkt.Timestamp)
			if aligned && int32(mapped-f.outTSHi) <= 0 {
				return rtp.Packet{}, false // an older/same captured picture
			}
			f.outSeq = f.outHi + 1
			// Before both SRs arrive, retain the existing arrival-time fallback.
			// Once known, capture time below replaces this provisional estimate.
			ticks := uint32(time.Since(f.lastOut).Seconds() * 90000)
			if ticks == 0 {
				ticks = 1
			}
			f.outTS = f.outTSHi + ticks
			if aligned {
				f.outTS = mapped
			}
		} else {
			f.outSeq = pkt.SequenceNumber
			f.outTS = pkt.Timestamp
			f.outHi = f.outSeq
			f.outTSHi = f.outTS
			f.started = true
		}
		f.current = rid
		f.inputSSRC = pkt.SSRC
		f.inSeq = pkt.SequenceNumber
		f.inTS = pkt.Timestamp
		f.justSwitched.Store(true)
	} else {
		// Ordinary packet: advance the output by however much the input
		// advanced. Using the difference, rather than a fixed offset, means a
		// gap in the input stays a gap in the output — which is what tells the
		// subscriber to ask for a retransmission, and is true.
		f.outSeq += pkt.SequenceNumber - f.inSeq
		f.outTS += pkt.Timestamp - f.inTS
		f.inSeq = pkt.SequenceNumber
		f.inTS = pkt.Timestamp
	}
	// The high-water marks only ever move forward. Comparing with a signed
	// difference is what makes 65535 -> 0 count as forward and 102 -> 101 as
	// backward, on a counter that wraps.
	if int16(f.outSeq-f.outHi) > 0 {
		f.outHi = f.outSeq
	}
	if int32(f.outTS-f.outTSHi) > 0 {
		f.outTSHi = f.outTS
	}
	f.lastOut = time.Now()
	out := *pkt
	out.SequenceNumber = f.outSeq
	out.Timestamp = f.outTS
	return out, true
}

// settleLocked works out the effective layer from the two that decide it, and
// says whether it moved. The caller holds f.mu and asks for a fresh picture
// outside it.
func (f *forwarder) settleLocked() bool {
	want := f.lossWant
	if f.ceiling != "" && layerIndex(f.ceiling) > layerIndex(want) {
		want = f.ceiling
	}
	if want == "" || want == f.want {
		return false
	}
	f.want = want
	return true
}

// setCeiling is the subscriber saying how big the picture needs to be, which
// it knows and this server cannot: it is the size the video is being drawn at
// on their screen, or nothing at all when the app is behind something else.
//
// An empty rid takes the ceiling off.
func (f *forwarder) setCeiling(rid string) {
	f.mu.Lock()
	if rid != "" && layerIndex(rid) < 0 {
		f.mu.Unlock()
		return
	}
	if f.ceiling == rid {
		f.mu.Unlock()
		return
	}
	f.ceiling = rid
	moved := f.settleLocked()
	target := f.want
	f.mu.Unlock()
	if moved {
		f.pub.askLayer(target)
		f.pub.owner.room.sfu.log.Info("layer ceiling", "session", f.sess.id,
			"dono", f.pub.owner.username, "teto", rid, "camada", target)
	}
	// Always, and not only when the layer moved. layersWanted reads the
	// ceiling itself, so a subscriber already held down on the small layer
	// who then shrinks the window changes the answer without moving — and the
	// person showing went on encoding two big layers for nobody. The other
	// way round, it may have this layer switched off, because until now
	// nobody wanted it. watchersChanged sends nothing when nothing changed.
	f.pub.wantedChanged()
}

// watching is the layer this subscriber is on, or on its way to.
func (f *forwarder) watching() string {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.want != "" {
		return f.want
	}
	return f.current
}

// ------------------------------------------------------------------ choosing

// These numbers decide who gets what. They are deliberately slow: a picture
// that changes size every two seconds is worse to watch than one that is
// slightly too small and stays put.
const (
	// Above this share of packets lost in one report, the line is not
	// delivering what it was given.
	lossDown = 0.05
	// Reports in a row with almost nothing lost before trying a bigger layer.
	// They arrive about once a second.
	cleanBeforeUp = 6
	// The floor under any change, in either direction.
	holdAfterMove = 8 * time.Second
	// How long a layer that broke this line stays untried, the first time.
	// It is the hold itself, so a single bad second still costs exactly what
	// it costs today: this is memory being added, not patience.
	retryBase = holdAfterMove
	// And every further failure of the SAME layer multiplies that wait.
	// LiveKit's probe regulator uses this same factor (ccutils,
	// DefaultProbeRegulatorConfig: BaseInterval 3s, BackoffFactor 1.5,
	// MaxInterval 2min).
	retryFactor = 1.5
	// Where the wait stops growing. Two minutes is LiveKit's number as well,
	// and they can afford to be the impatient one: they probe with padding,
	// which does not disturb the picture anybody is watching. This commits to
	// the layer instead, and pays a full picture and a freeze for every guess
	// — so if the two should differ, this is the one that ought to wait more.
	retryMax = 2 * time.Minute
	// A layer this subscriber held for a whole minute was not a mistake,
	// whatever happened afterwards. The count starts over, so somebody who
	// walks from mobile data onto wi-fi is not held down by an afternoon of
	// old evidence.
	retryProven = time.Minute
)

// mayTry says whether enough time has passed to believe in a layer that broke
// this line before. The wait is retryBase for the first failure and grows by
// retryFactor for each one after, up to retryMax.
//
// The caller holds f.mu.
func (f *forwarder) mayTry(rid string, now time.Time) bool {
	n := f.failures[rid]
	if n <= 0 {
		return true
	}
	wait := float64(retryBase)
	for i := 1; i < n; i++ {
		wait *= retryFactor
		if wait >= float64(retryMax) {
			break
		}
	}
	if wait > float64(retryMax) {
		wait = float64(retryMax)
	}
	return now.Sub(f.failedAt[rid]) >= time.Duration(wait)
}

// rememberLayers makes the three maps on first use. A forwarder is built in
// more than one place, and a nil map reads as empty and only panics on the
// write — which is the kind of trap that waits for the one line added later.
//
// The caller holds f.mu.
func (f *forwarder) rememberLayers() {
	if f.failures == nil {
		f.failures = map[string]int{}
		f.failedAt = map[string]time.Time{}
		f.promoted = map[string]time.Time{}
	}
}

// noteFailure writes down that this layer is what the line was carrying when
// it started losing packets.
//
// The caller holds f.mu.
func (f *forwarder) noteFailure(rid string, now time.Time) {
	f.rememberLayers()
	if at, ok := f.promoted[rid]; ok && now.Sub(at) >= retryProven {
		// It carried this layer for a long time and only now stopped. That is
		// fresh trouble, not the next step of an old pattern.
		f.failures[rid] = 1
	} else {
		f.failures[rid]++
	}
	f.failedAt[rid] = now
}

// report takes one receiver report from this subscriber and decides whether to
// move it. The fraction is the share of packets lost since the last report,
// which is the one honest thing a subscriber tells us about its own line: it is
// measured at the far end, after everything in between.
//
// It only ever moves one step. A line that is genuinely bad produces another
// report a second later, and the steps are big enough — half the width each
// time — that two of them are the whole ladder.
func (f *forwarder) report(fraction float64) {
	f.mu.Lock()
	now := time.Now()
	held := now.Sub(f.lastMove) < holdAfterMove
	// The ladder walks in two different places, and it has to.
	//
	// **Down** from what this person is actually watching. A ceiling holding
	// them at the middle layer while their line falls apart must not cost them
	// an extra eight seconds of hold before the fall even starts.
	//
	// **Up** from what the line itself last said. Otherwise a ceiling would
	// erase the memory of a good line, and taking the ceiling off would land
	// the person on a small picture they would then have to climb out of.
	cur := f.want
	if cur == "" {
		cur = f.current
	}
	upFrom := f.lossWant
	if upFrom == "" {
		upFrom = cur
	}
	idx := layerIndex(cur)
	upIdx := layerIndex(upFrom)
	// A change that has been asked for but not yet happened. The swap waits
	// for a fresh full picture, and the request for one can be lost, or
	// swallowed by the rate limit, or simply not answered — and nothing else
	// in here would ever ask again. So every report asks again while the swap
	// is still pending; askLayer is what keeps that from becoming a flood.
	pending := f.current != "" && cur != "" && cur != f.current
	var target, reason string
	switch {
	case fraction >= lossDown:
		f.clean = 0
		if !held && idx >= 0 && idx+1 < len(layerOrder) {
			target = layerOrder[idx+1]
			reason = "loss"
			// What the line was carrying when it broke. Remembering it is the
			// whole difference between a ladder and an oscillator.
			f.noteFailure(cur, now)
		}
	case fraction < lossDown/5:
		f.clean++
		if !held && f.clean >= cleanBeforeUp && upIdx > 0 {
			up := layerOrder[upIdx-1]
			if f.mayTry(up, now) {
				target = up
				reason = "clean"
				f.rememberLayers()
				f.promoted[up] = now
			}
			// Held back by the backoff. The clean count is deliberately NOT
			// reset: the line is fine, it is this particular layer that is not
			// trusted yet, and when the wait is over the climb should happen
			// at once rather than six seconds later.
		}
	default:
		// In between: not bad enough to fall, not clean enough to climb. The
		// counter resets, because "almost fine" repeated forever should not
		// add up to a promotion.
		f.clean = 0
	}
	if target == "" {
		f.mu.Unlock()
		// Waiting for a picture from an encoder that has stopped is waiting
		// forever — and so is SITTING on one. Until 20/09/2026 this only ran
		// while a swap was pending, so a subscriber settled on a layer that
		// went quiet had nothing at all to move it: the owner's second screen
		// showed `0 fps 0 kbit/s` for forty seconds and only came back
		// because he stopped watching and started again by hand. Being
		// already on the dead layer is the worse half of the same problem,
		// not a different one.
		//
		// What the two owners settled on — the line's lossWant under the
		// screen's ceiling. A layer that is not arriving is a fact about the
		// sender, not about this person's line, so it never writes lossWant:
		// until 08/10/2026 it did, and a viewer who enlarged the picture while
		// the sender was still switching the big layer back on had their line's
		// memory overwritten with the small layer and climbed back one held
		// step at a time — 30 fps for four seconds after asking for 60 (r23).
		f.mu.Lock()
		settled := f.lossWant
		if f.ceiling != "" && layerIndex(f.ceiling) > layerIndex(settled) {
			settled = f.ceiling
		}
		f.mu.Unlock()
		// The settled layer is arriving: go to it. This is what lifts a
		// stand-in (below) the moment the real layer is back.
		if settled != "" && settled != cur && f.pub.alive(settled) {
			f.mu.Lock()
			f.want = settled
			f.mu.Unlock()
			f.pub.askLayer(settled)
			return
		}
		if pending || !f.pub.alive(cur) {
			if !f.pub.alive(cur) {
				// A stand-in, under the ceiling: a phone that said "480 is my
				// size" is not sent 1080 because the layer it wanted went away
				// (Codex, 20/09/2026).
				live := f.pub.bestLiveUnder(settled)
				if live == "" {
					f.mu.Lock()
					teto := f.ceiling
					f.mu.Unlock()
					live = f.pub.bestLiveUnder(teto)
				}
				if live == "" {
					// Nothing at or below the ceiling is arriving. A picture
					// that is too big beats a picture that is frozen.
					live = f.pub.bestLive()
				}
				if live != "" {
					f.mu.Lock()
					f.want = live
					f.mu.Unlock()
				}
			}
			f.pub.askLayer(f.watching())
		}
		return
	}
	f.lossWant = target
	f.lastMove = now
	f.clean = 0
	f.moved = true
	moved := f.settleLocked()
	lossTarget := target
	target = f.want
	f.mu.Unlock()
	f.pub.owner.room.sfu.log.Info("layer decision", "session", f.sess.id,
		"source", f.pub.source, "from", cur, "layer", target,
		"loss_layer", lossTarget, "reason", reason, "fraction", fraction,
		"changed", moved)

	if !moved {
		// The line moved but the ceiling was already holding it there, so
		// nothing changes for this subscriber.
		return
	}
	f.pub.askLayer(target)
	f.pub.wantedChanged()
}

// --------------------------------------------------------- reading a header

// startsKeyframe says whether this packet begins a fresh full picture.
//
// This reads the first byte or two of the payload, and it is worth being
// explicit that it is **not decoding**: no pixel is produced here, nothing is
// re-encoded, and the bytes are copied onward untouched. It is the same kind of
// act as reading the RTP header — a header says what the thing is, and this one
// happens to live one layer in. Without it a layer change would have to happen
// blind, and a blind change is a smeared picture for as long as the encoder
// takes to start over by itself.
//
// An unknown codec answers true, so a switch still happens rather than never.
func startsKeyframe(mime string, b []byte) bool {
	switch {
	case strings.EqualFold(mime, webrtc.MimeTypeH264):
		return h264Keyframe(b)
	case strings.EqualFold(mime, webrtc.MimeTypeVP8):
		return vp8Keyframe(b)
	default:
		return true
	}
}

// h264Keyframe follows RFC 6184. A picture that can be decoded on its own is an
// IDR (NAL type 5), and a sender puts the two descriptions of the stream in
// front of it — SPS (7), then PPS (8), then the picture.
//
// SPS counts as a starting point and PPS on its own does not, and the asymmetry
// is deliberate. A subscriber that joins at the IDR and never saw the SPS
// cannot decode it, so the SPS is where the change has to begin. A PPS,
// meanwhile, is the one of the three a sender may repeat without a picture
// behind it; treating it as a starting point would change layer into frames
// that lean on pictures this subscriber never received. Codex raised this on
// 18/09/2026 — it had "any of the three", which was wrong in both directions.
func h264Keyframe(b []byte) bool {
	if len(b) < 1 {
		return false
	}
	switch t := b[0] & 0x1F; {
	case t == 5 || t == 7:
		return true
	case t == 24: // STAP-A: several small units in one packet
		for i := 1; i+2 <= len(b); {
			size := int(binary.BigEndian.Uint16(b[i : i+2]))
			i += 2
			if size == 0 || i+size > len(b) {
				return false
			}
			if n := b[i] & 0x1F; n == 5 || n == 7 {
				return true
			}
			i += size
		}
	case t == 28 || t == 29: // FU-A/FU-B: one unit split across packets
		// FU-B carries a two-byte decoding order number after the header; a
		// packet too short to hold it is malformed, not a starting point.
		least := 2
		if t == 29 {
			least = 4
		}
		if len(b) < least {
			return false
		}
		start, end := b[1]&0x80 != 0, b[1]&0x40 != 0
		if !start || end { // not the first fragment, or both at once
			return false
		}
		if n := b[1] & 0x1F; n == 5 || n == 7 {
			return true
		}
	}
	return false
}

// vp8Keyframe follows RFC 7741: skip the payload descriptor, then the first bit
// of the payload header is 0 on a key frame.
func vp8Keyframe(b []byte) bool {
	if len(b) < 1 {
		return false
	}
	// S set and partition index 0: this packet starts a frame.
	if b[0]&0x10 == 0 || b[0]&0x07 != 0 {
		return false
	}
	i := 1
	if b[0]&0x80 != 0 { // X: extended control bits present
		if len(b) < 2 {
			return false
		}
		ext := b[1]
		i++
		if ext&0x80 != 0 { // I: picture id, one or two bytes
			if len(b) <= i {
				return false
			}
			if b[i]&0x80 != 0 {
				i += 2
			} else {
				i++
			}
		}
		if ext&0x40 != 0 { // L: temporal level zero index
			i++
		}
		if ext&0x30 != 0 { // T/K: temporal index, key index
			i++
		}
	}
	if len(b) <= i {
		return false
	}
	return b[i]&0x01 == 0
}
