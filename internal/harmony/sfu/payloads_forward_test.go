package sfu

import (
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"
)

// Unlike TestBundlePayloadNegotiation, the peers use an independent codec
// table (Pion defaults, including H264 level 3.1), and receive actual RTP.
func TestChromiumCameraThenScreenPayloads(t *testing.T) {
	s := testServer(t, 8)
	var mu sync.Mutex
	clients := map[string]*client{}
	errors := make(chan error, 64)
	s.cfg.Signal = func(_ int64, _ string, kind string, data any) {
		if kind != "voice_offer" {
			return
		}
		o := data.(Offer)
		mu.Lock()
		c := clients[o.SessionID]
		mu.Unlock()
		if c == nil {
			return
		}
		media, err := descriptionCodecs(o.SDP)
		if err != nil {
			errors <- err
			return
		}
		// Literal production numbers, independent of cameraCodecs/codecKey.
		for _, m := range media {
			for _, codec := range m.codecs {
				want := chromiumPayload(codec)
				if want < 0 || int(codec.PayloadType) != want {
					errors <- fmt.Errorf("mid %s %s changed from %d to %d: %s", m.mid, codec.MimeType, want, codec.PayloadType, codec.SDPFmtpLine)
				}
			}
		}
		if err := c.answer(o); err != nil {
			errors <- err
		}
	}
	pub := newClient(t, s, "chromium", 1, "publisher", func(m *webrtc.MediaEngine) {
		for _, uri := range []string{"urn:ietf:params:rtp-hdrext:sdes:mid", "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"} {
			if err := m.RegisterHeaderExtension(webrtc.RTPHeaderExtensionCapability{URI: uri}, webrtc.RTPCodecTypeVideo); err != nil {
				t.Fatal(err)
			}
		}
	})
	mu.Lock()
	clients[pub.id] = pub
	mu.Unlock()
	viewer := newClient(t, s, "chromium", 2, "viewer")
	// Chromium answers H264 with its supported level, not an exact echo of
	// the server offer. The level does not change the payload's wire format.
	lowerLevel := func(raw string) string {
		for _, profile := range []string{"4200", "42e0", "4d00", "6400"} {
			raw = strings.ReplaceAll(raw, "profile-level-id="+profile+"2a", "profile-level-id="+profile+"1f")
		}
		return raw
	}
	pub.neg.Lock()
	pub.answerTransform = lowerLevel
	pub.neg.Unlock()
	viewer.neg.Lock()
	viewer.answerTransform = lowerLevel
	viewer.neg.Unlock()
	mu.Lock()
	clients[viewer.id] = viewer
	mu.Unlock()
	t.Cleanup(func() { mu.Lock(); clear(clients); mu.Unlock() })
	type received struct {
		id      string
		mid     string
		pt      uint8
		codec   webrtc.RTPCodecParameters
		payload byte
	}
	packets := make(chan received, 4096)
	viewer.pc.OnTrack(func(track *webrtc.TrackRemote, receiver *webrtc.RTPReceiver) {
		mid := ""
		for _, tr := range viewer.pc.GetTransceivers() {
			if tr.Receiver() == receiver {
				mid = tr.Mid()
			}
		}
		for {
			p, _, err := track.ReadRTP()
			if err != nil {
				return
			}
			if len(p.Payload) != 0 {
				select {
				case packets <- received{track.ID(), mid, p.PayloadType, track.Codec(), p.Payload[len(p.Payload)-1]}:
				default:
				}
			}
		}
	})
	stop := make(chan struct{})
	var wg sync.WaitGroup
	defer func() { close(stop); wg.Wait() }()
	cam := pub.share(SourceCamera, webrtc.MimeTypeVP8, "camera")
	feed(cam, stop)
	waitFor(t, "camera subscription", func() bool { return viewer.sources()[SourceCamera].Source == SourceCamera })
	// Chromium advertises its own H264 level when it originates the screen
	// offer, rather than echoing the server's level from codecs.go.
	cap := webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeH264, ClockRate: 90000, SDPFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f"}
	var screen *webrtc.RTPTransceiver
	var layers []*webrtc.TrackLocalStaticRTP
	pub.neg.Lock()
	for _, rid := range []string{"alta", "media", "baixa"} {
		track, err := webrtc.NewTrackLocalStaticRTP(cap, "screen", "screen", webrtc.WithRTPStreamID(rid))
		if err != nil {
			t.Fatal(err)
		}
		layers = append(layers, track)
		if screen == nil {
			screen, err = pub.pc.AddTransceiverFromTrack(track, webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionSendonly})
			if err != nil {
				t.Fatal(err)
			}
			// Independent Chromium-like offer: H264 first, original PT 102.
			if err := screen.SetCodecPreferences([]webrtc.RTPCodecParameters{{RTPCodecCapability: cap, PayloadType: 102}, {RTPCodecCapability: webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeRTX, ClockRate: 90000, SDPFmtpLine: "apt=102"}, PayloadType: 103}}); err != nil {
				t.Fatal(err)
			}
		} else if err := screen.Sender().AddEncoding(track); err != nil {
			t.Fatal(err)
		}
	}
	pub.neg.Unlock()
	if _, err := pub.offerOnce([]string{SourceScreen}); err != nil {
		t.Fatal(err)
	}
	for i, track := range layers {
		wg.Add(1)
		go func(i int, track *webrtc.TrackLocalStaticRTP) {
			defer wg.Done()
			tick := time.NewTicker(20 * time.Millisecond)
			defer tick.Stop()
			var seq uint16
			for {
				select {
				case <-stop:
					return
				case <-tick.C:
					seq++
					packet := &rtp.Packet{Header: rtp.Header{Version: 2, PayloadType: 102, SequenceNumber: seq, Timestamp: uint32(seq) * 1800, Marker: true}, Payload: []byte{0x65, byte(i + 1)}}
					for _, ext := range screen.Sender().GetParameters().HeaderExtensions {
						switch ext.URI {
						case "urn:ietf:params:rtp-hdrext:sdes:mid":
							_ = packet.SetExtension(uint8(ext.ID), []byte(screen.Mid()))
						case "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id":
							_ = packet.SetExtension(uint8(ext.ID), []byte(track.RID()))
						}
					}
					_ = track.WriteRTP(packet)
				}
			}
		}(i, track)
	}
	viewer.watch(t, "publisher")
	waitFor(t, "screen subscription", func() bool { return viewer.sources()[SourceScreen].Source == SourceScreen })
	for _, rid := range []string{"alta", "media", "alta"} {
		// Discard earlier packets: each assertion observes the requested stage.
		for len(packets) > 0 {
			<-packets
		}
		if err := s.Layer(viewer.id, viewer.user, "publisher", rid); err != nil {
			t.Fatal(err)
		}
		wanted := byte(1)
		if rid == "media" {
			wanted = 2
		}
		seenCam, seenScreen := false, false
		deadline := time.After(5 * time.Second)
		for !seenCam || !seenScreen {
			select {
			case err := <-errors:
				t.Error(err)
			case p := <-packets:
				media, err := descriptionCodecs(viewer.pc.CurrentRemoteDescription().SDP)
				if err != nil {
					t.Fatal(err)
				}
				matched := false
				for _, m := range media {
					if m.mid != p.mid {
						continue
					}
					for _, codec := range m.codecs {
						if uint8(codec.PayloadType) == p.pt && codec.MimeType == p.codec.MimeType {
							matched = true
						}
					}
				}
				if !matched {
					t.Fatalf("mid %s %s RTP PT %d is absent from its negotiated offer", p.mid, p.id, p.pt)
				}
				if p.pt != uint8(p.codec.PayloadType) {
					t.Fatalf("RTP PT %d differs from negotiated %d", p.pt, p.codec.PayloadType)
				}
				switch p.id {
				case "camera":
					if p.pt != 96 {
						t.Fatalf("camera RTP PT %d", p.pt)
					}
					seenCam = true
				case "screen":
					if p.pt != 102 {
						t.Fatalf("screen RTP PT %d", p.pt)
					}
					seenScreen = p.payload == wanted
				}
			case <-deadline:
				t.Fatalf("layer %s: camera=%v screen=%v", rid, seenCam, seenScreen)
			}
		}
	}
	for len(errors) > 0 {
		t.Error(<-errors)
	}
}

func chromiumPayload(c webrtc.RTPCodecParameters) int {
	switch c.MimeType {
	case webrtc.MimeTypeOpus:
		return 111
	case webrtc.MimeTypeVP8:
		return 96
	case webrtc.MimeTypeVP9:
		if c.SDPFmtpLine == "profile-id=0" {
			return 98
		}
		if c.SDPFmtpLine == "profile-id=2" {
			return 100
		}
	case webrtc.MimeTypeH264:
		for _, p := range []struct {
			profile string
			mode    string
			pt      int
		}{
			{"4200", "1", 102}, {"4200", "0", 104}, {"42e0", "1", 106}, {"42e0", "0", 108},
			{"4d00", "1", 127}, {"4d00", "0", 39}, {"6400", "1", 112},
		} {
			if strings.Contains(c.SDPFmtpLine, "profile-level-id="+p.profile) && strings.Contains(c.SDPFmtpLine, "packetization-mode="+p.mode) {
				return p.pt
			}
		}
	case webrtc.MimeTypeRTX:
		if pt, ok := map[string]int{"apt=96": 97, "apt=98": 99, "apt=100": 101, "apt=102": 103, "apt=104": 105, "apt=106": 107, "apt=108": 109, "apt=127": 125, "apt=39": 40, "apt=112": 113}[c.SDPFmtpLine]; ok {
			return pt
		}
	}
	return -1
}

func TestH264PayloadIdentity(t *testing.T) {
	base := webrtc.RTPCodecParameters{RTPCodecCapability: webrtc.RTPCodecCapability{
		MimeType: webrtc.MimeTypeH264, ClockRate: 90000,
		SDPFmtpLine: "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e02a",
	}, PayloadType: 106}
	for _, tc := range []struct {
		name, fmtp string
		same       bool
	}{
		{"chromium-level", "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1", true},
		{"native-level", "profile-level-id=42e033;packetization-mode=1;level-asymmetry-allowed=1", true},
		{"different-profile", "profile-level-id=64002a;packetization-mode=1;level-asymmetry-allowed=1", false},
		{"different-constraints", "profile-level-id=42002a;packetization-mode=1;level-asymmetry-allowed=1", false},
		{"different-packetization", "profile-level-id=42e02a;packetization-mode=0;level-asymmetry-allowed=1", false},
		{"no-asymmetry", "profile-level-id=42e01f;packetization-mode=1", false},
		{"disabled-asymmetry", "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=0", false},
		{"invalid-profile", "profile-level-id=42e0zz;packetization-mode=1;level-asymmetry-allowed=1", false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			other := base
			other.SDPFmtpLine = tc.fmtp
			if same := codecKey(base) == codecKey(other); same != tc.same {
				t.Fatalf("same payload identity = %v, want %v", same, tc.same)
			}
		})
	}
}
