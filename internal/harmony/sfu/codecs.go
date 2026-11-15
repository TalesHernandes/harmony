package sfu

import (
	"strconv"

	"github.com/pion/webrtc/v4"
)

// What this media server is willing to carry, and what it refuses to know
// about.
//
// The server never decodes anything, so at first sight the list of codecs
// looks like it should not matter. It matters because the server is the one
// who *offers*: a browser can only send what our offer lists, so this file is
// where "the screen is VP9" and "AV1 is not allowed" (CLAUDE.md) stop being a
// wish and become a property of the binary. Nothing at a call site has to
// remember them, because AV1 is never registered at all — there is no way to
// reach it from anywhere in this program.
//
// Pion's RegisterDefaultCodecs would bring AV1 and H.265 along. This is that
// list minus those two, and minus the three telephone audio codecs (G.722,
// PCMU, PCMA) that no browser would ever pick over Opus and that only widen
// what we would have to be able to carry.

// videoFeedback is the same set Pion offers by default: a bandwidth estimate,
// a request for a full picture, and retransmission of a lost packet. All three
// are messages *about* the stream; none of them opens one.
var videoFeedback = []webrtc.RTCPFeedback{
	{Type: "goog-remb"},
	{Type: "ccm", Parameter: "fir"},
	{Type: "nack"},
	{Type: "nack", Parameter: "pli"},
}

// The payload numbers are Pion's own, kept identical on purpose: they are what
// the D2 voice path negotiated in production, and changing a number nobody
// asked to change is how a working call stops working.
var (
	opusCodec = webrtc.RTPCodecParameters{
		RTPCodecCapability: webrtc.RTPCodecCapability{
			MimeType: webrtc.MimeTypeOpus, ClockRate: 48000, Channels: 2,
			SDPFmtpLine: "minptime=10;useinbandfec=1",
		},
		PayloadType: 111,
	}

	// VP9 profile 0 is the screen codec the owner chose, by quality
	// (CLAUDE.md). Profile 2 is ten bits per colour; it is registered because
	// a browser that offers it should not be refused, but it is never the
	// preference.
	vp9Codec = webrtc.RTPCodecParameters{
		RTPCodecCapability: webrtc.RTPCodecCapability{
			MimeType: webrtc.MimeTypeVP9, ClockRate: 90000,
			SDPFmtpLine: "profile-id=0", RTCPFeedback: videoFeedback,
		},
		PayloadType: 98,
	}
	vp9RTX = rtxFor(98, 99)

	vp9P2Codec = webrtc.RTPCodecParameters{
		RTPCodecCapability: webrtc.RTPCodecCapability{
			MimeType: webrtc.MimeTypeVP9, ClockRate: 90000,
			SDPFmtpLine: "profile-id=2", RTCPFeedback: videoFeedback,
		},
		PayloadType: 100,
	}
	vp9P2RTX = rtxFor(100, 101)

	vp8Codec = webrtc.RTPCodecParameters{
		RTPCodecCapability: webrtc.RTPCodecCapability{
			MimeType: webrtc.MimeTypeVP8, ClockRate: 90000, RTCPFeedback: videoFeedback,
		},
		PayloadType: 96,
	}
	vp8RTX = rtxFor(96, 97)
)

func rtxFor(apt, pt webrtc.PayloadType) webrtc.RTPCodecParameters {
	return webrtc.RTPCodecParameters{
		RTPCodecCapability: webrtc.RTPCodecCapability{
			MimeType: webrtc.MimeTypeRTX, ClockRate: 90000,
			SDPFmtpLine: "apt=" + strconv.Itoa(int(apt)),
		},
		PayloadType: pt,
	}
}

// h264Codecs are the profiles Pion offers by default. They stay because a
// phone's camera very often has hardware for H.264 and for nothing else, and a
// camera that costs no battery is a camera that stays on.
//
// The level is 4.2 — the `2a` on the end of every profile-level-id — and it was
// 3.1 until 19/09/2026, which was Pion's default and was wrong for this
// service. Level 3.1 tops out at 1280x720 and 30 frames; the shared screen here
// is offered at 1920x1080 and up to 60, which is more than twice the picture
// and twice the rate of what was being announced. Every receiver negotiated
// believing in 720p30 and then got handed 1080p60, and it is the telephone —
// whose decoder is hardware and does read the level it agreed to — that showed
// twenty-nine freezes in two minutes while losing no packets at all.
//
// 4.0 covers 1080p30 and 4.2 covers 1080p60, so 4.2 is the one that covers what
// the settings can ask for. `level-asymmetry-allowed=1` is already here and is
// what makes this safe: each end still answers with the level it can really
// manage, and a device that cannot do 4.2 says so instead of being told a
// number nobody was honouring.
func h264Codecs() []webrtc.RTPCodecParameters {
	profiles := []struct {
		fmtp string
		pt   webrtc.PayloadType
		rtx  webrtc.PayloadType
	}{
		{"level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42002a", 102, 103},
		{"level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42002a", 104, 105},
		{"level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e02a", 106, 107},
		{"level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e02a", 108, 109},
		{"level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d002a", 127, 125},
		{"level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=4d002a", 39, 40},
		{"level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=64002a", 112, 113},
	}
	out := make([]webrtc.RTPCodecParameters, 0, len(profiles)*2)
	for _, p := range profiles {
		out = append(out, webrtc.RTPCodecParameters{
			RTPCodecCapability: webrtc.RTPCodecCapability{
				MimeType: webrtc.MimeTypeH264, ClockRate: 90000,
				SDPFmtpLine: p.fmtp, RTCPFeedback: videoFeedback,
			},
			PayloadType: p.pt,
		})
		out = append(out, rtxFor(p.pt, p.rtx))
	}
	return out
}

// registerCodecs is the whole list, and the absence of AV1 from it is the rule
// in CLAUDE.md made structural.
func registerCodecs(m *webrtc.MediaEngine) error {
	if err := m.RegisterCodec(opusCodec, webrtc.RTPCodecTypeAudio); err != nil {
		return err
	}
	video := []webrtc.RTPCodecParameters{
		vp9Codec, vp9RTX, vp9P2Codec, vp9P2RTX, vp8Codec, vp8RTX,
	}
	video = append(video, h264Codecs()...)
	for _, c := range video {
		if err := m.RegisterCodec(c, webrtc.RTPCodecTypeVideo); err != nil {
			return err
		}
	}
	return nil
}

// screenCodecs and cameraCodecs are the same list in two orders, and the order
// is the whole mechanism: the browser answering an offer picks the first codec
// in it that it can do, so what leads a screen's slot is what a shared screen
// becomes.
//
// Why an order and not a list of one. A connection's codecs are settled the
// first time a picture is negotiated on it, and everything negotiated
// afterwards is confined to what was settled then. A slot offered VP9 alone
// would therefore not only be VP9 — it would make every camera switched on
// later that same evening VP9 too, on the same connection, which is exactly
// what a telephone must not be made to do. Narrowing the first slot narrows
// the connection; ordering it does not.
//
// What is genuinely impossible rather than merely unpreferred is AV1, and the
// reason is that it is never registered at all (registerCodecs above). There is
// no order that can bring back something the engine does not know.
//
// Which codec really arrived is not assumed: the media server logs it for every
// track it takes in, and the browser puts it in the diagnostic log, so "the
// screen is VP9" is a thing that can be checked rather than believed.
// A screen leads with H.264 since 19/09/2026, and the reason is the encoder
// rather than the picture. VP9 led here from the start, chosen for quality, and
// the measurement of 18/09 is what changed the owner's mind: no published
// figure shows VP9 winning on *screen* content — the ~33% it saves is measured
// on camera and motion video and does not transfer to still text — while it
// costs about three times the CPU of H.264 in software and nine times the CPU
// of H.264 on a graphics chip. And on most machines it really is software:
// NVIDIA and AMD do not encode VP9 at all, and only Intel does from its 12th
// generation. Discord sends H.264 for the same reason.
//
// VP9 stays in the list, one place down. It is not refused; it is simply no
// longer what a browser reaches for first.
func screenCodecs() []webrtc.RTPCodecParameters {
	out := h264Codecs()
	out = append(out, vp9Codec, vp9RTX, vp9P2Codec, vp9P2RTX)
	return append(out, vp8Codec, vp8RTX)
}

// cameraCodecs leads with VP8, and the reason is battery rather than quality. A
// telephone almost always has a chip that encodes VP8 or H.264 and almost never
// one that encodes VP9, so leading a camera with VP9 would heat the phone up to
// send a picture the size of a thumbnail.
//
// This one did not move when the screen did. The decision of 19/09/2026 was
// about the screen, which is the thing that has to stay readable, and a camera
// was never sending VP9 to begin with — there was nothing here for that
// decision to fix.
func cameraCodecs() []webrtc.RTPCodecParameters {
	out := []webrtc.RTPCodecParameters{vp8Codec, vp8RTX}
	out = append(out, h264Codecs()...)
	return append(out, vp9Codec, vp9RTX, vp9P2Codec, vp9P2RTX)
}
