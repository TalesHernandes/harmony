package sfu

import "github.com/pion/rtcp"

// The receiving phone asks for the screen audio clock to be moved by this much.
// Positive delays the picture (the steady value, up to 500 ms); while the
// picture catches up, the phone may ask for up to twice that, or below zero to
// release a delay it no longer needs.
const (
	MinOutputDelayMs = -500
	MaxOutputDelayMs = 1000
)

// refillReports is how many reports the receiver's RTP-to-NTP estimator keeps
// (kNumRtcpReportsToUse in libwebrtc's rtc_base/rtp_to_ntp_estimator.cc). It
// fits a line through all of them, so a changed delay would otherwise reach the
// synchroniser only as old reports age out, one publisher report (~5 s) at a time.
const refillReports = 20

// OutputDelay changes only this user's receiving session, never a publication.
func (s *SFU) OutputDelay(sessionID string, userID int64, ms int) error {
	sess, err := s.session(sessionID, userID)
	if err != nil {
		return err
	}
	if ms < MinOutputDelayMs || ms > MaxOutputDelayMs {
		return ErrBadSource
	}
	sess.outputDelayMs.Store(int64(ms))
	// Only screen audio is moved by it (compensated), and the room has few
	// publications: telling them all is cheaper than finding this one viewer.
	if r := sess.room; r != nil {
		r.mu.RLock()
		for _, p := range r.pubs {
			if p.source == SourceScreenAudio {
				p.wakeSync()
			}
		}
		r.mu.RUnlock()
	}
	return nil
}

func (t *syncTrack) compensated() bool {
	return t.pub.source == SourceScreenAudio && t.viewer != nil
}

func (t *syncTrack) delayMs() int64 {
	if !t.compensated() {
		return 0
	}
	return t.viewer.outputDelayMs.Load()
}

func (t *syncTrack) outputNTP(ntp uint64) uint64 {
	// NTP is unsigned 32.32 seconds; the arithmetic intentionally wraps eras.
	return ntp - uint64(t.delayMs()<<32/1000)
}

// reportPoints returns n sender-report points (NTP, RTP) on the line the viewer
// must see: the publisher's capture clock moved by the requested delay. Every
// point is strictly newer than the last one sent, in both clocks, because the
// estimator drops a report whose NTP or RTP does not advance. Points a
// millisecond apart all sit exactly on the line, so a burst replaces the
// estimator's window without bending its fit.
func (t *syncTrack) reportPoints(sr captureReport, offset uint32, last *rtcp.SenderReport, n int) [][2]uint64 {
	ntp, rtpTS := t.outputNTP(sr.ntp), sr.rtp+offset
	rate := uint64(t.pub.codec.ClockRate)
	if rate == 0 {
		rate = 48000
	}
	var stepMs uint64
	if last != nil {
		// Only a step the delay range can explain is bridged; a publisher whose
		// clock really jumped is passed through for the estimator to reset on.
		const limit = 5000
		// Rounded up: a truncated millisecond lands exactly on the last report.
		if behind := int64(last.NTPTime - ntp); behind >= 0 && uint64(behind)*1000>>32 < limit {
			stepMs = max(stepMs, (uint64(behind)*1000+1<<32-1)>>32+1)
		}
		if behind := int32(last.RTPTime - rtpTS); behind >= 0 && uint64(behind)*1000/rate < limit {
			stepMs = max(stepMs, uint64(behind)*1000/rate+1)
		}
	}
	points := make([][2]uint64, n)
	for i := range points {
		ms := stepMs + uint64(i)
		points[i] = [2]uint64{ntp + ms<<32/1000, uint64(rtpTS + uint32(ms*rate/1000))}
	}
	return points
}
