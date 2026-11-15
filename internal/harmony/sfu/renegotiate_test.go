package sfu

import (
	"testing"
	"time"

	"github.com/pion/webrtc/v4"
)

// A screen is given to nobody until they ask, so its arrival changes nothing on
// a connection that did not ask — and a connection nothing changed on has no
// reason to be put through an offer and an answer. Asking to watch does change
// it, and a camera (which everybody gets) does too.
func TestOnlyAChangedConnectionIsRenegotiated(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")
	bruno := p.join(t, "sala", 2, "bruno")

	stop := make(chan struct{})
	defer close(stop)
	alice.talk(stop)
	bruno.talk(stop)
	waitFor(t, "the conversation to reach bruno", func() bool {
		return bruno.receivedKind("audio") > 10
	})
	offers := func() int {
		p.mu.Lock()
		defer p.mu.Unlock()
		return len(p.offers[bruno.id])
	}
	settled := offers()

	feed(alice.share(SourceScreen, webrtc.MimeTypeVP9, "tela"), stop)
	waitFor(t, "the room to say alice is sharing", func() bool {
		return len(publishingOf(p.s, "sala", alice.id)) == 1
	})
	// Long enough for an offer that was going to be pushed to have been.
	time.Sleep(300 * time.Millisecond)
	if n := offers(); n != settled {
		t.Fatalf("a screen nobody asked for cost bruno %d offers", n-settled)
	}

	bruno.watch(t, "alice")
	waitFor(t, "bruno to be offered the screen he asked for", func() bool {
		return offers() > settled
	})
	waitFor(t, "the screen to reach bruno", func() bool {
		return bruno.receivedKind("video") > 10
	})

	// A camera goes to everybody, so everybody is told.
	before := offers()
	feed(alice.share(SourceCamera, webrtc.MimeTypeVP8, "camera"), stop)
	waitFor(t, "bruno to be offered the camera", func() bool {
		_, ok := bruno.sources()[SourceCamera]
		return ok && offers() > before
	})
}
