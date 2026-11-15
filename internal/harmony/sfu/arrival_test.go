package sfu

import (
	"github.com/pion/webrtc/v4"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/interceptor/pkg/twcc"
	"github.com/pion/rtcp"
	"github.com/pion/rtp"
)

func arrivalPacket(t *testing.T, seq uint16) []byte {
	t.Helper()
	p := rtp.Packet{Header: rtp.Header{Version: 2, PayloadType: 102, SSRC: 71, SequenceNumber: seq, Timestamp: uint32(seq) * 1500}, Payload: make([]byte, 1000)}
	if err := p.SetExtension(3, []byte{byte(seq >> 8), byte(seq)}); err != nil {
		t.Fatal(err)
	}
	b, err := p.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	return b
}
func TestArrivalIdentityAndBounds(t *testing.T) {
	d := &arrivalTrace{}
	now := time.Now()
	a := arrivalPacket(t, 1)
	d.received(a, now)
	if _, ok := d.take(arrivalPacket(t, 2), now); ok {
		t.Fatal("unrelated packet matched")
	}
	if at, ok := d.take(a, now.Add(time.Millisecond)); !ok || !at.Equal(now) {
		t.Fatal("socket timestamp lost")
	}
	if _, ok := d.take(a, now); ok {
		t.Fatal("sample reused")
	}
	d.received(a, now)
	d.received(a, now.Add(time.Microsecond))
	if _, ok := d.take(a, now); ok {
		t.Fatal("ambiguous duplicate matched")
	}
	d.received(arrivalPacket(t, 2), now)
	if _, ok := d.take(arrivalPacket(t, 2), now.Add(6*time.Second)); ok {
		t.Fatal("stale sample matched")
	}
	for _, bad := range [][]byte{{}, make([]byte, 12), {0x80, 200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}} {
		if _, ok := packetArrivalKey(bad); ok {
			t.Fatal("non-RTP accepted")
		}
	}
}

// This uses the real UDP ReadFrom wrapper and the installed Pion TWCC factory.
// The controlled 40-ms stall represents consumer/feedback backpressure, NOT a
// claim that production suffered that stall or that goog_cc reduced its rate.
func TestArrivalMeasuresSocketBacklogAndFeedbackWait(t *testing.T) {
	for _, blockFeedback := range []bool{false, true} {
		name := "consumer"
		if blockFeedback {
			name = "feedback"
		}
		t.Run(name, func(t *testing.T) {
			d := &arrivalTrace{rows: make(chan arrivalRow, 128)}
			socket, err := net.ListenPacket("udp4", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			defer socket.Close()
			sender, err := net.Dial("udp4", socket.LocalAddr().String())
			if err != nil {
				t.Fatal(err)
			}
			defer sender.Close()
			conn := &arrivalConn{socket, d}
			var packets [][]byte
			for seq := uint16(1); seq <= 3; seq++ {
				raw := arrivalPacket(t, seq)
				if _, err = sender.Write(raw); err != nil {
					t.Fatal(err)
				}
				buf := make([]byte, 1500)
				_ = socket.SetReadDeadline(time.Now().Add(time.Second))
				n, _, e := conn.ReadFrom(buf)
				if e != nil {
					t.Fatal(e)
				}
				packets = append(packets, buf[:n])
			}
			reg := &interceptor.Registry{}
			factory, _ := twcc.NewSenderInterceptor(twcc.SendInterval(5 * time.Millisecond))
			reg.Add(factory)
			wrapped, err := (&arrivalFactory{reg, d}).NewInterceptor("test")
			if err != nil {
				t.Fatal(err)
			}
			defer wrapped.Close()
			entered, release := make(chan struct{}, 1), make(chan struct{})
			released := false
			defer func() {
				if !released {
					close(release)
				}
			}()
			writes := 0
			wrapped.BindRTCPWriter(interceptor.RTCPWriterFunc(func(p []rtcp.Packet, a interceptor.Attributes) (int, error) {
				writes++
				if blockFeedback && writes == 1 {
					entered <- struct{}{}
					<-release
				}
				return len(p), nil
			}))
			index := 0
			reader := wrapped.BindRemoteStream(&interceptor.StreamInfo{SSRC: 71, MimeType: "video/H264", RTPHeaderExtensions: []interceptor.RTPHeaderExtension{{ID: 3, URI: "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"}}},
				interceptor.RTPReaderFunc(func(buf []byte, a interceptor.Attributes) (int, interceptor.Attributes, error) {
					p := packets[index]
					index++
					return copy(buf, p), a, nil
				}))
			read := func() error { _, _, err := reader.Read(make([]byte, 1500), nil); return err }
			if err = read(); err != nil {
				t.Fatal(err)
			}
			if blockFeedback {
				select {
				case <-entered:
				case <-time.After(time.Second):
					t.Fatal("no feedback")
				}
			}
			done := make(chan error, 1)
			started := time.Now()
			if blockFeedback {
				go func() { done <- read() }()
			}
			time.Sleep(40 * time.Millisecond)
			close(release)
			released = true
			if blockFeedback {
				select {
				case err = <-done:
					if err != nil {
						t.Fatal(err)
					}
				case <-time.After(time.Second):
					t.Fatal("blocked reader")
				}
			} else {
				if err = read(); err != nil {
					t.Fatal(err)
				}
			}
			held := time.Since(started)
			if err = read(); err != nil {
				t.Fatal(err)
			}
			timeout := time.After(time.Second)
			stamps := map[uint16]arrivalRow{}
			var second arrivalRow
			for len(stamps) < 3 {
				select {
				case row := <-d.rows:
					if row.kind == "twcc_stamp" {
						stamps[row.seq] = row
					}
					if row.kind == "video" && row.seq == 2 {
						second = row
					}
				case <-timeout:
					t.Fatalf("missing stamps: %d", len(stamps))
				}
			}
			// Sequence 3 could not be stamped until the controlled hold ended.
			if stamps[3].lower < held-2*time.Millisecond {
				t.Fatalf("backlog underreported: %+v held=%v", stamps[3], held)
			}
			if stamps[3].upper-stamps[3].lower > 5*time.Millisecond {
				t.Fatal("TWCC stamp interval unexpectedly wide")
			}
			if blockFeedback && second.upper-second.lower < 35*time.Millisecond {
				t.Fatal("channel wait not measured")
			}
			if blockFeedback && stamps[2].upper >= stamps[3].lower {
				t.Fatal("post-stamp blocking incorrectly assigned to current stamp")
			}
			t.Logf("hold=%s packet2 stamp=[%s,%s] read=[%s,%s] packet3 stamp=[%s,%s]", held,
				stamps[2].lower, stamps[2].upper, second.lower, second.upper, stamps[3].lower, stamps[3].upper)
		})
	}
}

func TestArrivalReferenceWrapAndMissingPackets(t *testing.T) {
	d := &arrivalTrace{rows: make(chan arrivalRow, 10)}
	start := time.Now()
	period := time.Duration(int64(1<<24)*64000) * time.Microsecond
	i := &arrivalInterceptor{trace: d, originLower: start, originUpper: start.Add(time.Microsecond)}
	// Include a lost sequence, then a received one across uint16 wrap.
	at := start.Add(period + time.Second)
	i.pending[0] = arrivalPending{transport: 0, socket: at, row: arrivalRow{at: at.Add(10 * time.Millisecond), seq: 8, found: true}}
	f := &rtcp.TransportLayerCC{BaseSequenceNumber: 65535, PacketStatusCount: 2, ReferenceTime: 15,
		PacketChunks: []rtcp.PacketStatusChunk{&rtcp.StatusVectorChunk{SymbolList: []uint16{0, 1}}},
		RecvDeltas:   []*rtcp.RecvDelta{{Type: 1, Delta: 50000}}}
	i.feedbackStamps(f)
	select {
	case row := <-d.rows:
		if row.lower != 9750*time.Microsecond || row.upper != 10251*time.Microsecond {
			t.Fatalf("wrong wrap/delta: %+v", row)
		}
	default:
		t.Fatal("wrapped sample not found")
	}
}

func TestArrivalRealSFUBurst(t *testing.T) {
	t.Setenv("HARMONY_SFU_ARRIVAL_DIAG", "1")
	s := testServer(t, 8)
	if s.arrival == nil {
		t.Fatal("diagnostic was not wired to SFU")
	}
	var mu sync.Mutex
	var owner *client
	s.cfg.Signal = func(_ int64, _, kind string, data any) {
		if kind != "voice_offer" {
			return
		}
		mu.Lock()
		c := owner
		mu.Unlock()
		if c != nil {
			_ = c.answer(data.(Offer))
		}
	}
	c := newClient(t, s, "arrival", 1, "publisher")
	mu.Lock()
	owner = c
	mu.Unlock()
	screen := c.share(SourceScreen, webrtc.MimeTypeH264, "arrival-screen")
	waitFor(t, "screen sender negotiated", func() bool {
		for _, sender := range c.pc.GetSenders() {
			if sender.Track() != nil && sender.Track().ID() == "arrival-screen" {
				return len(sender.GetParameters().HeaderExtensions) > 0
			}
		}
		return false
	})
	var extID uint8
	for _, sender := range c.pc.GetSenders() {
		if sender.Track() != nil && sender.Track().ID() == "arrival-screen" {
			for _, e := range sender.GetParameters().HeaderExtensions {
				if e.URI == "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01" {
					extID = uint8(e.ID)
				}
			}
		}
	}
	if extID == 0 {
		t.Fatal("TWCC extension not negotiated")
	}
	waitFor(t, "connected", func() bool { return c.pc.ConnectionState() == webrtc.PeerConnectionStateConnected })
	// Two 24-kB bursts, no capture/encoder/window/audio devices.
	for seq := uint16(1); seq <= 48; seq++ {
		if seq == 25 {
			time.Sleep(40 * time.Millisecond)
		}
		p := rtp.Packet{Header: rtp.Header{Version: 2, PayloadType: 102, SequenceNumber: seq, Timestamp: uint32(seq/24) * 1500}, Payload: make([]byte, 1000)}
		if err := p.SetExtension(extID, []byte{byte(seq >> 8), byte(seq)}); err != nil {
			t.Fatal(err)
		}
		if err := screen.track.WriteRTP(&p); err != nil {
			t.Fatal(err)
		}
	}
	waitFor(t, "socket and feedback timestamps", func() bool { return s.arrival.stamped.Load() >= 48 })
	if s.arrival.missing.Load() != 0 || s.arrival.dropped.Load() != 0 {
		t.Fatalf("incomplete measurement: missing=%d dropped=%d", s.arrival.missing.Load(), s.arrival.dropped.Load())
	}
	t.Logf("real SFU burst: stamped=%d unmatched=%d dropped=%d", s.arrival.stamped.Load(), s.arrival.missing.Load(), s.arrival.dropped.Load())
}
