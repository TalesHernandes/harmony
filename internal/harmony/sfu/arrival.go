package sfu

// Optional startup timing. The socket timestamp is taken immediately after
// ReadFrom returns, NOT in the kernel. twcc_stamp reconstructs the timestamp
// actually sent in feedback, including its clock-origin/quantization bounds.
// video brackets parsing/stamping/channel handoff; its upper-lower interval
// can therefore include feedback-writer backpressure.
import (
	"encoding/binary"
	"log/slog"
	"net"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/rtcp"
	"github.com/pion/webrtc/v4"
)

const arrivalSlots = 16384

type arrivalKey struct {
	ssrc, timestamp uint32
	seq             uint16
}
type arrivalSlot struct {
	key       arrivalKey
	at        time.Time
	ambiguous bool
}
type arrivalRow struct {
	kind              string
	ssrc              uint32
	seq               uint16
	at                time.Time
	age, lower, upper time.Duration
	found             bool
}
type arrivalTrace struct {
	mu         sync.Mutex
	slots      [arrivalSlots]arrivalSlot
	rows       chan arrivalRow
	stop, done chan struct{}
	dropped    atomic.Uint64
	log        *slog.Logger
	stamped    atomic.Uint64
	missing    atomic.Uint64
}

func newArrivalTrace(log *slog.Logger) *arrivalTrace {
	d := &arrivalTrace{rows: make(chan arrivalRow, 8192), stop: make(chan struct{}), done: make(chan struct{}), log: log}
	go func() {
		defer close(d.done)
		write := func(r arrivalRow) {
			d.log.Info("sfu arrival", "event", r.kind, "ssrc", r.ssrc, "seq", r.seq,
				"at_unix_us", r.at.UnixMicro(), "age_us", r.age.Microseconds(), "matched", r.found,
				"socket_to_stamp_lower_us", r.lower.Microseconds(), "socket_to_stamp_upper_us", r.upper.Microseconds())
		}
		for {
			select {
			case r := <-d.rows:
				write(r)
			case <-d.stop:
				for {
					select {
					case r := <-d.rows:
						write(r)
					default:
						d.log.Info("sfu arrival stopped", "dropped", d.dropped.Load(), "stamped", d.stamped.Load(), "unmatched", d.missing.Load())
						return
					}
				}
			}
		}
	}()
	return d
}
func (d *arrivalTrace) close() { close(d.stop); <-d.done }
func (d *arrivalTrace) emit(r arrivalRow) {
	if r.kind == "twcc_stamp" {
		d.stamped.Add(1)
	}
	if r.kind == "video" && !r.found {
		d.missing.Add(1)
	}
	select {
	case d.rows <- r:
	default:
		d.dropped.Add(1)
	}
}
func packetArrivalKey(p []byte) (arrivalKey, bool) {
	if len(p) < 12 || p[0]>>6 != 2 || (p[1]&127 >= 64 && p[1]&127 <= 95) {
		return arrivalKey{}, false
	}
	return arrivalKey{binary.BigEndian.Uint32(p[8:12]), binary.BigEndian.Uint32(p[4:8]), binary.BigEndian.Uint16(p[2:4])}, true
}
func arrivalIndex(k arrivalKey) uint32 { return (k.ssrc ^ k.timestamp ^ uint32(k.seq)) % arrivalSlots }
func (d *arrivalTrace) received(p []byte, at time.Time) {
	k, ok := packetArrivalKey(p)
	if !ok {
		return
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	slot := &d.slots[arrivalIndex(k)]
	duplicate := slot.key == k && !slot.at.IsZero() && at.Sub(slot.at) < 5*time.Second
	*slot = arrivalSlot{k, at, duplicate}
}
func (d *arrivalTrace) take(p []byte, now time.Time) (time.Time, bool) {
	k, ok := packetArrivalKey(p)
	if !ok {
		return time.Time{}, false
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	slot := &d.slots[arrivalIndex(k)]
	if slot.key != k || slot.at.IsZero() || slot.ambiguous || now.Sub(slot.at) > 5*time.Second {
		return time.Time{}, false
	}
	at := slot.at
	*slot = arrivalSlot{}
	return at, true
}

type arrivalConn struct {
	net.PacketConn
	trace *arrivalTrace
}

func (c *arrivalConn) ReadFrom(p []byte) (int, net.Addr, error) {
	n, a, err := c.PacketConn.ReadFrom(p)
	at := time.Now()
	if err == nil {
		c.trace.received(p[:n], at)
	}
	return n, a, err
}

// Pion's standard registry is left intact when the diagnostic is disabled.
// When enabled, only TWCC is decorated; all standard factories remain present.
func arrivalInterceptors(m *webrtc.MediaEngine, d *arrivalTrace) (*interceptor.Registry, error) {
	if d == nil {
		return defaultInterceptors(m)
	}
	r := &interceptor.Registry{}
	if err := webrtc.ConfigureNack(m, r); err != nil {
		return nil, err
	}
	if err := configureReceiverReports(r); err != nil {
		return nil, err
	}
	if err := webrtc.ConfigureSimulcastExtensionHeaders(m); err != nil {
		return nil, err
	}
	if err := webrtc.ConfigureStatsInterceptor(r); err != nil {
		return nil, err
	}
	tw := &interceptor.Registry{}
	if err := webrtc.ConfigureTWCCSender(m, tw); err != nil {
		return nil, err
	}
	r.Add(&arrivalFactory{tw, d})
	return r, nil
}

type arrivalFactory struct {
	base  *interceptor.Registry
	trace *arrivalTrace
}

func (f *arrivalFactory) NewInterceptor(id string) (interceptor.Interceptor, error) {
	originLower := time.Now()
	base, err := f.base.Build(id)
	originUpper := time.Now()
	if err != nil {
		return nil, err
	}
	return &arrivalInterceptor{Interceptor: base, trace: f.trace, originLower: originLower, originUpper: originUpper}, nil
}

type arrivalPending struct {
	transport uint16
	row       arrivalRow
	socket    time.Time
}
type arrivalInterceptor struct {
	originLower, originUpper time.Time
	pendingMu                sync.Mutex
	pending                  [8192]arrivalPending
	interceptor.Interceptor
	trace *arrivalTrace
}
type arrivalAttribute struct{}
type arrivalRead struct {
	socket, lower time.Time
	found         bool
}

func (i *arrivalInterceptor) BindRemoteStream(info *interceptor.StreamInfo, r interceptor.RTPReader) interceptor.RTPReader {
	enabled := false
	var extensionID uint8
	var first time.Time
	for _, ext := range info.RTPHeaderExtensions {
		if ext.URI == "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01" {
			enabled = true
			extensionID = uint8(ext.ID)
		}
	}
	if !enabled || !strings.HasPrefix(strings.ToLower(info.MimeType), "video/") {
		return i.Interceptor.BindRemoteStream(info, r)
	}
	inner := interceptor.RTPReaderFunc(func(p []byte, a interceptor.Attributes) (int, interceptor.Attributes, error) {
		n, a, err := r.Read(p, a)
		if err != nil {
			return n, a, err
		}
		now := time.Now()
		at, ok := i.trace.take(p[:n], now)
		if a == nil {
			a = interceptor.Attributes{}
		}
		lower := time.Now()
		a[arrivalAttribute{}] = arrivalRead{at, lower, ok}
		if first.IsZero() {
			first = lower
			if ok {
				first = at
			}
		}
		if ok && lower.Sub(first) <= 3*time.Second {
			header, e := a.GetRTPHeader(p[:n])
			if e == nil {
				ext := header.GetExtension(extensionID)
				if len(ext) == 2 {
					transport := binary.BigEndian.Uint16(ext)
					i.pendingMu.Lock()
					i.pending[int(transport)%len(i.pending)] = arrivalPending{transport: transport, socket: at,
						row: arrivalRow{kind: "twcc_stamp", ssrc: info.SSRC, seq: header.SequenceNumber, at: lower, age: lower.Sub(first), found: true}}
					i.pendingMu.Unlock()
				}
			}
		}
		return n, a, nil
	})
	wrapped := i.Interceptor.BindRemoteStream(info, inner)
	return interceptor.RTPReaderFunc(func(p []byte, a interceptor.Attributes) (int, interceptor.Attributes, error) {
		n, a, err := wrapped.Read(p, a)
		end := time.Now()
		if err != nil {
			return n, a, err
		}
		sample, ok := a[arrivalAttribute{}].(arrivalRead)
		delete(a, arrivalAttribute{})
		if !ok {
			return n, a, nil
		}
		if first.IsZero() {
			first = sample.lower
			if sample.found {
				first = sample.socket
			}
		}
		if sample.lower.Sub(first) <= 3*time.Second {
			row := arrivalRow{kind: "video", ssrc: info.SSRC, at: end, age: sample.lower.Sub(first), found: sample.found}
			if k, ok := packetArrivalKey(p[:n]); ok {
				row.seq = k.seq
			}
			if sample.found {
				row.lower = sample.lower.Sub(sample.socket)
				row.upper = end.Sub(sample.socket)
			}
			i.trace.emit(row)
		}
		return n, a, nil
	})
}
func (i *arrivalInterceptor) BindRTCPWriter(w interceptor.RTCPWriter) interceptor.RTCPWriter {
	return i.Interceptor.BindRTCPWriter(interceptor.RTCPWriterFunc(func(p []rtcp.Packet, a interceptor.Attributes) (int, error) {
		start := time.Now()
		for _, packet := range p {
			if feedback, ok := packet.(*rtcp.TransportLayerCC); ok {
				i.feedbackStamps(feedback)
			}
		}
		n, err := w.Write(p, a)
		end := time.Now()
		for _, packet := range p {
			if _, ok := packet.(*rtcp.TransportLayerCC); ok {
				i.trace.emit(arrivalRow{kind: "feedback_write", at: end, upper: end.Sub(start)})
				break
			}
		}
		return n, err
	}))
}
func arrivalDiagnosticEnabled() bool { return os.Getenv("HARMONY_SFU_ARRIVAL_DIAG") == "1" }

// Reconstruct the actual TWCC timestamp, with the constructor's clock-origin
// interval and the wire's 250-us quantization. This excludes channel/write wait.
// Pion feeds rounding error into the next delta (twcc.go:addReceived), so it
// does not accumulate per packet. 250 us also covers integer-microsecond truncation.
func (i *arrivalInterceptor) feedbackStamps(f *rtcp.TransportLayerCC) {
	elapsed := int64(f.ReferenceTime) * 64000
	seq := f.BaseSequenceNumber
	count, delta := 0, 0
	visit := func(symbol uint16) {
		if count >= int(f.PacketStatusCount) {
			return
		}
		if symbol == rtcp.TypeTCCPacketReceivedSmallDelta || symbol == rtcp.TypeTCCPacketReceivedLargeDelta {
			if delta >= len(f.RecvDeltas) {
				return
			}
			elapsed += f.RecvDeltas[delta].Delta
			delta++
			i.pendingMu.Lock()
			slot := &i.pending[int(seq)%len(i.pending)]
			sample := *slot
			if sample.transport == seq && !sample.socket.IsZero() {
				*slot = arrivalPending{}
			}
			i.pendingMu.Unlock()
			if sample.transport == seq && !sample.socket.IsZero() {
				// ReferenceTime wraps every 2^24 * 64 ms; choose the epoch of this sample.
				const period = int64(1<<24) * 64000
				expected := sample.row.at.Sub(i.originLower).Microseconds()
				unwrapped := elapsed + ((expected-elapsed+period/2)/period)*period
				row := sample.row
				row.lower = i.originLower.Add(time.Duration(unwrapped)*time.Microsecond - 250*time.Microsecond).Sub(sample.socket)
				row.upper = i.originUpper.Add(time.Duration(unwrapped)*time.Microsecond + 250*time.Microsecond).Sub(sample.socket)
				if row.lower < 0 {
					row.lower = 0
				}
				i.trace.emit(row)
			}
		}
		seq++
		count++
	}
	for _, chunk := range f.PacketChunks {
		switch c := chunk.(type) {
		case *rtcp.RunLengthChunk:
			for n := 0; n < int(c.RunLength); n++ {
				visit(c.PacketStatusSymbol)
			}
		case *rtcp.StatusVectorChunk:
			for _, symbol := range c.SymbolList {
				visit(symbol)
			}
		}
	}
}
