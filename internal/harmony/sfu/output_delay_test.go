package sfu

import (
	"github.com/pion/rtcp"
	"github.com/pion/webrtc/v4"
	"testing"
	"time"
)

// Exercise the actual RTCP sending loop, including per-viewer isolation,
// reset, wraparound and the publication's unmodified capture clock.
func TestOutputDelaySenderReports(t *testing.T) {
	const ntp = uint64(7) << 28 // Subtracting 500 ms crosses an unsigned era.
	viewer := &session{}
	other := &session{}
	for _, source := range []string{SourceScreenAudio, SourceScreen, SourceMic, SourceCamera} {
		t.Run(source, func(t *testing.T) {
			pub := &publication{source: source, owner: &session{id: "publisher"}, clocks: map[string]captureReport{"": {ssrc: 7, ntp: ntp, rtp: 99}}}
			for _, ms := range []int64{250, 500, 0} {
				viewer.outputDelayMs.Store(ms)
				for _, recipient := range []*session{viewer, other} {
					reports := make(chan *rtcp.SenderReport, 8)
					track := &syncTrack{pub: pub, viewer: recipient, send: func(packets []rtcp.Packet) error {
						reports <- packets[0].(*rtcp.SenderReport)
						return nil
					}}
					out := &syncOutput{ssrc: 42, wake: make(chan struct{}, 1), stop: make(chan struct{}), done: make(chan struct{})}
					out.packets.Store(3)
					out.octets.Store(21)
					go track.run(out)
					var report *rtcp.SenderReport
					select {
					case report = <-reports:
					case <-time.After(time.Second):
					}
					close(out.stop)
					<-out.done
					if report == nil {
						t.Fatal("no sender report")
					}
					want := ntp
					if source == SourceScreenAudio && recipient == viewer {
						want -= uint64(ms) * 4294967296 / 1000
					}
					if report.NTPTime != want || report.RTPTime != 99 || report.SSRC != 42 || report.PacketCount != 3 || report.OctetCount != 21 {
						t.Fatalf("delay=%d, source=%s: report=%+v want NTP=%d", ms, source, report, want)
					}
					if pub.clocks[""].ntp != ntp {
						t.Fatal("publisher clock mutated")
					}
				}
			}
		})
	}
}

// A changed delay refills the receiver's 20-report window at once, every report
// on the new line and newer than the last in both clocks (the estimator drops
// anything else); other sources and other viewers never get a burst.
func TestOutputDelayRefillsEstimatorWindow(t *testing.T) {
	const ntp = uint64(9) << 32
	const rtp = uint32(4_000_000_000) // wraps within the burst arithmetic below
	viewer := &session{}
	for _, source := range []string{SourceScreenAudio, SourceScreen} {
		t.Run(source, func(t *testing.T) {
			viewer.outputDelayMs.Store(0)
			pub := &publication{source: source, codec: webrtc.RTPCodecCapability{ClockRate: 48000}, owner: &session{id: "publisher"}, clocks: map[string]captureReport{"": {ssrc: 7, ntp: ntp, rtp: rtp}}}
			reports := make(chan *rtcp.SenderReport, 256)
			track := &syncTrack{pub: pub, viewer: viewer, send: func(packets []rtcp.Packet) error {
				reports <- packets[0].(*rtcp.SenderReport)
				return nil
			}}
			out := &syncOutput{ssrc: 42, wake: make(chan struct{}, 1), stop: make(chan struct{}), done: make(chan struct{})}
					out.packets.Store(3)
					out.octets.Store(21)
			go track.run(out)
			defer func() { close(out.stop); <-out.done }()
			collect := func() []*rtcp.SenderReport {
				var got []*rtcp.SenderReport
				for {
					select {
					case r := <-reports:
						got = append(got, r)
					case <-time.After(200 * time.Millisecond):
						return got
					}
				}
			}
			var last *rtcp.SenderReport
			for _, delay := range []int64{0, 250, 1000, -500, 120} {
				viewer.outputDelayMs.Store(delay)
				got := collect()
				want := 20
				if delay == 0 {
					want = 1
				}
				if source != SourceScreenAudio {
					want = 0
					if delay == 0 {
						want = 1
					}
				}
				if len(got) != want {
					t.Fatalf("delay %d: %d reports, want %d", delay, len(got), want)
				}
				for _, r := range got {
					if last != nil && (int64(r.NTPTime-last.NTPTime) <= 0 || int32(r.RTPTime-last.RTPTime) <= 0) {
						t.Fatalf("delay %d: report does not advance: %+v after %+v", delay, r, last)
					}
					shift := int64(0)
					if source == SourceScreenAudio {
						shift = delay
					}
					// On the publisher's line moved by the delay: NTP - RTP/48 is constant.
					lineNTP := int64(r.NTPTime-(ntp-uint64(shift<<32/1000))) * 1000 >> 32
					lineRTP := int64(int32(r.RTPTime-rtp)) / 48
					if d := lineNTP - lineRTP; d < -1 || d > 1 {
						t.Fatalf("delay %d: report off the line by %d ms: %+v", delay, d, r)
					}
					last = r
				}
			}
			if pub.clocks[""].ntp != ntp || pub.clocks[""].rtp != rtp {
				t.Fatal("publisher clock mutated")
			}
		})
	}
}
