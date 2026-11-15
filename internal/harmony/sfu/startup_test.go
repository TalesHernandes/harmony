package sfu

import (
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/pion/rtcp"
	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"
)

type bindingTestContext struct {
	webrtc.TrackLocalContext
	id     string
	ssrc   webrtc.SSRC
	codec  webrtc.RTPCodecParameters
	writer *bindingTestWriter
}

func (c bindingTestContext) ID() string                              { return c.id }
func (c bindingTestContext) SSRC() webrtc.SSRC                       { return c.ssrc }
func (c bindingTestContext) SSRCRetransmission() webrtc.SSRC         { return 0 }
func (c bindingTestContext) SSRCForwardErrorCorrection() webrtc.SSRC { return 0 }
func (c bindingTestContext) CodecParameters() []webrtc.RTPCodecParameters {
	return []webrtc.RTPCodecParameters{c.codec}
}
func (c bindingTestContext) WriteStream() webrtc.TrackLocalWriter { return c.writer }

type bindingTestWriter struct{ headers []rtp.Header }

func (w *bindingTestWriter) WriteRTP(h *rtp.Header, p []byte) (int, error) {
	w.headers = append(w.headers, *h)
	return len(p), nil
}
func (w *bindingTestWriter) Write(p []byte) (int, error) { return len(p), nil }

func TestForwarderBindingContinuity(t *testing.T) {
	for _, change := range []string{"same", "sender", "ssrc", "payload"} {
		t.Run(change, func(t *testing.T) {
			cap := webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeVP8, ClockRate: 90000}
			local, err := webrtc.NewTrackLocalStaticRTP(cap, "cam", "cam")
			if err != nil {
				t.Fatal(err)
			}
			f := &forwarder{local: local, pub: &publication{}, want: "alta"}
			track := &forwarderTrack{TrackLocalStaticRTP: local, f: f}
			writer := &bindingTestWriter{}
			ctx := bindingTestContext{id: "sender", ssrc: 700, codec: webrtc.RTPCodecParameters{RTPCodecCapability: cap, PayloadType: 96}, writer: writer}
			key := &rtp.Packet{Header: rtp.Header{Version: 2, SequenceNumber: 100, Timestamp: 9000}, Payload: []byte{0x10, 0}}
			if f.write("alta", key, true) {
				t.Fatal("unbound keyframe consumed")
			}
			if _, err := track.Bind(ctx); err != nil {
				t.Fatal(err)
			}
			if !f.write("alta", key, true) {
				t.Fatal("initial keyframe rejected")
			}
			if err := track.Unbind(ctx); err != nil {
				t.Fatal(err)
			}
			key.SequenceNumber++
			key.Timestamp += 3000
			if f.write("alta", key, false) {
				t.Fatal("packet written without a binding")
			}
			switch change {
			case "sender":
				ctx.id = "another-sender"
			case "ssrc":
				ctx.ssrc++
			case "payload":
				ctx.codec.PayloadType++
			}
			if _, err := track.Bind(ctx); err != nil {
				t.Fatal(err)
			}
			if got := f.write("alta", key, false); got != (change == "same") {
				t.Fatalf("delta accepted=%v after %s binding", got, change)
			}
			if change != "same" && !f.write("alta", key, true) {
				t.Fatal("changed binding rejected fresh keyframe")
			}
			if len(writer.headers) != 2 {
				t.Fatalf("wire packets=%d", len(writer.headers))
			}
			last := writer.headers[1]
			if last.SSRC != uint32(ctx.ssrc) || last.PayloadType != uint8(ctx.codec.PayloadType) || last.SequenceNumber != 101 || last.Timestamp != 12000 {
				t.Fatalf("unexpected output identity or mapping: %+v", last)
			}
		})
	}
}

func TestLateScreenViewerKeepsKeyframeUntilBound(t *testing.T) {
	s := testServer(t, 8)
	owner := newClient(t, s, "startup", 1, "publisher")
	viewer := newClient(t, s, "startup", 2, "viewer")
	ownerSess, err := s.session(owner.id, owner.user)
	if err != nil {
		t.Fatal(err)
	}
	viewerSess, err := s.session(viewer.id, viewer.user)
	if err != nil {
		t.Fatal(err)
	}
	cap := webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeH264, ClockRate: 90000, SDPFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f"}
	p := &publication{id: "late-screen", owner: ownerSess, kind: "video", source: SourceScreen,
		simul: true, codec: cap, trackID: "screen", streamID: "screen",
		layers: map[string]*layer{}, outs: map[string]*forwarder{}}
	p.addLayer("alta", 500)
	p.addLayer("baixa", 501)
	viewerSess.watchMu.Lock()
	viewerSess.watching = map[string]struct{}{ownerSess.username: {}}
	viewerSess.watchMu.Unlock()
	if err := viewerSess.subscribe(p); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { viewerSess.unsubscribe(p) })
	f := p.forwarderFor(viewerSess)
	key := &rtp.Packet{Header: rtp.Header{Version: 2, SequenceNumber: 10, Timestamp: 9000, Marker: true}, Payload: []byte{0x65, 1}}
	// The publisher answers the initial request before the viewer answers SDP.
	if f.write("alta", key, true) {
		t.Error("unbound viewer consumed a keyframe that never reached the wire")
	}
	f.mu.Lock()
	started := f.started
	f.mu.Unlock()
	if started {
		t.Error("unbound viewer is marked started")
	}

	packets := make(chan *rtp.Packet, 16)
	viewer.pc.OnTrack(func(track *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		for {
			pkt, _, err := track.ReadRTP()
			if err != nil {
				return
			}
			select {
			case packets <- pkt:
			default:
			}
		}
	})
	viewerSess.negMu.Lock()
	offer, err := s.offerLocked(viewerSess)
	viewerSess.negMu.Unlock()
	if err != nil {
		t.Fatal(err)
	}
	viewerSess.subMu.Lock()
	sender := viewerSess.subs[p.id]
	viewerSess.subMu.Unlock()
	params := sender.GetParameters()
	if len(params.Encodings) != 1 {
		t.Fatalf("encodings: %d", len(params.Encodings))
	}
	ssrc := uint32(params.Encodings[0].SSRC)
	if ssrc == 0 || !strings.Contains(offer.SDP, fmt.Sprintf("a=ssrc:%d ", ssrc)) {
		t.Fatal("outgoing media SSRC is absent from the server SDP")
	}
	t.Logf("server SDP media SSRC=%d; publisher layer SSRC=500", ssrc)
	if err := viewer.answer(offer); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "viewer transport", func() bool { return viewer.pc.ConnectionState() == webrtc.PeerConnectionStateConnected })
	delta := &rtp.Packet{Header: rtp.Header{Version: 2, SequenceNumber: 11, Timestamp: 10500, Marker: true}, Payload: []byte{0x41, 2}}
	if f.write("alta", delta, false) {
		t.Error("viewer started on a delta after missing the keyframe")
	}

	// Chromium addresses PLI to the remote media SSRC advertised in SDP.
	// Send that identity through real SRTCP and the production sender reader.
	p.kfMu.Lock()
	clear(p.kfPer)
	p.kfMu.Unlock()
	if err := viewer.pc.WriteRTCP([]rtcp.Packet{&rtcp.PictureLossIndication{SenderSSRC: 700, MediaSSRC: ssrc}}); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "PLI for the watched layer", func() bool {
		p.kfMu.Lock()
		defer p.kfMu.Unlock()
		return !p.kfPer["alta"].IsZero()
	})
	p.kfMu.Lock()
	if !p.kfPer["baixa"].IsZero() {
		t.Error("PLI requested the wrong layer")
	}
	clear(p.kfPer)
	p.kfMu.Unlock()
	for _, other := range []uint32{0, 500, uint32(params.Encodings[0].RTX.SSRC), ssrc + 1} {
		if other == ssrc {
			continue
		}
		dispatchFeedback(t, p, sender.GetParameters(), f, &rtcp.PictureLossIndication{MediaSSRC: other})
	}
	p.kfMu.Lock()
	if len(p.kfPer) != 0 {
		t.Error("foreign/zero/RTX PLI requested a keyframe")
	}
	p.kfMu.Unlock()
	// Model the publisher's answer with a new IDR on exactly the requested layer.
	key.SequenceNumber, key.Timestamp = 12, 12000
	if !f.write("alta", key, true) {
		t.Fatal("bound viewer rejected its fresh keyframe")
	}
	select {
	case pkt := <-packets:
		if pkt.SSRC != ssrc || !startsKeyframe(webrtc.MimeTypeH264, pkt.Payload) {
			t.Fatalf("first wire packet is not the advertised stream's keyframe: %+v", pkt)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("viewer received no keyframe")
	}
}

// Like the browser regression, delay the restart request reaching the server
// while RTP proves recovery, then answer its late offer on the same session.
// A pending normal offer also exercises RestartICE's replay path. Only the
// initial packet is a keyframe: another one cannot rescue a broken rebind.
func TestFlowingMediaSurvivesCancelledRestartAndLateAnswer(t *testing.T) {
	s := testServer(t, 8)
	owner := newClient(t, s, "late-restart", 1, "publisher")
	viewer := newClient(t, s, "late-restart", 2, "viewer")
	ownerSess, err := s.session(owner.id, owner.user)
	if err != nil {
		t.Fatal(err)
	}
	viewerSess, err := s.session(viewer.id, viewer.user)
	if err != nil {
		t.Fatal(err)
	}
	p := &publication{id: "flowing-camera", owner: ownerSess, kind: "video", source: SourceCamera,
		simul: true, codec: webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeVP8, ClockRate: 90000},
		trackID: "cam", streamID: "cam", layers: map[string]*layer{}, outs: map[string]*forwarder{}}
	p.addLayer("alta", 500)
	if err := viewerSess.subscribe(p); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { viewerSess.unsubscribe(p) })
	f := p.forwarderFor(viewerSess)
	viewerSess.negMu.Lock()
	offer, err := s.offerLocked(viewerSess)
	viewerSess.negMu.Unlock()
	if err != nil {
		t.Fatal(err)
	}
	if err := viewer.answer(offer); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "viewer connected", func() bool { return viewer.pc.ConnectionState() == webrtc.PeerConnectionStateConnected })
	viewerSess.subMu.Lock()
	sender := viewerSess.subs[p.id]
	viewerSess.subMu.Unlock()
	var seq uint16
	write := func(key bool) {
		seq++
		payload := []byte{0x10, 1, 1}
		if key {
			payload[1] = 0
		}
		if !f.write("alta", &rtp.Packet{Header: rtp.Header{Version: 2, SequenceNumber: seq,
			Timestamp: uint32(seq) * 3000, Marker: true}, Payload: payload}, key) {
			t.Fatalf("output rejected packet %d (key=%v)", seq, key)
		}
	}
	write(true)
	waitFor(t, "initial RTP", func() bool { return viewer.receivedKind("video") > 0 })
	grow := func(stage string) {
		before := viewer.receivedKind("video")
		deadline := time.Now().Add(3 * time.Second)
		for time.Now().Before(deadline) && viewer.receivedKind("video") < before+5 {
			write(false)
			time.Sleep(20 * time.Millisecond)
		}
		if viewer.receivedKind("video") < before+5 {
			t.Fatalf("RTP stopped at %s without a fresh keyframe", stage)
		}
	}
	grow("flowing deltas")
	pc, sid := viewer.pc, viewer.id
	// The client has started recovery, but the HTTP request is still in flight.
	if err := sender.ReplaceTrack(sender.Track()); err != nil {
		t.Fatal(err)
	}
	grow("spontaneous recovery while restart request is delayed")
	// A pre-existing offer is replayed without disrupting the working network.
	viewerSess.negMu.Lock()
	pending, err := s.offerLocked(viewerSess)
	viewerSess.negMu.Unlock()
	if err != nil {
		t.Fatal(err)
	}
	replay, err := s.RestartICE(sid, viewer.user)
	if err != nil {
		t.Fatal(err)
	}
	if replay.SDP != pending.SDP || replay.ICERestart {
		t.Fatal("expected pending offer replay")
	}
	grow("RTP while replayed offer is unanswered")
	if err := viewer.answer(replay); err != nil {
		t.Fatal(err)
	}
	// Recovery was cancelled by growing RTP. The original delayed request can
	// still create a fresh server offer, which must be answered on this session.
	offer, err = s.RestartICE(sid, viewer.user)
	if err != nil {
		t.Fatal(err)
	}
	if !offer.ICERestart {
		t.Fatal("expected fresh restart offer")
	}
	// Exercise Pion's actual Unbind/Bind pair on the same output. This is
	// stronger than ICE alone, which does not replace the server's track.
	if err := sender.ReplaceTrack(sender.Track()); err != nil {
		t.Fatal(err)
	}
	if viewerSess.pc.SignalingState() != webrtc.SignalingStateHaveLocalOffer {
		t.Fatal("late server offer was not pending at recovery")
	}
	if err := viewer.answer(offer); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "late answer stable", func() bool { return viewerSess.pc.SignalingState() == webrtc.SignalingStateStable })
	if err := sender.ReplaceTrack(sender.Track()); err != nil {
		t.Fatal(err)
	}
	grow("RTP after late answer without fresh keyframe")
	if viewer.pc != pc || viewer.id != sid {
		t.Fatal("recovery replaced the session")
	}
}

func TestLateScreenViewerRetriesWhileAnotherWatches(t *testing.T) {
	s := testServer(t, 8)
	var clientsMu sync.Mutex
	clients := map[string]*client{}
	offers := make(chan Offer, 8)
	errors := make(chan error, 8)
	var held atomic.Pointer[client]
	s.cfg.Signal = func(_ int64, _ string, kind string, data any) {
		if kind != "voice_offer" {
			return
		}
		o := data.(Offer)
		clientsMu.Lock()
		c := clients[o.SessionID]
		clientsMu.Unlock()
		if c == nil {
			return
		}
		if c == held.Load() {
			offers <- o
			return
		}
		if err := c.answer(o); err != nil {
			errors <- err
		}
	}
	register := func(c *client) { clientsMu.Lock(); clients[c.id] = c; clientsMu.Unlock() }
	owner := newClient(t, s, "retry", 1, "publisher", func(m *webrtc.MediaEngine) {
		for _, uri := range []string{"urn:ietf:params:rtp-hdrext:sdes:mid", "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"} {
			if err := m.RegisterHeaderExtension(webrtc.RTPHeaderExtensionCapability{URI: uri}, webrtc.RTPCodecTypeVideo); err != nil {
				t.Fatal(err)
			}
		}
	})
	register(owner)
	first := newClient(t, s, "retry", 2, "first")
	register(first)
	late := newClient(t, s, "retry", 3, "late")
	register(late)
	t.Cleanup(func() { clientsMu.Lock(); clear(clients); clientsMu.Unlock() })
	cap := webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeH264, ClockRate: 90000, SDPFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f"}
	track, err := webrtc.NewTrackLocalStaticRTP(cap, "screen", "screen", webrtc.WithRTPStreamID("alta"))
	if err != nil {
		t.Fatal(err)
	}
	transceiver, err := owner.pc.AddTransceiverFromTrack(track, webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionSendonly})
	if err != nil {
		t.Fatal(err)
	}
	other, err := webrtc.NewTrackLocalStaticRTP(cap, "screen", "screen", webrtc.WithRTPStreamID("baixa"))
	if err != nil {
		t.Fatal(err)
	}
	if err := transceiver.Sender().AddEncoding(other); err != nil {
		t.Fatal(err)
	}
	if _, err := owner.offerOnce([]string{SourceScreen}); err != nil {
		t.Fatal(err)
	}
	var sender *webrtc.RTPSender
	for _, snd := range owner.pc.GetSenders() {
		if snd.Track() == track {
			sender = snd
		}
	}
	if sender == nil {
		t.Fatal("publisher sender not found")
	}
	var keyRequested atomic.Bool
	var producerGate sync.Mutex
	keyRequested.Store(true)
	// The publisher produces a keyframe only in response to PLI, never on a
	// timer. This makes recovery depend on the server's actual retry path.
	go func() {
		for {
			pkts, _, err := sender.ReadRTCP()
			if err != nil {
				return
			}
			for _, pkt := range pkts {
				if _, ok := pkt.(*rtcp.PictureLossIndication); ok {
					keyRequested.Store(true)
				}
			}
		}
	}()
	stop := make(chan struct{})
	var wg sync.WaitGroup
	wg.Add(1)
	defer func() { close(stop); wg.Wait() }()
	go func() {
		defer wg.Done()
		tick := time.NewTicker(time.Second / 60)
		defer tick.Stop()
		var seq uint16
		for {
			select {
			case <-stop:
				return
			case <-tick.C:
				producerGate.Lock()
				seq++
				nal := byte(0x41)
				if keyRequested.Swap(false) {
					nal = 0x65
				}
				pkt := &rtp.Packet{Header: rtp.Header{Version: 2, SequenceNumber: seq, Timestamp: uint32(seq) * 1500, Marker: true}, Payload: []byte{nal, 1}}
				for _, ext := range sender.GetParameters().HeaderExtensions {
					switch ext.URI {
					case "urn:ietf:params:rtp-hdrext:sdes:mid":
						for _, tr := range owner.pc.GetTransceivers() {
							if tr.Sender() == sender {
								_ = pkt.SetExtension(uint8(ext.ID), []byte(tr.Mid()))
							}
						}
					case "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id":
						_ = pkt.SetExtension(uint8(ext.ID), []byte("alta"))
					}
				}
				_ = track.WriteRTP(pkt)
				producerGate.Unlock()
			}
		}
	}()
	first.watch(t, "publisher")
	waitFor(t, "first viewer screen", func() bool { return first.receivedKind("video") > 0 })
	ownerSess, err := s.session(owner.id, owner.user)
	if err != nil {
		t.Fatal(err)
	}
	ownerSess.room.mu.Lock()
	var p *publication
	for _, pub := range ownerSess.room.pubs {
		if pub.owner == ownerSess && pub.source == SourceScreen {
			p = pub
		}
	}
	ownerSess.room.mu.Unlock()
	if p == nil {
		t.Fatal("screen publication not found")
	}
	if !p.simul {
		t.Fatal("retry proof requires the simulcast forwarder")
	}
	held.Store(late)
	late.watch(t, "publisher")
	var offer Offer
	select {
	case offer = <-offers:
	case <-time.After(3 * time.Second):
		t.Fatal("late viewer offer not found")
	}
	// Wait beyond the keyframe rate limit while the SDP answer is withheld.
	// Every IDR in this interval can serve the first viewer but not the late one.
	time.Sleep(keyframeEvery + 100*time.Millisecond)
	producerGate.Lock()
	if err := late.answer(offer); err != nil {
		producerGate.Unlock()
		t.Fatal(err)
	}
	// Do not let an IDR already requested before binding slip through after
	// the answer. Only a new post-answer request may restart the publisher.
	keyRequested.Store(false)
	producerGate.Unlock()
	waitFor(t, "late viewer transport", func() bool { return late.pc.ConnectionState() == webrtc.PeerConnectionStateConnected })
	// There is no RTP at the late viewer yet, so it cannot rescue the server
	// with PLI. The ongoing first viewer must not suppress the upstream retry.
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) && late.receivedKind("video") == 0 {
		time.Sleep(20 * time.Millisecond)
	}
	if late.receivedKind("video") == 0 {
		t.Fatal("settled viewer suppressed the late viewer's keyframe retry")
	}
	select {
	case err := <-errors:
		t.Fatal(err)
	default:
	}
}
