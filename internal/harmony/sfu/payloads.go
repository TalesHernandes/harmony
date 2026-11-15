package sfu

import (
	"fmt"
	"sort"
	"strconv"
	"strings"

	"github.com/pion/sdp/v3"
	"github.com/pion/webrtc/v4"
)

// codecKey compares the wire format, not the encoder preference or feedback.
// Parameter order and MIME case do not create a different format.
func codecKey(c webrtc.RTPCodecParameters) string {
	params := strings.Split(c.SDPFmtpLine, ";")
	asymmetricH264 := false
	profileIndex := -1
	for i := range params {
		parts := strings.SplitN(strings.TrimSpace(params[i]), "=", 2)
		parts[0] = strings.ToLower(strings.TrimSpace(parts[0]))
		if len(parts) == 2 {
			parts[1] = strings.TrimSpace(parts[1])
		}
		params[i] = strings.Join(parts, "=")
		if len(parts) == 2 {
			if parts[0] == "level-asymmetry-allowed" && parts[1] == "1" {
				asymmetricH264 = true
			}
			if parts[0] == "profile-level-id" {
				profileIndex = i
			}
		}
	}
	// H264 level is a decoder/encoder limit, not a different RTP format.
	// Chromium answers our level 4.2 with 3.1. With level asymmetry enabled
	// these belong to the same PT; treating them as a collision changes the
	// SDP of an already bound sender without rebinding its RTP output.
	// Keep profile, constraints and packetization mode distinct. Without
	// explicit level asymmetry, retain the full level in the identity.
	if strings.EqualFold(c.MimeType, webrtc.MimeTypeH264) && asymmetricH264 && profileIndex >= 0 {
		profile := strings.TrimPrefix(params[profileIndex], "profile-level-id=")
		if len(profile) == 6 {
			if _, err := strconv.ParseUint(profile, 16, 24); err == nil {
				params[profileIndex] = "profile-level-id=" + strings.ToLower(profile[:4])
			}
		}
	}
	// VP8/VP9 max-fs and max-fr describe receiver limits, not an RTP format
	// (RFC 7741 section 6.2.2; RFC 9628 section 6.1.2). Firefox adds them in
	// answers. Counting them as a different codec allocates new BUNDLE PTs
	// when a camera joins an already negotiated screen. VP9's omitted
	// profile-id is profile 0; a genuinely different profile stays distinct.
	if strings.EqualFold(c.MimeType, webrtc.MimeTypeVP8) || strings.EqualFold(c.MimeType, webrtc.MimeTypeVP9) {
		filtered := params[:0]
		hasProfile := false
		for _, param := range params {
			key, _, _ := strings.Cut(param, "=")
			if key == "max-fs" || key == "max-fr" || param == "" {
				continue
			}
			hasProfile = hasProfile || key == "profile-id"
			filtered = append(filtered, param)
		}
		if strings.EqualFold(c.MimeType, webrtc.MimeTypeVP9) && !hasProfile {
			filtered = append(filtered, "profile-id=0")
		}
		params = filtered
	}
	sort.Strings(params)
	return fmt.Sprintf("%s/%d/%d;%s", strings.ToLower(c.MimeType), c.ClockRate, c.Channels, strings.Join(params, ";"))
}

type payloadMedia struct {
	mid    string
	codecs []webrtc.RTPCodecParameters
}

func descriptionCodecs(raw string) ([]payloadMedia, error) {
	var desc sdp.SessionDescription
	if err := desc.Unmarshal([]byte(raw)); err != nil {
		return nil, err
	}
	var result []payloadMedia
	for _, md := range desc.MediaDescriptions {
		if md.MediaName.Media != "audio" && md.MediaName.Media != "video" {
			continue
		}
		mid, _ := md.Attribute("mid")
		media := payloadMedia{mid: mid}
		// Parse each m-line separately: the bug itself is two different formats
		// using the same PT in different m-lines.
		one := sdp.SessionDescription{MediaDescriptions: []*sdp.MediaDescription{md}}
		for _, format := range md.MediaName.Formats {
			pt, err := strconv.ParseUint(format, 10, 7)
			if err != nil {
				return nil, err
			}
			c, err := one.GetCodecForPayloadType(uint8(pt))
			if err != nil {
				return nil, err
			}
			channels := uint16(0)
			if c.EncodingParameters != "" {
				n, err := strconv.ParseUint(c.EncodingParameters, 10, 16)
				if err != nil {
					return nil, err
				}
				channels = uint16(n)
			}
			codec := webrtc.RTPCodecParameters{PayloadType: webrtc.PayloadType(pt), RTPCodecCapability: webrtc.RTPCodecCapability{
				MimeType: md.MediaName.Media + "/" + c.Name, ClockRate: c.ClockRate, Channels: channels, SDPFmtpLine: c.Fmtp,
			}}
			for _, feedback := range c.RTCPFeedback {
				parts := strings.SplitN(feedback, " ", 2)
				fb := webrtc.RTCPFeedback{Type: parts[0]}
				if len(parts) == 2 {
					fb.Parameter = parts[1]
				}
				codec.RTCPFeedback = append(codec.RTCPFeedback, fb)
			}
			media.codecs = append(media.codecs, codec)
		}
		result = append(result, media)
	}
	return result, nil
}

// allocatePayload keeps a free default number, reuses an identical negotiated
// format on collision, then finds a free dynamic number. The lower dynamic
// range excludes 64..95, which would be ambiguous with RTCP on a muxed socket.
func allocatePayload(c webrtc.RTPCodecParameters, used map[webrtc.PayloadType]string) (webrtc.PayloadType, error) {
	key := codecKey(c)
	if old, taken := used[c.PayloadType]; !taken || old == key {
		used[c.PayloadType] = key
		return c.PayloadType, nil
	}
	for pt := 0; pt < 128; pt++ {
		if used[webrtc.PayloadType(pt)] == key {
			return webrtc.PayloadType(pt), nil
		}
	}
	for _, bounds := range [][2]int{{96, 127}, {35, 63}} {
		for pt := bounds[0]; pt <= bounds[1]; pt++ {
			if _, taken := used[webrtc.PayloadType(pt)]; !taken {
				used[webrtc.PayloadType(pt)] = key
				return webrtc.PayloadType(pt), nil
			}
		}
	}
	return 0, fmt.Errorf("no free BUNDLE payload type")
}

// alignPayloads runs with negMu held, before every server offer. It applies
// preferences to the actual Pion transceivers, so SDP and sender parameters
// agree; rewriting only the SDP would leave RTP using the old numbers.
func (sess *session) alignPayloads(proposed webrtc.SessionDescription) error {
	media, err := descriptionCodecs(proposed.SDP)
	if err != nil {
		return err
	}
	used := map[webrtc.PayloadType]string{}
	for pt, key := range sess.reservedPayloads {
		used[pt] = key
	}
	if current := sess.pc.CurrentLocalDescription(); current != nil {
		negotiated, err := descriptionCodecs(current.SDP)
		if err != nil {
			return err
		}
		for _, m := range negotiated {
			for _, c := range m.codecs {
				key := codecKey(c)
				if old, ok := used[c.PayloadType]; ok && old != key {
					return fmt.Errorf("negotiated BUNDLE payload collision: %d", c.PayloadType)
				}
				used[c.PayloadType] = key
			}
		}
	}
	sess.reservedPayloads = make(map[webrtc.PayloadType]string, len(used))
	for pt, key := range used {
		sess.reservedPayloads[pt] = key
	}
	// Reserve all non-conflicting defaults before allocating alternatives, so
	// fixing one codec cannot gratuitously renumber another codec later on.
	for _, m := range media {
		for _, c := range m.codecs {
			if _, ok := used[c.PayloadType]; !ok {
				used[c.PayloadType] = codecKey(c)
			}
		}
	}
	transceivers := sess.pc.GetTransceivers()
	for _, m := range media {
		remap := map[webrtc.PayloadType]webrtc.PayloadType{}
		for i, c := range m.codecs {
			if strings.EqualFold(c.MimeType, webrtc.MimeTypeRTX) {
				continue
			}
			pt, err := allocatePayload(c, used)
			if err != nil {
				return err
			}
			remap[c.PayloadType] = pt
			m.codecs[i].PayloadType = pt
		}
		for i, c := range m.codecs {
			if !strings.EqualFold(c.MimeType, webrtc.MimeTypeRTX) {
				continue
			}
			params := strings.Split(c.SDPFmtpLine, ";")
			aptIndex := -1
			var apt uint64
			for j, param := range params {
				pair := strings.SplitN(strings.TrimSpace(param), "=", 2)
				if len(pair) == 2 && strings.EqualFold(strings.TrimSpace(pair[0]), "apt") {
					apt, err = strconv.ParseUint(strings.TrimSpace(pair[1]), 10, 7)
					if err != nil {
						return fmt.Errorf("invalid RTX apt: %w", err)
					}
					aptIndex = j
				}
			}
			if aptIndex < 0 {
				return fmt.Errorf("RTX without apt")
			}
			primary, ok := remap[webrtc.PayloadType(apt)]
			if !ok {
				return fmt.Errorf("RTX without primary payload: %d", apt)
			}
			params[aptIndex] = "apt=" + strconv.Itoa(int(primary))
			c.SDPFmtpLine = strings.Join(params, ";")
			pt, err := allocatePayload(c, used)
			if err != nil {
				return err
			}
			c.PayloadType = pt
			m.codecs[i] = c
		}
		for _, tr := range transceivers {
			if tr.Mid() == m.mid {
				if err := tr.SetCodecPreferences(m.codecs); err != nil {
					return err
				}
				break
			}
		}
	}
	return nil
}
