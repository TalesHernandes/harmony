package main

import "testing"

// The profiler shows the inside of the server, so the one thing that must hold
// is that it never listens anywhere but this machine.
func TestProfilerRefusesAnythingButLoopback(t *testing.T) {
	ok := []string{"127.0.0.1:6060", "localhost:6060", "[::1]:6060"}
	bad := []string{":6060", "0.0.0.0:6060", "[::]:6060", "10.0.0.5:6060", "harmony.taleshernandes.com:6060", "6060"}
	for _, a := range ok {
		if err := loopbackOnly(a); err != nil {
			t.Errorf("%s refused: %v", a, err)
		}
	}
	for _, a := range bad {
		if err := loopbackOnly(a); err == nil {
			t.Errorf("%s accepted, and it is not the loopback", a)
		}
	}
}
