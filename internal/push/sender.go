package push

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"time"
)

// RingTTL is how long the push service may hold a ring before giving up on
// it (RFC 8030 §5.2).
//
// This is the whole staleness story, and it is why no payload is needed. An
// Android radio that was asleep hands the queued message over when it wakes;
// without a TTL that can be minutes later, and the phone rings for a call
// that ended long ago. At 45 s the push service drops it instead, and nothing
// stale is ever delivered — a stronger guarantee than a timestamp inside an
// encrypted payload, which would only let the worker decide to stay quiet
// after the phone had already been woken.
const RingTTL = 45 * time.Second

// ErrGone means the push service says the subscription is dead: the app was
// uninstalled, or the browser dropped its registration. The endpoint should
// be forgotten (RFC 8030 §7.3).
var ErrGone = errors.New("push: subscription gone")

// Sender posts pushes to a push service.
type Sender struct {
	keys   *Keys
	client *http.Client
	now    func() time.Time
}

// NewSender returns a Sender using keys.
func NewSender(keys *Keys) *Sender {
	return &Sender{
		keys: keys,
		// Bounded on purpose: this runs off the signaling read pump's
		// goroutine and must never be able to pile up.
		client: &http.Client{Timeout: 10 * time.Second},
		now:    time.Now,
	}
}

// Send delivers one bodyless push to endpoint. It returns ErrGone when the
// subscription should be dropped.
//
// No body and no Content-Encoding: a push with no payload is valid, and the
// worker needs no data to know what it means (see the package comment).
func (s *Sender) Send(ctx context.Context, endpoint string) error {
	auth, err := s.keys.authorization(endpoint, s.now())
	if err != nil {
		return err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint, http.NoBody)
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", auth)
	req.Header.Set("TTL", strconv.Itoa(int(RingTTL.Seconds())))
	// Wake the device even in doze: this is a phone call, which is the case
	// the "high" urgency exists for (RFC 8030 §5.3).
	req.Header.Set("Urgency", "high")
	// A second ring replaces a first one still queued, instead of the phone
	// getting a burst of notifications when it comes back (RFC 8030 §5.4).
	req.Header.Set("Topic", "ring")
	req.Header.Set("Content-Length", "0")

	resp, err := s.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	// Drained and capped: the connection can be reused, and a push service
	// having a bad day cannot feed us an unbounded error page.
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 512))

	switch {
	case resp.StatusCode == http.StatusNotFound || resp.StatusCode == http.StatusGone:
		return ErrGone
	case resp.StatusCode >= 200 && resp.StatusCode < 300:
		return nil
	default:
		return fmt.Errorf("push: %s: %s", resp.Status, body)
	}
}
