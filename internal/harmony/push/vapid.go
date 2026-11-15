// Package push is the server half of Web Push (RFC 8030) with VAPID (RFC 8292),
// on the standard library alone: sign one ES256 JWT, POST one empty body.
//
// Why this is a copy of internal/push, and not that package reused. The 1:1
// call app is in production on another machine and its deploy is not part of
// this sprint; sharing the package would mean every change made for harmony
// lands in a binary nobody here is testing or restarting. The two also want
// different things from it: the 1:1 app has one doorbell for one meaning and
// keeps its subscriptions in a JSON file, while harmony has many people, many
// kinds of notification, and a database with a sessions table that already
// means "this device, signed in as this person". What survived the copy is the
// part that is pure protocol — the VAPID signature and the POST — which is also
// the part with the trap in it (see sign). Roughly 300 of the 1.580 lines came
// across; the rest of that package is the 1:1 app's own answers.
//
// Why no encrypted payload: RFC 8291 exists so a push can carry data. Here the
// push carries nothing and the woken service worker asks the server what to
// show, over the session it already has. That drops the ECDH/HKDF/AES-GCM half
// of the problem, and it means the text of what happened never passes through
// Google's or Mozilla's push service at all — see notify.go.
package push

import (
	"crypto/ecdh"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"math/big"
	"net/url"
	"strings"
	"time"
)

// tokenTTL is how long a VAPID JWT stays valid. RFC 8292 §2 caps this at 24 h;
// a fresh one is signed per request anyway, so this is only slack for clock
// skew between us and the push service.
const tokenTTL = 12 * time.Hour

// P-256 sizes: a scalar is 32 bytes and an uncompressed point is 0x04
// followed by the two 32-byte coordinates.
const (
	scalarLen = 32
	pointLen  = 65
)

// Keys is the application server's VAPID identity: one P-256 key pair plus
// the contact subject that identifies us to the push service.
type Keys struct {
	private   *ecdsa.PrivateKey
	publicB64 string
	subject   string
}

// Generate returns a fresh VAPID key pair, base64url encoded, for the
// `-vapid-keygen` flag of cmd/harmony.
func Generate() (publicB64, privateB64 string, err error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return "", "", err
	}
	pub, err := key.PublicKey.ECDH()
	if err != nil {
		return "", "", err
	}
	enc := base64.RawURLEncoding
	return enc.EncodeToString(pub.Bytes()),
		enc.EncodeToString(key.D.FillBytes(make([]byte, scalarLen))),
		nil
}

// NewKeys parses a base64url key pair and the VAPID subject (a mailto: or
// https: URL identifying the operator).
//
// The public key is not merely decoded, it is checked against the one derived
// from the private key. A mismatched pair — the easiest possible .env typo,
// since the two values are indistinguishable blobs — otherwise fails only at
// send time, as a 401 from a push service whose error body says nothing about
// which of the two is wrong.
func NewKeys(publicB64, privateB64, subject string) (*Keys, error) {
	priv, err := decodeB64(privateB64)
	if err != nil {
		return nil, fmt.Errorf("vapid private key: %w", err)
	}
	if len(priv) != scalarLen {
		return nil, fmt.Errorf("vapid private key: got %d bytes, want %d", len(priv), scalarLen)
	}
	pub, err := decodeB64(publicB64)
	if err != nil {
		return nil, fmt.Errorf("vapid public key: %w", err)
	}
	if len(pub) != pointLen || pub[0] != 4 {
		return nil, fmt.Errorf("vapid public key: want %d bytes of uncompressed P-256 point, got %d", pointLen, len(pub))
	}
	if subject == "" {
		return nil, errors.New("vapid subject is required (mailto: or https: URL)")
	}
	if !strings.HasPrefix(subject, "mailto:") && !strings.HasPrefix(subject, "https:") {
		// Not the value itself: whatever was pasted here by mistake — the
		// private key, say — would go straight into the log.
		return nil, errors.New("vapid subject must be a mailto: or https: URL")
	}

	// crypto/ecdh does the scalar validation (range, non-zero) and the
	// public-point derivation without touching the deprecated
	// elliptic.Marshal / ScalarBaseMult path.
	ecdhPriv, err := ecdh.P256().NewPrivateKey(priv)
	if err != nil {
		return nil, fmt.Errorf("vapid private key: %w", err)
	}
	derived := ecdhPriv.PublicKey().Bytes()
	if subtle.ConstantTimeCompare(derived, pub) != 1 {
		return nil, errors.New("vapid keys do not match: the public key is not the one derived from the private key")
	}

	key := &ecdsa.PrivateKey{
		PublicKey: ecdsa.PublicKey{
			Curve: elliptic.P256(),
			X:     new(big.Int).SetBytes(derived[1 : 1+scalarLen]),
			Y:     new(big.Int).SetBytes(derived[1+scalarLen:]),
		},
		D: new(big.Int).SetBytes(priv),
	}
	return &Keys{
		private:   key,
		publicB64: base64.RawURLEncoding.EncodeToString(derived),
		subject:   subject,
	}, nil
}

// PublicKey is the base64url application server key the browser passes to
// pushManager.subscribe().
func (k *Keys) PublicKey() string { return k.publicB64 }

// authorization builds the RFC 8292 §3 header for one request to endpoint.
func (k *Keys) authorization(endpoint string, now time.Time) (string, error) {
	aud, err := audienceOf(endpoint)
	if err != nil {
		return "", err
	}
	jws, err := k.sign(aud, now.Add(tokenTTL).Unix())
	if err != nil {
		return "", err
	}
	return "vapid t=" + jws + ", k=" + k.publicB64, nil
}

// audienceOf is the origin of the endpoint — scheme and host only. RFC 8292
// §2 is explicit that the path must not be included, and push services do
// reject a token whose `aud` carries one.
func audienceOf(endpoint string) (string, error) {
	u, err := url.Parse(endpoint)
	if err != nil {
		return "", fmt.Errorf("push endpoint: %w", err)
	}
	if u.Scheme != "https" || u.Host == "" {
		return "", fmt.Errorf("push endpoint %q is not an https URL", endpoint)
	}
	return u.Scheme + "://" + u.Host, nil
}

// sign produces a compact JWS with alg ES256.
func (k *Keys) sign(audience string, exp int64) (string, error) {
	enc := base64.RawURLEncoding
	header := enc.EncodeToString([]byte(`{"typ":"JWT","alg":"ES256"}`))
	claims, err := json.Marshal(struct {
		Aud string `json:"aud"`
		Exp int64  `json:"exp"`
		Sub string `json:"sub"`
	}{audience, exp, k.subject})
	if err != nil {
		return "", err
	}
	signingInput := header + "." + enc.EncodeToString(claims)

	digest := sha256.Sum256([]byte(signingInput))
	r, s, err := ecdsa.Sign(rand.Reader, k.private, digest[:])
	if err != nil {
		return "", err
	}
	// JWS wants the raw fixed-width r||s of JWA (RFC 7518 §3.4), not the
	// ASN.1 DER that ecdsa.SignASN1 returns. Reaching for the obvious
	// function here produces a signature every push service rejects with a
	// bare 401, and nothing in that response says why — this is the one real
	// trap in hand-rolled VAPID.
	sig := make([]byte, 2*scalarLen)
	r.FillBytes(sig[:scalarLen])
	s.FillBytes(sig[scalarLen:])

	return signingInput + "." + enc.EncodeToString(sig), nil
}

// decodeB64 accepts base64url with or without padding: the keys are pasted
// into .env by hand and both forms are in circulation.
func decodeB64(s string) ([]byte, error) {
	s = strings.TrimRight(strings.TrimSpace(s), "=")
	return base64.RawURLEncoding.DecodeString(s)
}
