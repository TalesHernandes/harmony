package sfu

import (
	"bytes"
	"encoding/json"
	"log/slog"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/pion/rtcp"
	"github.com/pion/webrtc/v4"
)

func feedbackSender(t *testing.T, pc *webrtc.PeerConnection, id string) *webrtc.RTPSender {
	t.Helper()
	track, err := webrtc.NewTrackLocalStaticRTP(webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeH264}, id, "test")
	if err != nil {
		t.Fatal(err)
	}
	sender, err := pc.AddTrack(track)
	if err != nil {
		t.Fatal(err)
	}
	return sender
}

func TestReceiverReportUsesOnlyOutgoingMediaEncodings(t *testing.T) {
	params := webrtc.RTPSendParameters{Encodings: []webrtc.RTPEncodingParameters{
		{RTPCodingParameters: webrtc.RTPCodingParameters{SSRC: 100, RTX: webrtc.RTPRtxParameters{SSRC: 101}}},
		{RTPCodingParameters: webrtc.RTPCodingParameters{SSRC: 200, RTX: webrtc.RTPRtxParameters{SSRC: 201}}},
	}}
	for _, ssrc := range []uint32{0, 101, 201, 999} {
		t.Run(strconv.FormatUint(uint64(ssrc), 10), func(t *testing.T) {
			f := layered(t, "alta")
			f.pub.kind = "video"
			dispatchFeedback(t, f.pub, params, f, &rtcp.ReceiverReport{Reports: []rtcp.ReceptionReport{{SSRC: ssrc, FractionLost: 255}}})
			if f.watching() != "alta" || f.clean != 0 || f.moved || len(f.failures) != 0 {
				t.Fatalf("non-media SSRC %d changed the ladder", ssrc)
			}
		})
	}
	for _, ssrc := range []uint32{100, 200} {
		f := layered(t, "alta")
		f.pub.kind = "video"
		dispatchFeedback(t, f.pub, params, f, &rtcp.ReceiverReport{Reports: []rtcp.ReceptionReport{{SSRC: ssrc, FractionLost: 51}}})
		if f.watching() != "media" {
			t.Fatalf("outgoing media SSRC %d did not lower the layer", ssrc)
		}
	}
	// Neither unbound parameters nor unrelated clean reports may build up
	// the clean streak that promotes a subscriber.
	f := layered(t, "media")
	f.pub.kind = "video"
	for range cleanBeforeUp {
		dispatchFeedback(t, f.pub, params, f, &rtcp.ReceiverReport{Reports: []rtcp.ReceptionReport{{SSRC: 999}}})
		dispatchFeedback(t, f.pub, webrtc.RTPSendParameters{}, f, &rtcp.ReceiverReport{Reports: []rtcp.ReceptionReport{{SSRC: 100, FractionLost: 255}}})
	}
	if f.clean != 0 || f.watching() != "media" {
		t.Fatal("unrelated feedback changed the clean streak or layer")
	}
}

func TestKeyframeFeedbackIsScopedToOutgoingMedia(t *testing.T) {
	params := webrtc.RTPSendParameters{Encodings: []webrtc.RTPEncodingParameters{
		{RTPCodingParameters: webrtc.RTPCodingParameters{SSRC: 100, RTX: webrtc.RTPRtxParameters{SSRC: 101}}},
		{RTPCodingParameters: webrtc.RTPCodingParameters{SSRC: 200}},
	}}
	tests := []struct {
		name   string
		packet rtcp.Packet
		want   bool
	}{
		{"media PLI", &rtcp.PictureLossIndication{MediaSSRC: 100}, true},
		{"second encoding PLI", &rtcp.PictureLossIndication{MediaSSRC: 200}, true},
		{"other PLI", &rtcp.PictureLossIndication{SenderSSRC: 100, MediaSSRC: 999}, false},
		{"RTX PLI", &rtcp.PictureLossIndication{MediaSSRC: 101}, false},
		{"zero PLI", &rtcp.PictureLossIndication{}, false},
		{"media FIR", &rtcp.FullIntraRequest{FIR: []rtcp.FIREntry{{SSRC: 100}}}, true},
		{"mixed FIR", &rtcp.FullIntraRequest{FIR: []rtcp.FIREntry{{SSRC: 999}, {SSRC: 200}, {SSRC: 100}}}, true},
		{"other FIR", &rtcp.FullIntraRequest{MediaSSRC: 100, FIR: []rtcp.FIREntry{{SSRC: 999}}}, false},
		{"RTX FIR", &rtcp.FullIntraRequest{FIR: []rtcp.FIREntry{{SSRC: 101}}}, false},
		{"zero FIR", &rtcp.FullIntraRequest{MediaSSRC: 100, FIR: []rtcp.FIREntry{{SSRC: 0}}}, false},
		{"NACK handled by interceptor", &rtcp.TransportLayerNack{MediaSSRC: 100, Nacks: []rtcp.NackPair{{PacketID: 1}}}, false},
	}
	for _, simul := range []bool{false, true} {
		for _, tt := range tests {
			t.Run(tt.name+map[bool]string{false: "/camera", true: "/screen"}[simul], func(t *testing.T) {
				f := layered(t, "alta")
				p := f.pub
				p.kind = "video"
				requests := 0
				p.askAgain = func() { requests++ }
				var out *forwarder
				if simul {
					out = f
					l := &layer{rid: "alta", ssrc: 500}
					l.live.Store(true)
					p.layers["alta"] = l
				}
				dispatchFeedback(t, p, params, out, tt.packet)
				if simul && len(p.kfPer) != 0 {
					requests++
				}
				if (requests == 1) != tt.want || requests > 1 {
					t.Fatalf("got %d keyframe requests, want request=%v", requests, tt.want)
				}
			})
		}
	}
	// Audio is drained, without asking for pictures or moving a video ladder.
	f := layered(t, "alta")
	f.pub.kind = "audio"
	f.pub.askAgain = func() { t.Fatal("audio requested a keyframe") }
	dispatchFeedback(t, f.pub, params, f,
		&rtcp.PictureLossIndication{MediaSSRC: 100},
		&rtcp.FullIntraRequest{FIR: []rtcp.FIREntry{{SSRC: 100}}},
		&rtcp.ReceiverReport{Reports: []rtcp.ReceptionReport{{SSRC: 100, FractionLost: 255}}})
	if f.watching() != "alta" {
		t.Fatal("audio feedback moved the video ladder")
	}
}

func TestLayerDecisionLogHasReasonAndNoAccountData(t *testing.T) {
	f := layered(t, "alta")
	f.pub.source = SourceScreen
	var buf bytes.Buffer
	f.pub.owner.room.sfu.log = slog.New(slog.NewJSONHandler(&buf, nil))
	f.report(0.20)
	f.lastMove = time.Now().Add(-holdAfterMove - time.Second)
	f.failedAt["alta"] = f.lastMove
	for range cleanBeforeUp {
		f.report(0)
	}
	lines := strings.Split(strings.TrimSpace(buf.String()), "\n")
	if len(lines) != 2 {
		t.Fatalf("expected down and up decisions, got %s", buf.String())
	}
	for i, line := range lines {
		var entry map[string]any
		if err := json.Unmarshal([]byte(line), &entry); err != nil {
			t.Fatal(err)
		}
		wantLayer, wantReason, wantFraction := "media", "loss", 0.20
		if i == 1 {
			wantLayer, wantReason, wantFraction = "alta", "clean", 0
		}
		if entry["session"] != f.sess.id || entry["source"] != SourceScreen || entry["layer"] != wantLayer || entry["reason"] != wantReason || entry["fraction"] != wantFraction {
			t.Fatalf("incomplete decision: %s", line)
		}
		if strings.Contains(line, f.pub.owner.username) || entry["dono"] != nil || entry["username"] != nil || entry["email"] != nil {
			t.Fatalf("account data in layer log: %s", line)
		}
	}
}

func dispatchFeedback(t *testing.T, p *publication, params webrtc.RTPSendParameters, f *forwarder, packets ...rtcp.Packet) {
	t.Helper()
	raw, err := rtcp.Marshal(packets)
	if err != nil {
		t.Fatal(err)
	}
	parsed, err := rtcp.Unmarshal(raw)
	if err != nil {
		t.Fatal(err)
	}
	p.handleSubscriberRTCP(params, f, parsed)
}

func TestScreenReceiverReportIgnoresCameraLoss(t *testing.T) {
	f := layered(t, "alta")
	f.pub.kind, f.pub.source = "video", SourceScreen
	for _, rid := range layerOrder {
		l := &layer{rid: rid}
		l.live.Store(true)
		l.lastAt.Store(time.Now().UnixNano())
		f.pub.layers[rid] = l
	}
	screen := feedbackSender(t, f.sess.pc, "screen")
	camera := feedbackSender(t, f.sess.pc, "camera")
	params := screen.GetParameters()
	screenSSRC := uint32(params.Encodings[0].SSRC)
	cameraSSRC := uint32(camera.GetParameters().Encodings[0].SSRC)
	if screenSSRC == 0 || screenSSRC == cameraSSRC {
		t.Fatal("senders need distinct nonzero media SSRCs")
	}
	// FractionLost is quantized in units of 1/256: 51 is approximately 20%.
	dispatchFeedback(t, f.pub, params, f, &rtcp.ReceiverReport{Reports: []rtcp.ReceptionReport{
		{SSRC: screenSSRC, FractionLost: 0},
		{SSRC: cameraSSRC, FractionLost: 51},
	}})
	if got := f.watching(); got != "alta" {
		t.Fatalf("camera loss lowered the clean screen to %q", got)
	}
	dispatchFeedback(t, f.pub, params, f, &rtcp.ReceiverReport{Reports: []rtcp.ReceptionReport{
		{SSRC: cameraSSRC, FractionLost: 0},
		{SSRC: screenSSRC, FractionLost: 51},
	}})
	if got := f.watching(); got != "media" {
		t.Fatalf("screen loss should lower its own layer, got %q", got)
	}
}
