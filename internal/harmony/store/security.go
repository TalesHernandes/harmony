package store

import (
	"crypto/hmac"
	"crypto/sha1"
	"crypto/subtle"
	"database/sql"
	"encoding/base32"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"net/mail"
	"strings"
	"time"
)

func NormalizeEmail(v string) string { return strings.ToLower(strings.TrimSpace(v)) }
func ValidEmail(v string) bool {
	if len(v) > 254 || strings.ContainsAny(v, "\r\n") {
		return false
	}
	a, err := mail.ParseAddress(v)
	return err == nil && a.Address == v && strings.Contains(v, "@")
}

func (s *Store) UserByIdentifier(v string) (User, string, error) {
	v = NormalizeEmail(v)
	return s.scanUser(s.db.QueryRow(`SELECT id,username,display_name,password_hash,is_admin,created_at,disabled_at,avatar_version,COALESCE(email,''),email_verified_at IS NOT NULL,twofa,security_version FROM users WHERE username=? OR email=?`, v, v))
}

// TOTPCode implements RFC 6238's SHA-1 variant. Digits is configurable for
// the RFC's eight-digit test vectors; the public service always uses six.
func TOTPCode(secret string, step int64, digits int) string {
	key, err := base32.StdEncoding.WithPadding(base32.NoPadding).DecodeString(secret)
	if err != nil || step < 0 || (digits != 6 && digits != 8) {
		return ""
	}
	var b [8]byte
	binary.BigEndian.PutUint64(b[:], uint64(step))
	h := hmac.New(sha1.New, key)
	h.Write(b[:])
	sum := h.Sum(nil)
	off := sum[len(sum)-1] & 15
	n := binary.BigEndian.Uint32(sum[off:off+4]) & 0x7fffffff
	mod := uint32(1000000)
	if digits == 8 {
		mod = 100000000
	}
	return fmt.Sprintf("%0*d", digits, n%mod)
}

func NewSecuritySecret() (string, error) {
	b, err := randomBytes(32)
	if err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}
func NewEmailCode() (string, error) {
	// Rejection sampling avoids a modulo bias in the six-digit space.
	for {
		b, err := randomBytes(4)
		if err != nil {
			return "", err
		}
		n := binary.BigEndian.Uint32(b)
		if n < 4294000000 {
			return fmt.Sprintf("%06d", n%1000000), nil
		}
	}
}

type ProofBinding struct {
	PasswordHash string
	Version      int64
}

type SecurityProof struct {
	Version                                    int64
	ID                                         string
	UserID                                     int64
	Purpose, TargetEmail, PasswordHash, Method string
}

// IssueProof invalidates earlier proofs for this purpose and binds the proof
// to the password and second-factor configuration which issued it.
func (s *Store) IssueProof(userID int64, purpose, target, code string, ttl time.Duration, bindings ...ProofBinding) (string, error) {
	token, err := NewSecuritySecret()
	if err != nil {
		return "", err
	}
	id, err := newID()
	if err != nil {
		return "", err
	}
	tx, err := s.db.Begin()
	if err != nil {
		return "", err
	}
	defer tx.Rollback()
	if _, err = tx.Exec(`DELETE FROM security_tokens WHERE user_id=? AND purpose=?`, userID, purpose); err != nil {
		return "", err
	}
	codeHash := ""
	if code != "" {
		codeHash = hashSecret(token + ":" + code)
	}
	if len(bindings) > 0 {
		var n int
		if err = tx.QueryRow(`SELECT count(*) FROM users WHERE id=? AND password_hash=? AND security_version=? AND disabled_at IS NULL`, userID, bindings[0].PasswordHash, bindings[0].Version).Scan(&n); err != nil {
			return "", err
		}
		if n != 1 {
			return "", ErrCredentials
		}
	}
	res, err := tx.Exec(`INSERT INTO security_tokens(id,user_id,purpose,secret_hash,code_hash,target_email,password_hash,method,expires_at,security_version) SELECT ?,id,?,?,?,?,password_hash,twofa,?,security_version FROM users WHERE id=? AND disabled_at IS NULL`, id, purpose, hashSecret(token), codeHash, target, s.now().Add(ttl).Unix(), userID)
	if err == nil {
		n, _ := res.RowsAffected()
		if n != 1 {
			return "", ErrCredentials
		}
	}
	if err != nil {
		return "", err
	}
	return token, tx.Commit()
}

// Email codes are scoped to a signed-in user, so they do not need a public
// bearer token. Their hash is salted by the row id, never by an address.
func (s *Store) IssueEmailProof(userID int64, target, code string, bindings ...ProofBinding) error {
	token, err := s.IssueProof(userID, "verify_email", target, "", 10*time.Minute, bindings...)
	if err != nil {
		return err
	}
	_, err = s.db.Exec(`UPDATE security_tokens SET code_hash=? WHERE secret_hash=?`, hashSecret(hashSecret(token)+":"+code), hashSecret(token))
	return err
}

func (s *Store) PendingEmail(userID int64) (string, error) {
	var email string
	err := s.db.QueryRow(`SELECT target_email FROM security_tokens WHERE user_id=? AND purpose='verify_email' AND used_at IS NULL ORDER BY rowid DESC LIMIT 1`, userID).Scan(&email)
	if err == sql.ErrNoRows {
		return "", ErrNotFound
	}
	return email, err
}

func (s *Store) Proof(token, purpose string) (SecurityProof, error) {
	var p SecurityProof
	var attempts int
	err := s.db.QueryRow(`SELECT t.id,t.user_id,t.purpose,t.target_email,t.password_hash,t.method,t.security_version,t.attempts FROM security_tokens t JOIN users u ON u.id=t.user_id WHERE t.secret_hash=? AND t.purpose=? AND t.used_at IS NULL AND t.expires_at>? AND u.disabled_at IS NULL AND u.password_hash=t.password_hash AND u.twofa=t.method AND u.security_version=t.security_version`, hashSecret(token), purpose, s.now().Unix()).Scan(&p.ID, &p.UserID, &p.Purpose, &p.TargetEmail, &p.PasswordHash, &p.Method, &p.Version, &attempts)
	if err == sql.ErrNoRows {
		return p, ErrCredentials
	}
	if err == nil && attempts >= 5 {
		return p, ErrRateLimited
	}
	return p, err
}

func (s *Store) verifyTOTP(tx *sql.Tx, userID int64, code string, pending bool) bool {
	if len(code) != 6 {
		return false
	}
	var secret string
	var last, until int64
	query := `SELECT totp_secret,totp_last_step,0 FROM users WHERE id=?`
	if pending {
		query = `SELECT totp_pending,-1,totp_pending_until FROM users WHERE id=?`
	}
	if tx.QueryRow(query, userID).Scan(&secret, &last, &until) != nil || secret == "" || (pending && until <= s.now().Unix()) {
		return false
	}
	matched := int64(-1)
	step := s.now().Unix() / 30
	for d := int64(-1); d <= 1; d++ {
		expected := TOTPCode(secret, step+d, 6)
		equal := subtle.ConstantTimeCompare([]byte(expected), []byte(code))
		if equal == 1 && step+d > last {
			matched = step + d
		}
	}
	if matched < 0 {
		return false
	}
	res, err := tx.Exec(`UPDATE users SET totp_last_step=? WHERE id=? AND totp_last_step<?`, matched, userID, matched)
	if pending {
		res, err = tx.Exec(`UPDATE users SET totp_last_step=? WHERE id=?`, matched, userID)
	}
	if err != nil {
		return false
	}
	n, _ := res.RowsAffected()
	return n == 1
}

func (s *Store) secondFactor(tx *sql.Tx, userID int64, code string) bool {
	if s.verifyTOTP(tx, userID, strings.TrimSpace(code), false) {
		return true
	}
	normalized := normalizeRecovery(code)
	res, err := tx.Exec(`UPDATE recovery_codes SET used_at=? WHERE user_id=? AND code_hash=? AND used_at IS NULL`, s.now().Unix(), userID, hashSecret(normalized))
	if err != nil {
		return false
	}
	n, _ := res.RowsAffected()
	return n == 1
}

// UseProof spends an attempt before checking and commits failed attempts too.
// Successful consumption and its effect share one transaction: concurrent
// reset, verification or challenge requests cannot use the same proof twice.
func (s *Store) UseProof(userID int64, token, purpose, code, newHash string) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var id, secretHash, codeHash, target, oldHash, method string
	var attempts int
	err = tx.QueryRow(`SELECT t.id,t.secret_hash,t.code_hash,t.target_email,t.password_hash,t.method,t.attempts FROM security_tokens t JOIN users u ON u.id=t.user_id WHERE t.user_id=? AND t.purpose=? AND (?='' OR t.secret_hash=?) AND t.used_at IS NULL AND t.expires_at>? AND u.disabled_at IS NULL AND u.password_hash=t.password_hash AND u.twofa=t.method AND u.security_version=t.security_version ORDER BY t.rowid DESC LIMIT 1`, userID, purpose, token, hashSecret(token), s.now().Unix()).Scan(&id, &secretHash, &codeHash, &target, &oldHash, &method, &attempts)
	if err != nil {
		return ErrCredentials
	}
	if attempts >= 5 {
		return ErrRateLimited
	}
	if _, err = tx.Exec(`UPDATE security_tokens SET attempts=attempts+1 WHERE id=?`, id); err != nil {
		return err
	}
	good := false
	switch purpose {
	case "verify_email":
		good = subtle.ConstantTimeCompare([]byte(codeHash), []byte(hashSecret(secretHash+":"+code))) == 1
	case "login_challenge":
		if method == "email" {
			good = subtle.ConstantTimeCompare([]byte(codeHash), []byte(hashSecret(token+":"+code))) == 1
		} else if method == "totp" {
			good = s.secondFactor(tx, userID, code)
		}
	case "reset_password":
		good = method != "totp" || s.secondFactor(tx, userID, code)
	}
	if !good {
		if err = tx.Commit(); err != nil {
			return err
		}
		return ErrCredentials
	}
	if _, err = tx.Exec(`UPDATE security_tokens SET used_at=? WHERE id=?`, s.now().Unix(), id); err != nil {
		return err
	}
	switch purpose {
	case "verify_email":
		_, err = tx.Exec(`UPDATE users SET security_version=security_version+1,email=?,email_verified_at=? WHERE id=?`, target, s.now().Unix(), userID)
	case "reset_password":
		err = s.passwordTx(tx, userID, oldHash, newHash, "")
	}
	if err != nil {
		if isUniqueViolation(err) {
			return ErrNameTaken
		}
		return err
	}
	return tx.Commit()
}

func (s *Store) passwordTx(tx *sql.Tx, userID int64, oldHash, newHash, keep string) error {
	res, err := tx.Exec(`UPDATE users SET security_version=security_version+1,password_hash=?,totp_pending='',totp_pending_until=0 WHERE id=? AND password_hash=? AND disabled_at IS NULL`, newHash, userID, oldHash)
	if err != nil {
		return err
	}
	n, _ := res.RowsAffected()
	if n != 1 {
		return ErrCredentials
	}
	if _, err = tx.Exec(`UPDATE sessions SET revoked_at=? WHERE user_id=? AND id<>? AND revoked_at IS NULL`, s.now().Unix(), userID, keep); err != nil {
		return err
	}
	_, err = tx.Exec(`DELETE FROM security_tokens WHERE user_id=?`, userID)
	return err
}
func (s *Store) ChangePassword(userID int64, oldHash, newHash, keep string) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if err = s.passwordTx(tx, userID, oldHash, newHash, keep); err != nil {
		return err
	}
	return tx.Commit()
}

func (s *Store) SetupTOTP(userID int64, passwordHash string) (string, error) {
	b, err := randomBytes(20)
	if err != nil {
		return "", err
	}
	secret := base32.StdEncoding.WithPadding(base32.NoPadding).EncodeToString(b)
	res, err := s.db.Exec(`UPDATE users SET totp_pending=?,totp_pending_until=? WHERE id=? AND password_hash=? AND twofa='' AND disabled_at IS NULL`, secret, s.now().Add(10*time.Minute).Unix(), userID, passwordHash)
	if err != nil {
		return "", err
	}
	n, _ := res.RowsAffected()
	if n != 1 {
		return "", ErrCredentials
	}
	return secret, nil
}

// recoveryAlphabet leaves out the characters people misread when copying a
// code from paper or a screen: 0/O, 1/I/L, 5/S and 8/B. Sixteen of the
// remaining 27 carry about 76 bits, plenty for a single-use code.
const recoveryAlphabet = "ACDEFGHJKMNPQRTUVWXYZ234679"

func recoveryCode() (string, error) {
	out := make([]byte, 0, 16)
	for len(out) < 16 {
		b, err := randomBytes(16)
		if err != nil {
			return "", err
		}
		for _, c := range b {
			// Rejection sampling: 243 is the largest multiple of 27 under 256.
			if c < 243 && len(out) < 16 {
				out = append(out, recoveryAlphabet[int(c)%len(recoveryAlphabet)])
			}
		}
	}
	return string(out), nil
}

// normalizeRecovery accepts a code however it was typed: lower case, with or
// without the dashes, with spaces where the dashes were.
func normalizeRecovery(code string) string {
	return strings.ToUpper(strings.NewReplacer("-", "", " ", "").Replace(strings.TrimSpace(code)))
}

func recoveryTx(tx *sql.Tx, userID int64) ([]string, error) {
	if _, err := tx.Exec(`DELETE FROM recovery_codes WHERE user_id=?`, userID); err != nil {
		return nil, err
	}
	codes := make([]string, 10)
	for i := range codes {
		raw, err := recoveryCode()
		if err != nil {
			return nil, err
		}
		codes[i] = raw[:4] + "-" + raw[4:8] + "-" + raw[8:12] + "-" + raw[12:]
		if _, err = tx.Exec(`INSERT INTO recovery_codes(user_id,code_hash) VALUES (?,?)`, userID, hashSecret(raw)); err != nil {
			return nil, err
		}
	}
	return codes, nil
}

// ChangeFactor checks the current password again within the transaction.
// Existing factors must be disabled with their proof before switching method.
func (s *Store) ChangeFactor(userID int64, passwordHash, action, code string) ([]string, error) {
	tx, err := s.db.Begin()
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	var method string
	var verified bool
	if err = tx.QueryRow(`SELECT twofa,email_verified_at IS NOT NULL FROM users WHERE id=? AND password_hash=? AND disabled_at IS NULL`, userID, passwordHash).Scan(&method, &verified); err != nil {
		return nil, ErrCredentials
	}
	var codes []string
	switch action {
	case "totp":
		if method != "" || !verified || !s.verifyTOTP(tx, userID, code, true) {
			return nil, ErrCredentials
		}
		_, err = tx.Exec(`UPDATE users SET security_version=security_version+1,twofa='totp',totp_secret=totp_pending,totp_pending='',totp_pending_until=0 WHERE id=?`, userID)
		if err == nil {
			codes, err = recoveryTx(tx, userID)
		}
	case "email":
		if method != "" || !verified {
			return nil, ErrCredentials
		}
		_, err = tx.Exec(`UPDATE users SET security_version=security_version+1,twofa='email',totp_pending='',totp_pending_until=0 WHERE id=?`, userID)
	case "disable":
		good := method == ""
		if method == "totp" {
			good = s.secondFactor(tx, userID, code)
		}
		if method == "email" {
			var id, secretHash, codeHash string
			var tries int
			err = tx.QueryRow(`SELECT id,secret_hash,code_hash,attempts FROM security_tokens WHERE user_id=? AND purpose='disable_2fa' AND used_at IS NULL AND expires_at>?`, userID, s.now().Unix()).Scan(&id, &secretHash, &codeHash, &tries)
			if err == nil && tries < 5 {
				tx.Exec(`UPDATE security_tokens SET attempts=attempts+1 WHERE id=?`, id)
				good = subtle.ConstantTimeCompare([]byte(codeHash), []byte(hashSecret(secretHash+":"+code))) == 1
			}
			if !good {
				if err = tx.Commit(); err != nil {
					return nil, err
				}
				return nil, ErrCredentials
			}
		}
		if !good {
			return nil, ErrCredentials
		}
		_, err = tx.Exec(`UPDATE users SET security_version=security_version+1,twofa='',totp_secret='',totp_pending='',totp_pending_until=0,totp_last_step=-1 WHERE id=?`, userID)
		if err == nil {
			_, err = tx.Exec(`DELETE FROM recovery_codes WHERE user_id=?`, userID)
		}
	case "recovery":
		if method != "totp" {
			return nil, ErrCredentials
		}
		codes, err = recoveryTx(tx, userID)
		if err == nil {
			_, err = tx.Exec(`UPDATE users SET security_version=security_version+1 WHERE id=?`, userID)
		}
	default:
		return nil, ErrCredentials
	}
	if err != nil {
		return nil, err
	}
	if _, err = tx.Exec(`DELETE FROM security_tokens WHERE user_id=?`, userID); err != nil {
		return nil, err
	}
	return codes, tx.Commit()
}

func (s *Store) IssueDisableCode(userID int64, code string) error {
	token, err := s.IssueProof(userID, "disable_2fa", "", "", 10*time.Minute)
	if err != nil {
		return err
	}
	_, err = s.db.Exec(`UPDATE security_tokens SET code_hash=? WHERE secret_hash=?`, hashSecret(hashSecret(token)+":"+code), hashSecret(token))
	return err
}

func (s *Store) ResetTwoFA(userID int64) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err = tx.Exec(`UPDATE users SET security_version=security_version+1,twofa='',totp_secret='',totp_pending='',totp_pending_until=0,totp_last_step=-1 WHERE id=?`, userID); err != nil {
		return err
	}
	for _, q := range []string{`DELETE FROM recovery_codes WHERE user_id=?`, `DELETE FROM security_tokens WHERE user_id=?`, `UPDATE sessions SET revoked_at=strftime('%s','now') WHERE user_id=? AND revoked_at IS NULL`} {
		if _, err = tx.Exec(q, userID); err != nil {
			return err
		}
	}
	return tx.Commit()
}
