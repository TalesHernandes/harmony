// This file is PLANO-DISCORD.md §5 turned into tests. Each section below names
// the line of the plan it stands for, so a rule that gets quietly dropped shows
// up here as a failure rather than as an assumption nobody rechecked.
package api

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"io"
	"io/fs"
	"log/slog"
	"net/http"
	"net/http/cookiejar"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"

	"call/internal/harmony/passwd"
	"call/internal/harmony/store"
	"fmt"
)

// cheap argon2 settings: the format rules are the passwd package's job, and a
// realistic cost here would make this suite take minutes.
var testParams = passwd.Params{Memory: 8 * 1024, Time: 1, Parallelism: 1, SaltLen: 16, KeyLen: 32}

const (
	baseURL    = "https://harmony.taleshernandes.com"
	defaultIP  = "203.0.113.10"
	goodPass   = "uma senha bem comprida"
	goodPass2  = "outra senha bem comprida"
	agentPhone = "test-phone"
)

// safeBuffer collects log output; the server writes from its own goroutines, so
// the -race build needs the lock.
type safeBuffer struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *safeBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(p)
}

func (b *safeBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}

type harness struct {
	t        *testing.T
	st       *store.Store
	api      *Server
	srv      *httptest.Server
	logs     *safeBuffer
	dbPath   string
	filesDir string
}

// newHarness builds a real server over a real SQLite file. The file matters:
// two of the tests below read its bytes to prove what is not written into it.
func newHarness(t *testing.T) *harness {
	return newHarnessTweaked(t, nil)
}

// newHarnessTweaked is the same, with a chance to change one setting first. The
// direct call's relay is configured this way: it is off everywhere else,
// because a laptop and a test have no relay and must not need one.
func newHarnessTweaked(t *testing.T, tweak func(*Config)) *harness {
	t.Helper()
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "harmony.db")
	filesDir := filepath.Join(dir, "files")
	if err := os.MkdirAll(filesDir, 0o700); err != nil {
		t.Fatal(err)
	}
	st, err := store.Open(dbPath)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })

	logs := &safeBuffer{}
	cfg := Config{
		Store:    st,
		Mailer:   &capturingMailer{},
		FilesDir: filesDir,
		BaseURL:  baseURL,
		Secure:   true,
		// On, so a test can choose the address an attempt appears to come from.
		// In production this is only safe because Caddy sits in front and
		// rewrites the header; see docs/DEPLOY.md.
		TrustProxy: true,
		Logger:     slog.New(slog.NewTextHandler(logs, &slog.HandlerOptions{Level: slog.LevelDebug})),
		Params:     &testParams,
	}
	if tweak != nil {
		tweak(&cfg)
	}
	handler := New(cfg)
	srv := httptest.NewServer(handler)
	t.Cleanup(srv.Close)

	return &harness{t: t, st: st, api: handler, srv: srv, logs: logs, dbPath: dbPath, filesDir: filesDir}
}

// device is one browser: its own cookie jar, its own apparent address.
type device struct {
	h  *harness
	c  *http.Client
	ip string
	ua string
}

func (h *harness) device(ua string) *device {
	jar, err := cookiejar.New(nil)
	if err != nil {
		h.t.Fatal(err)
	}
	return &device{h: h, ip: defaultIP, ua: ua, c: &http.Client{
		Jar: httpsJar{jar},
		// Follow nothing: the tests care about the redirect itself.
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
	}}
}

// httpsJar files and reads cookies as if every URL were https. The session
// cookie is Secure, as it must be in production, but httptest serves plain
// http on 127.0.0.1: cookiejar only sends Secure cookies there from Go 1.26
// on, which treats loopback as secure like browsers do. Before that the jar
// keeps the cookie and never sends it, and every request after signup is
// anonymous. The device stands for a browser on https behind Caddy, so the
// jar is told so instead of depending on the Go version that runs the tests.
type httpsJar struct{ http.CookieJar }

func (j httpsJar) SetCookies(u *url.URL, cookies []*http.Cookie) {
	j.CookieJar.SetCookies(asHTTPS(u), cookies)
}

func (j httpsJar) Cookies(u *url.URL) []*http.Cookie {
	return j.CookieJar.Cookies(asHTTPS(u))
}

func asHTTPS(u *url.URL) *url.URL {
	if u.Scheme != "http" && u.Scheme != "ws" {
		return u
	}
	c := *u
	c.Scheme = "https"
	return &c
}

type reply struct {
	status int
	body   map[string]any
	raw    string
	header http.Header
}

func (r reply) errorCode() string {
	s, _ := r.body["error"].(string)
	return s
}

func (d *device) do(method, path string, body any, tweak ...func(*http.Request)) reply {
	d.h.t.Helper()
	var r io.Reader
	if body != nil {
		encoded, err := json.Marshal(body)
		if err != nil {
			d.h.t.Fatal(err)
		}
		r = bytes.NewReader(encoded)
	}
	req, err := http.NewRequest(method, d.h.srv.URL+path, r)
	if err != nil {
		d.h.t.Fatal(err)
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	req.Header.Set("X-Forwarded-For", d.ip)
	req.Header.Set("User-Agent", d.ua)
	if method != http.MethodGet && method != http.MethodHead {
		req.Header.Set("Origin", baseURL)
	}
	for _, f := range tweak {
		f(req)
	}
	resp, err := d.c.Do(req)
	if err != nil {
		d.h.t.Fatal(err)
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)
	out := reply{status: resp.StatusCode, raw: string(raw), header: resp.Header}
	_ = json.Unmarshal(raw, &out.body)
	return out
}

func (d *device) get(path string, tweak ...func(*http.Request)) reply {
	return d.do(http.MethodGet, path, nil, tweak...)
}

// sessionCookie returns the raw token this device is holding, which several
// tests need in order to prove it is nowhere else.
func (d *device) sessionCookie() string {
	for _, c := range d.c.Jar.Cookies(mustURL(d.h.t, d.h.srv.URL)) {
		if c.Name == SessionCookie {
			return c.Value
		}
	}
	return ""
}

func mustURL(t *testing.T, s string) *url.URL {
	t.Helper()
	u, err := url.Parse(s)
	if err != nil {
		t.Fatal(err)
	}
	return u
}

// bootstrapInvite mints the invite the owner would make from the command line.
func (h *harness) bootstrapInvite() string {
	h.t.Helper()
	code, _, err := h.st.CreateInvite(0, "bootstrap", store.InviteTTL)
	if err != nil {
		h.t.Fatal(err)
	}
	return code
}

// join signs an account up with code and returns the signed-in device.
func (h *harness) join(code, username, password string) *device {
	h.t.Helper()
	d := h.device(agentPhone)
	res := d.do(http.MethodPost, "/api/signup", map[string]string{
		"email": "signup-test-" + store.NormalizeUsername(fmt.Sprint(time.Now().UnixNano())) + "@example.org",
		"code":  code, "username": username, "display_name": username, "password": password,
	})
	if res.status != http.StatusCreated {
		h.t.Fatalf("signup for %s: %d %s", username, res.status, res.raw)
	}
	return d
}

// inviteFrom is one member inviting the next, through the API the owner uses.
func (d *device) inviteFrom(note string) (code, id string) {
	d.h.t.Helper()
	res := d.do(http.MethodPost, "/api/invites", map[string]string{"note": note})
	if res.status != http.StatusCreated {
		d.h.t.Fatalf("create invite: %d %s", res.status, res.raw)
	}
	code, _ = res.body["code"].(string)
	id, _ = res.body["id"].(string)
	if code == "" || id == "" {
		d.h.t.Fatalf("invite response has no code or id: %s", res.raw)
	}
	return code, id
}

// ---------------------------------------------------------------------------
// §5 — "Cadastro só com convite válido, de uso único, que expira.
//        Sem convite, a página de cadastro nem existe."
// ---------------------------------------------------------------------------

// TestDeviceKeepsItsSecureSession guards the harness rather than the server:
// if the device stops sending its Secure cookie to the plain-http test
// server, every test that signs in fails with a 401 far from the cause.
func TestDeviceKeepsItsSecureSession(t *testing.T) {
	h := newHarness(t)
	d := h.join(h.bootstrapInvite(), "tales", goodPass)
	if d.sessionCookie() == "" {
		t.Fatal("the jar does not hand back the Secure session cookie for the test server")
	}
	if res := d.get("/api/me"); res.status != http.StatusOK {
		t.Fatalf("GET /api/me right after signup: %d %s", res.status, res.raw)
	}
}

func TestSignupNeedsAValidInvite(t *testing.T) {
	h := newHarness(t)
	code := h.bootstrapInvite()

	cases := []struct {
		name string
		code string
	}{
		{"no code at all", ""},
		{"made up code", "AAAAAAAAAAAAAAAAAAAAAAAAAA"},
		{"the real code with a character changed", flip(code)},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			d := h.device("stranger")
			res := d.do(http.MethodPost, "/api/signup", map[string]string{
				"email": "signup-test-" + store.NormalizeUsername(fmt.Sprint(time.Now().UnixNano())) + "@example.org",
				"code":  tc.code, "username": "estranho", "display_name": "Estranho", "password": goodPass,
			})
			if res.status != http.StatusForbidden || res.errorCode() != "invite_unusable" {
				t.Fatalf("got %d %s, want 403 invite_unusable", res.status, res.raw)
			}
		})
	}
	if n, _ := h.st.CountUsers(); n != 0 {
		t.Fatalf("%d accounts exist after only invalid attempts", n)
	}
}

// The signup page is not merely guarded, it is absent: same 404 as any address
// nobody ever defined, and nothing that tells a stranger a code once existed.
func TestSignupPageDoesNotExistWithoutAUsableInvite(t *testing.T) {
	h := newHarness(t)
	code := h.bootstrapInvite()
	d := h.device("stranger")

	unknown := d.get("/nao-existe-mesmo")
	for _, path := range []string{"/signup", "/signup?code=", "/signup?code=" + flip(code)} {
		res := d.get(path)
		if res.status != http.StatusNotFound {
			t.Fatalf("GET %s = %d, want 404", path, res.status)
		}
		if res.raw != unknown.raw {
			t.Fatalf("GET %s answers differently from an unknown address; that difference is the tell", path)
		}
	}
	if res := d.get("/signup?code=" + code); res.status != http.StatusOK {
		t.Fatalf("GET /signup with a good code = %d, want 200", res.status)
	}
}

func TestInviteIsSingleUse(t *testing.T) {
	h := newHarness(t)
	code := h.bootstrapInvite()
	h.join(code, "tales", goodPass)

	second := h.device("stranger")
	res := second.do(http.MethodPost, "/api/signup", map[string]string{
		"email": "signup-test-" + store.NormalizeUsername(fmt.Sprint(time.Now().UnixNano())) + "@example.org",
		"code":  code, "username": "outro", "display_name": "Outro", "password": goodPass,
	})
	if res.status != http.StatusForbidden || res.errorCode() != "invite_unusable" {
		t.Fatalf("reusing an invite: %d %s, want 403 invite_unusable", res.status, res.raw)
	}
	// And the page for that code is gone too.
	if page := second.get("/signup?code=" + code); page.status != http.StatusNotFound {
		t.Fatalf("the signup page for a spent invite = %d, want 404", page.status)
	}
}

func TestInviteExpires(t *testing.T) {
	h := newHarness(t)
	code := h.bootstrapInvite()
	h.st.SetClock(func() time.Time { return time.Now().Add(store.InviteTTL + time.Hour) })

	d := h.device("stranger")
	res := d.do(http.MethodPost, "/api/signup", map[string]string{
		"email": "signup-test-" + store.NormalizeUsername(fmt.Sprint(time.Now().UnixNano())) + "@example.org",
		"code":  code, "username": "atrasado", "display_name": "Atrasado", "password": goodPass,
	})
	if res.status != http.StatusForbidden {
		t.Fatalf("expired invite: %d %s, want 403", res.status, res.raw)
	}
}

func TestSignupRefusesWeakPasswordsAndBadNames(t *testing.T) {
	h := newHarness(t)
	code := h.bootstrapInvite()
	d := h.device("stranger")

	cases := []struct {
		name, username, password, want string
	}{
		{"short password", "tales", "curta1", "weak_password"},
		{"empty password", "tales", "", "weak_password"},
		{"username with a space", "ta les", goodPass, "bad_username"},
		{"username too short", "ta", goodPass, "bad_username"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			res := d.do(http.MethodPost, "/api/signup", map[string]string{
				"email": "signup-test-" + store.NormalizeUsername(fmt.Sprint(time.Now().UnixNano())) + "@example.org",
				"code":  code, "username": tc.username, "display_name": "X", "password": tc.password,
			})
			if res.errorCode() != tc.want {
				t.Fatalf("got %d %s, want %s", res.status, res.raw, tc.want)
			}
		})
	}
	// None of that spent the invite.
	if !h.st.InviteUsable(code) {
		t.Fatal("a rejected signup consumed the invite")
	}
}

// ---------------------------------------------------------------------------
// §5 — "Senha guardada com argon2id (nunca em texto, nunca com hash fraco)."
// ---------------------------------------------------------------------------

func TestPasswordIsArgon2idAndTheDatabaseNeverHoldsIt(t *testing.T) {
	h := newHarness(t)
	h.join(h.bootstrapInvite(), "tales", goodPass)

	var stored string
	if err := h.st.DB().QueryRow(`SELECT password_hash FROM users WHERE username = 'tales'`).Scan(&stored); err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(stored, "$argon2id$v=19$") {
		t.Fatalf("stored password is not argon2id: %q", stored)
	}
	assertNotOnDisk(t, h.dbPath, map[string]string{"the password": goodPass})
}

// assertNotOnDisk searches every file SQLite keeps for this database — the main
// file, the write-ahead log and the shared-memory index — because a secret that
// is only absent from one of the three is still on the disk.
func assertNotOnDisk(t *testing.T, dbPath string, secrets map[string]string) {
	t.Helper()
	for _, suffix := range []string{"", "-wal", "-shm"} {
		data, err := os.ReadFile(dbPath + suffix)
		if os.IsNotExist(err) {
			continue
		}
		if err != nil {
			t.Fatal(err)
		}
		for what, secret := range secrets {
			if secret == "" {
				t.Fatalf("%s is empty; the test would pass for the wrong reason", what)
			}
			if bytes.Contains(data, []byte(secret)) {
				t.Fatalf("%s is written in the clear inside %s", what, filepath.Base(dbPath+suffix))
			}
		}
	}
}

// ---------------------------------------------------------------------------
// §5 — "Sessão por token aleatório de 256 bits, com validade e revogação."
// ---------------------------------------------------------------------------

func TestSessionCookieIs256RandomBitsAndIsNotStored(t *testing.T) {
	h := newHarness(t)
	d := h.join(h.bootstrapInvite(), "tales", goodPass)

	token := d.sessionCookie()
	raw, err := base64.RawURLEncoding.DecodeString(token)
	if err != nil {
		t.Fatalf("the session token is not the base64url of raw bytes: %v", err)
	}
	if len(raw) != store.SessionTokenBytes {
		t.Fatalf("session token carries %d bytes, want %d (256 bits)", len(raw), store.SessionTokenBytes)
	}
	assertNotOnDisk(t, h.dbPath, map[string]string{"the session token": token})
}

func TestSessionCookieFlags(t *testing.T) {
	h := newHarness(t)
	d := h.device("phone")
	res := d.do(http.MethodPost, "/api/signup", map[string]string{
		"email": "signup-test-" + store.NormalizeUsername(fmt.Sprint(time.Now().UnixNano())) + "@example.org",
		"code":  h.bootstrapInvite(), "username": "tales", "display_name": "Tales", "password": goodPass,
	})
	set := res.header.Get("Set-Cookie")
	for _, flag := range []string{"HttpOnly", "Secure", "SameSite=Lax", "Path=/"} {
		if !strings.Contains(set, flag) {
			t.Errorf("session cookie is missing %s: %q", flag, set)
		}
	}
}

func TestSignedOutRequestsAreRefused(t *testing.T) {
	h := newHarness(t)
	h.join(h.bootstrapInvite(), "tales", goodPass)
	stranger := h.device("stranger")

	for _, path := range []string{"/api/me", "/api/sessions", "/api/invites"} {
		if res := stranger.get(path); res.status != http.StatusUnauthorized {
			t.Errorf("GET %s without a session = %d, want 401", path, res.status)
		}
	}
	if res := stranger.do(http.MethodPost, "/api/invites", map[string]string{}); res.status != http.StatusUnauthorized {
		t.Errorf("creating an invite without a session = %d, want 401", res.status)
	}
	// A cookie that was never issued is not a session either.
	forged := h.device("forger")
	res := forged.get("/api/me", func(r *http.Request) {
		r.AddCookie(&http.Cookie{Name: SessionCookie, Value: strings.Repeat("A", 43)})
	})
	if res.status != http.StatusUnauthorized {
		t.Errorf("a made-up token = %d, want 401", res.status)
	}
}

func TestLogoutAndRevokingOtherDevices(t *testing.T) {
	h := newHarness(t)
	code := h.bootstrapInvite()
	phone := h.join(code, "tales", goodPass)

	// The same person on a second device.
	pc := h.device("pc")
	if res := pc.do(http.MethodPost, "/api/login", map[string]string{"username": "tales", "password": goodPass}); res.status != http.StatusOK {
		t.Fatalf("login on a second device: %d %s", res.status, res.raw)
	}
	if res := pc.get("/api/sessions"); len(res.body["sessions"].([]any)) != 2 {
		t.Fatalf("the device list does not show both devices: %s", res.raw)
	}

	if res := pc.do(http.MethodPost, "/api/sessions/revoke-others", map[string]string{}); res.status != http.StatusOK {
		t.Fatalf("revoke others: %d %s", res.status, res.raw)
	}
	if res := phone.get("/api/me"); res.status != http.StatusUnauthorized {
		t.Fatalf("the other device is still signed in: %d", res.status)
	}
	if res := pc.get("/api/me"); res.status != http.StatusOK {
		t.Fatalf("the device that asked was signed out too: %d", res.status)
	}

	if res := pc.do(http.MethodPost, "/api/logout", map[string]string{}); res.status != http.StatusOK {
		t.Fatalf("logout: %d %s", res.status, res.raw)
	}
	if res := pc.get("/api/me"); res.status != http.StatusUnauthorized {
		t.Fatal("the session survived logging out")
	}
}

// ---------------------------------------------------------------------------
// §5 — "Toda leitura checa se quem pede pode ver aquilo: uma pessoa não lê a
//        DM de outra trocando um número na URL."
//
// D0 has no messages yet, so the objects with ids are sessions and invites.
// The shape of the test is the one D1 will reuse: A creates, B asks for it by
// id, B is told the same thing a made-up id is told.
// ---------------------------------------------------------------------------

func TestOnePersonCannotReachAnothersData(t *testing.T) {
	h := newHarness(t)
	owner := h.join(h.bootstrapInvite(), "tales", goodPass)
	inviteForB, _ := owner.inviteFrom("namorada")
	other := h.join(inviteForB, "namorada", goodPass2)

	// A's own things.
	ownerMe := owner.get("/api/me")
	ownerSessionID, _ := ownerMe.body["session_id"].(string)
	_, ownerInviteID := owner.inviteFrom("um amigo")

	t.Run("B cannot log A's device out", func(t *testing.T) {
		res := other.do(http.MethodDelete, "/api/sessions/"+ownerSessionID, nil)
		if res.status != http.StatusNotFound {
			t.Fatalf("got %d %s, want 404", res.status, res.raw)
		}
		if got := owner.get("/api/me"); got.status != http.StatusOK {
			t.Fatal("A was logged out by B")
		}
	})

	t.Run("B cannot cancel A's invite", func(t *testing.T) {
		res := other.do(http.MethodDelete, "/api/invites/"+ownerInviteID, nil)
		if res.status != http.StatusNotFound {
			t.Fatalf("got %d %s, want 404", res.status, res.raw)
		}
	})

	t.Run("a stranger id and another person's id look the same", func(t *testing.T) {
		real := other.do(http.MethodDelete, "/api/invites/"+ownerInviteID, nil)
		fake := other.do(http.MethodDelete, "/api/invites/does-not-exist", nil)
		if real.status != fake.status || real.raw != fake.raw {
			t.Fatalf("existing-but-not-yours (%d %s) differs from nonexistent (%d %s)",
				real.status, real.raw, fake.status, fake.raw)
		}
	})

	t.Run("B's lists hold only B's rows", func(t *testing.T) {
		sessions := other.get("/api/sessions").body["sessions"].([]any)
		for _, s := range sessions {
			if s.(map[string]any)["id"] == ownerSessionID {
				t.Fatal("A's session is in B's device list")
			}
		}
		if invites := other.get("/api/invites").body["invites"].([]any); len(invites) != 0 {
			t.Fatalf("B sees %d invites that are not B's", len(invites))
		}
	})

	t.Run("A's invite still works after B tried to cancel it", func(t *testing.T) {
		list := owner.get("/api/invites").body["invites"].([]any)
		// A is the owner, so the list is every invite there is: the bootstrap
		// one A signed up with, B's, and the one A just opened.
		if len(list) != 3 {
			t.Fatalf("A sees %d invites, want 3", len(list))
		}
		for _, raw := range list {
			if raw.(map[string]any)["id"] == ownerInviteID {
				return
			}
		}
		t.Fatal("the invite B tried to cancel is gone")
	})
}

// ---------------------------------------------------------------------------
// "Contas só por convite do dono": the owner decides who is in the server.
// A member being able to mint invites would quietly turn an invite-only
// service into "anyone already inside can bring anyone".
// ---------------------------------------------------------------------------

func TestOnlyTheOwnerCanInvite(t *testing.T) {
	h := newHarness(t)
	owner := h.join(h.bootstrapInvite(), "tales", goodPass)
	codeForB, inviteB := owner.inviteFrom("namorada")
	member := h.join(codeForB, "namorada", goodPass2)

	t.Run("a member is refused", func(t *testing.T) {
		res := member.do(http.MethodPost, "/api/invites", map[string]string{"note": "um amigo meu"})
		if res.status != http.StatusForbidden || res.errorCode() != "not_allowed" {
			t.Fatalf("got %d %s, want 403 not_allowed", res.status, res.raw)
		}
	})

	t.Run("and is told so before the button is drawn", func(t *testing.T) {
		if can, _ := member.get("/api/invites").body["can_invite"].(bool); can {
			t.Fatal("the member is told it may invite")
		}
		if can, _ := owner.get("/api/invites").body["can_invite"].(bool); !can {
			t.Fatal("the owner is told it may not invite")
		}
	})

	t.Run("a member cannot cancel the owner's invite either", func(t *testing.T) {
		_, open := owner.inviteFrom("outro amigo")
		if res := member.do(http.MethodDelete, "/api/invites/"+open, nil); res.status != http.StatusNotFound {
			t.Fatalf("got %d %s, want 404", res.status, res.raw)
		}
	})

	t.Run("no invite was created by any of that", func(t *testing.T) {
		list := owner.get("/api/invites").body["invites"].([]any)
		// The bootstrap one, namorada's, and the one just opened.
		if len(list) != 3 {
			t.Fatalf("%d invites exist, want 3", len(list))
		}
	})

	t.Run("the owner sees who let whom in", func(t *testing.T) {
		var spent map[string]any
		for _, raw := range owner.get("/api/invites").body["invites"].([]any) {
			if inv := raw.(map[string]any); inv["id"] == inviteB {
				spent = inv
			}
		}
		if spent == nil {
			t.Fatal("the spent invite is not in the owner's list")
		}
		if spent["created_by"] != "tales" || spent["used_by"] != "namorada" {
			t.Fatalf("created_by=%v used_by=%v, want tales and namorada",
				spent["created_by"], spent["used_by"])
		}
	})

	// The bootstrap invite has no creator, and it is exactly the one a
	// per-creator query would hide from the person who most needs to see it.
	t.Run("including the one made from the command line", func(t *testing.T) {
		for _, raw := range owner.get("/api/invites").body["invites"].([]any) {
			if inv := raw.(map[string]any); inv["created_by"] == "" && inv["used_by"] == "tales" {
				return
			}
		}
		t.Fatal("the bootstrap invite is missing from the owner's list")
	})
}

// A signed-in person still cannot be made to act by another site: the cookie is
// SameSite=Lax and the origin is checked as well.
func TestCrossSiteWriteIsRefused(t *testing.T) {
	h := newHarness(t)
	owner := h.join(h.bootstrapInvite(), "tales", goodPass)

	before := len(owner.get("/api/invites").body["invites"].([]any))

	evil := func(r *http.Request) { r.Header.Set("Origin", "https://nao-e-o-harmony.example") }
	res := owner.do(http.MethodPost, "/api/invites", map[string]string{"note": "roubado"}, evil)
	if res.status != http.StatusForbidden || res.errorCode() != "bad_origin" {
		t.Fatalf("cross-site invite creation: %d %s, want 403 bad_origin", res.status, res.raw)
	}
	if after := len(owner.get("/api/invites").body["invites"].([]any)); after != before {
		t.Fatalf("the cross-site request created an invite anyway (%d -> %d)", before, after)
	}
}

// ---------------------------------------------------------------------------
// §5 — "Limite de tentativas de login por conta e por IP."
// ---------------------------------------------------------------------------

func TestLoginIsLimitedPerAccount(t *testing.T) {
	h := newHarness(t)
	h.join(h.bootstrapInvite(), "tales", goodPass)

	// Each attempt from a different address, so only the per-account counter
	// can be what stops this.
	for i := 0; i < AccountLimit; i++ {
		d := h.device("guesser")
		d.ip = "198.51.100." + string(rune('1'+i))
		res := d.do(http.MethodPost, "/api/login", map[string]string{"username": "tales", "password": "chute errado"})
		if res.status != http.StatusUnauthorized {
			t.Fatalf("attempt %d: %d %s, want 401", i+1, res.status, res.raw)
		}
	}
	fresh := h.device("guesser")
	fresh.ip = "198.51.100.200"
	res := fresh.do(http.MethodPost, "/api/login", map[string]string{"username": "tales", "password": "chute errado"})
	if res.status != http.StatusTooManyRequests {
		t.Fatalf("attempt %d: %d %s, want 429", AccountLimit+1, res.status, res.raw)
	}
	// And the lockout holds even for the right password, which is the point.
	right := h.device("tales")
	right.ip = "198.51.100.201"
	if res := right.do(http.MethodPost, "/api/login", map[string]string{"username": "tales", "password": goodPass}); res.status != http.StatusTooManyRequests {
		t.Fatalf("locked account with the right password: %d, want 429", res.status)
	}

	// It is a window, not a ban: past it, the account works again.
	h.st.SetClock(func() time.Time { return time.Now().Add(FailureWindow + time.Minute) })
	after := h.device("tales")
	after.ip = "198.51.100.202"
	if res := after.do(http.MethodPost, "/api/login", map[string]string{"username": "tales", "password": goodPass}); res.status != http.StatusOK {
		t.Fatalf("after the window: %d %s, want 200", res.status, res.raw)
	}
}

func TestLoginIsLimitedPerAddress(t *testing.T) {
	h := newHarness(t)
	h.join(h.bootstrapInvite(), "tales", goodPass)

	// A different username every time, so no account counter reaches its limit
	// and the address counter is the only thing left.
	d := h.device("botnet")
	d.ip = "192.0.2.66"
	for i := 0; i < IPLimit; i++ {
		res := d.do(http.MethodPost, "/api/login", map[string]string{
			"username": "alvo" + strings.Repeat("x", i), "password": "chute errado",
		})
		if res.status != http.StatusUnauthorized {
			t.Fatalf("attempt %d: %d %s, want 401", i+1, res.status, res.raw)
		}
	}
	if res := d.do(http.MethodPost, "/api/login", map[string]string{"username": "tales", "password": goodPass}); res.status != http.StatusTooManyRequests {
		t.Fatalf("attempt %d from the same address: %d, want 429", IPLimit+1, res.status)
	}
	// Somebody else's connection is untouched.
	other := h.device("tales")
	other.ip = "192.0.2.99"
	if res := other.do(http.MethodPost, "/api/login", map[string]string{"username": "tales", "password": goodPass}); res.status != http.StatusOK {
		t.Fatalf("an unrelated address was caught by the limit: %d %s", res.status, res.raw)
	}
}

func TestSuccessfulLoginForgetsTheFailures(t *testing.T) {
	h := newHarness(t)
	h.join(h.bootstrapInvite(), "tales", goodPass)

	d := h.device("tales")
	for i := 0; i < AccountLimit-1; i++ {
		d.do(http.MethodPost, "/api/login", map[string]string{"username": "tales", "password": "errei"})
	}
	if res := d.do(http.MethodPost, "/api/login", map[string]string{"username": "tales", "password": goodPass}); res.status != http.StatusOK {
		t.Fatalf("login after four mistakes: %d %s", res.status, res.raw)
	}
	for i := 0; i < AccountLimit-1; i++ {
		res := d.do(http.MethodPost, "/api/login", map[string]string{"username": "tales", "password": "errei"})
		if res.status != http.StatusUnauthorized {
			t.Fatalf("the old failures were still being counted after a success: %d", res.status)
		}
	}
}

// An unknown username and a wrong password must be indistinguishable, or the
// login form becomes a way to ask who is a member.
func TestLoginDoesNotRevealWhoExists(t *testing.T) {
	h := newHarness(t)
	h.join(h.bootstrapInvite(), "tales", goodPass)
	d := h.device("stranger")
	d.ip = "192.0.2.7"

	wrongPassword := d.do(http.MethodPost, "/api/login", map[string]string{"username": "tales", "password": "errado"})
	noSuchUser := d.do(http.MethodPost, "/api/login", map[string]string{"username": "ninguem", "password": "errado"})
	if wrongPassword.status != noSuchUser.status || wrongPassword.raw != noSuchUser.raw {
		t.Fatalf("wrong password (%d %s) differs from unknown user (%d %s)",
			wrongPassword.status, wrongPassword.raw, noSuchUser.status, noSuchUser.raw)
	}
}

// ---------------------------------------------------------------------------
// §5 — "Logs sem conteúdo de mensagem, sem senha, sem token."
// ---------------------------------------------------------------------------

func TestLogsNeverCarryASecret(t *testing.T) {
	h := newHarness(t)
	code := h.bootstrapInvite()
	owner := h.join(code, "tales", goodPass)
	inviteCode, _ := owner.inviteFrom("namorada")
	token := owner.sessionCookie()

	// Exercise the paths most likely to log too much: a bad login, a rejected
	// signup, and a page fetched with the code in the query string.
	owner.do(http.MethodPost, "/api/login", map[string]string{"username": "tales", "password": goodPass + "!"})
	owner.get("/signup?code=" + inviteCode)
	owner.get("/api/me")

	logs := h.logs.String()
	if logs == "" {
		t.Fatal("nothing was logged; this test would pass for the wrong reason")
	}
	for what, secret := range map[string]string{
		"a password":      goodPass,
		"a session token": token,
		"an invite code":  inviteCode,
		"a spent invite":  code,
	} {
		if strings.Contains(logs, secret) {
			t.Errorf("%s appears in the logs", what)
		}
	}
	// Specifically: the query string never gets logged, because that is where
	// invite codes travel.
	if strings.Contains(logs, "code=") {
		t.Error("a query string was logged")
	}
}

// ---------------------------------------------------------------------------
// §5 — "HTTPS obrigatório, HSTS" and "sem serviços de terceiros".
// ---------------------------------------------------------------------------

func TestEveryResponseCarriesTheSecurityHeaders(t *testing.T) {
	h := newHarness(t)
	d := h.device("browser")

	want := map[string]string{
		"Strict-Transport-Security":  "max-age=31536000; includeSubDomains",
		"X-Content-Type-Options":     "nosniff",
		"Referrer-Policy":            "no-referrer",
		"Cross-Origin-Opener-Policy": "same-origin",
	}
	// Including the error pages: a 404 and a 401 are responses too.
	for _, path := range []string{"/login", "/nao-existe", "/api/me", "/static/index.html"} {
		res := d.get(path)
		for header, value := range want {
			if got := res.header.Get(header); got != value {
				t.Errorf("%s: %s = %q, want %q", path, header, got, value)
			}
		}
		csp := res.header.Get("Content-Security-Policy")
		for _, directive := range []string{"default-src 'self'", "frame-ancestors 'none'", "object-src 'none'"} {
			if !strings.Contains(csp, directive) {
				t.Errorf("%s: CSP is missing %q (got %q)", path, directive, csp)
			}
		}
		// 'wasm-unsafe-eval' is allowed and 'unsafe-eval' is not, and the
		// difference is the whole point: the first permits compiling
		// WebAssembly this page fetched from this origin (the noise
		// suppressor, D4e) and nothing else; the second would let a string of
		// text become running JavaScript, which is the mechanism this policy
		// exists to remove. Removing the one word we do allow and testing the
		// rest is how both stay true.
		bare := strings.ReplaceAll(csp, "'wasm-unsafe-eval'", "")
		if strings.Contains(bare, "unsafe-inline") || strings.Contains(bare, "unsafe-eval") {
			t.Errorf("%s: CSP allows inline code: %q", path, csp)
		}
		if !strings.Contains(csp, "script-src 'self' 'wasm-unsafe-eval'") {
			t.Errorf("%s: script-src is not the one D4e set: %q", path, csp)
		}
	}
}

// Nothing in the pages may be fetched from anywhere but this server: no CDN, no
// font service, no analytics. The rule is easier to keep than to recover, so it
// is checked on the files themselves.
//
// The client became a compiled bundle in D1, and a bundle does contain two
// kinds of address that are not fetches: the XML namespace strings the DOM
// library carries, and the licence banner at the top of the stylesheet. Both
// are named below and nothing else is allowed, so a CDN or a font service
// appearing in a later build still fails here. The HTML itself, which is the
// only file that can *declare* something to load, must have no absolute
// address at all.
var addressRE = regexp.MustCompile(`https?://([a-zA-Z0-9.:-]+)`)

var addressesThatAreNotFetches = map[string]string{
	"www.w3.org":      "XML namespace identifiers, never requested",
	"tailwindcss.com": "the licence banner in the generated stylesheet",
	// Both of these live inside the *text of an error message* in the noise
	// suppressor's worklet — "this is not supported in most browsers, see
	// <link>" — so they are only ever read by a person debugging, and only if
	// the thing has already gone wrong. Verified by hand when the dependency
	// came in (D4e). Nothing in that file fetches anything.
	"emscripten.org": "a documentation link inside an error message in the rnnoise worklet",
	"rolldown.rs":    "a documentation link inside an error message in the rnnoise worklet",
	// The Apache licence header of a Rust crate, compiled into DeepFilterNet's
	// wasm as text (25/09/2026). The module cannot fetch anything even if it
	// wanted to: its only imports are a random-number source, a Float32Array
	// constructor and an error thrower, checked by hand when it came in.
	"www.apache.org": "the licence header of a crate inside the DeepFilterNet wasm",
}

func TestPagesLoadNothingFromAThirdParty(t *testing.T) {
	err := fs.WalkDir(assets, ".", func(path string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		body, err := assets.ReadFile(path)
		if err != nil {
			return err
		}
		for _, marker := range []string{"//cdn", "src=\"//", "href=\"//"} {
			if bytes.Contains(body, []byte(marker)) {
				t.Errorf("%s refers to %q: everything must be served from this machine", path, marker)
			}
		}
		for _, m := range addressRE.FindAllSubmatch(body, -1) {
			host := string(m[1])
			if strings.HasSuffix(path, ".html") {
				t.Errorf("%s names %s: a page may not name an address off this machine at all", path, host)
				continue
			}
			if _, ok := addressesThatAreNotFetches[host]; !ok {
				t.Errorf("%s refers to %s: everything must be served from this machine", path, host)
			}
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
}

// A CSP without unsafe-inline is only true if nothing in the pages needs it.
// The browser blocks an inline style attribute under that policy exactly as it
// blocks an inline script, and it does so silently unless somebody has the
// console open — which is how the first version of these pages shipped with
// three buttons in the wrong place.
func TestPagesUseNoInlineStyleOrScript(t *testing.T) {
	banned := map[string]string{
		`style="`:  "an inline style attribute",
		`style='`:  "an inline style attribute",
		`onclick=`: "an inline event handler",
		`<script>`: "an inline script block",
	}
	err := fs.WalkDir(assets, ".", func(path string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		// The stylesheet is where the rule sends everything, so it is the one
		// file allowed to talk about inline styles.
		if strings.HasSuffix(path, ".css") {
			return nil
		}
		body, err := assets.ReadFile(path)
		if err != nil {
			return err
		}
		for marker, what := range banned {
			if bytes.Contains(body, []byte(marker)) {
				t.Errorf("%s has %s (%q); the CSP blocks it — put it in the stylesheet", path, what, marker)
			}
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
}

// The check above reads the built bundle, and a bundle is not where a style
// attribute would be written by hand — the source is. This reads the source,
// with one extra rule the compiled file cannot carry: no style assignment from
// script either.
//
// (`element.style.color = x` is not in fact blocked by the CSP — only the style
// *attribute* is — which is why the compiled Preact runtime, which does exactly
// that, is not a failure. In our own code it is still banned, because a colour
// set from script is a colour that is not in the stylesheet.)
func TestTheClientSourceHasNoInlineStyle(t *testing.T) {
	root := filepath.Join("..", "..", "..", "web", "harmony", "src")
	seen := 0
	err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() || strings.HasSuffix(path, ".css") {
			return err
		}
		body, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		seen++
		// One exception, and a narrow one: drag.ts may set a transform, which
		// is what puts a dragged picture anywhere in the call (the owner,
		// 24/09/2026). Only that property, and only in that file.
		if filepath.Base(path) == "drag.ts" {
			body = bytes.ReplaceAll(body, []byte(`.style.transform =`), nil)
		}
		for _, marker := range []string{`style="`, `style='`, `.style.`, `onclick=`} {
			if bytes.Contains(body, []byte(marker)) {
				t.Errorf("%s has %q; the CSP blocks it — put it in style.css", path, marker)
			}
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	// A path that stops matching anything is a test that silently stops
	// testing.
	if seen == 0 {
		t.Fatalf("no client source found under %s", root)
	}
}

// The static handler serves the embedded subtree and nothing else. There is no
// path a visitor can spell that reaches a file outside it.
func TestStaticFilesCannotEscapeTheEmbeddedFolder(t *testing.T) {
	h := newHarness(t)
	d := h.device("browser")
	// The bundle's names carry a content hash, so the test asks the embedded
	// filesystem what they are rather than hard-coding one that changes on
	// every build.
	built, err := fs.Glob(assets, appDir+"/assets/*.css")
	if err != nil || len(built) == 0 {
		t.Fatalf("no built stylesheet in the embedded assets (run npm --prefix web run build:harmony): %v", err)
	}
	served := "/static/" + strings.TrimPrefix(built[0], appDir+"/")
	if res := d.get(served); res.status != http.StatusOK {
		t.Fatalf("%s is not served: %d", served, res.status)
	}
	for _, path := range []string{
		"/static/../404.html",
		"/static/%2e%2e/404.html",
		"/static/../../../../etc/passwd",
	} {
		res := d.get(path)
		if res.status == http.StatusOK && strings.Contains(res.raw, "<!DOCTYPE") {
			t.Errorf("GET %s escaped the static folder", path)
		}
	}
}

// ---------------------------------------------------------------------------
// The pages themselves
// ---------------------------------------------------------------------------

// The error page is the page a stranger is most likely to see — it is what a
// used-up invite answers — and from D0 until this was written it was the only
// page served with no stylesheet at all: it linked /static/harmony.css, a file
// that never existed, because the real one is named by the build with a content
// hash. Nothing failed; it just rendered in Times New Roman on white.
//
// So this does not check the name. It follows the link the way a browser does:
// ask for a page that does not exist, read the stylesheet out of it, ask for
// that, and require a stylesheet to come back.
func TestTheErrorPageStylesheetIsActuallyServed(t *testing.T) {
	h := newHarness(t)
	d := h.device("browser")

	page := d.get("/uma-coisa-que-nao-existe")
	if page.status != http.StatusNotFound {
		t.Fatalf("GET an unknown path = %d, want 404", page.status)
	}
	link := regexp.MustCompile(`<link[^>]+rel="stylesheet"[^>]+href="([^"]+)"`).FindStringSubmatch(page.raw)
	if link == nil {
		t.Fatalf("the error page links no stylesheet: %s", page.raw)
	}
	href := link[1]
	if href == stylesheetPlaceholder {
		t.Fatalf("the page still carries the placeholder %q: the build emitted no stylesheet, or page() stopped swapping it", href)
	}

	css := d.get(href)
	if css.status != http.StatusOK {
		t.Fatalf("GET %s = %d, want 200: the error page points at a file that is not served", href, css.status)
	}
	if ct := css.header.Get("Content-Type"); !strings.HasPrefix(ct, "text/css") {
		t.Fatalf("GET %s served %q, want text/css", href, ct)
	}
	// And it is the real stylesheet, not some other file that happens to answer.
	// (--color-background: the old --color-ink alias left the palette on
	// 24/09/2026, and this marker went with it.)
	if !strings.Contains(css.raw, "--color-background") {
		t.Errorf("%s does not look like the harmony stylesheet: the palette is not in it", href)
	}
}

func TestSignedOutVisitorIsSentToLogin(t *testing.T) {
	h := newHarness(t)
	d := h.device("browser")
	res := d.get("/")
	if res.status != http.StatusSeeOther || res.header.Get("Location") != "/login" {
		t.Fatalf("GET / signed out = %d %s, want 303 to /login", res.status, res.header.Get("Location"))
	}
	owner := h.join(h.bootstrapInvite(), "tales", goodPass)
	if res := owner.get("/"); res.status != http.StatusOK {
		t.Fatalf("GET / signed in = %d, want 200", res.status)
	}
	if res := owner.get("/login"); res.status != http.StatusSeeOther {
		t.Fatalf("GET /login signed in = %d, want 303 to /", res.status)
	}
}

func TestHealthz(t *testing.T) {
	h := newHarness(t)
	if res := h.device("probe").get("/healthz"); res.status != http.StatusOK {
		t.Fatalf("healthz = %d", res.status)
	}
}

// flip changes one character of a code, producing something the right shape
// that is nonetheless not a real invite.
func flip(code string) string {
	if code == "" {
		return "A"
	}
	last := code[len(code)-1]
	replacement := byte('A')
	if last == 'A' {
		replacement = 'B'
	}
	return code[:len(code)-1] + string(replacement)
}

// The noise suppressor is the first thing this server has ever had to hand a
// browser that is not a page, a script or a stylesheet: a WebAssembly binary
// and an audio worklet (D4e). Both are served from here, like everything else,
// and both are easy to lose silently — a build that stops emitting them, or a
// handler that stops serving that extension, turns noise suppression off with
// no error anywhere.
//
// The names carry a content hash, so the test finds them rather than knowing
// them, which is also how it notices if they stop being built at all.
func TestTheNoiseSuppressorIsServedFromThisMachine(t *testing.T) {
	var wasm []string
	err := fs.WalkDir(assets, ".", func(path string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		if strings.HasSuffix(path, ".wasm") {
			wasm = append(wasm, path)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(wasm) == 0 {
		t.Fatal("no .wasm in the built client: the noise suppressor was not bundled")
	}

	h := newHarness(t)
	d := h.device("browser")
	for _, path := range wasm {
		// The embedded path is assets/app/assets/x.wasm; the address is
		// /static/assets/x.wasm.
		url := "/static/" + strings.TrimPrefix(path, appDir+"/")
		res := d.get(url)
		if res.status != http.StatusOK {
			t.Errorf("GET %s = %d, want 200", url, res.status)
		}
		if len(res.raw) < 1000 {
			t.Errorf("GET %s came back %d bytes; that is not a wasm binary", url, len(res.raw))
		}
	}
}

// ---------------------------------------------------------------------------
// the 2026-09-20 audit: what the five confirmed defects must never do again
// ---------------------------------------------------------------------------

// F1. Revoking a device has to stop that device from reading. The session was
// checked once, at the handshake, and never again: the cookie was refused at
// the door while the socket already inside kept being handed every message of
// every conversation.
func TestRevokedDeviceStopsReceivingOnItsSocket(t *testing.T) {
	c := newCast(t)
	ws := c.owner.dial(t)
	defer ws.Close()
	dm := c.alice.openDM("tales")

	pc := c.h.device("pc")
	if res := pc.do(http.MethodPost, "/api/login", map[string]string{"username": "tales", "password": goodPass}); res.status != http.StatusOK {
		t.Fatalf("login on a second device: %d %s", res.status, res.raw)
	}
	if res := pc.do(http.MethodPost, "/api/sessions/revoke-others", map[string]string{}); res.status != http.StatusOK {
		t.Fatalf("revoke others: %d %s", res.status, res.raw)
	}
	if res := c.owner.get("/api/me"); res.status != http.StatusUnauthorized {
		t.Fatalf("the revoked cookie still works over HTTP: %d", res.status)
	}

	c.alice.mustSay(dm, "isto e privado")

	_ = ws.SetReadDeadline(time.Now().Add(3 * time.Second))
	for {
		_, raw, err := ws.ReadMessage()
		if err != nil {
			return // the socket ended, which is the point
		}
		var ev struct {
			Type string `json:"type"`
		}
		if json.Unmarshal(raw, &ev) == nil && ev.Type == "message" {
			t.Fatalf("a revoked socket was delivered a message: %s", raw)
		}
	}
}

// F5. The limit is five wrong guesses per account. Twenty arriving together
// used to be twenty verifications, because the count was read before argon2id
// and written after it — a tenth of a second in which every one of them read
// the same number.
func TestParallelLoginAttemptsCannotOutrunTheLimit(t *testing.T) {
	h := newHarness(t)
	h.join(h.bootstrapInvite(), "tales", goodPass)

	const tries = 20
	status := make([]int, tries)
	var wg sync.WaitGroup
	for i := 0; i < tries; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			d := h.device(fmt.Sprintf("device-%d", i))
			status[i] = d.do(http.MethodPost, "/api/login",
				map[string]string{"username": "tales", "password": "chute errado"}).status
		}(i)
	}
	wg.Wait()

	verified, blocked := 0, 0
	for _, s := range status {
		switch s {
		case http.StatusUnauthorized:
			verified++
		case http.StatusTooManyRequests:
			blocked++
		default:
			t.Errorf("unexpected answer to a wrong password: %d", s)
		}
	}
	if verified > AccountLimit {
		t.Errorf("%d of %d attempts were verified; the limit is %d", verified, tries, AccountLimit)
	}
	if blocked == 0 {
		t.Error("not one attempt was turned away")
	}
	// And the account is locked afterwards, so the burst did not spend the
	// window in a way that leaves the door open.
	if res := h.device("later").do(http.MethodPost, "/api/login",
		map[string]string{"username": "tales", "password": goodPass}); res.status != http.StatusTooManyRequests {
		t.Errorf("after twenty wrong guesses the account answers %d, want 429", res.status)
	}
}

// F4. A password reset ends every session of the account. A sign-in that was
// already verifying the old password when the reset landed used to insert its
// session afterwards, so the one device the reset existed to cut off was the
// one left standing.
func TestASignInCannotOutliveThePasswordItUsed(t *testing.T) {
	h := newHarnessTweaked(t, func(c *Config) { c.Params = &passwd.Default })
	phone := h.join(h.bootstrapInvite(), "tales", goodPass)
	user, _, err := h.st.UserByUsername("tales")
	if err != nil {
		t.Fatal(err)
	}

	// Hashed before the race: hashing costs the same tenth of a second, and
	// doing it inside the window would push the reset past the sign-in.
	fresh, err := passwd.Hash("uma senha nova bem comprida", passwd.Default)
	if err != nil {
		t.Fatal(err)
	}

	// The reset lands exactly between the check and the session. It used to
	// be placed there by a sleep, and a sleep that missed — the reset landing
	// before the check — made the sign-in fail for the ordinary reason, and
	// the test passed without having tested anything: the one assertion that
	// mattered only ran when the sign-in succeeded (the audit of the tests,
	// 23/09/2026).
	h.api.verified = func() {
		if err := h.st.SetPasswordHash(user.ID, fresh); err != nil {
			t.Error(err)
		}
	}
	pc := h.device("pc")
	res := pc.do(http.MethodPost, "/api/login",
		map[string]string{"username": "tales", "password": goodPass})
	h.api.verified = nil

	if res := phone.get("/api/me"); res.status != http.StatusUnauthorized {
		t.Fatalf("the reset did not even end the existing session: %d", res.status)
	}
	if res.status != http.StatusUnauthorized {
		t.Errorf("a sign-in verified against the old password answered %d %s, want 401", res.status, res.raw)
	}
	if me := pc.get("/api/me"); me.status == http.StatusOK {
		t.Fatalf("a session made with the old password survived the reset: %s", me.raw)
	}
}

// ---------------------------------------------------------------------------
// the 2026-09-20 audit: the medium and low findings
// ---------------------------------------------------------------------------

// F6. A push endpoint is an address, not a deed. Naming somebody else's used
// to move their registration to whoever asked — their device stopped being
// rung, and the row showed up under the other account.
func TestAPushEndpointDoesNotChangeHands(t *testing.T) {
	c := newPushCast(t)
	const endpoint = "https://push.example/alice-device"

	if res := c.alice.subscribePush(endpoint); res.status != http.StatusCreated && res.status != http.StatusOK {
		t.Fatalf("alice subscribing: %d %s", res.status, res.raw)
	}
	if res := c.bo.subscribePush(endpoint); res.status != http.StatusConflict {
		t.Errorf("bruno took over alice's endpoint: %d %s", res.status, res.raw)
	}
	if res := c.alice.pushStatus(endpoint); res.status != http.StatusOK || res.body["subscribed"] != true {
		t.Errorf("alice lost her own registration: %d %s", res.status, res.raw)
	}
	if res := c.bo.pushStatus(endpoint); res.status == http.StatusOK && res.body["subscribed"] == true {
		t.Errorf("bruno now holds alice's endpoint: %s", res.raw)
	}
	// And re-registering your own is the ordinary case, which must still work.
	if res := c.alice.subscribePush(endpoint); res.status != http.StatusCreated && res.status != http.StatusOK {
		t.Errorf("alice re-subscribing her own device: %d %s", res.status, res.raw)
	}
}

// F7. Deleting a message used to leave its picture downloadable: the history
// hid it, and the file endpoint went on serving it to anybody in the channel.
func TestDeletingAMessageTakesItsAttachmentWithIt(t *testing.T) {
	c := newCast(t)
	dm := c.owner.openDM("alice")

	up := c.owner.upload(dm, "foto.png", pngBytes(t, 8, 8))
	if up.status != http.StatusCreated {
		t.Fatalf("upload: %d %s", up.status, up.raw)
	}
	fileID, _ := up.body["id"].(string)
	msg := c.owner.mustSay(dm, "olha isso", fileID)
	msgID := fmt.Sprintf("%.0f", msg["id"].(float64))

	if res := c.alice.get("/api/files/" + fileID); res.status != http.StatusOK {
		t.Fatalf("alice cannot see the picture before the deletion: %d", res.status)
	}
	if res := c.owner.do(http.MethodDelete, "/api/messages/"+msgID, nil); res.status != http.StatusOK {
		t.Fatalf("delete: %d %s", res.status, res.raw)
	}

	// Both ends: the person it was sent to, and the person who sent it.
	if res := c.alice.get("/api/files/" + fileID); res.status != http.StatusNotFound {
		t.Errorf("the attachment of a deleted message is still served: %d", res.status)
	}
	if res := c.owner.get("/api/files/" + fileID); res.status != http.StatusNotFound {
		t.Errorf("the author can still fetch the attachment of a deleted message: %d", res.status)
	}
}

// F12. The owner cancelling their own invite ran the member's delete and then
// the owner's, and the second one answered "not found" about the row the first
// had just deleted: a cancellation that worked, reported as a failure.
func TestCancellingYourOwnInviteSaysItWorked(t *testing.T) {
	h := newHarness(t)
	owner := h.join(h.bootstrapInvite(), "tales", goodPass)

	res := owner.do(http.MethodPost, "/api/invites", map[string]string{"note": "para o joao"})
	if res.status != http.StatusCreated {
		t.Fatalf("create invite: %d %s", res.status, res.raw)
	}
	id, _ := res.body["id"].(string)

	if res := owner.do(http.MethodDelete, "/api/invites/"+id, nil); res.status != http.StatusOK {
		t.Fatalf("cancelling own invite answered %d, want 200: %s", res.status, res.raw)
	}
	// And it really is gone, so the 200 is not the other kind of lie.
	list := owner.get("/api/invites")
	for _, raw := range list.body["invites"].([]any) {
		if raw.(map[string]any)["id"] == id {
			t.Fatal("the invite is still listed after a successful cancellation")
		}
	}
	if res := owner.do(http.MethodDelete, "/api/invites/"+id, nil); res.status != http.StatusNotFound {
		t.Errorf("cancelling it twice answers %d, want 404", res.status)
	}
}

// F10. Hanging up used to clear the twenty second cooldown unless the word
// "rejected" was in the body — and the word comes from the caller. Cancel,
// call, cancel, call was a doorbell nobody could switch off.
func TestCancellingACallKeepsTheCooldown(t *testing.T) {
	c := newCast(t)

	first := c.owner.call("alice")
	if first.status != http.StatusCreated {
		t.Fatalf("first call: %d %s", first.status, first.raw)
	}
	id, _ := first.body["id"].(string)
	if res := c.owner.do(http.MethodPost, "/api/calls/"+id+"/hangup", map[string]any{}); res.status != http.StatusOK {
		t.Fatalf("hangup: %d %s", res.status, res.raw)
	}

	second := c.owner.call("alice")
	if second.status != http.StatusTooManyRequests {
		t.Errorf("ringing again right after cancelling answered %d, want 429: %s", second.status, second.raw)
	}
}

// A file that never reached the disk was still charged: the row and the use
// were committed before the bytes were written, and the failure path left both.
// Here the files directory is not a directory, so every write fails.
func TestAnUploadThatFailsToWriteCostsNothing(t *testing.T) {
	c := newCast(t)
	dm := c.owner.openDM("alice")
	owner, _, err := c.h.st.UserByUsername("tales")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.RemoveAll(c.h.filesDir); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(c.h.filesDir, []byte("not a directory"), 0o600); err != nil {
		t.Fatal(err)
	}

	up := c.owner.upload(dm, "foto.png", pngBytes(t, 8, 8))
	if up.status != http.StatusInternalServerError {
		t.Fatalf("an upload with nowhere to go answered %d %s", up.status, up.raw)
	}
	if n, err := c.h.st.RecordedUsage(owner.ID, store.UsageUpload, 24*time.Hour); err != nil || n != 0 {
		t.Errorf("the failed upload was counted as %d uses of the day's allowance (err %v)", n, err)
	}
	if b, err := c.h.st.UploadedBytes(owner.ID, 24*time.Hour); err != nil || b != 0 {
		t.Errorf("the failed upload was counted as %d bytes of the day's allowance (err %v)", b, err)
	}
}

// The audit of the tests, 23/09/2026, walked every route against the rule "a
// new endpoint comes with the test that A cannot reach B's data". Two had none.

// A member may remove their own words, and the owner anybody's. Nothing said a
// member could not remove another member's: every existing test had the owner
// doing the removing, which is the one case that is allowed.
func TestAMemberCannotRemoveSomebodyElsesMessage(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	posted := c.alice.mustSay(general, "isto é meu")
	id := fmt.Sprintf("%.0f", posted["id"].(float64))

	real := c.bo.do(http.MethodDelete, "/api/messages/"+id, nil)
	fake := c.bo.do(http.MethodDelete, "/api/messages/999999", nil)
	if real.status != http.StatusNotFound || real.raw != fake.raw {
		t.Errorf("Bo removing Alice's message in a channel they share = %d %q, want what an invented id gets (%d %q)",
			real.status, real.raw, fake.status, fake.raw)
	}
	after := c.alice.history(general)
	if len(after) == 0 || after[len(after)-1]["deleted"] == true || after[len(after)-1]["body"] != "isto é meu" {
		t.Errorf("after Bo's attempt the message is %v", after[len(after)-1])
	}
}

// Every other door into a private conversation was tried from outside it —
// reading, writing, typing, muting, closing — except putting a file into it.
func TestNobodyUploadsIntoAConversationTheyAreNotIn(t *testing.T) {
	c := newCast(t)
	dm := c.owner.openDM("alice")

	real := c.bo.upload(dm, "foto.png", pngBytes(t, 8, 8))
	fake := c.bo.upload("AAAAAAAAAAAAAAAA", "foto.png", pngBytes(t, 8, 8))
	if real.status != http.StatusNotFound || real.raw != fake.raw {
		t.Errorf("Bo uploading into Alice and the owner's conversation = %d %q, want what an invented id gets (%d %q)",
			real.status, real.raw, fake.status, fake.raw)
	}
	// And it cost him nothing, because nothing was stored.
	bo, _, err := c.h.st.UserByUsername("bruno")
	if err != nil {
		t.Fatal(err)
	}
	if n, _ := c.h.st.RecordedUsage(bo.ID, store.UsageUpload, 24*time.Hour); n != 0 {
		t.Errorf("a refused upload was stored and counted: %d", n)
	}
}
