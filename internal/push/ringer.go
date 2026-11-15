package push

import (
	"context"
	"errors"
	"log/slog"
	"sync"
	"time"
)

// RingCooldown throttles pushes. Two people cannot legitimately ring more
// often than this, and a client stuck in a retry loop must not turn into a
// burst at the push service — or a burst of notifications on the phone.
const RingCooldown = 10 * time.Second

// sendTimeout bounds one delivery attempt end to end.
const sendTimeout = 15 * time.Second

// Ringer turns a `ring` that found nobody in the room into a push.
//
// It is deliberately split in two: whether a device is registered is answered
// synchronously, because the signaling read pump has to decide *now* which
// reply the caller gets (`push-sent` or the `no_peer` error), and the actual
// HTTP to the push service — hundreds of milliseconds against a third party —
// happens on its own goroutine, where it cannot hold up the room.
type Ringer struct {
	sender *Sender
	store  *Store
	log    *slog.Logger

	mu       sync.Mutex
	lastRing time.Time
	now      func() time.Time
}

// NewRinger wires a Ringer. keys must be non-nil; callers that have no VAPID
// configuration leave the whole Ringer nil instead, which disables push.
func NewRinger(keys *Keys, store *Store, log *slog.Logger) *Ringer {
	return &Ringer{sender: NewSender(keys), store: store, log: log, now: time.Now}
}

// RingClosedPeer reports whether a device is registered — and, when one is,
// sends the push in the background.
//
// The return value is about reachability, not delivery: it says the caller's
// ring has somewhere to go, which is what distinguishes "their app is closed"
// from "nobody has ever set up the doorbell". Delivery is asynchronous and
// nobody waits for it, so failures show up in the log, not in the room.
func (r *Ringer) RingClosedPeer() bool {
	subs := r.store.List()
	if len(subs) == 0 {
		return false
	}
	if !r.take() {
		// Within the cooldown a push already went out for this ring; the
		// caller is still told the peer is reachable, because it is.
		r.log.Info("push suppressed by cooldown")
		return true
	}
	go r.deliver(subs)
	return true
}

// take reports whether a push may go out now, and records it if so.
func (r *Ringer) take() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	now := r.now()
	if !r.lastRing.IsZero() && now.Sub(r.lastRing) < RingCooldown {
		return false
	}
	r.lastRing = now
	return true
}

// deliver pushes to every registered device and forgets the dead ones.
func (r *Ringer) deliver(subs []Subscription) {
	ctx, cancel := context.WithTimeout(context.Background(), sendTimeout)
	defer cancel()
	for _, sub := range subs {
		err := r.sender.Send(ctx, sub.Endpoint)
		switch {
		case err == nil:
			r.log.Info("push sent")
		case errors.Is(err, ErrGone):
			// The push service is authoritative here: this registration will
			// never work again, and keeping it would make Len() lie about
			// whether the doorbell can ring.
			r.log.Info("push subscription gone, dropping")
			if err := r.store.Remove(sub.Endpoint); err != nil {
				r.log.Error("push store", "err", err)
			}
		default:
			// No endpoint in the message: it is a capability URL, and the
			// log is the one place in this project that is allowed to be
			// boring about secrets (docs/historico/PLANO.md §8).
			r.log.Error("push failed", "err", err)
		}
	}
}
