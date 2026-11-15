package auth

import (
	"sync"
	"time"
)

// RateLimiter is a fixed-window counter per key (an IP address).
// Two users hit this endpoint a handful of times a day; anything fancier is
// over-engineering.
type RateLimiter struct {
	limit  int
	window time.Duration
	now    func() time.Time

	mu      sync.Mutex
	buckets map[string]*bucket
}

type bucket struct {
	start time.Time
	count int
}

// NewRateLimiter allows limit hits per key per window.
func NewRateLimiter(limit int, window time.Duration) *RateLimiter {
	return &RateLimiter{
		limit:   limit,
		window:  window,
		now:     time.Now,
		buckets: make(map[string]*bucket),
	}
}

// Allow records a hit for key and reports whether it is within the limit.
func (r *RateLimiter) Allow(key string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()

	now := r.now()
	b, ok := r.buckets[key]
	if !ok || now.Sub(b.start) >= r.window {
		b = &bucket{start: now}
		r.buckets[key] = b
		r.sweep(now)
	}
	b.count++
	return b.count <= r.limit
}

// sweep drops expired buckets so the map cannot grow unbounded under a scan.
func (r *RateLimiter) sweep(now time.Time) {
	for k, b := range r.buckets {
		if now.Sub(b.start) >= r.window {
			delete(r.buckets, k)
		}
	}
}
