package api

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"

	"call/internal/harmony/store"
)

type capturedMail struct{ to, subject, body string }
type capturingMailer struct {
	mu       sync.Mutex
	messages []capturedMail
	fail     bool
}

func (m *capturingMailer) Send(_ context.Context, to, subject, body string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.messages = append(m.messages, capturedMail{to, subject, body})
	if m.fail {
		return errors.New(to + " " + body)
	}
	return nil
}
func (m *capturingMailer) wait(t *testing.T, to, contains string) capturedMail {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		m.mu.Lock()
		for i := len(m.messages) - 1; i >= 0; i-- {
			v := m.messages[i]
			if v.to == to && strings.Contains(v.body, contains) {
				m.mu.Unlock()
				return v
			}
		}
		m.mu.Unlock()
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatal("mail was not delivered")
	return capturedMail{}
}
func emailCode(t *testing.T, v capturedMail) string {
	t.Helper()
	code := regexp.MustCompile(`\b[0-9]{6}\b`).FindString(v.body)
	if code == "" {
		t.Fatal("code missing")
	}
	return code
}
func resetToken(t *testing.T, v capturedMail) string {
	t.Helper()
	match := regexp.MustCompile(`#reset=([A-Za-z0-9_-]+)`).FindStringSubmatch(v.body)
	if len(match) != 2 {
		t.Fatal("reset token missing")
	}
	return match[1]
}
func emailHarness(t *testing.T, required bool) (*harness, *capturingMailer) {
	m := &capturingMailer{}
	h := newHarnessTweaked(t, func(c *Config) { c.Mailer = m; c.EmailRequired = required })
	return h, m
}
func joinEmail(h *harness, m *capturingMailer, name string) (*device, string) {
	d := h.device(agentPhone)
	email := name + "@example.org"
	r := d.do("POST", "/api/signup", map[string]string{"code": h.bootstrapInvite(), "username": name, "email": email, "password": goodPass})
	if r.status != 201 {
		h.t.Fatalf("signup: %d %s", r.status, r.raw)
	}
	return d, emailCode(h.t, m.wait(h.t, email, "confirmar"))
}
func confirmEmail(t *testing.T, d *device, code string) {
	t.Helper()
	r := d.do("POST", "/api/me/email/verify", map[string]string{"code": code})
	if r.status != 200 {
		t.Fatalf("verify: %d %s", r.status, r.raw)
	}
}

func TestEmailOwnershipAndGate(t *testing.T) {
	h, m := emailHarness(t, true)
	a, ac := joinEmail(h, m, "alice")
	b, bc := joinEmail(h, m, "bruno")
	if r := a.do("POST", "/api/me/email/verify", map[string]string{"code": bc}); r.status == 200 {
		t.Fatal("A used B's code")
	}
	for _, path := range []string{"/api/channels", "/api/members", "/api/sessions", "/api/invites", "/api/admin/accounts", "/api/presence", "/api/voice/rooms", "/ws"} {
		if r := a.get(path); r.status != 403 || !strings.Contains(r.raw, "email_required") {
			t.Fatalf("gate %s: %d %s", path, r.status, r.raw)
		}
	}
	if r := a.get("/api/me"); r.status != 200 {
		t.Fatal("me blocked")
	}
	confirmEmail(t, a, ac)
	confirmEmail(t, b, bc)
	if r := a.get("/api/channels"); r.status != 200 {
		t.Fatal("confirmed account blocked")
	}
	if r := a.get("/api/me"); strings.Contains(r.raw, "bruno@example.org") {
		t.Fatal("other email exposed")
	}
	if r := a.get("/api/admin/accounts"); strings.Contains(r.raw, "@example.org") {
		t.Fatal("admin email exposed")
	}
	r := a.do("POST", "/api/me/email", map[string]string{"password": goodPass, "email": " NEW@example.org "})
	if r.status != 202 {
		t.Fatal(r.raw)
	}
	next := emailCode(t, m.wait(t, "new@example.org", "confirmar"))
	u, _, _ := h.st.UserByUsername("alice")
	if u.Email != "alice@example.org" {
		t.Fatal("address changed before verification")
	}
	if r := b.do("POST", "/api/me/email/verify", map[string]string{"code": next}); r.status == 200 {
		t.Fatal("B confirmed A's change")
	}
	confirmEmail(t, a, next)
	m.wait(t, "alice@example.org", "trocado")
	if r := a.do("POST", "/api/me/email/verify", map[string]string{"code": next}); r.status == 200 {
		t.Fatal("email code reused")
	}
}

func TestEmailOptionalAndUnavailable(t *testing.T) {
	h, m := emailHarness(t, false)
	a, _ := joinEmail(h, m, "alice")
	if r := a.get("/api/channels"); r.status != 200 {
		t.Fatal("optional gate blocked")
	}
	h2 := newHarnessTweaked(t, func(c *Config) { c.Mailer = nil; c.EmailRequired = true })
	if h2.api.cfg.EmailRequired {
		t.Fatal("gate enabled without mail")
	}
	// A server that came up without its mail settings still lets an invite
	// in: the address waits unconfirmed instead of the door being shut.
	d := h2.join(h2.bootstrapInvite(), "alice", goodPass)
	if r := d.get("/api/me"); r.status != 200 || r.body["user"].(map[string]any)["email_verified"] != false {
		t.Fatalf("signup without mail: %d %s", r.status, r.raw)
	}
	if r := d.get("/api/channels"); r.status != 200 {
		t.Fatalf("a member was gated with no way to confirm: %d", r.status)
	}
}

func TestForgotUniformResetSingleUseExpiryAndSessions(t *testing.T) {
	h, m := emailHarness(t, false)
	a, ac := joinEmail(h, m, "alice")
	confirmEmail(t, a, ac)
	_, _ = joinEmail(h, m, "legacy")
	u, _, _ := h.st.UserByUsername("legacy")
	h.st.DB().Exec(`UPDATE users SET email=NULL WHERE id=?`, u.ID)
	stranger := h.device("stranger")
	var response string
	for _, id := range []string{"missing", "legacy", "alice@example.org"} {
		r := stranger.do("POST", "/api/password/forgot", map[string]string{"identifier": id})
		if r.status != 202 {
			t.Fatal(r.raw)
		}
		if response != "" && response != r.raw {
			t.Fatal("forgot reveals membership")
		}
		response = r.raw
	}
	token := resetToken(t, m.wait(t, "alice@example.org", "#reset="))
	r := stranger.do("POST", "/api/password/reset", map[string]string{"token": token, "password": goodPass2})
	if r.status != 200 {
		t.Fatal(r.raw)
	}
	if r = a.get("/api/me"); r.status != 401 {
		t.Fatal("session survived reset")
	}
	if r = stranger.do("POST", "/api/password/reset", map[string]string{"token": token, "password": goodPass}); r.status == 200 {
		t.Fatal("reset token reused")
	}
	r = stranger.do("POST", "/api/login", map[string]string{"username": "alice@example.org", "password": goodPass2})
	if r.status != 200 {
		t.Fatal(r.raw)
	}
	u, _, _ = h.st.UserByUsername("alice")
	expired, err := h.st.IssueProof(u.ID, "reset_password", "", "", time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	h.st.SetClock(func() time.Time { return time.Now().Add(2 * time.Minute) })
	if r = stranger.do("POST", "/api/password/reset", map[string]string{"token": expired, "password": goodPass}); r.status == 200 {
		t.Fatal("expired reset accepted")
	}
}

func TestEmail2FALoginAndRecoveryPolicy(t *testing.T) {
	h, m := emailHarness(t, false)
	a, c := joinEmail(h, m, "alice")
	confirmEmail(t, a, c)
	if r := a.do("POST", "/api/me/2fa/email/enable", map[string]string{"password": goodPass}); r.status != 200 {
		t.Fatal(r.raw)
	}
	d := h.device("new-device")
	r := d.do("POST", "/api/login", map[string]string{"username": "alice", "password": goodPass})
	if r.status != 200 || r.body["twofa"] != "email" || d.sessionCookie() != "" {
		t.Fatal("password opened session with 2FA")
	}
	token := r.body["challenge"].(string)
	code := emailCode(t, m.wait(t, "alice@example.org", "entrar"))
	if r = d.get("/api/me"); r.status != 401 {
		t.Fatal("challenge opened account")
	}
	if r = d.do("POST", "/api/login/2fa", map[string]string{"challenge": token, "code": code}); r.status != 200 {
		t.Fatal(r.raw)
	}
	if r = d.do("POST", "/api/login/2fa", map[string]string{"challenge": token, "code": code}); r.status == 200 {
		t.Fatal("challenge reused")
	}
	d.do("POST", "/api/password/forgot", map[string]string{"identifier": "alice"})
	reset := resetToken(t, m.wait(t, "alice@example.org", "#reset="))
	if r = d.do("POST", "/api/password/reset", map[string]string{"token": reset, "password": goodPass2}); r.status != 200 {
		t.Fatal("email reset requires redundant second email proof")
	}
	u, _, _ := h.st.UserByUsername("alice")
	if u.TwoFA != "email" {
		t.Fatal("password reset disabled email 2FA")
	}
}

func enableTOTP(t *testing.T, h *harness, a *device) []string {
	t.Helper()
	r := a.do("POST", "/api/me/2fa/totp/setup", map[string]string{"password": goodPass})
	if r.status != 200 {
		t.Fatal(r.raw)
	}
	secret := r.body["secret"].(string)
	code := store.TOTPCode(secret, time.Now().Unix()/30, 6)
	r = a.do("POST", "/api/me/2fa/totp/enable", map[string]string{"password": goodPass, "code": code})
	if r.status != 200 {
		t.Fatal(r.raw)
	}
	codes := []string{}
	for _, v := range r.body["recovery_codes"].([]any) {
		codes = append(codes, v.(string))
	}
	if len(codes) != 10 {
		t.Fatal("recovery count")
	}
	return codes
}
func TestTOTPResetAndRecoverySingleUse(t *testing.T) {
	h, m := emailHarness(t, false)
	a, c := joinEmail(h, m, "alice")
	confirmEmail(t, a, c)
	codes := enableTOTP(t, h, a)
	b, bc := joinEmail(h, m, "bruno")
	confirmEmail(t, b, bc)
	enableTOTP(t, h, b)
	if r := b.do("POST", "/api/me/2fa/disable", map[string]string{"password": goodPass, "code": codes[0]}); r.status == 200 {
		t.Fatal("B used A recovery")
	}
	d := h.device("new-device")
	d.do("POST", "/api/password/forgot", map[string]string{"identifier": "alice"})
	token := resetToken(t, m.wait(t, "alice@example.org", "#reset="))
	r := d.do("POST", "/api/password/reset", map[string]string{"token": token, "password": goodPass2})
	if r.status != 400 || !strings.Contains(r.raw, "totp_required") {
		t.Fatal("reset bypassed app")
	}
	if r = d.do("POST", "/api/password/reset", map[string]string{"token": token, "password": goodPass2, "code": codes[0]}); r.status != 200 {
		t.Fatal(r.raw)
	}
	r = d.do("POST", "/api/login", map[string]string{"username": "alice", "password": goodPass2})
	challenge := r.body["challenge"].(string)
	if r = d.do("POST", "/api/login/2fa", map[string]string{"challenge": challenge, "code": codes[0]}); r.status == 200 {
		t.Fatal("recovery reused")
	}
	if r = d.do("POST", "/api/login/2fa", map[string]string{"challenge": challenge, "code": codes[1]}); r.status != 200 {
		t.Fatal(r.raw)
	}
}

func TestLoginIdentifierSharesAccountLimit(t *testing.T) {
	h, m := emailHarness(t, false)
	a, c := joinEmail(h, m, "alice")
	confirmEmail(t, a, c)
	for i := 0; i < AccountLimit+1; i++ {
		id := "alice"
		if i%2 == 1 {
			id = "alice@example.org"
		}
		r := h.device(fmt.Sprint(i)).do("POST", "/api/login", map[string]string{"username": id, "password": "wrong"})
		want := 401
		if i == AccountLimit {
			want = 429
		}
		if r.status != want {
			t.Fatalf("attempt %d: %d", i, r.status)
		}
	}
}
func TestMailFailureDoesNotLogSecrets(t *testing.T) {
	h, m := emailHarness(t, false)
	m.fail = true
	_, code := joinEmail(h, m, "alice")
	deadline := time.Now().Add(time.Second)
	for !strings.Contains(h.logs.String(), "mail failed") && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	logs := h.logs.String()
	if strings.Contains(logs, "alice@example.org") || strings.Contains(logs, code) {
		t.Fatal("mail error leaked secret")
	}
	if !strings.Contains(logs, "mail failed") {
		t.Fatal("failure not logged")
	}
}

func TestNewSecurityRoutesRejectOtherAccountFields(t *testing.T) {
	h, m := emailHarness(t, false)
	a, c := joinEmail(h, m, "alice")
	confirmEmail(t, a, c)
	_, _ = joinEmail(h, m, "bruno")
	for _, path := range []string{"email", "email/verify", "password", "2fa/totp/setup", "2fa/totp/enable", "2fa/email/enable", "2fa/disable", "2fa/recovery"} {
		r := a.do(http.MethodPost, "/api/me/"+path, map[string]string{"user_id": "2", "username": "bruno", "password": goodPass, "code": "123456"})
		if r.status != 400 {
			t.Fatalf("%s accepted targeting fields: %d", path, r.status)
		}
	}
}

func TestLoginChallengeFiveAttemptsAndCrossAccountCodes(t *testing.T) {
	h, m := emailHarness(t, false)
	a, ac := joinEmail(h, m, "alice")
	confirmEmail(t, a, ac)
	b, bc := joinEmail(h, m, "bruno")
	confirmEmail(t, b, bc)
	for _, d := range []*device{a, b} {
		if r := d.do("POST", "/api/me/2fa/email/enable", map[string]string{"password": goodPass}); r.status != 200 {
			t.Fatal(r.raw)
		}
	}
	d := h.device("attacker")
	r := d.do("POST", "/api/login", map[string]string{"username": "alice", "password": goodPass})
	challenge := r.body["challenge"].(string)
	d.do("POST", "/api/login", map[string]string{"username": "bruno", "password": goodPass})
	bCode := emailCode(t, m.wait(t, "bruno@example.org", "entrar"))
	if r = d.do("POST", "/api/login/2fa", map[string]string{"challenge": challenge, "code": bCode}); r.status == 200 {
		t.Fatal("B code opened A challenge")
	}
	for i := 1; i < 5; i++ {
		r = d.do("POST", "/api/login/2fa", map[string]string{"challenge": challenge, "code": "not-a-code"})
		if r.status != 400 {
			t.Fatalf("attempt %d: %d %s", i+1, r.status, r.raw)
		}
	}
	code := emailCode(t, m.wait(t, "alice@example.org", "entrar"))
	if r = d.do("POST", "/api/login/2fa", map[string]string{"challenge": challenge, "code": code}); r.status != 429 {
		t.Fatalf("sixth attempt: %d %s", r.status, r.raw)
	}
	if d.sessionCookie() != "" {
		t.Fatal("failed factor set session")
	}
	// A fresh password challenge cannot clear this account's failed codes.
	if r = d.do("POST", "/api/login", map[string]string{"username": "alice@example.org", "password": goodPass}); r.status != 429 {
		t.Fatal("identifier bypassed factor account limit", r.raw)
	}
}

func TestPasswordChangeKeepsOnlyCurrentSession(t *testing.T) {
	h, m := emailHarness(t, false)
	a, c := joinEmail(h, m, "alice")
	confirmEmail(t, a, c)
	b, bc := joinEmail(h, m, "bruno")
	confirmEmail(t, b, bc)
	other := h.device("other")
	if r := other.do("POST", "/api/login", map[string]string{"username": "alice", "password": goodPass}); r.status != 200 {
		t.Fatal(r.raw)
	}
	if r := a.do("POST", "/api/me/password", map[string]string{"current": goodPass, "new": goodPass2}); r.status != 200 {
		t.Fatal(r.raw)
	}
	if r := a.get("/api/me"); r.status != 200 {
		t.Fatal("current session revoked")
	}
	if r := other.get("/api/me"); r.status != 401 {
		t.Fatal("other session retained")
	}
	if r := b.get("/api/me"); r.status != 200 {
		t.Fatal("B session revoked by A")
	}
	_, hash, err := h.st.UserByUsername("bruno")
	if err != nil || hash == "" {
		t.Fatal("B password damaged")
	}
}

func TestEmailResendBelongsToCurrentUserAndLimits(t *testing.T) {
	h, m := emailHarness(t, false)
	a, _ := joinEmail(h, m, "alice")
	_, _ = joinEmail(h, m, "bruno")
	if r := a.do("POST", "/api/me/email/resend", map[string]string{"username": "bruno"}); r.status != 400 {
		t.Fatal(r.raw)
	}
	if r := a.do("POST", "/api/me/email/resend", map[string]string{}); r.status != 202 {
		t.Fatal(r.raw)
	}
	m.wait(t, "alice@example.org", "confirmar")
	if r := a.do("POST", "/api/me/email/resend", map[string]string{}); r.status != 429 {
		t.Fatal("minute throttle absent")
	}
	start := time.Now()
	for i := 1; i < 5; i++ {
		at := start.Add(time.Duration(i) * 61 * time.Second)
		h.st.SetClock(func() time.Time { return at })
		if r := a.do("POST", "/api/me/email/resend", map[string]string{}); r.status != 202 {
			t.Fatal(r.raw)
		}
	}
	h.st.SetClock(func() time.Time { return start.Add(6 * 61 * time.Second) })
	if r := a.do("POST", "/api/me/email/resend", map[string]string{}); r.status != 429 {
		t.Fatal("hour throttle absent")
	}
}

func TestFactorEndpointsMutateOnlyCookieOwner(t *testing.T) {
	h, m := emailHarness(t, false)
	a, ac := joinEmail(h, m, "alice")
	confirmEmail(t, a, ac)
	b, bc := joinEmail(h, m, "bruno")
	confirmEmail(t, b, bc)
	bCodes := enableTOTP(t, h, b)
	aCodes := enableTOTP(t, h, a)
	if r := a.do("POST", "/api/me/2fa/recovery", map[string]string{"password": goodPass}); r.status != 200 {
		t.Fatal(r.raw)
	}
	if r := a.do("POST", "/api/me/2fa/disable", map[string]string{"password": goodPass, "code": aCodes[0]}); r.status == 200 {
		t.Fatal("rotated recovery survived")
	}
	if r := b.do("POST", "/api/me/2fa/disable", map[string]string{"password": goodPass, "code": bCodes[0]}); r.status != 200 {
		t.Fatal("A rotation affected B", r.raw)
	}
	u, _, _ := h.st.UserByUsername("alice")
	if u.TwoFA != "totp" {
		t.Fatal("B disabled A factor")
	}
}

func TestGateAllowsOnlyAccountRepairAndDisabledGateNeverBlocks(t *testing.T) {
	h, m := emailHarness(t, true)
	a, _ := joinEmail(h, m, "alice")
	for _, v := range []struct {
		path string
		body map[string]string
	}{
		{"email", map[string]string{"password": "wrong", "email": "new@example.org"}},
		{"email/verify", map[string]string{"code": "invalid"}},
		{"email/resend", map[string]string{}},
		{"password", map[string]string{"current": "wrong", "new": goodPass2}},
	} {
		r := a.do("POST", "/api/me/"+v.path, v.body)
		if strings.Contains(r.raw, "email_required") {
			t.Fatal("repair blocked", v.path)
		}
	}
	if r := a.do("POST", "/api/logout", nil); r.status != 200 {
		t.Fatal("logout blocked")
	}
}

// The new-access notice names the device in words, from the agents the three
// clients really send: the owner's Chrome, the Android app (Core.kt) and the
// Windows program (main.cpp appends its token to WebView2's own agent).
func TestDeviceNameSpeaksPlainly(t *testing.T) {
	for agent, want := range map[string]string{
		"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/152.0.7977.130 Safari/537.36":                              "Chrome no Windows",
		"Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:140.0) Gecko/20100101 Firefox/140.0":                                                                  "Firefox no Windows",
		"Mozilla/5.0 (Linux; Android 15) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/152.0.0.0 Mobile Safari/537.36":                                      "Chrome no Android",
		"harmony-android/0.21 (Android 15; Xiaomi Redmi 15)":                                                                                                "app do harmony no Android (Xiaomi Redmi 15)",
		"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/152.0.0.0 Safari/537.36 Edg/152.0.0.0 harmony-windows/0.2": "programa do harmony no Windows",
		"": "navegador",
	} {
		if got := deviceName(agent); got != want {
			t.Errorf("deviceName(%q) = %q, want %q", agent, got, want)
		}
	}
}
