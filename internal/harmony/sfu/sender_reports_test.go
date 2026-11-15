package sfu

import (
	"github.com/pion/interceptor"
	"github.com/pion/rtcp"
	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"
	"sync"
	"testing"
	"time"
)

// Known capture instants cross real publisher -> SFU -> subscriber SRTP/SRTCP.
// Audio arrives 200 ms after video. Arrival is deliberately NOT capture time.
func TestSenderReportsPreserveCaptureAlignment(t *testing.T) {
	t.Run("uncompensated", func(t *testing.T) { checkSenderReportAlignment(t, 0) })
	t.Run("output-delay-250ms", func(t *testing.T) { checkSenderReportAlignment(t, 250) })
}

func checkSenderReportAlignment(t *testing.T, outputDelay int) {
	party := newParty(t, 8)
	pub := party.join(t, "sync", 1, "publisher")
	sub := party.join(t, "sync", 2, "viewer")
	if err := sub.sfu.OutputDelay(sub.id, sub.user, outputDelay); err != nil {
		t.Fatal(err)
	}
	type observed struct {
		id string
		sr rtcp.SenderReport
	}
	reports := make(chan observed, 256)
	sub.pc.OnTrack(func(track *webrtc.TrackRemote, recv *webrtc.RTPReceiver) {
		if track.StreamID() != "screen" {
			t.Errorf("screen media stream = %q", track.StreamID())
		}
		go func() {
			for {
				packets, _, err := recv.ReadRTCP()
				if err != nil {
					return
				}
				for _, packet := range packets {
					if sr, ok := packet.(*rtcp.SenderReport); ok {
						select {
						case reports <- observed{track.ID(), *sr}:
						default:
						}
					}
				}
			}
		}()
		for {
			if _, _, err := track.ReadRTP(); err != nil {
				return
			}
		}
	})
	sub.watch(t, "publisher")
	share := func(source, id string, cap webrtc.RTPCodecCapability) *sharedTrack {
		track, err := webrtc.NewTrackLocalStaticRTP(cap, id, "screen")
		if err != nil {
			t.Fatal(err)
		}
		pub.mu.Lock()
		pub.shared[source] = track
		pub.mu.Unlock()
		if err := pub.sfu.Publish(pub.id, pub.user, []string{source}); err != nil {
			t.Fatal(err)
		}
		return &sharedTrack{track: track}
	}
	video := share(SourceScreen, "sync-video", webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeVP8, ClockRate: 90000})
	audio := share(SourceScreenAudio, "sync-audio", webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus, ClockRate: 48000, Channels: 2})
	stop := make(chan struct{})
	var wg sync.WaitGroup
	defer func() { close(stop); wg.Wait() }()
	// Synthetic NTP epoch and unrelated RTP origins make the oracle independent
	// of SFU wall time and of each codec's clock rate.
	const ntp = uint64(3900000000) << 32
	for _, item := range []struct {
		track  *sharedTrack
		origin uint32
		rate   uint32
		delay  time.Duration
	}{
		{video, 0xffff0000, 90000, 0}, {audio, 123456, 48000, 200 * time.Millisecond},
	} {
		wg.Add(1)
		go func(item struct {
			track  *sharedTrack
			origin uint32
			rate   uint32
			delay  time.Duration
		}) {
			defer wg.Done()
			timer := time.NewTimer(item.delay)
			defer timer.Stop()
			select {
			case <-stop:
				return
			case <-timer.C:
			}
			tick := time.NewTicker(20 * time.Millisecond)
			defer tick.Stop()
			seq := uint16(0)
			for {
				select {
				case <-stop:
					return
				case <-tick.C:
					seq++
					_ = item.track.track.WriteRTP(&rtp.Packet{Header: rtp.Header{Version: 2, SequenceNumber: seq, Timestamp: item.origin + uint32(seq)*item.rate/50, Marker: true}, Payload: []byte{0x10, 0, 1}})
					for _, sender := range pub.pc.GetSenders() {
						if sender.Track() == item.track.track {
							for _, enc := range sender.GetParameters().Encodings {
								_ = pub.pc.WriteRTCP([]rtcp.Packet{&rtcp.SenderReport{SSRC: uint32(enc.SSRC), NTPTime: ntp, RTPTime: item.origin}})
							}
						}
					}
				}
			}
		}(item)
	}
	matched := map[string]bool{}
	captureErrors := map[string]float64{}
	deadline := time.NewTimer(6 * time.Second)
	defer deadline.Stop()
	var last observed
	stable := time.NewTimer(24 * time.Hour)
	defer stable.Stop()
	for {
		select {
		case last = <-reports:
			origin, rate := uint32(0xffff0000), float64(90000)
			if last.id == "sync-audio" {
				origin = 123456
				rate = 48000
			}
			capture := float64(int64(last.sr.NTPTime-ntp))/4294967296 + float64(int32(origin-last.sr.RTPTime))/rate
			if last.id == "sync-audio" {
				capture += float64(outputDelay) / 1000
			}
			captureErrors[last.id] = capture
			if len(captureErrors) == 2 && (captureErrors["sync-audio"] <= -0.0001 || captureErrors["sync-audio"] >= 0.0001 || captureErrors["sync-video"] <= -0.0001 || captureErrors["sync-video"] >= 0.0001) {
				t.Fatalf("capture mapping lost: audio-video=%f ms; capture errors (s)=%v", (captureErrors["sync-audio"]-captureErrors["sync-video"])*1000, captureErrors)
			}
			if capture <= -0.0001 || capture >= 0.0001 {
				continue
			}
			first := len(matched) < 2
			matched[last.id] = true
			if first && len(matched) == 2 {
				stable.Reset(1500 * time.Millisecond)
			}
		case <-stable.C:
			return
		case <-deadline.C:
			t.Fatalf("publisher capture clock not preserved: matched=%v last=%+v", matched, last)
		}
	}
}

func TestSenderReportLayerSwitchCaptureTimeline(t *testing.T) {
	p := &publication{layers: map[string]*layer{"alta": {ssrc: 11}, "baixa": {ssrc: 22}}}
	// Unrelated layer RTP origins, with report epochs a quarter second apart.
	const ntp = uint64(3900000000) << 32
	p.saveSenderReport("alta", captureReport{11, ntp, 0xfffff000})
	p.saveSenderReport("baixa", captureReport{22, ntp + (1 << 30), 700000})
	f := &forwarder{pub: p, want: "alta", bound: true}
	tr := &syncTrack{pub: p, f: f}
	first, ok := f.next("alta", &rtp.Packet{Header: rtp.Header{SequenceNumber: 65535, Timestamp: 0xfffff000}}, true)
	if !ok {
		t.Fatal("initial keyframe refused")
	}
	// Arrival gap says 700 ms, capture clocks say 300 ms. Capture must win.
	f.lastOut = time.Now().Add(-700 * time.Millisecond)
	f.want = "baixa"
	low, ok := f.next("baixa", &rtp.Packet{Header: rtp.Header{SequenceNumber: 400, Timestamp: 704500}}, true)
	if !ok || low.Timestamp-first.Timestamp != 27000 || low.SequenceNumber != 0 {
		t.Fatalf("layer change followed arrival clock: %+v", low.Header)
	}
	sr, off, ok := tr.mapping()
	if !ok || sr.ssrc != 22 || int32(low.Timestamp-(sr.rtp+off)) != 4500 {
		t.Fatal("low SR mapping lost")
	}
	// Out-of-order packets do not alter the translation offset.
	_, _ = f.next("baixa", &rtp.Packet{Header: rtp.Header{SequenceNumber: 399, Timestamp: 703000}}, false)
	_, after, _ := tr.mapping()
	if after != off {
		t.Fatal("late RTP changed report translation")
	}
	f.want = "alta"
	// A high-layer frame captured 400 ms after the first crosses RTP wrap.
	highTS := uint32(0xfffff000)
	highTS += 36000
	high, ok := f.next("alta", &rtp.Packet{Header: rtp.Header{SequenceNumber: 1, Timestamp: highTS}}, true)
	if !ok || high.Timestamp-first.Timestamp != 36000 || high.SequenceNumber != 1 {
		t.Fatalf("return to high broke capture time/sequence: %+v", high.Header)
	}
	sr, off, ok = tr.mapping()
	if !ok || sr.ssrc != 11 || off != 0 {
		t.Fatalf("return mapping=%+v offset=%d", sr, off)
	}
	// Old/same captured pictures cannot move the output backwards on a switch.
	f.want = "baixa"
	if _, ok = f.next("baixa", &rtp.Packet{Header: rtp.Header{Timestamp: 704500}}, true); ok {
		t.Fatal("obsolete keyframe accepted")
	}
	// Reports from an old incarnation of a layer cannot describe its new SSRC.
	p.layers["alta"].ssrc = 33
	if _, _, ok := tr.mapping(); ok {
		t.Fatal("reused RID accepted old SSRC clock")
	}
}

func TestSenderReportCacheAndMissingClock(t *testing.T) {
	p := &publication{}
	tr := &syncTrack{pub: p}
	if _, _, ok := tr.mapping(); ok {
		t.Fatal("fabricated capture clock before SR")
	}
	p.saveSenderReport("", captureReport{1, 100, 200})
	p.saveSenderReport("", captureReport{1, 99, 300})
	sr, offset, ok := tr.mapping()
	if !ok || sr.ntp != 100 || sr.rtp != 200 || offset != 0 {
		t.Fatal("reordered SR replaced current reference")
	}
	p.saveSenderReport("", captureReport{2, 98, 400})
	sr, _, _ = tr.mapping()
	if sr.ssrc != 2 || sr.rtp != 400 {
		t.Fatal("new SSRC did not replace old clock")
	}
}

func TestSenderReportSubscriberBindingAndSwitch(t *testing.T) {
	cap := webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeVP8, ClockRate: 90000}
	local, err := webrtc.NewTrackLocalStaticRTP(cap, "screen", "screen")
	if err != nil {
		t.Fatal(err)
	}
	p := &publication{owner: &session{id: "publisher"}, layers: map[string]*layer{"alta": {ssrc: 11}, "baixa": {ssrc: 22}}}
	const epoch = uint64(3900000000) << 32
	p.saveSenderReport("alta", captureReport{11, epoch, 1000})
	p.saveSenderReport("baixa", captureReport{22, epoch, 999000})
	f := &forwarder{pub: p, local: local, want: "alta"}
	reports := make(chan []rtcp.Packet, 16)
	tr := &syncTrack{TrackLocal: &forwarderTrack{TrackLocalStaticRTP: local, f: f}, pub: p, f: f, send: func(pkts []rtcp.Packet) error { reports <- pkts; return nil }}
	ctx := bindingTestContext{id: "viewer", ssrc: 700, codec: webrtc.RTPCodecParameters{RTPCodecCapability: cap, PayloadType: 96}, writer: &bindingTestWriter{}}
	if _, err := tr.Bind(ctx); err != nil {
		t.Fatal(err)
	}
	defer tr.Unbind(ctx)
	read := func(wantRTP, count uint32) {
		t.Helper()
		select {
		case pkts := <-reports:
			sr := pkts[0].(*rtcp.SenderReport)
			if sr.SSRC != 700 || sr.NTPTime != epoch || sr.RTPTime != wantRTP || sr.PacketCount != count || sr.OctetCount != count*3 || len(sr.Reports) != 0 {
				t.Fatalf("subscriber report=%+v", sr)
			}
			cname := pkts[1].(*rtcp.SourceDescription).Chunks[0]
			if cname.Source != 700 || cname.Items[0].Text != "publisher" {
				t.Fatal("CNAME not scoped to publisher")
			}
		case <-time.After(time.Second):
			t.Fatal("translated report missing")
		}
	}
	packet := func(rid string, seq uint16, ts uint32) {
		t.Helper()
		if !f.write(rid, &rtp.Packet{Header: rtp.Header{Version: 2, SequenceNumber: seq, Timestamp: ts}, Payload: []byte{0x10, 0, 1}}, true) {
			t.Fatal("RTP rejected")
		}
	}
	packet("alta", 500, 1000)
	read(1000, 1)
	f.mu.Lock()
	f.want = "baixa"
	f.mu.Unlock()
	packet("baixa", 800, 1008000)
	read(1000, 2)
	// An inactive layer's SR must not be sent under the active output SSRC.
	p.saveSenderReport("alta", captureReport{11, epoch + (1 << 32), 91000})
	select {
	case <-reports:
		t.Fatal("inactive layer emitted a report")
	case <-time.After(60 * time.Millisecond):
	}
	f.mu.Lock()
	f.want = "alta"
	f.mu.Unlock()
	packet("alta", 501, 109000)
	select {
	case pkts := <-reports:
		sr := pkts[0].(*rtcp.SenderReport)
		if sr.RTPTime != 91000 || sr.NTPTime != epoch+(1<<32) || sr.PacketCount != 3 {
			t.Fatalf("return report=%+v", sr)
		}
	case <-time.After(time.Second):
		t.Fatal("return report missing")
	}
	if err := tr.Unbind(ctx); err != nil {
		t.Fatal(err)
	}
	p.saveSenderReport("alta", captureReport{11, epoch + (2 << 32), 181000})
	select {
	case <-reports:
		t.Fatal("unbound subscriber emitted report")
	case <-time.After(60 * time.Millisecond):
	}
}

func TestSenderReportsSimulcastWireAndLateViewer(t *testing.T) {
	party := newParty(t, 8)
	pub := newClient(t, party.s, "wire-sync", 1, "publisher", func(m *webrtc.MediaEngine) {
		for _, uri := range []string{"urn:ietf:params:rtp-hdrext:sdes:mid", "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"} {
			if err := m.RegisterHeaderExtension(webrtc.RTPHeaderExtensionCapability{URI: uri}, webrtc.RTPCodecTypeVideo); err != nil {
				t.Fatal(err)
			}
		}
	})
	party.mu.Lock()
	party.clients[pub.id] = pub
	party.mu.Unlock()
	viewer := party.join(t, "wire-sync", 2, "viewer")
	cap := webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeH264, ClockRate: 90000, SDPFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f"}
	var tr *webrtc.RTPTransceiver
	var tracks []*webrtc.TrackLocalStaticRTP
	for _, rid := range []string{"alta", "baixa"} {
		track, err := webrtc.NewTrackLocalStaticRTP(cap, "screen", "screen", webrtc.WithRTPStreamID(rid))
		if err != nil {
			t.Fatal(err)
		}
		tracks = append(tracks, track)
		if tr == nil {
			tr, err = pub.pc.AddTransceiverFromTrack(track, webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionSendonly})
			if err != nil {
				t.Fatal(err)
			}
		} else if err := tr.Sender().AddEncoding(track); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := pub.offerOnce([]string{SourceScreen}); err != nil {
		t.Fatal(err)
	}
	const epoch = uint64(3900000000) << 32
	origins := []uint32{0xffff0000, 500000}
	stop := make(chan struct{})
	done := make(chan struct{})
	defer func() { close(stop); <-done }()
	go func() {
		defer close(done)
		tick := time.NewTicker(20 * time.Millisecond)
		defer tick.Stop()
		var seq uint16
		for {
			select {
			case <-stop:
				return
			case <-tick.C:
				seq++
				params := tr.Sender().GetParameters()
				for i, track := range tracks {
					pkt := &rtp.Packet{Header: rtp.Header{Version: 2, SequenceNumber: seq, Timestamp: origins[i] + uint32(seq)*1800, Marker: true}, Payload: []byte{0x65, byte(i), byte(seq >> 8), byte(seq)}}
					for _, ext := range params.HeaderExtensions {
						switch ext.URI {
						case "urn:ietf:params:rtp-hdrext:sdes:mid":
							_ = pkt.SetExtension(uint8(ext.ID), []byte(tr.Mid()))
						case "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id":
							_ = pkt.SetExtension(uint8(ext.ID), []byte(track.RID()))
						}
					}
					_ = track.WriteRTP(pkt)
					for _, enc := range params.Encodings {
						if enc.RID == track.RID() {
							_ = pub.pc.WriteRTCP([]rtcp.Packet{&rtcp.SenderReport{SSRC: uint32(enc.SSRC), NTPTime: epoch, RTPTime: origins[i]}})
						}
					}
				}
			}
		}
	}()
	owner, _ := party.s.session(pub.id, pub.user)
	var publication *publication
	waitFor(t, "both publisher layer clocks before viewer subscribes", func() bool {
		owner.room.mu.RLock()
		defer owner.room.mu.RUnlock()
		for _, p := range owner.room.pubs {
			if p.owner == owner && p.source == SourceScreen {
				p.clockMu.Lock()
				ready := len(p.clocks) == 2
				p.clockMu.Unlock()
				if ready {
					publication = p
					return true
				}
			}
		}
		return false
	})
	packets := make(chan *rtp.Packet, 256)
	reports := make(chan *rtcp.SenderReport, 64)
	viewer.pc.OnTrack(func(track *webrtc.TrackRemote, recv *webrtc.RTPReceiver) {
		if track.StreamID() != "screen" {
			t.Errorf("simulcast stream id=%q", track.StreamID())
		}
		go func() {
			for {
				pkts, _, err := recv.ReadRTCP()
				if err != nil {
					return
				}
				for _, pkt := range pkts {
					if sr, ok := pkt.(*rtcp.SenderReport); ok {
						select {
						case reports <- sr:
						default:
						}
					}
				}
			}
		}()
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
	viewer.watch(t, "publisher")
	for i, rid := range []string{"alta", "baixa", "alta"} {
		if err := party.s.Layer(viewer.id, viewer.user, "publisher", rid); err != nil {
			t.Fatal(err)
		}
		stagePacket, stageReport := false, false
		deadline := time.NewTimer(4 * time.Second)
		for !stagePacket || !stageReport {
			select {
			case pkt := <-packets:
				if len(pkt.Payload) != 4 {
					continue
				}
				want := byte(0)
				if rid == "baixa" {
					want = 1
				}
				if pkt.Payload[1] != want {
					continue
				}
				seq := uint16(pkt.Payload[2])<<8 | uint16(pkt.Payload[3])
				if pkt.Timestamp-origins[0] != uint32(seq)*1800 {
					t.Fatalf("wire capture timestamp changed at %s: %+v", rid, pkt.Header)
				}
				stagePacket = true
			case sr := <-reports:
				if sr.NTPTime != epoch || sr.RTPTime != origins[0] {
					t.Fatalf("wire SR lost capture alignment at %s: %+v", rid, sr)
				}
				stageReport = true
			case <-deadline.C:
				t.Fatalf("stage %d %s RTP=%v SR=%v", i, rid, stagePacket, stageReport)
			}
		}
		deadline.Stop()
	}
	viewerSess, _ := party.s.session(viewer.id, viewer.user)
	f := publication.forwarderFor(viewerSess)
	f.mu.Lock()
	current := f.current
	f.mu.Unlock()
	if current != "alta" {
		t.Fatal("test did not return to high layer")
	}
}

func TestSenderReportReconnectedLayer(t *testing.T) {
	const epoch = uint64(3900000000) << 32
	t.Run("old reader cannot replace new clock", func(t *testing.T) {
		p := &publication{layers: map[string]*layer{"alta": {ssrc: 22}}}
		p.saveSenderReport("alta", captureReport{22, epoch, 900000})
		p.saveSenderReport("alta", captureReport{11, epoch + (1 << 32), 1000})
		p.clockMu.Lock()
		got := p.clocks["alta"]
		p.clockMu.Unlock()
		if got.ssrc != 22 {
			t.Fatalf("old publisher replaced new clock: %+v", got)
		}
	})
	t.Run("new SSRC waits for a keyframe and a matching output", func(t *testing.T) {
		p := &publication{layers: map[string]*layer{"alta": {ssrc: 11}}}
		p.saveSenderReport("alta", captureReport{11, epoch, 1000})
		f := &forwarder{pub: p, want: "alta", bound: true}
		tr := &syncTrack{pub: p, f: f}
		first, ok := f.next("alta", &rtp.Packet{Header: rtp.Header{SSRC: 11, SequenceNumber: 100, Timestamp: 1000}}, true)
		if !ok {
			t.Fatal("first keyframe rejected")
		}
		p.addLayer("alta", 22)
		p.saveSenderReport("alta", captureReport{22, epoch + (1 << 32), 900000})
		if _, _, ok := tr.mapping(); ok {
			t.Error("new SR described packets of the old SSRC")
		}
		if _, ok := f.next("alta", &rtp.Packet{Header: rtp.Header{SSRC: 22, SequenceNumber: 800, Timestamp: 900000}}, false); ok {
			t.Error("new SSRC forwarded before keyframe")
		}
		next, ok := f.next("alta", &rtp.Packet{Header: rtp.Header{SSRC: 22, SequenceNumber: 801, Timestamp: 901500}}, true)
		if !ok || next.SequenceNumber != first.SequenceNumber+1 {
			t.Fatalf("reconnected sequence not continuous: %+v", next.Header)
		}
		sr, offset, ok := tr.mapping()
		if !ok || sr.ssrc != 22 || sr.rtp+offset+1500 != next.Timestamp {
			t.Fatal("reconnected report mismatch")
		}
		if _, ok := f.next("alta", &rtp.Packet{Header: rtp.Header{SSRC: 11, SequenceNumber: 101, Timestamp: 2500}}, true); ok {
			t.Fatal("old reader forwarded after reconnect")
		}
	})
}

func TestSenderReportVoiceStartsAfterRTPAndStopsOnUnbind(t *testing.T) {
	cap := webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus, ClockRate: 48000, Channels: 2}
	local, err := webrtc.NewTrackLocalStaticRTP(cap, "mic", "voice")
	if err != nil {
		t.Fatal(err)
	}
	p := &publication{source: SourceMic, owner: &session{id: "publisher"}}
	p.saveSenderReport("", captureReport{11, uint64(3900000000) << 32, 1000})
	reports := make(chan []rtcp.Packet, 8)
	tr := &syncTrack{TrackLocal: local, pub: p, send: func(pkts []rtcp.Packet) error { reports <- pkts; return nil }}
	ctx := bindingTestContext{id: "voice-viewer", ssrc: 700, codec: webrtc.RTPCodecParameters{RTPCodecCapability: cap, PayloadType: 111}, writer: &bindingTestWriter{}}
	for cycle := 0; cycle < 2; cycle++ {
		if _, err := tr.Bind(ctx); err != nil {
			t.Fatal(err)
		}
		o := tr.output
		select {
		case <-reports:
			t.Fatal("SR before first RTP")
		case <-time.After(50 * time.Millisecond):
		}
		if err := local.WriteRTP(&rtp.Packet{Header: rtp.Header{Version: 2, Timestamp: 1000}, Payload: []byte{1, 2, 3}}); err != nil {
			t.Fatal(err)
		}
		select {
		case pkts := <-reports:
			sr := pkts[0].(*rtcp.SenderReport)
			if sr.SSRC != 700 || sr.RTPTime != 1000 || sr.PacketCount != 1 || sr.OctetCount != 3 {
				t.Fatalf("voice SR=%+v", sr)
			}
		case <-time.After(time.Second):
			t.Fatal("voice SR missing")
		}
		if err := tr.Unbind(ctx); err != nil {
			t.Fatal(err)
		}
		select {
		case <-o.done:
		default:
			t.Fatal("run survived Unbind")
		}
	}
}

func TestSenderReportRunStopsWithSession(t *testing.T) {
	party := newParty(t, 8)
	pub := party.join(t, "close-sync", 1, "publisher")
	viewer := party.join(t, "close-sync", 2, "viewer")
	stop := make(chan struct{})
	defer close(stop)
	pub.talk(stop)
	sess, _ := party.s.session(viewer.id, viewer.user)
	var output *syncOutput
	waitFor(t, "bound ordinary voice reporter", func() bool {
		for _, sender := range sess.pc.GetSenders() {
			tr, ok := sender.Track().(*syncTrack)
			if !ok || tr.pub.source != SourceMic {
				continue
			}
			tr.mu.Lock()
			output = tr.output
			tr.mu.Unlock()
			if output != nil {
				return true
			}
		}
		return false
	})
	if err := sess.pc.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case <-output.done:
	case <-time.After(time.Second):
		t.Fatal("report goroutine survived session closure")
	}
}

func TestSenderReportOldLayerClosureDoesNotRetireReplacement(t *testing.T) {
	p := &publication{layers: map[string]*layer{}}
	old := p.addLayer("alta", 11)
	current := p.addLayer("alta", 22)
	p.removeLayer("alta", 11)
	if old == current || !p.alive("alta") {
		t.Fatal("old reader retired replacement layer")
	}
}

func TestSenderReportRegistriesNeverInventCaptureTime(t *testing.T) {
	for _, diagnostic := range []bool{false, true} {
		name := "default"
		var d *arrivalTrace
		if diagnostic {
			name = "diagnostic"
			d = &arrivalTrace{}
		}
		t.Run(name, func(t *testing.T) {
			m := &webrtc.MediaEngine{}
			if err := m.RegisterDefaultCodecs(); err != nil {
				t.Fatal(err)
			}
			registry, err := arrivalInterceptors(m, d)
			if err != nil {
				t.Fatal(err)
			}
			chain, err := registry.Build("no-generated-sr")
			if err != nil {
				t.Fatal(err)
			}
			defer chain.Close()
			reports := make(chan struct{}, 8)
			chain.BindRTCPWriter(interceptor.RTCPWriterFunc(func(pkts []rtcp.Packet, _ interceptor.Attributes) (int, error) {
				for _, pkt := range pkts {
					if _, ok := pkt.(*rtcp.SenderReport); ok {
						select {
						case reports <- struct{}{}:
						default:
						}
					}
				}
				return 0, nil
			}))
			info := &interceptor.StreamInfo{SSRC: 700, ClockRate: 48000, MimeType: webrtc.MimeTypeOpus}
			writer := chain.BindLocalStream(info, interceptor.RTPWriterFunc(func(_ *rtp.Header, p []byte, _ interceptor.Attributes) (int, error) { return len(p), nil }))
			defer chain.UnbindLocalStream(info)
			if _, err := writer.Write(&rtp.Header{Version: 2, SSRC: 700, Timestamp: 1000}, []byte{1, 2, 3}, nil); err != nil {
				t.Fatal(err)
			}
			select {
			case <-reports:
				t.Fatal("registry invented a capture clock from forwarding time")
			case <-time.After(1200 * time.Millisecond):
			}
		})
	}
}

// Every change a sender report is computed from has to announce itself. With
// the slow tick out of the way, a missing announcement is a report that never
// arrives instead of one that arrives a quarter of a second late.
func TestSenderReportsFollowWakesWithoutTheFallback(t *testing.T) {
	old := syncFallback
	syncFallback = time.Hour
	defer func() { syncFallback = old }()
	const epoch = uint64(3900000000) << 32
	next := func(t *testing.T, reports chan []rtcp.Packet, why string) *rtcp.SenderReport {
		t.Helper()
		select {
		case pkts := <-reports:
			return pkts[0].(*rtcp.SenderReport)
		case <-time.After(time.Second):
			t.Fatalf("no report after %s", why)
			return nil
		}
	}
	t.Run("voice", func(t *testing.T) {
		cap := webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus, ClockRate: 48000, Channels: 2}
		local, err := webrtc.NewTrackLocalStaticRTP(cap, "mic", "voice")
		if err != nil {
			t.Fatal(err)
		}
		p := &publication{source: SourceMic, owner: &session{id: "publisher"}}
		reports := make(chan []rtcp.Packet, 8)
		tr := &syncTrack{TrackLocal: local, pub: p, send: func(pkts []rtcp.Packet) error { reports <- pkts; return nil }}
		ctx := bindingTestContext{id: "voice-viewer", ssrc: 700, codec: webrtc.RTPCodecParameters{RTPCodecCapability: cap, PayloadType: 111}, writer: &bindingTestWriter{}}
		if _, err := tr.Bind(ctx); err != nil {
			t.Fatal(err)
		}
		defer tr.Unbind(ctx)
		// Media first and the publisher clock afterwards: the reverse of the
		// late joiner, and the order the old poll covered without saying so.
		if err := local.WriteRTP(&rtp.Packet{Header: rtp.Header{Version: 2, Timestamp: 1000}, Payload: []byte{1, 2, 3}}); err != nil {
			t.Fatal(err)
		}
		p.saveSenderReport("", captureReport{11, epoch, 1000})
		if sr := next(t, reports, "the first publisher report"); sr.RTPTime != 1000 {
			t.Fatalf("report=%+v", sr)
		}
		p.saveSenderReport("", captureReport{11, epoch + (1 << 32), 49000})
		if sr := next(t, reports, "a newer publisher report"); sr.RTPTime != 49000 {
			t.Fatalf("report=%+v", sr)
		}
	})
	t.Run("voice heard late", func(t *testing.T) {
		// The publisher clock is already known and nothing about it will move:
		// only the first packet out can say that a report is now allowed.
		cap := webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeOpus, ClockRate: 48000, Channels: 2}
		local, err := webrtc.NewTrackLocalStaticRTP(cap, "mic", "voice")
		if err != nil {
			t.Fatal(err)
		}
		p := &publication{source: SourceMic, owner: &session{id: "publisher"}}
		p.saveSenderReport("", captureReport{11, epoch, 1000})
		reports := make(chan []rtcp.Packet, 8)
		tr := &syncTrack{TrackLocal: local, pub: p, send: func(pkts []rtcp.Packet) error { reports <- pkts; return nil }}
		ctx := bindingTestContext{id: "voice-viewer", ssrc: 700, codec: webrtc.RTPCodecParameters{RTPCodecCapability: cap, PayloadType: 111}, writer: &bindingTestWriter{}}
		if _, err := tr.Bind(ctx); err != nil {
			t.Fatal(err)
		}
		defer tr.Unbind(ctx)
		select {
		case <-reports:
			t.Fatal("report before the first packet")
		case <-time.After(50 * time.Millisecond):
		}
		if err := local.WriteRTP(&rtp.Packet{Header: rtp.Header{Version: 2, Timestamp: 1000}, Payload: []byte{1, 2, 3}}); err != nil {
			t.Fatal(err)
		}
		if sr := next(t, reports, "the first packet"); sr.PacketCount != 1 {
			t.Fatalf("report=%+v", sr)
		}
	})
	t.Run("layers", func(t *testing.T) {
		cap := webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeVP8, ClockRate: 90000}
		local, err := webrtc.NewTrackLocalStaticRTP(cap, "screen", "screen")
		if err != nil {
			t.Fatal(err)
		}
		p := &publication{owner: &session{id: "publisher"}, layers: map[string]*layer{"alta": {ssrc: 11}, "baixa": {ssrc: 22}}}
		p.saveSenderReport("alta", captureReport{11, epoch, 1000})
		p.saveSenderReport("baixa", captureReport{22, epoch, 999000})
		f := &forwarder{pub: p, local: local, want: "alta"}
		reports := make(chan []rtcp.Packet, 8)
		tr := &syncTrack{TrackLocal: &forwarderTrack{TrackLocalStaticRTP: local, f: f}, pub: p, f: f, send: func(pkts []rtcp.Packet) error { reports <- pkts; return nil }}
		ctx := bindingTestContext{id: "viewer", ssrc: 700, codec: webrtc.RTPCodecParameters{RTPCodecCapability: cap, PayloadType: 96}, writer: &bindingTestWriter{}}
		if _, err := tr.Bind(ctx); err != nil {
			t.Fatal(err)
		}
		defer tr.Unbind(ctx)
		packet := func(rid string, seq uint16, ts uint32) {
			t.Helper()
			if !f.write(rid, &rtp.Packet{Header: rtp.Header{Version: 2, SequenceNumber: seq, Timestamp: ts}, Payload: []byte{0x10, 0, 1}}, true) {
				t.Fatal("RTP rejected")
			}
		}
		packet("alta", 500, 1000)
		if sr := next(t, reports, "the first packet"); sr.RTPTime != 1000 || sr.PacketCount != 1 {
			t.Fatalf("report=%+v", sr)
		}
		f.mu.Lock()
		f.want = "baixa"
		f.mu.Unlock()
		packet("baixa", 800, 1008000)
		if sr := next(t, reports, "a layer change"); sr.PacketCount != 2 {
			t.Fatalf("report=%+v", sr)
		}
	})
	t.Run("delay", func(t *testing.T) {
		s := &SFU{sessions: map[string]*session{}}
		pub := &publication{source: SourceScreenAudio, codec: webrtc.RTPCodecCapability{ClockRate: 48000}, owner: &session{id: "publisher"}, clocks: map[string]captureReport{"": {ssrc: 7, ntp: epoch, rtp: 99}}}
		viewer := &session{id: "viewer", userID: 2, room: &room{pubs: map[string]*publication{"p": pub}}}
		s.sessions[viewer.id] = viewer
		reports := make(chan []rtcp.Packet, 64)
		track := &syncTrack{pub: pub, viewer: viewer, send: func(pkts []rtcp.Packet) error { reports <- pkts; return nil }}
		out := &syncOutput{ssrc: 42, wake: make(chan struct{}, 1), stop: make(chan struct{}), done: make(chan struct{})}
		out.packets.Store(3)
		go track.run(out)
		defer func() { close(out.stop); <-out.done }()
		first := next(t, reports, "start")
		if err := s.OutputDelay(viewer.id, viewer.userID, 250); err != nil {
			t.Fatal(err)
		}
		if moved := next(t, reports, "a requested delay"); moved.NTPTime == first.NTPTime {
			t.Fatalf("delay did not move the report: %+v", moved)
		}
	})
}
