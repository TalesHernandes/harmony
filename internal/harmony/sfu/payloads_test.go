package sfu

import (
	"fmt"
	"net"
	"reflect"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/pion/webrtc/v4"
)

func assertBundlePayloads(t *testing.T, raw string) []payloadMedia {
	t.Helper()
	media, err := descriptionCodecs(raw)
	if err != nil {
		t.Fatal(err)
	}
	used := map[webrtc.PayloadType]string{}
	for _, m := range media {
		primaries := map[webrtc.PayloadType]bool{}
		repairs := map[webrtc.PayloadType]bool{}
		for _, c := range m.codecs {
			if !strings.EqualFold(c.MimeType, webrtc.MimeTypeRTX) {
				primaries[c.PayloadType] = true
			}
			key := codecKey(c)
			if previous, ok := used[c.PayloadType]; ok && previous != key {
				t.Fatalf("BUNDLE collision PT %d: %s / %s", c.PayloadType, previous, key)
			}
			used[c.PayloadType] = key
		}
		for _, c := range m.codecs {
			if strings.EqualFold(c.MimeType, webrtc.MimeTypeRTX) {
				apt, err := strconv.Atoi(strings.TrimPrefix(c.SDPFmtpLine, "apt="))
				if err != nil || !primaries[webrtc.PayloadType(apt)] {
					t.Fatalf("orphan RTX in mid %s: %s", m.mid, c.SDPFmtpLine)
				}
				repairs[webrtc.PayloadType(apt)] = true
			}
		}
		for _, c := range m.codecs {
			if strings.HasPrefix(c.MimeType, "video/") && !strings.EqualFold(c.MimeType, webrtc.MimeTypeRTX) && !repairs[c.PayloadType] {
				t.Fatalf("missing RTX in mid %s for PT %d", m.mid, c.PayloadType)
			}
		}
	}
	return media
}

func TestBundlePayloadNegotiation(t *testing.T) {
	for _, chromium := range []bool{false, true} {
		for _, cameraFirst := range []bool{false, true} {
			t.Run(fmt.Sprintf("chromium=%v/cameraFirst=%v", chromium, cameraFirst), func(t *testing.T) {
				s := testServer(t, 8)
				events := make(chan Offer, 32)
				s.cfg.Signal = func(_ int64, _, kind string, data any) {
					if kind == "voice_offer" {
						events <- data.(Offer)
					}
				}
				engine := &webrtc.MediaEngine{}
				if err := engine.RegisterCodec(opusCodec, webrtc.RTPCodecTypeAudio); err != nil {
					t.Fatal(err)
				}
				codecs := cameraCodecs()
				if !chromium {
					// Native factory: H264 42e02a first, PT 96; RTX 97.
					codecs = []webrtc.RTPCodecParameters{h264Codecs()[4], rtxFor(96, 97), vp8Codec, rtxFor(100, 101)}
					codecs[0].PayloadType = 96
					codecs[2].PayloadType = 100
					vp9, vp9p2 := vp9Codec, vp9P2Codec
					vp9.PayloadType, vp9p2.PayloadType = 102, 104
					codecs = append(codecs, vp9, rtxFor(102, 103), vp9p2, rtxFor(104, 107))
				}
				for _, codec := range codecs {
					if err := engine.RegisterCodec(codec, webrtc.RTPCodecTypeVideo); err != nil {
						t.Fatal(err)
					}
				}
				se := webrtc.SettingEngine{}
				se.SetIncludeLoopbackCandidate(true)
				se.SetNetworkTypes([]webrtc.NetworkType{webrtc.NetworkTypeUDP4})
				se.SetIPFilter(func(ip net.IP) bool { return ip.IsLoopback() })
				api := webrtc.NewAPI(webrtc.WithMediaEngine(engine), webrtc.WithSettingEngine(se))
				pc, err := api.NewPeerConnection(webrtc.Configuration{})
				if err != nil {
					t.Fatal(err)
				}
				t.Cleanup(func() { pc.Close() })
				c := &client{t: t, api: api, pc: pc, sfu: s, user: 1, seen: map[string]TrackInfo{}, waiting: map[string]*webrtc.TrackLocalStaticSample{}, shared: map[string]*webrtc.TrackLocalStaticRTP{}}
				mic, _ := webrtc.NewTrackLocalStaticSample(opusCodec.RTPCodecCapability, "mic", "sender")
				c.waiting[SourceMic] = mic
				if _, err := pc.AddTrack(mic); err != nil {
					t.Fatal(err)
				}
				join, err := s.Join("payloads", Joiner{UserID: 1, LoginSession: "sender", Username: "sender"})
				if err != nil {
					t.Fatal(err)
				}
				c.id = join.SessionID
				if err := c.answer(join); err != nil {
					t.Fatal(err)
				}
				next := func() Offer {
					select {
					case offer := <-events:
						return offer
					case <-time.After(5 * time.Second):
						t.Fatal("server offer not found")
						return Offer{}
					}
				}
				camera := func(enabled bool, screenMid string) {
					if enabled {
						track, _ := webrtc.NewTrackLocalStaticRTP(vp8Codec.RTPCodecCapability, "cam", "sender")
						c.shared[SourceCamera] = track
						err = s.Publish(c.id, 1, []string{SourceCamera})
					} else {
						delete(c.shared, SourceCamera)
						err = s.Unpublish(c.id, 1, []string{SourceCamera})
					}
					if err != nil {
						t.Fatal(err)
					}
					offer := next()
					media := assertBundlePayloads(t, offer.SDP)
					if screenMid != "" && offer.SendMids[SourceScreen] != screenMid {
						t.Fatal("screen mid changed")
					}
					if enabled {
						if offer.SendMids[SourceCamera] == screenMid {
							t.Fatal("camera borrowed screen mid")
						}
						if chromium {
							for _, m := range media {
								if m.mid == offer.SendMids[SourceCamera] {
									got := map[string]webrtc.PayloadType{}
									want := map[string]webrtc.PayloadType{}
									for _, codec := range m.codecs {
										got[codecKey(codec)] = codec.PayloadType
									}
									for _, codec := range cameraCodecs() {
										want[codecKey(codec)] = codec.PayloadType
									}
									if !reflect.DeepEqual(got, want) {
										t.Fatalf("collision-free defaults changed: got %v want %v", got, want)
									}
								}
							}
						}
					}
					if err := c.answer(offer); err != nil {
						t.Fatal(err)
					}
					assertBundlePayloads(t, pc.LocalDescription().SDP)
				}
				if cameraFirst {
					camera(true, "")
				}
				primary := h264Codecs()[4]
				if !chromium {
					primary.PayloadType = 96
				}
				var screen *webrtc.RTPTransceiver
				for _, rid := range []string{"alta", "media", "baixa"} {
					track, _ := webrtc.NewTrackLocalStaticRTP(primary.RTPCodecCapability, "screen", "sender", webrtc.WithRTPStreamID(rid))
					if screen == nil {
						screen, err = pc.AddTransceiverFromTrack(track, webrtc.RTPTransceiverInit{Direction: webrtc.RTPTransceiverDirectionSendonly})
						if err != nil {
							t.Fatal(err)
						}
						// Advertise all profiles, as Chromium/native do, with H264 first.
						prefs := screenCodecs()
						if !chromium && !cameraFirst {
							prefs = codecs
						}
						if !chromium && cameraFirst {
							prefs = []webrtc.RTPCodecParameters{h264Codecs()[4], rtxFor(106, 107), vp8Codec, vp8RTX}
						}
						if err := screen.SetCodecPreferences(prefs); err != nil {
							t.Fatal(err)
						}
					} else if err := screen.Sender().AddEncoding(track); err != nil {
						t.Fatal(err)
					}
				}
				answer, err := c.offerOnce([]string{SourceScreen})
				if err != nil {
					t.Fatal(err)
				}
				assertBundlePayloads(t, answer)
				screenMid := screen.Mid()
				if !chromium && !cameraFirst && !strings.Contains(answer, "a=rtpmap:96 H264/90000") {
					t.Fatal("native H264/96 scenario missing")
				}
				if !strings.Contains(answer, "a=simulcast:recv alta;media;baixa") {
					t.Fatal("simulcast RIDs missing")
				}
				if cameraFirst {
					camera(false, screenMid)
				}
				camera(true, screenMid)
				for i := 0; i < 4; i++ {
					camera(false, screenMid)
					camera(true, screenMid)
				}
				if screen.Mid() != screenMid || len(screen.Sender().GetParameters().Encodings) != 3 {
					t.Fatal("screen identity or layers changed")
				}
			})
		}
	}
}

func TestPayloadAllocatorFormats(t *testing.T) {
	used := map[webrtc.PayloadType]string{96: codecKey(h264Codecs()[4])}
	codec := h264Codecs()[12] // A different H264 profile is a different wire format.
	codec.PayloadType = 96
	pt, err := allocatePayload(codec, used)
	if err != nil || pt == 96 {
		t.Fatalf("H264 profiles collided: %d %v", pt, err)
	}
	codec.PayloadType = 96
	if repeated, err := allocatePayload(codec, used); err != nil || repeated != pt {
		t.Fatal("identical format was not reused")
	}
	ordered := codec
	ordered.SDPFmtpLine = "profile-level-id=64002a; packetization-mode = 1; level-asymmetry-allowed=1"
	if codecKey(ordered) != codecKey(codec) {
		t.Fatal("fmtp parameter order changed identity")
	}
	for _, bounds := range [][2]int{{96, 127}, {35, 63}} {
		for i := bounds[0]; i <= bounds[1]; i++ {
			used[webrtc.PayloadType(i)] = "reserved"
		}
	}
	if _, err := allocatePayload(vp8Codec, used); err == nil {
		t.Fatal("payload exhaustion silently reused a number")
	}
}
