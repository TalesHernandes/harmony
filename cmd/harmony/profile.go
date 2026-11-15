package main

import (
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"net/http/pprof"
	"os"
	"time"
)

// startProfiler serves Go's CPU and memory profiles, for the bench
// (scripts/bancada) and nothing else.
//
// It is off unless HARMONY_PPROF_ADDR is set, and it refuses any address that
// is not the loopback: a profile shows what the server is doing and where, and
// that is not something to hand to the internet. Production never sets the
// variable; the bench sets it on the throwaway harmony it starts for itself.
//
// It is its own listener with its own mux rather than a route on the API, so
// that nothing about it — not even a 404 — exists on the port people reach.
func startProfiler(log *slog.Logger) error {
	addr := os.Getenv("HARMONY_PPROF_ADDR")
	if addr == "" {
		return nil
	}
	if err := loopbackOnly(addr); err != nil {
		return err
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/debug/pprof/", pprof.Index)
	mux.HandleFunc("/debug/pprof/profile", pprof.Profile)
	mux.HandleFunc("/debug/pprof/symbol", pprof.Symbol)
	mux.HandleFunc("/debug/pprof/trace", pprof.Trace)
	srv := &http.Server{
		Addr:              addr,
		Handler:           mux,
		ReadHeaderTimeout: 10 * time.Second,
	}
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}
	log.Info("profiler on", "addr", ln.Addr().String())
	go func() { _ = srv.Serve(ln) }()
	return nil
}

// loopbackOnly is the whole safety of the profiler: an address with no host,
// or with any host that is not the loopback, is refused.
func loopbackOnly(addr string) error {
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		return fmt.Errorf("HARMONY_PPROF_ADDR %q: %w", addr, err)
	}
	if host == "localhost" {
		return nil
	}
	if ip := net.ParseIP(host); ip != nil && ip.IsLoopback() {
		return nil
	}
	return fmt.Errorf("HARMONY_PPROF_ADDR %q: only the loopback is allowed (127.0.0.1:port)", addr)
}
