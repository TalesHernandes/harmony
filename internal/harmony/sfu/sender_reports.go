package sfu

import (
	"os"
	"sync"
	"sync/atomic"
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/interceptor/pkg/report"
	"github.com/pion/rtcp"
	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"
)

// Keep receiver reports/NACK/TWCC, but never invent a capture clock from the
// time a forwarded packet crossed this server. Only publishers own that clock.
func configureReceiverReports(reg *interceptor.Registry) error {
	receiver, err := report.NewReceiverInterceptor()
	if err == nil {
		reg.Add(receiver)
	}
	return err
}

type captureReport struct {
	ssrc uint32
	ntp  uint64
	rtp  uint32
}

func (p *publication) readSenderReports(recv *webrtc.RTPReceiver, rid string, ssrc uint32) {
	for {
		var packets []rtcp.Packet
		var err error
		if rid == "" {
			packets, _, err = recv.ReadRTCP()
		} else {
			packets, _, err = recv.ReadSimulcastRTCP(rid)
		}
		if err != nil {
			return
		}
		for _, packet := range packets {
			if sr, ok := packet.(*rtcp.SenderReport); ok && sr.SSRC == ssrc {
				p.saveSenderReport(rid, captureReport{sr.SSRC, sr.NTPTime, sr.RTPTime})
			}
		}
	}
}

func (p *publication) saveSenderReport(rid string, sr captureReport) {
	p.clockMu.Lock()
	// The old receiver may finish reading after this RID has been replaced.
	// Keep clockMu -> layMu order; no caller holds layMu while acquiring clockMu.
	if rid != "" {
		p.layMu.Lock()
		active := p.layers[rid]
		valid := active != nil && active.ssrc == sr.ssrc
		p.layMu.Unlock()
		if !valid {
			p.clockMu.Unlock()
			return
		}
	}
	if p.clocks == nil {
		p.clocks = map[string]captureReport{}
	}
	old, exists := p.clocks[rid]
	// A reordered report must not wind the reference clock backwards.
	accepted := !exists || old.ssrc != sr.ssrc || int64(sr.ntp-old.ntp) > 0
	if accepted {
		p.clocks[rid] = sr
	}
	p.clockMu.Unlock()
	if accepted {
		p.wakeSync()
		p.logSenderReport("published", rid, sr, 0, sr.rtp)
	}
}

// wakeSync tells every subscription of this publication that something its
// sender report is computed from has moved: a new publisher report, a layer
// change, a requested delay. Never blocks; a wake already pending is enough,
// because the check that follows reads the newest state.
func (p *publication) wakeSync() {
	p.syncMu.Lock()
	for wake := range p.syncWakes {
		select {
		case wake <- struct{}{}:
		default:
		}
	}
	p.syncMu.Unlock()
}

func (p *publication) watchSync(wake chan struct{}, on bool) {
	p.syncMu.Lock()
	if on {
		if p.syncWakes == nil {
			p.syncWakes = map[chan struct{}]struct{}{}
		}
		p.syncWakes[wake] = struct{}{}
	} else {
		delete(p.syncWakes, wake)
	}
	p.syncMu.Unlock()
}

// Opt-in clock metadata only: never media contents, account names or credentials.
func (p *publication) logSenderReport(side, rid string, sr captureReport, outSSRC, outRTP uint32) {
	if os.Getenv("HARMONY_SFU_SYNC_DIAG") != "1" || p.owner == nil || p.owner.room == nil || p.owner.room.sfu == nil {
		return
	}
	if p.source != SourceScreen && p.source != SourceScreenAudio {
		return
	}
	p.owner.room.sfu.log.Info("voice sync", "side", side, "source", p.source, "rid", rid,
		"in_ssrc", sr.ssrc, "out_ssrc", outSSRC, "ntp", sr.ntp, "in_rtp", sr.rtp, "out_rtp", outRTP)
}

// syncTrack is per subscription, including ordinary audio tracks. The binding
// supplies the viewer's SSRC; publisher counts and receiver-report blocks are
// never copied into that independent transport.
type syncTrack struct {
	webrtc.TrackLocal
	pub    *publication
	f      *forwarder
	viewer *session
	send   func([]rtcp.Packet) error
	mu     sync.Mutex
	output *syncOutput
}

type syncContext struct {
	webrtc.TrackLocalContext
	writer webrtc.TrackLocalWriter
}

func (c syncContext) WriteStream() webrtc.TrackLocalWriter { return c.writer }

type syncOutput struct {
	writer webrtc.TrackLocalWriter
	ssrc   uint32
	// Counted on every packet of every subscription, so without a lock. A
	// report may pair a packet count with an octet count one packet apart;
	// receivers use neither for anything but statistics.
	packets, octets atomic.Uint32
	wake            chan struct{}
	stop            chan struct{}
	done            chan struct{}
}

func (o *syncOutput) WriteRTP(h *rtp.Header, payload []byte) (int, error) {
	n, err := o.writer.WriteRTP(h, payload)
	if err == nil {
		o.octets.Add(uint32(len(payload)))
		if o.packets.Add(1) == 1 {
			// No report goes out before the first packet does.
			select {
			case o.wake <- struct{}{}:
			default:
			}
		}
	}
	return n, err
}
func (o *syncOutput) Write(raw []byte) (int, error) {
	var p rtp.Packet
	if err := p.Unmarshal(raw); err != nil {
		return 0, err
	}
	return o.WriteRTP(&p.Header, p.Payload)
}

func (t *syncTrack) Bind(ctx webrtc.TrackLocalContext) (webrtc.RTPCodecParameters, error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	o := &syncOutput{writer: ctx.WriteStream(), ssrc: uint32(ctx.SSRC()),
		wake: make(chan struct{}, 1), stop: make(chan struct{}), done: make(chan struct{})}
	codec, err := t.TrackLocal.Bind(syncContext{ctx, o})
	if err != nil {
		return codec, err
	}
	if t.output != nil {
		close(t.output.stop)
		<-t.output.done
	}
	t.output = o
	go t.run(o)
	return codec, nil
}
func (t *syncTrack) Unbind(ctx webrtc.TrackLocalContext) error {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.output != nil {
		close(t.output.stop)
		<-t.output.done
		t.output = nil
	}
	return t.TrackLocal.Unbind(ctx)
}

// syncFallback is how often a subscription looks without having been told to.
// A variable so a test can take the net away and prove the wakes alone work.
var syncFallback = 250 * time.Millisecond

func (t *syncTrack) run(o *syncOutput) {
	defer close(o.done)
	// A report is sent only on a new publisher report or a changed translation,
	// and each of those says so (wakeSync, the first packet): a late viewer and
	// a layer switch use the cached report immediately, without waiting for
	// another multi-second publisher reporting interval.
	//
	// This used to be a 20 ms poll per subscription. Eight people in a room are
	// some seventy subscriptions, so the server woke 3500 times a second to
	// find nothing new. The slow tick that remains is a net under the wakes,
	// not the mechanism: a change nobody announced costs a quarter of a second
	// instead of going unreported.
	if t.pub != nil {
		t.pub.watchSync(o.wake, true)
		defer t.pub.watchSync(o.wake, false)
	}
	tick := time.NewTicker(syncFallback)
	defer tick.Stop()
	select { // the cached report, if there is one, goes out now
	case o.wake <- struct{}{}:
	default:
	}
	var previous captureReport
	var previousOffset uint32
	var previousDelay int64
	var last *rtcp.SenderReport
	var sent bool
	for {
		select {
		case <-o.stop:
			return
		case <-o.wake:
		case <-tick.C:
		}
		{
			if t.f != nil {
				t.f.wmu.Lock()
			}
			sr, offset, ok := t.mapping()
			packets, octets := o.packets.Load(), o.octets.Load()
			delay := t.delayMs()
			moved := sent && delay != previousDelay
			if ok && packets != 0 && (!sent || previous != sr || previousOffset != offset || moved) {
				points := [][2]uint64{{t.outputNTP(sr.ntp), uint64(sr.rtp + offset)}}
				if t.compensated() {
					n := 1
					if moved {
						n = refillReports
					}
					points = t.reportPoints(sr, offset, last, n)
				}
				var err error
				for _, point := range points {
					report := &rtcp.SenderReport{SSRC: o.ssrc, NTPTime: point[0], RTPTime: uint32(point[1]), PacketCount: packets, OctetCount: octets}
					err = t.send([]rtcp.Packet{
						report,
						// One publisher session is one synchronization context, shared by its
						// screen audio/video but never by another person with the same stream ID.
						&rtcp.SourceDescription{Chunks: []rtcp.SourceDescriptionChunk{{Source: o.ssrc, Items: []rtcp.SourceDescriptionItem{{Type: rtcp.SDESCNAME, Text: t.pub.owner.id}}}}},
					})
					if err != nil {
						break
					}
					last = report
				}
				if err == nil {
					previous, previousOffset, previousDelay, sent = sr, offset, delay, true
					rid := ""
					if t.f != nil {
						t.f.mu.Lock()
						rid = t.f.current
						t.f.mu.Unlock()
					}
					logged := sr
					logged.ntp = last.NTPTime
					t.pub.logSenderReport("forwarded", rid, logged, o.ssrc, last.RTPTime)
				}
			}
			if t.f != nil {
				t.f.wmu.Unlock()
			}
		}
	}
}

func (t *syncTrack) mapping() (captureReport, uint32, bool) {
	// With a forwarder, the caller holds wmu (including while sending the SR).
	rid, offset, inputSSRC := "", uint32(0), uint32(0)
	if t.f != nil {
		t.f.mu.Lock()
		if !t.f.started || !t.f.bound {
			t.f.mu.Unlock()
			return captureReport{}, 0, false
		}
		rid, offset, inputSSRC = t.f.current, t.f.outTS-t.f.inTS, t.f.inputSSRC
		t.f.mu.Unlock()
	}
	t.pub.clockMu.Lock()
	sr, ok := t.pub.clocks[rid]
	t.pub.clockMu.Unlock()
	if t.f != nil {
		t.pub.layMu.Lock()
		layer := t.pub.layers[rid]
		ok = ok && layer != nil && layer.ssrc == sr.ssrc && (inputSSRC == 0 || inputSSRC == sr.ssrc)
		t.pub.layMu.Unlock()
	}
	return sr, offset, ok
}

// captureTimestamp maps another layer onto the current output clock. Called
// under f.mu. Unsigned RTP arithmetic deliberately preserves 32-bit wrap.
func (f *forwarder) captureTimestamp(rid string, timestamp uint32) (uint32, bool) {
	if f.pub == nil {
		return 0, false
	}
	f.pub.clockMu.Lock()
	old, oldOK := f.pub.clocks[f.current]
	next, nextOK := f.pub.clocks[rid]
	f.pub.clockMu.Unlock()
	f.pub.layMu.Lock()
	oldLayer, nextLayer := f.pub.layers[f.current], f.pub.layers[rid]
	valid := oldLayer != nil && nextLayer != nil && oldLayer.ssrc == old.ssrc && nextLayer.ssrc == next.ssrc
	f.pub.layMu.Unlock()
	if !oldOK || !nextOK || !valid || (f.inputSSRC != 0 && old.ssrc != f.inputSSRC) {
		return 0, false
	}
	delta := int64(next.ntp - old.ntp)
	ticks := (delta>>32)*90000 + ((delta & 0xffffffff) * 90000 >> 32)
	return old.rtp + (f.outTS - f.inTS) + uint32(ticks) + (timestamp - next.rtp), true
}
