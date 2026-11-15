package sfu

import (
	"os"
	"runtime"
	"strconv"
	"strings"
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/webrtc/v4"
)

// defaultInterceptors keeps Pion's transport feedback but leaves sender reports
// to syncTrack, which translates the publisher's capture clock.
func defaultInterceptors(m *webrtc.MediaEngine) (*interceptor.Registry, error) {
	reg := &interceptor.Registry{}
	for _, configure := range []func() error{
		func() error { return webrtc.ConfigureNack(m, reg) },
		func() error { return configureReceiverReports(reg) },
		func() error { return webrtc.ConfigureSimulcastExtensionHeaders(m) },
		func() error { return webrtc.ConfigureStatsInterceptor(reg) },
		func() error { return webrtc.ConfigureTWCCSender(m, reg) },
	} {
		if err := configure(); err != nil {
			return nil, err
		}
	}
	return reg, nil
}

// Stats is the whole instrument for the D2 measurement. It is deliberately the
// server's own view: bytes counted as they crossed the media path, and the
// process's own processor time, so a number here is not diluted by whatever
// else the machine happens to be doing.
type Stats struct {
	UptimeSeconds float64 `json:"uptime_seconds"`
	Rooms         int     `json:"rooms"`
	Peers         int     `json:"peers"`
	Publications  int     `json:"publications"`
	// Subscriptions is the number of outgoing streams: it is what the fan-out
	// actually costs, and the number that grows with the square of the room.
	Subscriptions int `json:"subscriptions"`

	BytesIn    int64 `json:"bytes_in"`
	BytesOut   int64 `json:"bytes_out"`
	PacketsIn  int64 `json:"packets_in"`
	PacketsOut int64 `json:"packets_out"`

	// CPUSeconds is user+system time for the whole process since it started, as
	// the kernel counts it. Two readings a minute apart divided by sixty gives
	// the share of one core; the load generator reads it exactly that way.
	CPUSeconds float64 `json:"cpu_seconds"`
	// RSSBytes is resident memory: what the process is actually holding in RAM.
	RSSBytes   int64  `json:"rss_bytes"`
	HeapBytes  uint64 `json:"heap_bytes"`
	Goroutines int    `json:"goroutines"`
	NumCPU     int    `json:"num_cpu"`
}

// Stats reads the counters. It is cheap enough to call once a second.
func (s *SFU) Stats() Stats {
	out := Stats{
		UptimeSeconds: time.Since(s.startedAt).Seconds(),
		BytesIn:       s.bytesIn.Load(),
		BytesOut:      s.bytesOut.Load(),
		PacketsIn:     s.packetsIn.Load(),
		PacketsOut:    s.packetsOut.Load(),
		Goroutines:    runtime.NumGoroutine(),
		NumCPU:        runtime.NumCPU(),
	}
	s.mu.RLock()
	rooms := make([]*room, 0, len(s.rooms))
	for _, r := range s.rooms {
		rooms = append(rooms, r)
	}
	out.Rooms = len(rooms)
	out.Peers = len(s.sessions)
	s.mu.RUnlock()
	for _, r := range rooms {
		r.mu.RLock()
		out.Publications += len(r.pubs)
		for _, p := range r.pubs {
			out.Subscriptions += int(p.subs.Load())
		}
		r.mu.RUnlock()
	}

	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)
	out.HeapBytes = ms.HeapAlloc
	out.CPUSeconds, out.RSSBytes = processCost()
	return out
}

// processCost reads the kernel's own accounting for this process. It is Linux
// only; on the owner's Windows machine both numbers come back zero, which is
// correct — there is nothing to measure there and a made-up number would be
// worse than none.
func processCost() (cpuSeconds float64, rssBytes int64) {
	raw, err := os.ReadFile("/proc/self/stat")
	if err != nil {
		return 0, 0
	}
	// The second field is the executable's name in brackets and may itself
	// contain spaces, so the fields are counted from after the closing one.
	line := string(raw)
	close := strings.LastIndex(line, ")")
	if close < 0 || close+2 >= len(line) {
		return 0, 0
	}
	fields := strings.Fields(line[close+2:])
	// After the name and state come utime and stime, fields 14 and 15 of the
	// whole line, which are index 11 and 12 here.
	if len(fields) < 22 {
		return 0, 0
	}
	utime, _ := strconv.ParseFloat(fields[11], 64)
	stime, _ := strconv.ParseFloat(fields[12], 64)
	// Clock ticks per second is 100 on every Linux this will ever run on.
	cpuSeconds = (utime + stime) / 100
	rss, _ := strconv.ParseInt(fields[21], 10, 64)
	return cpuSeconds, rss * int64(os.Getpagesize())
}
