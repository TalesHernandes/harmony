package push

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"

	"call/internal/auth"
)

func newHandler(t *testing.T, withKeys bool) (*Handler, *auth.Tokens, *Store) {
	t.Helper()
	tokens := auth.NewTokens([]byte("test-secret-test-secret"))
	store, err := NewStore(filepath.Join(t.TempDir(), "push.json"))
	if err != nil {
		t.Fatal(err)
	}
	h := &Handler{Store: store, Tokens: tokens}
	if withKeys {
		h.Keys = mustKeys(t)
	}
	return h, tokens, store
}

func do(t *testing.T, h *Handler, method, path, token, body string) (int, map[string]any) {
	t.Helper()
	var reader *strings.Reader
	if body == "" {
		reader = strings.NewReader("")
	} else {
		reader = strings.NewReader(body)
	}
	req := httptest.NewRequest(method, path, reader)
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	var out map[string]any
	_ = json.Unmarshal(rec.Body.Bytes(), &out)
	return rec.Code, out
}

func TestKeyEndpoint(t *testing.T) {
	h, _, _ := newHandler(t, true)
	// No token: an application server key is public by definition, and the
	// page needs it before it can subscribe at all.
	code, body := do(t, h, http.MethodGet, "/push/key", "", "")
	if code != http.StatusOK {
		t.Fatalf("status %d", code)
	}
	if body["enabled"] != true || body["key"] != testPublic {
		t.Fatalf("body = %v", body)
	}
}

func TestKeyEndpointReportsDisabled(t *testing.T) {
	h, _, _ := newHandler(t, false)
	code, body := do(t, h, http.MethodGet, "/push/key", "", "")
	if code != http.StatusOK || body["enabled"] != false {
		t.Fatalf("status %d body %v: an unconfigured server must say so, not 404", code, body)
	}
	if _, ok := body["key"]; ok {
		t.Error("no key should be advertised when push is off")
	}
}

func TestSubscribeRequiresAToken(t *testing.T) {
	h, tokens, store := newHandler(t, true)
	sub := `{"endpoint":"https://push.example.org/a","keys":{"p256dh":"pk","auth":"au"}}`

	code, _ := do(t, h, http.MethodPost, "/push/subscribe", "", sub)
	if code != http.StatusUnauthorized {
		t.Fatalf("no token: status %d, want 401", code)
	}
	code, _ = do(t, h, http.MethodPost, "/push/subscribe", "not-a-token", sub)
	if code != http.StatusUnauthorized {
		t.Fatalf("bad token: status %d, want 401", code)
	}
	if store.Len() != 0 {
		t.Fatal("an unauthorized subscribe reached the store")
	}

	code, body := do(t, h, http.MethodPost, "/push/subscribe", tokens.Mint(), sub)
	if code != http.StatusOK || body["subscribed"] != true {
		t.Fatalf("status %d body %v", code, body)
	}
	if !store.Has("https://push.example.org/a") {
		t.Fatal("not stored")
	}
}

// The store feeds an outbound POST from this server. Anything that is not an
// https URL would make it a request forwarder for whoever wrote the row.
func TestSubscribeRejectsNonHTTPSEndpoints(t *testing.T) {
	h, tokens, store := newHandler(t, true)
	for _, endpoint := range []string{
		"http://push.example.org/a",
		"file:///etc/passwd",
		"https://",
		"",
	} {
		body, _ := json.Marshal(map[string]any{"endpoint": endpoint})
		code, _ := do(t, h, http.MethodPost, "/push/subscribe", tokens.Mint(), string(body))
		if code != http.StatusBadRequest {
			t.Errorf("endpoint %q: status %d, want 400", endpoint, code)
		}
	}
	if store.Len() != 0 {
		t.Fatal("something got through")
	}
}

// pushsubscriptionchange fires in the service worker, where there is no page
// and therefore no token. Knowing a live endpoint — a capability URL nobody
// else has — is what stands in for one, and it can only ever swap one
// registration for another.
func TestSubscribeCanRotateWithoutATokenButOnlyAKnownEndpoint(t *testing.T) {
	h, tokens, store := newHandler(t, true)
	old := "https://push.example.org/old"
	if _, _ = do(t, h, http.MethodPost, "/push/subscribe", tokens.Mint(),
		`{"endpoint":"`+old+`"}`); store.Len() != 1 {
		t.Fatal("setup failed")
	}

	// An unknown `replaces` buys nothing.
	code, _ := do(t, h, http.MethodPost, "/push/subscribe", "",
		`{"endpoint":"https://push.example.org/new","replaces":"https://push.example.org/never-seen"}`)
	if code != http.StatusUnauthorized {
		t.Fatalf("unknown replaces: status %d, want 401", code)
	}

	code, _ = do(t, h, http.MethodPost, "/push/subscribe", "",
		`{"endpoint":"https://push.example.org/new","replaces":"`+old+`"}`)
	if code != http.StatusOK {
		t.Fatalf("rotation: status %d", code)
	}
	if store.Has(old) {
		t.Error("the rotated-out endpoint should be gone")
	}
	if !store.Has("https://push.example.org/new") {
		t.Error("the new endpoint was not stored")
	}
	if store.Len() != 1 {
		t.Errorf("a rotation must swap, not add: Len = %d", store.Len())
	}
}

func TestUnsubscribe(t *testing.T) {
	h, tokens, store := newHandler(t, true)
	_, _ = do(t, h, http.MethodPost, "/push/subscribe", tokens.Mint(), `{"endpoint":"https://push.example.org/a"}`)

	code, _ := do(t, h, http.MethodPost, "/push/unsubscribe", "", `{"endpoint":"https://push.example.org/a"}`)
	if code != http.StatusUnauthorized {
		t.Fatalf("status %d, want 401", code)
	}
	if store.Len() != 1 {
		t.Fatal("an unauthorized unsubscribe took effect")
	}

	code, body := do(t, h, http.MethodPost, "/push/unsubscribe", tokens.Mint(), `{"endpoint":"https://push.example.org/a"}`)
	if code != http.StatusOK || body["subscribed"] != false {
		t.Fatalf("status %d body %v", code, body)
	}
	if store.Len() != 0 {
		t.Fatal("still stored")
	}
}

// The whole point of /push/status: "the server does not have my
// registration" has to be readable, or a dead doorbell looks exactly like a
// live one.
func TestStatusDistinguishesThisDeviceFromAnyDevice(t *testing.T) {
	h, tokens, _ := newHandler(t, true)
	_, _ = do(t, h, http.MethodPost, "/push/subscribe", tokens.Mint(), `{"endpoint":"https://push.example.org/a"}`)

	_, body := do(t, h, http.MethodGet, "/push/status?endpoint=https%3A%2F%2Fpush.example.org%2Fa", tokens.Mint(), "")
	if body["subscribed"] != true || body["enabled"] != true || body["count"] != float64(1) {
		t.Fatalf("this device: %v", body)
	}

	_, body = do(t, h, http.MethodGet, "/push/status?endpoint=https%3A%2F%2Fpush.example.org%2Fother", tokens.Mint(), "")
	if body["subscribed"] != false {
		t.Errorf("another device must not report as subscribed: %v", body)
	}
	if body["count"] != float64(1) {
		t.Errorf("count is about the server, not this device: %v", body)
	}

	code, _ := do(t, h, http.MethodGet, "/push/status", "", "")
	if code != http.StatusUnauthorized {
		t.Errorf("status without a token: %d", code)
	}
}

func TestSubscribeIsRefusedWhenPushIsDisabled(t *testing.T) {
	h, tokens, _ := newHandler(t, false)
	code, body := do(t, h, http.MethodPost, "/push/subscribe", tokens.Mint(), `{"endpoint":"https://push.example.org/a"}`)
	if code != http.StatusServiceUnavailable || body["error"] != "push_disabled" {
		t.Fatalf("status %d body %v: a server with no VAPID keys must say so", code, body)
	}
}

func TestMethodsAndUnknownPaths(t *testing.T) {
	h, tokens, _ := newHandler(t, true)
	if code, _ := do(t, h, http.MethodGet, "/push/subscribe", tokens.Mint(), ""); code != http.StatusMethodNotAllowed {
		t.Errorf("GET /push/subscribe: %d", code)
	}
	if code, _ := do(t, h, http.MethodPost, "/push/key", tokens.Mint(), ""); code != http.StatusMethodNotAllowed {
		t.Errorf("POST /push/key: %d", code)
	}
	if code, _ := do(t, h, http.MethodGet, "/push/nope", tokens.Mint(), ""); code != http.StatusNotFound {
		t.Errorf("unknown path: %d", code)
	}
}
