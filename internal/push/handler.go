package push

import (
	"encoding/json"
	"io"
	"net/http"
	"net/url"
	"strings"

	"call/internal/auth"
)

// maxBodyBytes caps a subscribe body. An endpoint URL plus two base64 keys is
// well under 1 KB.
const maxBodyBytes = 4 << 10

// Handler serves the legacy /push/ subscription endpoints when registered
// as a prefix on an HTTP mux. Harmony uses its own subscription handlers.
//
//	GET  /push/key          the VAPID public key (and whether push is on)
//	POST /push/subscribe    register a device
//	POST /push/unsubscribe  forget a device
//	GET  /push/status       does the server hold this device's registration
//
// Everything but /push/key requires the same bearer token as the WebSocket:
// the key is a public key by definition and the page needs it before it can
// subscribe at all.
type Handler struct {
	Keys   *Keys // nil when VAPID is not configured: push is off
	Store  *Store
	Tokens *auth.Tokens
}

func (h *Handler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	switch r.URL.Path {
	case "/push/key":
		h.key(w, r)
	case "/push/subscribe":
		h.subscribe(w, r)
	case "/push/unsubscribe":
		h.unsubscribe(w, r)
	case "/push/status":
		h.status(w, r)
	default:
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "not_found"})
	}
}

func (h *Handler) key(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	if h.Keys == nil {
		writeJSON(w, http.StatusOK, map[string]any{"enabled": false})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"enabled": true, "key": h.Keys.PublicKey()})
}

func (h *Handler) subscribe(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}
	if h.Keys == nil {
		writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "push_disabled"})
		return
	}
	var body struct {
		Endpoint string `json:"endpoint"`
		Keys     struct {
			P256dh string `json:"p256dh"`
			Auth   string `json:"auth"`
		} `json:"keys"`
		// Replaces carries the endpoint being rotated out. It is what lets
		// the service worker re-register from `pushsubscriptionchange`, an
		// event that fires with no page around and therefore no token: only
		// something that already knows a live endpoint — a capability URL
		// nobody else has — can use it, and it can only ever swap one
		// registration for another rather than add a new device.
		Replaces string `json:"replaces"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, maxBodyBytes)).Decode(&body); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad_request"})
		return
	}
	if !validEndpoint(body.Endpoint) {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad_endpoint"})
		return
	}
	if !h.authorized(r) && !(body.Replaces != "" && h.Store.Has(body.Replaces)) {
		writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "unauthorized"})
		return
	}
	if body.Replaces != "" && body.Replaces != body.Endpoint {
		if err := h.Store.Remove(body.Replaces); err != nil {
			writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "store"})
			return
		}
	}
	sub := Subscription{Endpoint: body.Endpoint, P256dh: body.Keys.P256dh, Auth: body.Keys.Auth}
	if err := h.Store.Add(sub); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "store"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"subscribed": true, "count": h.Store.Len()})
}

func (h *Handler) unsubscribe(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodPost) {
		return
	}
	if !h.authorized(r) {
		writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "unauthorized"})
		return
	}
	var body struct {
		Endpoint string `json:"endpoint"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, maxBodyBytes)).Decode(&body); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad_request"})
		return
	}
	if err := h.Store.Remove(body.Endpoint); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "store"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"subscribed": false, "count": h.Store.Len()})
}

// status is the antidote to the failure this feature is prone to: a doorbell
// that is off and looks fine. The page shows it, so "the server does not have
// my registration" is something you can read instead of something you find
// out by not being called.
func (h *Handler) status(w http.ResponseWriter, r *http.Request) {
	if !requireMethod(w, r, http.MethodGet) {
		return
	}
	if !h.authorized(r) {
		writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "unauthorized"})
		return
	}
	endpoint := r.URL.Query().Get("endpoint")
	writeJSON(w, http.StatusOK, map[string]any{
		"enabled":    h.Keys != nil,
		"subscribed": endpoint != "" && h.Store.Has(endpoint),
		"count":      h.Store.Len(),
	})
}

// authorized accepts the same token the WebSocket does, as a bearer header.
func (h *Handler) authorized(r *http.Request) bool {
	header := r.Header.Get("Authorization")
	token, ok := strings.CutPrefix(header, "Bearer ")
	if !ok {
		return false
	}
	return h.Tokens.Verify(strings.TrimSpace(token)) == nil
}

// validEndpoint keeps anything that is not an https URL out of the store —
// the store feeds an outbound POST, and a stored `file:` or `http://10.x`
// would make this server a request forwarder for whoever wrote it.
func validEndpoint(endpoint string) bool {
	if endpoint == "" || len(endpoint) > 1024 {
		return false
	}
	u, err := url.Parse(endpoint)
	return err == nil && u.Scheme == "https" && u.Host != ""
}

func requireMethod(w http.ResponseWriter, r *http.Request, method string) bool {
	if r.Method == method {
		return true
	}
	w.Header().Set("Allow", method)
	writeJSON(w, http.StatusMethodNotAllowed, map[string]string{"error": "method_not_allowed"})
	return false
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
