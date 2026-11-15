package sfu

import (
	"testing"

	"github.com/pion/webrtc/v4"
)

// Firefox answers VP8/VP9 with receive limits, and omits VP9 profile 0.
// Those declarations must not create another RTP format or another BUNDLE PT.
func TestFirefoxReceiveLimitsKeepPayloadIdentity(t *testing.T) {
	for _, tc := range []struct {
		name string
		base webrtc.RTPCodecParameters
		fmtp string
		same bool
	}{
		{"vp8-limits", vp8Codec, "max-fs=12288;max-fr=60", true},
		{"vp8-other-limits", vp8Codec, "max-fr=30;max-fs=3600", true},
		{"vp8-unknown-format-parameter", vp8Codec, "new-format=1", false},
		{"vp8-normalized-limits", vp8Codec, " MAX-FS = 12288 ; Max-Fr = 60 ", true},
		{"vp9-default-profile", vp9Codec, "max-fs=12288;max-fr=60", true},
		{"vp9-explicit-profile", vp9Codec, "max-fr=60;profile-id=0;max-fs=12288", true},
		{"vp9-different-profile", vp9Codec, "max-fr=60;profile-id=2;max-fs=12288", false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			answer := tc.base
			answer.SDPFmtpLine = tc.fmtp
			if got := codecKey(tc.base) == codecKey(answer); got != tc.same {
				t.Fatalf("same RTP format = %v, want %v", got, tc.same)
			}
			if tc.same {
				used := map[webrtc.PayloadType]string{tc.base.PayloadType: codecKey(tc.base)}
				pt, err := allocatePayload(answer, used)
				if err != nil || pt != tc.base.PayloadType {
					t.Fatalf("Firefox changed PT %d to %d: %v", tc.base.PayloadType, pt, err)
				}
			}
		})
	}
}
