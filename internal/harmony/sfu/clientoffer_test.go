package sfu

import (
	"testing"

	"github.com/pion/webrtc/v4"
)

// A screen the browser offers into a connection that already carries somebody
// else's camera to it. The slot the offer opened was found by elimination
// among this session's own slots — and the transceiver bringing the other
// person's camera to this device is not one of those, so it was taken for the
// screen. The screen itself then arrived on a slot nobody had named, and was
// dropped as "track on unknown slot": nobody in the room ever saw it.
//
// Found on 23/09/2026 while taking pictures of the room for the design, not
// by a test: every test of a shared screen shared it before anybody had a
// camera on, and the Go harness publishes by the server's offer, never by the
// browser's — which is the only way a browser shares a screen since D4r.
func TestAScreenOfferedBesideSomebodysCameraIsTheScreen(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")
	bruno := p.join(t, "sala", 2, "bruno")
	stop := make(chan struct{})
	defer close(stop)
	feed(alice.share(SourceCamera, webrtc.MimeTypeVP8, "cam"), stop)
	waitFor(t, "alice's camera to reach bruno", func() bool {
		_, ok := bruno.sources()[SourceCamera]
		return ok
	})

	// What a browser does to share a screen: a send-only picture of its own,
	// in an offer it makes itself.
	track, err := webrtc.NewTrackLocalStaticRTP(
		webrtc.RTPCodecCapability{MimeType: webrtc.MimeTypeVP8, ClockRate: 90000}, "tela", "tela")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := bruno.offer([]string{SourceScreen}, track); err != nil {
		t.Fatalf("the screen offer was refused: %v", err)
	}
	feed(&sharedTrack{track: track, payloadType: 96}, stop)

	waitFor(t, "bruno's screen to be published", func() bool {
		return screenOf(p.s, "sala", bruno.id) != nil
	})
	// And the camera coming to bruno is still alice's camera, not a slot
	// renamed out from under it.
	sess := sessionOf(p.s, bruno.id)
	sess.slotMu.Lock()
	defer sess.slotMu.Unlock()
	for _, sl := range sess.slots {
		if sl.source == SourceScreen && sl.tr.Sender() != nil && sl.tr.Sender().Track() != nil {
			t.Fatalf("the screen slot is the transceiver sending %q to bruno", sl.tr.Sender().Track().ID())
		}
	}
}
