package api

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
	_ "time/tzdata"

	"call/internal/harmony/passwd"
	"call/internal/harmony/store"
)

func ownUserJSON(u store.User) map[string]any {
	out := userJSON(u)
	out["email"] = u.Email
	out["email_verified"] = u.EmailVerified
	out["twofa"] = u.TwoFA
	return out
}
func emailGateAllowed(path string) bool {
	switch path {
	case "/api/me", "/api/logout", "/api/me/email", "/api/me/email/verify", "/api/me/email/resend", "/api/me/password":
		return true
	}
	return false
}
func (s *Server) securityRoutes() {
	s.mux.HandleFunc("POST /api/login/2fa", s.handleLogin2FA)
	s.mux.HandleFunc("POST /api/password/forgot", s.handleForgot)
	s.mux.HandleFunc("POST /api/password/reset", s.handleReset)
	s.mux.HandleFunc("POST /api/me/email", s.requireUser(s.handleEmail))
	s.mux.HandleFunc("POST /api/me/email/verify", s.requireUser(s.handleEmailVerify))
	s.mux.HandleFunc("POST /api/me/email/resend", s.requireUser(s.handleEmailResend))
	s.mux.HandleFunc("POST /api/me/password", s.requireUser(s.handlePassword))
	for _, action := range []string{"totp/setup", "totp/enable", "email/enable", "disable", "recovery"} {
		s.mux.HandleFunc("POST /api/me/2fa/"+action, s.requireUser(func(w http.ResponseWriter, r *http.Request, cu ctxUser) { s.handleFactor(w, r, cu, action) }))
	}
}

func (s *Server) mailAvailable(w http.ResponseWriter) bool {
	if s.cfg.Mailer != nil {
		return true
	}
	fail(w, 503, "mail_unavailable")
	return false
}
func (s *Server) sendMail(userID int64, to, purpose, subject, body string) {
	if s.cfg.Mailer == nil || to == "" {
		return
	}
	select {
	case s.mailSlots <- struct{}{}:
	default:
		s.log.Error("mail failed", "user_id", userID, "purpose", purpose)
		return
	}
	go func() {
		defer func() { <-s.mailSlots }()
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		// Transport errors can contain recipients or credentials. Never log them.
		if s.cfg.Mailer.Send(ctx, to, subject, body) != nil {
			s.log.Error("mail failed", "user_id", userID, "purpose", purpose)
		}
	}()
}
func (s *Server) sendCode(userID int64, to, purpose, code string) {
	what := "confirmar seu e-mail"
	if purpose == "login_challenge" {
		what = "entrar na sua conta"
	}
	if purpose == "disable_2fa" {
		what = "desligar a verificação em duas etapas"
	}
	// The code leads the subject so the phone's notification shows it and
	// nobody has to open the message to copy six digits.
	s.sendMail(userID, to, purpose, code+" é o seu código do harmony", fmt.Sprintf("Seu código para %s: %s\nVale por 10 minutos e só pode ser usado uma vez.\nSe não foi você, ignore este e-mail e proteja sua conta.", what, code))
}
func (s *Server) sendVerification(userID int64, to string, bindings ...store.ProofBinding) error {
	code, err := store.NewEmailCode()
	if err != nil {
		return err
	}
	if err = s.st.IssueEmailProof(userID, to, code, bindings...); err != nil {
		return err
	}
	s.sendCode(userID, to, "verify_email", code)
	return nil
}
func (s *Server) notice(u store.User, purpose, body string) {
	s.sendMail(u.ID, u.Email, purpose, "Segurança da sua conta harmony", body+"\nSe não foi você, troque a senha e procure o dono do servidor.")
}
// deviceName turns a user agent into the words a person would use for their
// own device. A raw "Mozilla/5.0 (Windows NT 10.0; …)" line in a security
// notice is one nobody can check against "was that me?".
func deviceName(agent string) string {
	agent = strings.Join(strings.Fields(agent), " ")
	switch {
	case strings.HasPrefix(agent, "harmony-android/"):
		// harmony-android/0.21 (Android 15; Xiaomi Redmi 15)
		if i, j := strings.LastIndex(agent, "; "), strings.LastIndex(agent, ")"); i > 0 && j > i {
			return "app do harmony no Android (" + agent[i+2:j] + ")"
		}
		return "app do harmony no Android"
	case strings.Contains(agent, "harmony-windows/"):
		return "programa do harmony no Windows"
	}
	browser := "navegador"
	for _, b := range []struct{ token, name string }{
		{"Edg/", "Edge"}, {"OPR/", "Opera"}, {"Firefox/", "Firefox"},
		{"Chrome/", "Chrome"}, {"Safari/", "Safari"},
	} {
		if strings.Contains(agent, b.token) {
			browser = b.name
			break
		}
	}
	system := ""
	for _, o := range []struct{ token, name string }{
		{"Android", "Android"}, {"iPhone", "iPhone"}, {"iPad", "iPad"},
		{"Windows", "Windows"}, {"Mac OS X", "Mac"}, {"Linux", "Linux"},
	} {
		if strings.Contains(agent, o.token) {
			system = o.name
			break
		}
	}
	if system == "" {
		return browser
	}
	return browser + " no " + system
}

func (s *Server) newAccess(u store.User, agent string) {
	if !u.EmailVerified {
		return
	}
	agent = deviceName(agent)
	loc, err := time.LoadLocation("America/Sao_Paulo")
	if err != nil {
		loc = time.FixedZone("America/Sao_Paulo", -3*3600)
	}
	s.sendMail(u.ID, u.Email, "new_access", "Novo acesso à sua conta harmony", fmt.Sprintf("Novo acesso à sua conta.\nAparelho: %s\nHora: %s (America/Sao_Paulo)\nSe não foi você, troque a senha.", agent, time.Now().In(loc).Format("02/01/2006 15:04")))
}
func (s *Server) reserve(w http.ResponseWriter, kind, key string, window time.Duration, limit int) bool {
	ok, err := s.st.ReserveAttempt(kind, key, window, limit)
	if err != nil || !ok {
		fail(w, 429, "rate_limited")
		return false
	}
	return true
}
func (s *Server) securityError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, store.ErrNameTaken):
		fail(w, 409, "email_unavailable")
	case errors.Is(err, store.ErrRateLimited):
		fail(w, 429, "rate_limited")
	case errors.Is(err, store.ErrCredentials), errors.Is(err, store.ErrNotFound):
		fail(w, 400, "invalid_code")
	default:
		fail(w, 500, "server_error")
	}
}
func (s *Server) currentPassword(w http.ResponseWriter, r *http.Request, cu ctxUser, password string) (string, bool) {
	if !s.reserve(w, "security_password", strconv.FormatInt(cu.user.ID, 10), FailureWindow, AccountLimit) {
		return "", false
	}
	release, held := s.holdHashSlot(r)
	if !held {
		return "", false
	}
	defer release()
	u, hash, err := s.st.UserByUsername(cu.user.Username)
	if err != nil || u.Disabled {
		fail(w, 401, "bad_credentials")
		return "", false
	}
	good, err := passwd.Verify(hash, password)
	if err != nil || !good {
		fail(w, 401, "bad_credentials")
		return "", false
	}
	_ = s.st.ClearFailures("security_password", strconv.FormatInt(cu.user.ID, 10))
	return hash, true
}
func publicWrite(s *Server, w http.ResponseWriter, r *http.Request) bool {
	if s.sameOrigin(r) {
		return true
	}
	fail(w, 403, "bad_origin")
	return false
}

func (s *Server) handleEmail(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		Password string `json:"password"`
		Email    string `json:"email"`
	}
	if !decode(w, r, &req) || !s.mailAvailable(w) {
		return
	}
	req.Email = store.NormalizeEmail(req.Email)
	if !store.ValidEmail(req.Email) {
		fail(w, 400, "bad_email")
		return
	}
	hash, ok := s.currentPassword(w, r, cu, req.Password)
	if !ok {
		return
	}
	if !s.reserve(w, "email_change", strconv.FormatInt(cu.user.ID, 10), time.Hour, 5) {
		return
	}
	if err := s.sendVerification(cu.user.ID, req.Email, store.ProofBinding{PasswordHash: hash, Version: cu.user.SecurityVersion}); err != nil {
		s.securityError(w, err)
		return
	}
	writeJSON(w, 202, map[string]bool{"ok": true})
}
func (s *Server) handleEmailVerify(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		Code string `json:"code"`
	}
	if !decode(w, r, &req) {
		return
	}
	if !s.mailAvailable(w) {
		return
	}
	err := s.st.UseProof(cu.user.ID, "", "verify_email", req.Code, "")
	if err != nil {
		s.securityError(w, err)
		return
	}
	u, err := s.st.UserByID(cu.user.ID)
	if err != nil {
		s.securityError(w, err)
		return
	}
	// Only an address that was ever proven is told about the change: an
	// unconfirmed one may be a typo, which is somebody else's inbox.
	if cu.user.EmailVerified && cu.user.Email != u.Email {
		s.notice(cu.user, "email_changed", "O e-mail da sua conta foi trocado.")
	}
	writeJSON(w, 200, map[string]any{"user": ownUserJSON(u)})
}
func (s *Server) handleEmailResend(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct{}
	if r.ContentLength > 0 && !decode(w, r, &req) {
		return
	}
	if !s.mailAvailable(w) {
		return
	}
	key := strconv.FormatInt(cu.user.ID, 10)
	if !s.reserve(w, "email_resend_minute", key, time.Minute, 1) || !s.reserve(w, "email_resend_hour", key, time.Hour, 5) {
		return
	}
	target, err := s.st.PendingEmail(cu.user.ID)
	if err != nil {
		target = cu.user.Email
	}
	if target == "" {
		fail(w, 400, "bad_email")
		return
	}
	if err = s.sendVerification(cu.user.ID, target); err != nil {
		s.securityError(w, err)
		return
	}
	writeJSON(w, 202, map[string]bool{"ok": true})
}

func (s *Server) handleLogin2FA(w http.ResponseWriter, r *http.Request) {
	if !publicWrite(s, w, r) {
		return
	}
	var req struct {
		Challenge string `json:"challenge"`
		Code      string `json:"code"`
	}
	if !decode(w, r, &req) {
		return
	}
	ip := s.clientIP(r)
	ipAllowed, ipReceipt, err := s.st.ReserveAttemptID(store.FailIP, ip, FailureWindow, IPLimit)
	if err != nil || !ipAllowed {
		fail(w, 429, "rate_limited")
		return
	}
	p, err := s.st.Proof(req.Challenge, "login_challenge")
	if err != nil {
		s.securityError(w, err)
		return
	}
	u, err := s.st.UserByID(p.UserID)
	if err != nil {
		s.securityError(w, err)
		return
	}
	if !s.spendAttempt(store.FailAccount, u.Username, AccountLimit) {
		fail(w, 429, "rate_limited")
		return
	}
	if err = s.st.UseProof(u.ID, req.Challenge, "login_challenge", req.Code, ""); err != nil {
		s.securityError(w, err)
		return
	}
	// As in the password step: the address gets this attempt back, not a
	// clean slate.
	_ = s.st.ClearFailures(store.FailAccount, u.Username)
	_ = s.st.RefundAttempt(store.FailIP, ip, ipReceipt)
	s.startSessionForHash(w, r, u, p.PasswordHash, 200)
}

func (s *Server) handleForgot(w http.ResponseWriter, r *http.Request) {
	if !publicWrite(s, w, r) {
		return
	}
	var req struct {
		Identifier string `json:"identifier"`
	}
	if !decode(w, r, &req) {
		return
	}
	// Availability, lockout, membership and ownership all have the same answer,
	// and the same time. Writing the answer first was not enough: it stays in
	// the response buffer until the handler returns, so an existing account —
	// two more writes to the database — answered about a millisecond later.
	// Only the per-address booking, the same for everybody, stays on the
	// answer's path; it also bounds how many of the goroutines one address
	// can start.
	ipOK, err := s.st.ReserveAttempt("forgot_ip", s.clientIP(r), time.Hour, 20)
	writeJSON(w, 202, map[string]bool{"ok": true})
	if err != nil || !ipOK {
		return
	}
	go s.forgotMail(req.Identifier)
}

// forgotMail is the part of "forgot my password" that depends on whether the
// account exists, run after the answer has gone.
func (s *Server) forgotMail(identifier string) {
	u, hash, err := s.st.UserByIdentifier(identifier)
	if err != nil || u.Disabled {
		return
	}
	ok, err := s.st.ReserveAttempt("forgot_account", strconv.FormatInt(u.ID, 10), time.Hour, 3)
	if err != nil || !ok {
		return
	}
	if !u.EmailVerified || s.cfg.Mailer == nil {
		return
	}
	token, err := s.st.IssueProof(u.ID, "reset_password", "", "", 30*time.Minute, store.ProofBinding{PasswordHash: hash, Version: u.SecurityVersion})
	if err != nil {
		return
	}
	link := strings.TrimSuffix(s.cfg.BaseURL, "/") + "/#reset=" + token
	s.sendMail(u.ID, u.Email, "reset_password", "Redefinir sua senha do harmony", "Para criar uma nova senha, abra:\n"+link+"\nO link vale por 30 minutos e só funciona uma vez.\nSe não foi você, ignore este e-mail. Sua senha continua a mesma.")
}
func (s *Server) handleReset(w http.ResponseWriter, r *http.Request) {
	if !publicWrite(s, w, r) {
		return
	}
	var req struct {
		Token    string `json:"token"`
		Password string `json:"password"`
		Code     string `json:"code"`
	}
	if !decode(w, r, &req) {
		return
	}
	if !s.mailAvailable(w) || !s.reserve(w, "reset_ip", s.clientIP(r), FailureWindow, IPLimit) {
		return
	}
	if len(req.Password) < passwd.MinLength || len(req.Password) > passwd.MaxLength {
		fail(w, 400, "weak_password")
		return
	}
	p, err := s.st.Proof(req.Token, "reset_password")
	if err != nil {
		s.securityError(w, err)
		return
	}
	if p.Method == "totp" && req.Code == "" {
		fail(w, 400, "totp_required")
		return
	}
	if !s.reserve(w, "reset_account", strconv.FormatInt(p.UserID, 10), FailureWindow, AccountLimit) {
		return
	}
	release, held := s.holdHashSlot(r)
	if !held {
		return
	}
	defer release()
	hash, err := passwd.Hash(req.Password, s.params)
	if err == nil {
		err = s.st.UseProof(p.UserID, req.Token, "reset_password", req.Code, hash)
	}
	if err != nil {
		s.securityError(w, err)
		return
	}
	u, err := s.st.UserByID(p.UserID)
	if err != nil {
		s.securityError(w, err)
		return
	}
	s.endAccountAccess(u.ID, u.Username)
	s.notice(u, "password_reset", "A senha da sua conta foi redefinida. Todos os aparelhos foram desconectados.")
	writeJSON(w, 200, map[string]bool{"ok": true})
}
func (s *Server) handlePassword(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		Current string `json:"current"`
		New     string `json:"new"`
	}
	if !decode(w, r, &req) || !s.mailAvailable(w) {
		return
	}
	if len(req.New) < passwd.MinLength || len(req.New) > passwd.MaxLength {
		fail(w, 400, "weak_password")
		return
	}
	old, ok := s.currentPassword(w, r, cu, req.Current)
	if !ok {
		return
	}
	release, held := s.holdHashSlot(r)
	if !held {
		return
	}
	defer release()
	hash, err := passwd.Hash(req.New, s.params)
	if err == nil {
		err = s.st.ChangePassword(cu.user.ID, old, hash, cu.sess.ID)
	}
	if err != nil {
		s.securityError(w, err)
		return
	}
	if media := s.media.Load(); media != nil {
		media.LeaveAllExceptLogin(cu.user.ID, cu.sess.ID)
	}
	s.hub.dropAllExcept(cu.user.ID, cu.sess.ID)
	s.notice(cu.user, "password_changed", "A senha da sua conta foi trocada. Os outros aparelhos foram desconectados.")
	writeJSON(w, 200, map[string]bool{"ok": true})
}

func (s *Server) handleFactor(w http.ResponseWriter, r *http.Request, cu ctxUser, action string) {
	var req struct {
		Password string `json:"password"`
		Code     string `json:"code"`
	}
	if !decode(w, r, &req) || !s.mailAvailable(w) {
		return
	}
	hash, ok := s.currentPassword(w, r, cu, req.Password)
	if !ok {
		return
	}
	if !cu.user.EmailVerified {
		fail(w, 403, "email_required")
		return
	}
	if !s.reserve(w, "factor_changes", strconv.FormatInt(cu.user.ID, 10), FailureWindow, 5) {
		return
	}
	if action == "totp/setup" {
		secret, err := s.st.SetupTOTP(cu.user.ID, hash)
		if err != nil {
			s.securityError(w, err)
			return
		}
		uri := "otpauth://totp/harmony:" + url.PathEscape(cu.user.Username) + "?secret=" + secret + "&issuer=harmony&algorithm=SHA1&digits=6&period=30"
		writeJSON(w, 200, map[string]string{"secret": secret, "uri": uri})
		return
	}
	if action == "disable" && cu.user.TwoFA == "email" && req.Code == "" {
		if !s.reserve(w, "disable_email", strconv.FormatInt(cu.user.ID, 10), time.Minute, 1) {
			return
		}
		code, err := store.NewEmailCode()
		if err == nil {
			err = s.st.IssueDisableCode(cu.user.ID, code)
		}
		if err != nil {
			s.securityError(w, err)
			return
		}
		s.sendCode(cu.user.ID, cu.user.Email, "disable_2fa", code)
		writeJSON(w, 202, map[string]bool{"code_required": true})
		return
	}
	method := action
	switch action {
	case "totp/enable":
		method = "totp"
	case "email/enable":
		method = "email"
	}
	codes, err := s.st.ChangeFactor(cu.user.ID, hash, method, req.Code)
	if err != nil {
		s.securityError(w, err)
		return
	}
	what := map[string]string{
		"totp":     "foi ligada, com o app autenticador.",
		"email":    "foi ligada, com código por e-mail.",
		"disable":  "foi desligada.",
		"recovery": "ganhou códigos de recuperação novos; os antigos não valem mais.",
	}[method]
	s.notice(cu.user, "twofa_changed", "A verificação em duas etapas da sua conta "+what)
	u, err := s.st.UserByID(cu.user.ID)
	if err != nil {
		s.securityError(w, err)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, 200, map[string]any{"ok": true, "recovery_codes": codes, "user": ownUserJSON(u)})
}
