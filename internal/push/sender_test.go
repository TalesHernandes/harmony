package push

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// recordingPushService stands in for FCM: it captures the request and answers
// with whatever status the test wants.
type recordingPushService struct {
	*httptest.Server
	mu     sync.Mutex
	status int
	reqs   []*http.Request
	bodies []string
}

func newPushService(t *testing.T, status int) *recordingPushService {
	t.Helper()
	svc := &recordingPushService{status: status}
	svc.Server = httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		svc.mu.Lock()
		svc.reqs = append(svc.reqs, r.Clone(context.Background()))
		svc.bodies = append(svc.bodies, string(body))
		status := svc.status
		svc.mu.Unlock()
		w.WriteHeader(status)
	}))
	t.Cleanup(svc.Close)
	return svc
}

func (s *recordingPushService) count() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.reqs)
}

func (s *recordingPushService) last() (*http.Request, string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	n := len(s.reqs)
	return s.reqs[n-1], s.bodies[n-1]
}

// senderFor wires a Sender at an httptest TLS server. The endpoint must be
// https for VAPID, which is why the service is a TLS one and the client needs
// its certificate.
func senderFor(t *testing.T, svc *recordingPushService) *Sender {
	t.Helper()
	s := NewSender(mustKeys(t))
	s.client = svc.Client()
	s.client.Timeout = 5 * time.Second
	return s
}

func TestSendShapeOfTheRequest(t *testing.T) {
	svc := newPushService(t, http.StatusCreated)
	if err := senderFor(t, svc).Send(context.Background(), svc.URL+"/push/abc"); err != nil {
		t.Fatalf("Send: %v", err)
	}
	req, body := svc.last()

	if req.Method != http.MethodPost {
		t.Errorf("method = %s", req.Method)
	}
	if body != "" {
		t.Errorf("body = %q, want empty: this push carries no payload", body)
	}
	// No payload means no content coding. Announcing one with an empty body
	// is how a push service is told to expect bytes that never come.
	if ce := req.Header.Get("Content-Encoding"); ce != "" {
		t.Errorf("Content-Encoding = %q, want none for a bodyless push", ce)
	}
	if auth := req.Header.Get("Authorization"); !strings.HasPrefix(auth, "vapid t=") {
		t.Errorf("Authorization = %q", auth)
	}
	// The whole staleness strategy is this header — see the RingTTL comment.
	ttl, err := strconv.Atoi(req.Header.Get("TTL"))
	if err != nil || ttl != int(RingTTL.Seconds()) {
		t.Errorf("TTL = %q, want %d", req.Header.Get("TTL"), int(RingTTL.Seconds()))
	}
	if u := req.Header.Get("Urgency"); u != "high" {
		t.Errorf("Urgency = %q, want high: this is a ringing phone", u)
	}
	if topic := req.Header.Get("Topic"); topic != "ring" {
		t.Errorf("Topic = %q, want ring so a second ring replaces a queued one", topic)
	}
}

func TestSendAcceptsAny2xx(t *testing.T) {
	for _, status := range []int{http.StatusOK, http.StatusCreated, http.StatusAccepted, http.StatusNoContent} {
		svc := newPushService(t, status)
		if err := senderFor(t, svc).Send(context.Background(), svc.URL+"/push/abc"); err != nil {
			t.Errorf("status %d: %v", status, err)
		}
	}
}

// 404 and 410 are the push service saying the registration is dead. Anything
// else is a bad day, and the endpoint must survive it.
func TestSendReportsGoneOnlyForGoneStatuses(t *testing.T) {
	for _, status := range []int{http.StatusNotFound, http.StatusGone} {
		svc := newPushService(t, status)
		err := senderFor(t, svc).Send(context.Background(), svc.URL+"/push/abc")
		if !errors.Is(err, ErrGone) {
			t.Errorf("status %d: err = %v, want ErrGone", status, err)
		}
	}
	for _, status := range []int{http.StatusInternalServerError, http.StatusTooManyRequests, http.StatusUnauthorized} {
		svc := newPushService(t, status)
		err := senderFor(t, svc).Send(context.Background(), svc.URL+"/push/abc")
		if err == nil {
			t.Errorf("status %d: want an error", status)
		}
		if errors.Is(err, ErrGone) {
			t.Errorf("status %d must not drop the subscription", status)
		}
	}
}

// Delivery failure must never take a working registration away — only a 404
// or 410 does that.
func TestRingerDropsOnlyGoneSubscriptions(t *testing.T) {
	gone := newPushService(t, http.StatusGone)
	broken := newPushService(t, http.StatusInternalServerError)

	store, err := NewStore("")
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Add(Subscription{Endpoint: gone.URL + "/dead"}); err != nil {
		t.Fatal(err)
	}
	if err := store.Add(Subscription{Endpoint: broken.URL + "/flaky"}); err != nil {
		t.Fatal(err)
	}

	r := NewRinger(mustKeys(t), store, slog.New(slog.NewTextHandler(io.Discard, nil)))
	r.sender.client = gone.Client()
	// Both httptest TLS servers share the same self-signed CA in this
	// process, so one client trusts both.
	r.deliver(store.List())

	if store.Has(gone.URL + "/dead") {
		t.Error("a 410 subscription should have been dropped")
	}
	if !store.Has(broken.URL + "/flaky") {
		t.Error("a 500 must not cost a working subscription")
	}
}

func TestRingCooldownSuppressesABurst(t *testing.T) {
	svc := newPushService(t, http.StatusCreated)
	store, err := NewStore("")
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Add(Subscription{Endpoint: svc.URL + "/abc"}); err != nil {
		t.Fatal(err)
	}
	r := NewRinger(mustKeys(t), store, slog.New(slog.NewTextHandler(io.Discard, nil)))
	r.sender.client = svc.Client()

	now := time.Unix(1_000_000, 0)
	r.now = func() time.Time { return now }

	// Five rings in the same instant: the caller is told the peer is
	// reachable every time, because it is, but only one push goes out.
	for i := 0; i < 5; i++ {
		if !r.RingClosedPeer() {
			t.Fatal("a registered device is reachable on every ring")
		}
	}
	waitFor(t, func() bool { return svc.count() >= 1 })
	if got := svc.count(); got != 1 {
		t.Errorf("%d pushes went out for five rings in one instant, want 1", got)
	}

	// Past the cooldown, ringing works again.
	now = now.Add(RingCooldown + time.Second)
	if !r.RingClosedPeer() {
		t.Fatal("still reachable")
	}
	waitFor(t, func() bool { return svc.count() >= 2 })
}

func TestRingWithNoDeviceIsNotReachable(t *testing.T) {
	store, err := NewStore("")
	if err != nil {
		t.Fatal(err)
	}
	r := NewRinger(mustKeys(t), store, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if r.RingClosedPeer() {
		t.Fatal("with nothing registered the caller must get the no_peer error, not push-sent")
	}
}

func waitFor(t *testing.T, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatal("timed out waiting for the push to be delivered")
}
