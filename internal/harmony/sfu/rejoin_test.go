package sfu

import (
	"sync"
	"testing"
)

// TestFriendWhoRejoinsStillHearsAndIsHeard is the owner's report of 22/10/2026:
// he was talking in a room; a friend came in from one device, left, and came
// back from another. From then on the owner was not heard until he left and
// came back himself. A rejoin is a new session for the same account, and the
// old one's departure must not take anything of the new one with it.
func TestFriendWhoRejoinsStillHearsAndIsHeard(t *testing.T) {
	s := testServer(t, 8)

	// An offer can be sent while the join that caused it is still returning,
	// before the test knows the new client: a real device has its socket open
	// by then and gets it, so hold it until the client is known.
	var mu sync.Mutex
	clients := map[string]*client{}
	early := map[string][]Offer{}
	s.cfg.Signal = func(_ int64, _, kind string, data any) {
		if kind != "voice_offer" {
			return
		}
		offer := data.(Offer)
		mu.Lock()
		c := clients[offer.SessionID]
		if c == nil {
			early[offer.SessionID] = append(early[offer.SessionID], offer)
		}
		mu.Unlock()
		if c != nil {
			_ = c.answer(offer)
		}
	}
	add := func(c *client) {
		mu.Lock()
		clients[c.id] = c
		held := early[c.id]
		delete(early, c.id)
		mu.Unlock()
		for _, offer := range held {
			_ = c.answer(offer)
		}
	}

	stop := make(chan struct{})
	defer close(stop)

	// The owner is already talking when the friend arrives, as on 22/10.
	owner := newClient(t, s, "sala", 1, "dono")
	add(owner)
	owner.talk(stop)
	web := newClient(t, s, "sala", 2, "amigo")
	add(web)
	web.talk(stop)
	waitFor(t, "the friend on the web to hear the owner", func() bool { return web.received() > 10 })

	if err := s.Leave(web.id, 2); err != nil {
		t.Fatal(err)
	}
	_ = web.pc.Close()

	before := owner.received()
	native := newClient(t, s, "sala", 2, "amigo")
	add(native)
	native.talk(stop)

	waitFor(t, "the friend, back from another device, to hear the owner", func() bool { return native.received() > 10 })
	waitFor(t, "the owner to hear the friend again", func() bool { return owner.received() > before+10 })
}
