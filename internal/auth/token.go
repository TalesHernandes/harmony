// Package auth issues and verifies the shared-passphrase access token and
// rate-limits the endpoint that issues it. No accounts, no database.
package auth

import (
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"strconv"
	"strings"
	"time"
)

// TokenTTL is how long an issued token stays valid.
const TokenTTL = 12 * time.Hour

var (
	ErrBadToken = errors.New("auth: malformed token")
	ErrExpired  = errors.New("auth: token expired")
	ErrBadSig   = errors.New("auth: bad signature")
)

// Tokens mints and verifies HMAC-SHA256 tokens of the form
// base64url(exp) "." base64url(mac). exp is Unix seconds.
type Tokens struct {
	secret []byte
	now    func() time.Time
}

// NewTokens returns a Tokens signer keyed by secret.
func NewTokens(secret []byte) *Tokens {
	return &Tokens{secret: secret, now: time.Now}
}

func (t *Tokens) sign(payload string) []byte {
	m := hmac.New(sha256.New, t.secret)
	m.Write([]byte(payload))
	return m.Sum(nil)
}

// Mint issues a token that expires TokenTTL from now.
func (t *Tokens) Mint() string {
	exp := strconv.FormatInt(t.now().Add(TokenTTL).Unix(), 10)
	enc := base64.RawURLEncoding
	return enc.EncodeToString([]byte(exp)) + "." + enc.EncodeToString(t.sign(exp))
}

// Verify checks signature and expiry.
func (t *Tokens) Verify(token string) error {
	head, sig, ok := strings.Cut(token, ".")
	if !ok {
		return ErrBadToken
	}
	enc := base64.RawURLEncoding
	expB, err := enc.DecodeString(head)
	if err != nil {
		return ErrBadToken
	}
	got, err := enc.DecodeString(sig)
	if err != nil {
		return ErrBadToken
	}
	if subtle.ConstantTimeCompare(got, t.sign(string(expB))) != 1 {
		return ErrBadSig
	}
	exp, err := strconv.ParseInt(string(expB), 10, 64)
	if err != nil {
		return ErrBadToken
	}
	if t.now().Unix() >= exp {
		return ErrExpired
	}
	return nil
}

// CheckPassphrase compares in constant time.
func CheckPassphrase(expected, got string) bool {
	return subtle.ConstantTimeCompare([]byte(expected), []byte(got)) == 1
}
