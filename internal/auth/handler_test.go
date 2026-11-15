package auth

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func newHandler() *Handler {
	return &Handler{
		Passphrase: "open sesame",
		Tokens:     NewTokens([]byte("secret")),
		Limiter:    NewRateLimiter(5, time.Minute),
	}
}

func post(h http.Handler, body, xff string) *httptest.ResponseRecorder {
	req := httptest.NewRequest(http.MethodPost, "/auth", strings.NewReader(body))
	req.RemoteAddr = "10.0.0.1:1234"
	if xff != "" {
		req.Header.Set("X-Forwarded-For", xff)
	}
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	return rec
}

func TestAuthHandler(t *testing.T) {
	h := newHandler()

	rec := post(h, `{"passphrase":"open sesame"}`, "")
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"token"`) {
		t.Fatalf("good passphrase: %d %s", rec.Code, rec.Body)
	}
	if rec := post(h, `{"passphrase":"nope"}`, ""); rec.Code != http.StatusUnauthorized {
		t.Fatalf("bad passphrase: %d", rec.Code)
	}
	if rec := post(h, `not json`, ""); rec.Code != http.StatusBadRequest {
		t.Fatalf("bad json: %d", rec.Code)
	}
	req := httptest.NewRequest(http.MethodGet, "/auth", nil)
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("GET: %d", rec.Code)
	}
}

func TestAuthRateLimit(t *testing.T) {
	h := newHandler()
	// 4 hits already consumed above? No: fresh handler. 5 allowed, 6th blocked.
	for i := 0; i < 5; i++ {
		if rec := post(h, `{"passphrase":"nope"}`, ""); rec.Code != http.StatusUnauthorized {
			t.Fatalf("hit %d: %d", i+1, rec.Code)
		}
	}
	if rec := post(h, `{"passphrase":"open sesame"}`, ""); rec.Code != http.StatusTooManyRequests {
		t.Fatalf("6th hit should be 429, got %d", rec.Code)
	}
}

func TestAuthTrustProxy(t *testing.T) {
	h := newHandler()
	h.TrustProxy = true
	for i := 0; i < 5; i++ {
		post(h, `{"passphrase":"nope"}`, "203.0.113.7, 10.0.0.1")
	}
	if rec := post(h, `{"passphrase":"nope"}`, "203.0.113.7"); rec.Code != http.StatusTooManyRequests {
		t.Fatalf("same forwarded IP should be limited, got %d", rec.Code)
	}
	if rec := post(h, `{"passphrase":"nope"}`, "203.0.113.8"); rec.Code != http.StatusUnauthorized {
		t.Fatalf("other forwarded IP should pass the limiter, got %d", rec.Code)
	}
}
