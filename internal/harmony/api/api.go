// Package api is harmony's HTTP surface: the invite-only signup, the login,
// the session cookie, and the handful of things a signed-in person may ask
// about themselves.
//
// Two rules hold everywhere in this file. Every handler that touches data goes
// through requireUser and passes that user's id into the store, so "can this
// person see this?" is answered by the query itself and not by a check somebody
// can forget. And nothing that is a secret — password, session token, invite
// code — is ever written to a log or to a URL.
package api

import (
	"bufio"
	"crypto/rand"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net"
	"net/http"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"call/internal/harmony/mail"
	"call/internal/harmony/passwd"
	"call/internal/harmony/push"
	"call/internal/harmony/sfu"
	"call/internal/harmony/store"
)

// SessionCookie is the name of the cookie holding the session token.
const SessionCookie = "harmony_session"

// Rate limits. Fifteen minutes of memory: long enough that guessing is
// hopeless, short enough that a friend who forgot their password is not locked
// out for the evening.
const (
	FailureWindow = 15 * time.Minute
	AccountLimit  = 5
	IPLimit       = 20
	SignupWindow  = time.Hour
	SignupIPLimit = 10
	// MaxRequestBody covers the JSON bodies only; an upload goes through
	// MaxBytesReader in files.go instead. It has to fit the longest message a
	// person may send, which is 4000 characters and can be four bytes each.
	MaxRequestBody = 64 << 10
)

// Config is what the server needs from its environment.
type Config struct {
	Mailer        mail.Mailer
	EmailRequired bool
	Store         *store.Store
	// BaseURL is the public origin, e.g. https://harmony.taleshernandes.com.
	// It builds invite links and rejects cross-site writes.
	BaseURL string
	// FilesDir is the directory uploaded images are written to. Nothing under
	// it is ever named by anything a person typed: see files.go.
	FilesDir string
	AppDir   string
	// DiagnosticsDir receives the programs' "Enviar diagnóstico" reports.
	DiagnosticsDir string
	// Secure marks the session cookie Secure. Off only for local development
	// over plain http.
	Secure bool
	// TrustProxy reads the client address from X-Forwarded-For. On only behind
	// Caddy: without a proxy in front, a client could set the header itself and
	// walk around the per-address limit.
	TrustProxy bool
	Logger     *slog.Logger
	// Params overrides the argon2id cost; tests turn it down.
	Params *passwd.Params
	// VAPID is the Web Push identity, from the environment. Nil switches off
	// the browsers' doorbell and changes nothing else; a server started without
	// the keys says so in the log rather than failing to start.
	VAPID *push.Keys
	// FCM is the Android app's doorbell (A6), the same way: nil is off. Either
	// one of the two is enough for notifications to be decided and kept.
	FCM *push.FCM
	// ICE is what a direct 1:1 call is told to use to find a path between two
	// browsers. Empty is a working server: a call between two machines on
	// ordinary home connections needs nothing here at all — see call.go.
	ICE ICEConfig
}

// ICEConfig is the address book a direct call uses to find a way through.
//
// It is only ever used by the 1:1 call: a voice channel goes through this
// machine's own media server, which has a public address and needs no help
// being found. The secret is read from the environment and is never written
// down anywhere else — a TURN secret in the repository would be an open relay
// for whoever cloned it.
type ICEConfig struct {
	STUNURLs   []string
	TURNURLs   []string
	TURNSecret []byte
}

// VoiceOptions is what the operator chooses about the media server. It is a
// separate call rather than a field of Config because opening a UDP socket can
// fail for reasons the person starting the server needs to hear about, and
// because a harmony without voice — the tests, the maintenance commands — must
// not open a port at all.
type VoiceOptions struct {
	// UDPPort is the single port media arrives on. Zero picks a free one,
	// which is what the tests want.
	UDPPort int
	// PublicIP is the address to advertise to browsers.
	PublicIP string
	// MaxPerRoom caps a voice channel. The plan asks for four to eight.
	MaxPerRoom int
}

// Server routes every harmony HTTP request.
type Server struct {
	previews  *previewService
	mailSlots chan struct{}
	cfg       Config
	st        *store.Store
	log       *slog.Logger
	params    passwd.Params
	mux       *http.ServeMux
	hub       *hub
	// media is swapped in by StartVoice and out by StopVoice, and it is read
	// from the media server's own goroutines, so it is a pointer that is
	// exchanged rather than a field that is assigned.
	media atomic.Pointer[sfu.SFU]
	// appSeen is the newest published Windows code the sweep has seen; 0
	// until the first sweep, which only records it.
	appSeen atomic.Int64
	// diagnostics allows one report a minute per account.
	diagnostics diagnosticGate

	// The D4 three: who is where, what a woken phone should be told, and the
	// thing that wakes it. All three are memory — see presence.go and notify.go
	// for why neither belongs in the database.
	presence *presenceTracker
	pending  *pendingStore
	rings    *ringLimiter
	// Every direct call in flight. Memory, like presence and like who is in a
	// voice room, and for the same reason: nobody is on a call after a restart.
	callBook *callBook
	vapid    *push.Keys
	fcm      *push.FCM
	notify   *notifier

	avatarMu  sync.Mutex
	dummyOnce sync.Once
	dummyHash string

	// hashing bounds how many password hashes run at the same time. Each
	// argon2id costs 64 MiB by design (passwd.Default) — that is what makes a
	// stolen database expensive to crack — so unbounded concurrency here is a
	// way to spend the machine's whole memory from the sign-in page, without an
	// account. The rate limits are the first fence and this is the second one,
	// because they count per address and per name while the memory is shared.
	hashing chan struct{}

	// verified is a test seam: it runs in handleLogin after the password
	// has been checked and before the session is written, which is the window
	// a password reset must not be able to slip through. Nil in production.
	verified func()
}

// New builds the router.
func New(cfg Config) *Server {
	if cfg.Logger == nil {
		cfg.Logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	}
	s := &Server{cfg: cfg, st: cfg.Store, log: cfg.Logger, params: passwd.Default}
	s.previews = newPreviewService()
	s.mailSlots = make(chan struct{}, 64)
	if cfg.Mailer == nil {
		s.cfg.EmailRequired = false
		s.log.Warn("email disabled; email requirement disabled")
	}
	if cfg.Params != nil {
		s.params = *cfg.Params
	}
	slots := runtime.GOMAXPROCS(0)
	if slots < 2 {
		slots = 2
	}
	s.hashing = make(chan struct{}, slots)
	s.hub = newHub()
	s.presence = newPresenceTracker()
	s.pending = newPendingStore()
	s.rings = newRingLimiter()
	s.callBook = newCallBook()
	s.fcm = cfg.FCM
	if cfg.VAPID != nil || cfg.FCM != nil {
		s.vapid = cfg.VAPID
		s.notify = newNotifier(s, cfg.VAPID, cfg.FCM)
	}
	s.mux = http.NewServeMux()
	s.routes()
	return s
}

// StartVoice opens the media server and connects it to this HTTP server. The
// two callbacks are the only way the media server reaches a person: one offer
// to one device, and one "who is inside" to a channel's members — and the
// member list comes from the store, never from the media server itself.
func (s *Server) StartVoice(opts VoiceOptions) error {
	media, err := sfu.New(sfu.Config{
		UDPPort:     opts.UDPPort,
		PublicIP:    opts.PublicIP,
		MaxPerRoom:  opts.MaxPerRoom,
		Logger:      s.log,
		Signal:      s.voiceSignal,
		RoomChanged: s.voiceRoomChanged,
	})
	if err != nil {
		return err
	}
	s.media.Store(media)
	return nil
}

// StopVoice hangs up on everybody and releases the port.
func (s *Server) StopVoice() error {
	media := s.media.Swap(nil)
	if media == nil {
		return nil
	}
	return media.Close()
}

// VoicePort reports the port the media server actually bound, for the log line
// and for the tests that asked for a free one.
func (s *Server) VoicePort() int {
	if media := s.media.Load(); media != nil {
		return media.UDPPort()
	}
	return 0
}

func (s *Server) routes() {
	s.securityRoutes()
	s.mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		w.Write([]byte("ok\n"))
	})

	s.mux.HandleFunc("POST /api/signup", s.handleSignup)
	s.mux.HandleFunc("POST /api/login", s.handleLogin)
	s.mux.HandleFunc("POST /api/logout", s.handleLogout)

	s.mux.HandleFunc("PUT /api/me/avatar", s.requireUser(s.handlePutAvatar))
	s.mux.HandleFunc("DELETE /api/me/avatar", s.requireUser(s.handleDeleteAvatar))
	s.mux.HandleFunc("GET /api/people/{username}/avatar", s.requireUser(s.handleGetAvatar))
	s.mux.HandleFunc("GET /api/me/blocks", s.requireUser(s.handlePersonPreferences))
	s.mux.HandleFunc("PUT /api/me/mutes/{username}", s.requireUser(s.handlePersonPreference(false, true)))
	s.mux.HandleFunc("DELETE /api/me/mutes/{username}", s.requireUser(s.handlePersonPreference(false, false)))
	s.mux.HandleFunc("PUT /api/me/blocks/{username}", s.requireUser(s.handlePersonPreference(true, true)))
	s.mux.HandleFunc("DELETE /api/me/blocks/{username}", s.requireUser(s.handlePersonPreference(true, false)))
	s.mux.HandleFunc("GET /api/me", s.requireUser(s.handleMe))
	s.mux.HandleFunc("POST /api/diagnostics", s.requireUser(s.handleDiagnostic))
	s.mux.HandleFunc("GET /api/app/android", s.requireUser(s.appReleaseHandler(androidApp)))
	s.mux.HandleFunc("GET /api/app/android/{name}", s.requireUser(s.appDownloadHandler(androidApp)))
	s.mux.HandleFunc("GET /api/app/windows", s.requireUser(s.appReleaseHandler(windowsApp)))
	s.mux.HandleFunc("GET /api/app/windows/{name}", s.requireUser(s.appDownloadHandler(windowsApp)))
	s.mux.HandleFunc("GET /api/sessions", s.requireUser(s.handleListSessions))
	s.mux.HandleFunc("DELETE /api/sessions/{id}", s.requireUser(s.handleRevokeSession))
	s.mux.HandleFunc("POST /api/sessions/revoke-others", s.requireUser(s.handleRevokeOthers))
	s.mux.HandleFunc("GET /api/invites", s.requireUser(s.handleListInvites))
	s.mux.HandleFunc("POST /api/invites", s.requireUser(s.handleCreateInvite))
	s.mux.HandleFunc("DELETE /api/invites/{id}", s.requireUser(s.handleRevokeInvite))

	s.mux.HandleFunc("GET /api/admin/accounts", s.requireUser(s.handleAdminAccounts))
	for _, action := range []string{"disable", "enable", "reset-password"} {
		s.mux.HandleFunc("POST /api/admin/accounts/{username}/"+action, s.requireUser(func(w http.ResponseWriter, r *http.Request, cu ctxUser) {
			r.SetPathValue("action", action)
			s.handleAdminAccounts(w, r, cu)
		}))
	}

	s.mux.HandleFunc("GET /api/members", s.requireUser(s.handleMembers))
	s.mux.HandleFunc("GET /api/search", s.requireUser(s.handleSearch))
	s.mux.HandleFunc("GET /api/channels", s.requireUser(s.handleListChannels))
	s.mux.HandleFunc("POST /api/channels", s.requireUser(s.handleCreateChannel))
	s.mux.HandleFunc("GET /api/channels/{id}", s.requireUser(s.handleGetChannel))
	s.mux.HandleFunc("DELETE /api/channels/{id}", s.requireUser(s.handleDeleteChannel))
	s.mux.HandleFunc("PATCH /api/channels/{id}", s.requireUser(s.handleRenameChannel))
	s.mux.HandleFunc("GET /api/channels/{id}/pins", s.requireUser(s.handlePins))
	s.mux.HandleFunc("POST /api/categories", s.requireUser(s.handleCreateCategory))
	s.mux.HandleFunc("PATCH /api/categories/{id}", s.requireUser(s.handleRenameCategory))
	s.mux.HandleFunc("DELETE /api/categories/{id}", s.requireUser(s.handleDeleteCategory))
	s.mux.HandleFunc("PUT /api/layout", s.requireUser(s.handleSaveLayout))
	s.mux.HandleFunc("GET /api/roles", s.requireUser(s.handleRoles))
	s.mux.HandleFunc("POST /api/roles", s.requireUser(s.handleCreateRole))
	s.mux.HandleFunc("PUT /api/roles/order", s.requireUser(s.handleOrderRoles))
	s.mux.HandleFunc("PATCH /api/roles/{id}", s.requireUser(s.handleUpdateRole))
	s.mux.HandleFunc("DELETE /api/roles/{id}", s.requireUser(s.handleDeleteRole))
	s.mux.HandleFunc("PUT /api/roles/{id}/members/{username}", s.requireUser(s.handleRoleMember(true)))
	s.mux.HandleFunc("DELETE /api/roles/{id}/members/{username}", s.requireUser(s.handleRoleMember(false)))
	s.mux.HandleFunc("GET /api/channels/{id}/messages", s.requireUser(s.handleHistory))
	s.mux.HandleFunc("POST /api/channels/{id}/messages", s.requireUser(s.handlePostMessage))
	s.mux.HandleFunc("POST /api/channels/{id}/read", s.requireUser(s.handleMarkRead))
	s.mux.HandleFunc("POST /api/channels/{id}/typing", s.requireUser(s.handleTyping))
	s.mux.HandleFunc("POST /api/channels/{id}/mute", s.requireUser(s.handleMute))
	s.mux.HandleFunc("POST /api/channels/{id}/hide", s.requireUser(s.handleHide))
	s.mux.HandleFunc("POST /api/channels/{id}/uploads", s.requireUser(s.handleUpload))
	s.mux.HandleFunc("PATCH /api/messages/{id}", s.requireUser(s.handleEditMessage))
	s.mux.HandleFunc("DELETE /api/messages/{id}", s.requireUser(s.handleDeleteMessage))
	s.mux.HandleFunc("POST /api/messages/{id}/reactions", s.requireUser(s.handleReact))
	s.mux.HandleFunc("POST /api/messages/{id}/pin", s.requireUser(s.handlePin(true)))
	s.mux.HandleFunc("DELETE /api/messages/{id}/pin", s.requireUser(s.handlePin(false)))
	s.mux.HandleFunc("POST /api/dms", s.requireUser(s.handleOpenDM))
	// The direct 1:1 call. Everything here is an introduction between two
	// browsers; no audio and no picture ever passes through this server.
	s.mux.HandleFunc("POST /api/calls", s.requireUser(s.handleCallStart))
	s.mux.HandleFunc("GET /api/calls/{id}", s.requireUser(s.handleCallGet))
	s.mux.HandleFunc("POST /api/calls/{id}/accept", s.requireUser(s.handleCallAccept))
	s.mux.HandleFunc("POST /api/calls/{id}/hangup", s.requireUser(s.handleCallHangup))
	s.mux.HandleFunc("POST /api/calls/{id}/signal", s.requireUser(s.handleCallSignal))
	s.mux.HandleFunc("POST /api/calls/{id}/state", s.requireUser(s.handleCallState))
	s.mux.HandleFunc("PUT /api/people/{username}/nickname", s.requireUser(s.handleNickname))
	s.mux.HandleFunc("GET /api/files/{id}", s.requireUser(s.handleFile))
	s.mux.HandleFunc("GET /api/previews/{id}", s.requireUser(s.handlePreviewImage))
	s.mux.HandleFunc("POST /api/voice/{id}/join", s.requireUser(s.handleVoiceJoin))
	s.mux.HandleFunc("POST /api/voice/answer", s.requireUser(s.handleVoiceAnswer))
	s.mux.HandleFunc("POST /api/voice/restart", s.requireUser(s.handleVoiceRestart))
	// The one negotiation the browser starts, and only for a screen. See
	// sfu.ClientOffer for why it has to be that way round.
	s.mux.HandleFunc("POST /api/voice/offer", s.requireUser(s.handleVoiceOffer))
	s.mux.HandleFunc("POST /api/voice/layer", s.requireUser(s.handleVoiceLayer))
	s.mux.HandleFunc("POST /api/voice/output-delay", s.requireUser(s.handleVoiceOutputDelay))
	s.mux.HandleFunc("POST /api/voice/candidate", s.requireUser(s.handleVoiceCandidate))
	s.mux.HandleFunc("POST /api/voice/publish", s.requireUser(s.handleVoicePublish))
	s.mux.HandleFunc("POST /api/voice/unpublish", s.requireUser(s.handleVoiceUnpublish))
	s.mux.HandleFunc("POST /api/voice/leave", s.requireUser(s.handleVoiceLeave))
	s.mux.HandleFunc("POST /api/voice/state", s.requireUser(s.handleVoiceState))
	s.mux.HandleFunc("POST /api/voice/watch", s.requireUser(s.handleVoiceWatch))
	s.mux.HandleFunc("POST /api/voice/{id}/ring", s.requireUser(s.handleVoiceRing))
	s.mux.HandleFunc("GET /api/voice/rooms", s.requireUser(s.handleVoiceRooms))
	s.mux.HandleFunc("GET /api/voice/stats", s.requireUser(s.handleVoiceStats))

	// Presence and the doorbell (D4).
	s.mux.HandleFunc("GET /api/presence", s.requireUser(s.handlePresence))
	s.mux.HandleFunc("POST /api/presence", s.requireUser(s.handleSetPresence))
	s.mux.HandleFunc("POST /api/push/fcm", s.requireUser(s.handleFCMRegister))
	s.mux.HandleFunc("POST /api/push/fcm/remove", s.requireUser(s.handleFCMRemove))
	s.mux.HandleFunc("GET /api/push/key", s.requireUser(s.handlePushKey))
	s.mux.HandleFunc("GET /api/push/status", s.requireUser(s.handlePushStatus))
	s.mux.HandleFunc("POST /api/push/subscribe", s.requireUser(s.handlePushSubscribe))
	s.mux.HandleFunc("POST /api/push/unsubscribe", s.requireUser(s.handlePushUnsubscribe))
	s.mux.HandleFunc("GET /api/notifications/pending", s.requireUser(s.handlePendingNotifications))

	s.mux.HandleFunc("GET /ws", s.requireUser(s.handleGateway))

	s.registerPages()
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	setSecurityHeaders(w)
	if r.URL.Path == "/api/me" || strings.HasPrefix(r.URL.Path, "/api/me/") || strings.HasPrefix(r.URL.Path, "/api/login") || strings.HasPrefix(r.URL.Path, "/api/password/") {
		w.Header().Set("Cache-Control", "no-store")
	}
	start := time.Now()
	rec := &recorder{ResponseWriter: w, status: http.StatusOK}
	s.mux.ServeHTTP(rec, r)
	// r.URL.Path only. The query string is where an invite code travels, and a
	// log line is the easiest place in the world to leak one.
	s.log.Info("http",
		"method", r.Method, "path", r.URL.Path,
		"status", rec.status, "ms", time.Since(start).Milliseconds())
}

type recorder struct {
	http.ResponseWriter
	status int
}

// Unwrap lets http.ResponseController reach the connection through the
// wrapper. Without it a handler's SetReadDeadline answers "not supported" and
// does nothing — the upload deadlines depend on this.
func (r *recorder) Unwrap() http.ResponseWriter { return r.ResponseWriter }

func (r *recorder) WriteHeader(code int) {
	r.status = code
	r.ResponseWriter.WriteHeader(code)
}

// Hijack passes the connection through to the gateway. Without it the wrapper
// above hides the one capability a WebSocket upgrade needs, and the upgrade
// fails with a 500 that says nothing about why.
func (r *recorder) Hijack() (net.Conn, *bufio.ReadWriter, error) {
	h, ok := r.ResponseWriter.(http.Hijacker)
	if !ok {
		return nil, nil, http.ErrNotSupported
	}
	r.status = http.StatusSwitchingProtocols
	return h.Hijack()
}

// setSecurityHeaders is applied to every response, error pages included.
//
// The CSP is deliberately without unsafe-inline: the page's script and style
// are separate files, so a stray bit of injected markup cannot execute. HSTS is
// also set by Caddy; having it here means the rule survives a change of proxy.
func setSecurityHeaders(w http.ResponseWriter) {
	h := w.Header()
	// script-src is spelled out only to add one word to it, and the word is
	// worth a paragraph.
	//
	// 'wasm-unsafe-eval' is what lets this page compile WebAssembly, and the
	// page needs it for exactly one thing: the noise suppressor that runs on
	// this device before a voice leaves it (D4e, web/harmony/src/media.ts).
	//
	// It is a real loosening and it is the narrow one. 'unsafe-eval' would let
	// a string of text become running code — eval, new Function, a timer
	// handed a string — which is the mechanism half of the attacks this policy
	// exists for are built on. 'wasm-unsafe-eval' allows none of that: it
	// permits WebAssembly.compile and nothing else, on bytes this page fetched
	// from this origin, and JavaScript remains as locked as it was. Everything
	// else stays: no inline script, no inline style, nothing from anywhere but
	// here.
	h.Set("Content-Security-Policy",
		"default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; base-uri 'none'; "+
			"frame-ancestors 'none'; form-action 'self'; object-src 'none'; "+
			"img-src 'self' data:; connect-src 'self'")
	h.Set("Strict-Transport-Security", "max-age=31536000; includeSubDomains")
	h.Set("X-Content-Type-Options", "nosniff")
	h.Set("Referrer-Policy", "no-referrer")
	h.Set("Cross-Origin-Opener-Policy", "same-origin")
	h.Set("Permissions-Policy", "geolocation=(), payment=(), usb=()")
}

// ---------------------------------------------------------------- helpers

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

// fail answers with a code and a short machine-readable reason. The reason is
// never more specific than the caller is entitled to: a wrong password and an
// unknown username both come back as bad_credentials.
func fail(w http.ResponseWriter, status int, reason string) {
	writeJSON(w, status, map[string]string{"error": reason})
}

func decode(w http.ResponseWriter, r *http.Request, v any) bool {
	if ct := r.Header.Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
		fail(w, http.StatusUnsupportedMediaType, "expected_json")
		return false
	}
	dec := json.NewDecoder(io.LimitReader(r.Body, MaxRequestBody))
	dec.DisallowUnknownFields()
	if err := dec.Decode(v); err != nil {
		fail(w, http.StatusBadRequest, "bad_request")
		return false
	}
	return true
}

// sameOrigin rejects state-changing requests that a different site started.
// The SameSite=Lax cookie already refuses to travel on a cross-site POST; this
// is the belt to that pair of braces, and it is what a test can assert.
func (s *Server) sameOrigin(r *http.Request) bool {
	if s.cfg.BaseURL == "" {
		return true // development, no fixed origin to compare against
	}
	origin := r.Header.Get("Origin")
	if origin == "" {
		// Non-browser clients (curl, the deploy check) send no Origin. A
		// browser always does on fetch(), which is the only way this API is
		// called from a page.
		return true
	}
	return strings.EqualFold(strings.TrimSuffix(origin, "/"), strings.TrimSuffix(s.cfg.BaseURL, "/"))
}

func (s *Server) clientIP(r *http.Request) string {
	if s.cfg.TrustProxy {
		if xff := r.Header.Get("X-Forwarded-For"); xff != "" {
			first, _, _ := strings.Cut(xff, ",")
			return strings.TrimSpace(first)
		}
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

// dummy is a real argon2id hash of a random string, verified against whenever
// the username does not exist. Without it, "no such user" returns in a
// microsecond and "wrong password" in sixty milliseconds, and that difference
// is a list of who is a member.
func (s *Server) dummy() string {
	s.dummyOnce.Do(func() {
		var seed [32]byte
		_, _ = rand.Read(seed[:])
		h, err := passwd.Hash(string(seed[:]), s.params)
		if err != nil {
			h = "$argon2id$v=19$m=65536,t=3,p=2$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
		}
		s.dummyHash = h
	})
	return s.dummyHash
}

// ---------------------------------------------------------------- auth

type ctxUser struct {
	user store.User
	sess store.Session
}

func (s *Server) currentUser(r *http.Request) (ctxUser, bool) {
	c, err := r.Cookie(SessionCookie)
	if err != nil || c.Value == "" {
		return ctxUser{}, false
	}
	sess, user, err := s.st.Authenticate(c.Value)
	if err != nil {
		return ctxUser{}, false
	}
	return ctxUser{user: user, sess: sess}, true
}

// requireUser is the only door into anything that reads or writes data.
func (s *Server) requireUser(h func(http.ResponseWriter, *http.Request, ctxUser)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		cu, ok := s.currentUser(r)
		if !ok {
			clearSessionCookie(w, s.cfg.Secure)
			fail(w, http.StatusUnauthorized, "unauthenticated")
			return
		}
		if r.Method != http.MethodGet && !s.sameOrigin(r) {
			fail(w, http.StatusForbidden, "bad_origin")
			return
		}
		if s.cfg.EmailRequired && !cu.user.EmailVerified && !emailGateAllowed(r.URL.Path) {
			fail(w, http.StatusForbidden, "email_required")
			return
		}
		h(w, r, cu)
	}
}

func setSessionCookie(w http.ResponseWriter, token string, expires time.Time, secure bool) {
	http.SetCookie(w, &http.Cookie{
		Name:     SessionCookie,
		Value:    token,
		Path:     "/",
		Expires:  expires,
		HttpOnly: true,
		Secure:   secure,
		SameSite: http.SameSiteLaxMode,
	})
}

func clearSessionCookie(w http.ResponseWriter, secure bool) {
	http.SetCookie(w, &http.Cookie{
		Name: SessionCookie, Value: "", Path: "/", MaxAge: -1,
		HttpOnly: true, Secure: secure, SameSite: http.SameSiteLaxMode,
	})
}

// ---------------------------------------------------------------- signup

type signupRequest struct {
	Email       string `json:"email"`
	Code        string `json:"code"`
	Username    string `json:"username"`
	DisplayName string `json:"display_name"`
	Password    string `json:"password"`
}

func (s *Server) handleSignup(w http.ResponseWriter, r *http.Request) {
	if !s.sameOrigin(r) {
		fail(w, http.StatusForbidden, "bad_origin")
		return
	}
	ip := s.clientIP(r)
	// A cheap early no, so an address that is already locked out is not told
	// what is wrong with its username. The answer that counts is the booking
	// further down, which is atomic; this one only saves work.
	if n, err := s.st.Failures(store.FailSignup, ip, SignupWindow); err != nil || n >= SignupIPLimit {
		fail(w, http.StatusTooManyRequests, "rate_limited")
		return
	}
	var req signupRequest
	if !decode(w, r, &req) {
		return
	}
	// Deliberately no mailAvailable here: a server that came up without its
	// mail settings must not lock the only door in. The address is kept,
	// unconfirmed, and the confirmation screen asks again once mail works.
	req.Email = store.NormalizeEmail(req.Email)
	if !store.ValidEmail(req.Email) {
		fail(w, http.StatusBadRequest, "bad_email")
		return
	}

	username := store.NormalizeUsername(req.Username)
	display := strings.TrimSpace(req.DisplayName)
	if display == "" {
		display = username
	}
	if !store.ValidUsername(username) {
		fail(w, http.StatusBadRequest, "bad_username")
		return
	}
	if len([]rune(display)) > 40 {
		fail(w, http.StatusBadRequest, "bad_display_name")
		return
	}
	if len(req.Password) < passwd.MinLength || len(req.Password) > passwd.MaxLength {
		fail(w, http.StatusBadRequest, "weak_password")
		return
	}
	// Spend an invite attempt whether or not the code is good, so guessing
	// codes is rate limited exactly like guessing passwords. Spent before the
	// hashing, and counted in the same breath, for the reason in the login.
	if !s.spendSignupAttempt(ip) {
		fail(w, http.StatusTooManyRequests, "rate_limited")
		return
	}

	release, held := s.holdHashSlot(r)
	if !held {
		return // the client hung up while waiting its turn
	}
	defer release()

	hash, err := passwd.Hash(req.Password, s.params)
	if err != nil {
		s.log.Error("hash", "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	user, err := s.st.SignUp(req.Code, username, display, hash, req.Email)
	switch {
	case errors.Is(err, store.ErrInviteBad):
		fail(w, http.StatusForbidden, "invite_unusable")
		return
	case errors.Is(err, store.ErrNameTaken):
		fail(w, http.StatusConflict, "signup_unavailable")
		return
	case err != nil:
		s.log.Error("signup", "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	_ = s.st.ClearFailures(store.FailSignup, ip)
	s.log.Info("account created", "user_id", user.ID, "admin", user.IsAdmin)
	if err := s.sendVerification(user.ID, user.Email); err != nil {
		s.log.Error("verification creation failed", "user_id", user.ID, "purpose", "verify_email")
	}
	s.startSession(w, r, user, http.StatusCreated)
}

// ---------------------------------------------------------------- login

type loginRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

func (s *Server) handleLogin(w http.ResponseWriter, r *http.Request) {
	if !s.sameOrigin(r) {
		fail(w, http.StatusForbidden, "bad_origin")
		return
	}
	var req loginRequest
	if !decode(w, r, &req) {
		return
	}
	username := store.NormalizeUsername(req.Username)
	ip := s.clientIP(r)
	user, hash, lookupErr := s.st.UserByIdentifier(username)
	accountKey := username
	if lookupErr == nil {
		accountKey = user.Username
	}

	// The attempt is spent before the password is checked, not after. Counting
	// first and recording later left argon2id — a tenth of a second — between
	// the count and the write, and twenty attempts sent together all read the
	// same count and all went through. Deliberately the same answer for the
	// address and for the account, and the same one a locked account gets
	// whether or not it exists.
	ipAllowed, ipReceipt, ipErr := s.st.ReserveAttemptID(store.FailIP, ip, FailureWindow, IPLimit)
	if ipErr != nil || !ipAllowed {
		if ipErr != nil {
			s.log.Error("reserve attempt", "kind", store.FailIP, "err", ipErr)
		}
		fail(w, http.StatusTooManyRequests, "rate_limited")
		return
	}
	allowed, receipt, reserveErr := s.st.ReserveAttemptID(store.FailAccount, accountKey, FailureWindow, AccountLimit)
	if reserveErr != nil || !allowed {
		fail(w, http.StatusTooManyRequests, "rate_limited")
		return
	}

	release, ok := s.holdHashSlot(r)
	if !ok {
		return // the client hung up while waiting its turn
	}
	defer release()

	if lookupErr != nil {
		// Burn the same CPU an existing account would have cost.
		_, _ = passwd.Verify(s.dummy(), req.Password)
		fail(w, http.StatusUnauthorized, "bad_credentials")
		return
	}
	good, err := passwd.Verify(hash, req.Password)
	if err != nil {
		s.log.Error("verify", "err", err, "user_id", user.ID)
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	if !good || user.Disabled {
		fail(w, http.StatusUnauthorized, "bad_credentials")
		return
	}
	if user.TwoFA != "" {
		if err := s.st.RefundAttempt(store.FailAccount, accountKey, receipt); err != nil {
			fail(w, 500, "server_error")
			return
		}
		_ = s.st.RefundAttempt(store.FailIP, ip, ipReceipt)
		if user.TwoFA == "email" && !s.mailAvailable(w) {
			return
		}
		code := ""
		if user.TwoFA == "email" {
			code, err = store.NewEmailCode()
			if err != nil {
				fail(w, 500, "server_error")
				return
			}
		}
		token, err := s.st.IssueProof(user.ID, "login_challenge", "", code, 10*time.Minute, store.ProofBinding{PasswordHash: hash, Version: user.SecurityVersion})
		if err != nil {
			fail(w, 500, "server_error")
			return
		}
		if user.TwoFA == "email" {
			s.sendCode(user.ID, user.Email, "login_challenge", code)
		}
		writeJSON(w, http.StatusOK, map[string]string{"twofa": user.TwoFA, "challenge": token})
		return
	}
	// The account's own failures go; the address only gets this attempt back.
	// Wiping the address too let a member who knows one password reset the
	// counter between guesses at everybody else's.
	_ = s.st.ClearFailures(store.FailAccount, accountKey)
	_ = s.st.RefundAttempt(store.FailIP, ip, ipReceipt)
	if s.verified != nil {
		s.verified()
	}
	// The hash that was just verified goes with the session request: between
	// the read above and the insert below somebody may have reset this
	// password, and a session born from the old one would outlive it.
	s.startSessionForHash(w, r, user, hash, http.StatusOK)
}

// spendAttempt books one attempt against kind/key and says whether it may
// proceed. An empty key is not a key — a login with no username reaches here
// and there is nothing to count it against. A database error denies: the
// fence's job is to be closed when it cannot tell.
func (s *Server) spendAttempt(kind, key string, limit int) bool {
	if key == "" {
		return true
	}
	ok, err := s.st.ReserveAttempt(kind, key, FailureWindow, limit)
	if err != nil {
		s.log.Error("reserve attempt", "kind", kind, "err", err)
		return false
	}
	return ok
}

// spendSignupAttempt books one sign-up attempt against an address.
func (s *Server) spendSignupAttempt(ip string) bool {
	ok, err := s.st.ReserveAttempt(store.FailSignup, ip, SignupWindow, SignupIPLimit)
	if err != nil {
		s.log.Error("reserve signup attempt", "err", err)
		return false
	}
	return ok
}

// holdHashSlot waits for one of the argon2id slots and returns the function
// that gives it back. It returns false when the caller went away first, which
// is the honest answer to "should I still spend 64 MiB on this?".
func (s *Server) holdHashSlot(r *http.Request) (func(), bool) {
	select {
	case s.hashing <- struct{}{}:
		return func() { <-s.hashing }, true
	case <-r.Context().Done():
		return func() {}, false
	}
}

func (s *Server) startSession(w http.ResponseWriter, r *http.Request, user store.User, status int) {
	s.startSessionForHash(w, r, user, "", status)
}

// startSessionForHash issues the cookie. When hash is not empty it is the
// password hash this sign-in verified, and the session is only created while
// the account still has that hash — see store.CreateSessionForHash. A sign-up
// passes nothing: there is no earlier hash to have gone stale.
func (s *Server) startSessionForHash(w http.ResponseWriter, r *http.Request, user store.User, hash string, status int) {
	token, sess, err := s.st.CreateSessionForHash(user.ID, hash, r.UserAgent(), store.SessionTTL, user.SecurityVersion)
	if errors.Is(err, store.ErrCredentials) {
		// The password changed while we were checking the old one. Whoever did
		// that ended every session on purpose; this one must not be the
		// exception that survives it.
		s.log.Info("login raced a password change", "user_id", user.ID)
		fail(w, http.StatusUnauthorized, "bad_credentials")
		return
	}
	if err != nil {
		s.log.Error("session", "err", err, "user_id", user.ID)
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	setSessionCookie(w, token, sess.ExpiresAt, s.cfg.Secure)
	if status != http.StatusCreated {
		s.newAccess(user, r.UserAgent())
	}
	writeJSON(w, status, map[string]any{"user": ownUserJSON(user)})
}

func (s *Server) handleLogout(w http.ResponseWriter, r *http.Request) {
	if !s.sameOrigin(r) {
		fail(w, http.StatusForbidden, "bad_origin")
		return
	}
	if cu, ok := s.currentUser(r); ok {
		_ = s.st.RevokeSession(cu.user.ID, cu.sess.ID)
		s.voiceHangUp(cu.sess.ID)
		s.hub.dropSession(cu.user.ID, cu.sess.ID)
	}
	clearSessionCookie(w, s.cfg.Secure)
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// ---------------------------------------------------------------- me

func userJSON(u store.User) map[string]any {
	return map[string]any{
		"username":     u.Username,
		"display_name": u.DisplayName,
		"avatar":       u.Avatar,
		"is_admin":     u.IsAdmin,
		"created_at":   u.CreatedAt.Format(time.RFC3339),
	}
}

func (s *Server) handleMe(w http.ResponseWriter, _ *http.Request, cu ctxUser) {
	writeJSON(w, http.StatusOK, map[string]any{
		"user":           ownUserJSON(cu.user),
		"email_required": s.cfg.EmailRequired,
		"session_id":     cu.sess.ID,
		// So the client can tell "você não ligou as notificações" apart from
		// "este servidor não tem notificação", which are two different
		// sentences for the person reading them.
		"push_enabled": s.pushEnabled(),
		// What roles let this person do (D12c), the owner's being all of it.
		// The pages and apps draw buttons from it; the server still decides.
		"permissions": s.permissionsOf(cu.user.ID).Names(),
	})
}

// ---------------------------------------------------------------- sessions

func (s *Server) handleListSessions(w http.ResponseWriter, _ *http.Request, cu ctxUser) {
	list, err := s.st.ListSessions(cu.user.ID)
	if err != nil {
		s.log.Error("sessions", "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	out := make([]map[string]any, 0, len(list))
	for _, sess := range list {
		out = append(out, map[string]any{
			"id":           sess.ID,
			"created_at":   sess.CreatedAt.Format(time.RFC3339),
			"last_seen_at": sess.LastSeenAt.Format(time.RFC3339),
			"expires_at":   sess.ExpiresAt.Format(time.RFC3339),
			"user_agent":   sess.UserAgent,
			"current":      sess.ID == cu.sess.ID,
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{"sessions": out})
}

func (s *Server) handleRevokeSession(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	err := s.st.RevokeSession(cu.user.ID, r.PathValue("id"))
	if err == nil {
		s.voiceHangUp(r.PathValue("id"))
		// And the socket, which is the part that was still delivering. Cutting
		// the microphone and leaving the conversation flowing is half a
		// revocation.
		s.hub.dropSession(cu.user.ID, r.PathValue("id"))
	}
	if errors.Is(err, store.ErrNotFound) {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	if err != nil {
		s.log.Error("revoke session", "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (s *Server) handleRevokeOthers(w http.ResponseWriter, _ *http.Request, cu ctxUser) {
	n, err := s.st.RevokeAllSessions(cu.user.ID, cu.sess.ID)
	if media := s.media.Load(); err == nil && media != nil {
		// Every device of this person except the one asking. Leaving one of
		// them in a voice room after cutting its login would be a microphone
		// nobody can see and nobody can switch off.
		media.LeaveAllExceptLogin(cu.user.ID, cu.sess.ID)
	}
	if err == nil {
		s.hub.dropAllExcept(cu.user.ID, cu.sess.ID)
	}
	if err != nil {
		s.log.Error("revoke others", "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	writeJSON(w, http.StatusOK, map[string]int{"revoked": n})
}

// ---------------------------------------------------------------- invites

func (s *Server) inviteJSON(inv store.Invite) map[string]any {
	return map[string]any{
		"id":         inv.ID,
		"note":       inv.Note,
		"created_at": inv.CreatedAt.Format(time.RFC3339),
		"expires_at": inv.ExpiresAt.Format(time.RFC3339),
		"used":       inv.Used(),
		// Who let whom in. Empty creator means the command line, before any
		// account existed.
		"created_by": inv.CreatedByName,
		"used_by":    inv.UsedByName,
	}
}

func (s *Server) handleListInvites(w http.ResponseWriter, _ *http.Request, cu ctxUser) {
	// The owner sees every invite ever minted, the bootstrap one included;
	// anybody else sees the ones they created, which since invites became
	// owner-only is none.
	list, err := s.st.ListInvites(cu.user.ID)
	if cu.user.IsAdmin {
		list, err = s.st.ListAllInvites()
	}
	if err != nil {
		s.log.Error("invites", "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	out := make([]map[string]any, 0, len(list))
	for _, inv := range list {
		out = append(out, s.inviteJSON(inv))
	}
	writeJSON(w, http.StatusOK, map[string]any{"invites": out, "can_invite": cu.user.IsAdmin})
}

// handleCreateInvite is owner-only. The service is invite-only because the
// owner decides who is in it; letting every member mint invites would quietly
// turn that into "anyone already inside can bring anyone", which is a different
// service.
func (s *Server) handleCreateInvite(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	if !cu.user.IsAdmin {
		fail(w, http.StatusForbidden, "not_allowed")
		return
	}
	var req struct {
		Note string `json:"note"`
	}
	if r.ContentLength > 0 && !decode(w, r, &req) {
		return
	}
	if len([]rune(req.Note)) > 60 {
		fail(w, http.StatusBadRequest, "note_too_long")
		return
	}
	code, inv, err := s.st.CreateInvite(cu.user.ID, strings.TrimSpace(req.Note), store.InviteTTL)
	if err != nil {
		s.log.Error("create invite", "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	s.log.Info("invite created", "user_id", cu.user.ID, "invite_id", inv.ID)
	out := s.inviteJSON(inv)
	// The only moment the code exists outside the database.
	out["code"] = code
	out["url"] = strings.TrimSuffix(s.cfg.BaseURL, "/") + "/signup?code=" + code
	writeJSON(w, http.StatusCreated, out)
}

func (s *Server) handleRevokeInvite(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	// The owner can cancel any unused invite, including the bootstrap one that
	// has no creator. Anybody else is scoped to their own, so another person's
	// id answers what a made-up id answers.
	// One or the other, never both: running the owner's version after the
	// member's one asked twice for the same row, and the second ask answered
	// "not found" about a row the first had just deleted — a cancellation that
	// worked, reported as a failure.
	var err error
	if cu.user.IsAdmin {
		err = s.st.RevokeAnyInvite(r.PathValue("id"))
	} else {
		err = s.st.RevokeInvite(cu.user.ID, r.PathValue("id"))
	}
	if errors.Is(err, store.ErrNotFound) {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	if err != nil {
		s.log.Error("revoke invite", "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}
