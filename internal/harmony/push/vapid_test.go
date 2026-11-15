// Copied from internal/push together with vapid.go: the signature is the half
// of Web Push that fails silently — a push service answers a wrong one with a
// bare 401 that says nothing — so the copy travels with the tests that prove it
// is right.
package push

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"math/big"
	"strings"
	"testing"
	"time"
)

// testKeys is a fixed pair so the tests are deterministic. Generated with
// `harmony -vapid-keygen`; it protects nothing.
const (
	testPublic  = "BD9b0ova7IUrKv7n74YBoFsHVvHcAW0LnkV2zRypHsBYxFWWVafLscF3mlT4hbOqt_HuSDT7R6AYbGeiZtb4usU"
	testPrivate = "qrVt1wh2YAOkNdVpkO6mRjBvIyJqmK299DNzcFPBgn4"
	testSubject = "mailto:someone@example.org"
)

func mustKeys(t *testing.T) *Keys {
	t.Helper()
	k, err := NewKeys(testPublic, testPrivate, testSubject)
	if err != nil {
		t.Fatalf("NewKeys: %v", err)
	}
	return k
}

// TestAuthorizationIsAVerifiableES256JWT is the test that matters: it does not
// check that we produced *something*, it verifies our own signature the way a
// push service does, and reads back every claim RFC 8292 requires.
func TestAuthorizationIsAVerifiableES256JWT(t *testing.T) {
	k := mustKeys(t)
	now := time.Unix(1_757_000_000, 0)

	header, err := k.authorization("https://fcm.googleapis.com/fcm/send/abc123?x=1", now)
	if err != nil {
		t.Fatalf("authorization: %v", err)
	}

	scheme, rest, ok := strings.Cut(header, " ")
	if !ok || scheme != "vapid" {
		t.Fatalf("want a `vapid ...` header, got %q", header)
	}
	var jws, pub string
	for _, part := range strings.Split(rest, ", ") {
		name, value, _ := strings.Cut(part, "=")
		switch name {
		case "t":
			jws = value
		case "k":
			pub = value
		}
	}
	if pub != testPublic {
		t.Errorf("k= should be the application server key\n got %q\nwant %q", pub, testPublic)
	}

	parts := strings.Split(jws, ".")
	if len(parts) != 3 {
		t.Fatalf("want a three-part compact JWS, got %d parts", len(parts))
	}
	enc := base64.RawURLEncoding

	headerJSON, err := enc.DecodeString(parts[0])
	if err != nil {
		t.Fatalf("decode header: %v", err)
	}
	var head struct{ Typ, Alg string }
	if err := json.Unmarshal(headerJSON, &head); err != nil {
		t.Fatalf("header json: %v", err)
	}
	if head.Alg != "ES256" || head.Typ != "JWT" {
		t.Errorf("want typ=JWT alg=ES256, got %+v", head)
	}

	claimsJSON, err := enc.DecodeString(parts[1])
	if err != nil {
		t.Fatalf("decode claims: %v", err)
	}
	var claims struct {
		Aud string `json:"aud"`
		Exp int64  `json:"exp"`
		Sub string `json:"sub"`
	}
	if err := json.Unmarshal(claimsJSON, &claims); err != nil {
		t.Fatalf("claims json: %v", err)
	}
	// The audience is the origin only. A push service rejects a token whose
	// aud carries the endpoint path, and that is an easy mistake to make.
	if claims.Aud != "https://fcm.googleapis.com" {
		t.Errorf("aud = %q, want the endpoint origin with no path", claims.Aud)
	}
	if claims.Sub != testSubject {
		t.Errorf("sub = %q, want %q", claims.Sub, testSubject)
	}
	if want := now.Add(tokenTTL).Unix(); claims.Exp != want {
		t.Errorf("exp = %d, want %d", claims.Exp, want)
	}
	if claims.Exp-now.Unix() >= int64((24 * time.Hour).Seconds()) {
		t.Errorf("exp is more than the 24 h RFC 8292 allows")
	}

	sig, err := enc.DecodeString(parts[2])
	if err != nil {
		t.Fatalf("decode signature: %v", err)
	}
	// JWA fixed-width r||s, not ASN.1 DER. A DER signature would be a
	// different length and would verify with none of the code below — which
	// is exactly how this bug reaches production as an unexplained 401.
	if len(sig) != 2*scalarLen {
		t.Fatalf("signature is %d bytes, want %d (raw r||s, not DER)", len(sig), 2*scalarLen)
	}

	digest := sha256.Sum256([]byte(parts[0] + "." + parts[1]))
	pubBytes, err := enc.DecodeString(testPublic)
	if err != nil {
		t.Fatalf("decode public key: %v", err)
	}
	verifier := &ecdsa.PublicKey{
		Curve: elliptic.P256(),
		X:     new(big.Int).SetBytes(pubBytes[1:33]),
		Y:     new(big.Int).SetBytes(pubBytes[33:]),
	}
	r := new(big.Int).SetBytes(sig[:scalarLen])
	s := new(big.Int).SetBytes(sig[scalarLen:])
	if !ecdsa.Verify(verifier, digest[:], r, s) {
		t.Error("the signature does not verify against the advertised public key")
	}
}

func TestGeneratedKeysRoundTrip(t *testing.T) {
	pub, priv, err := Generate()
	if err != nil {
		t.Fatalf("Generate: %v", err)
	}
	k, err := NewKeys(pub, priv, testSubject)
	if err != nil {
		t.Fatalf("NewKeys on generated pair: %v", err)
	}
	if k.PublicKey() != pub {
		t.Errorf("PublicKey() = %q, want %q", k.PublicKey(), pub)
	}
}

// A mismatched pair is the easiest .env typo there is, because the two values
// look identical. Catching it at boot beats catching it as a 401 whose body
// explains nothing.
func TestMismatchedKeyPairIsRejected(t *testing.T) {
	otherPub, _, err := Generate()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := NewKeys(otherPub, testPrivate, testSubject); err == nil {
		t.Fatal("want an error for a public key that is not the private key's")
	}
}

func TestKeyValidation(t *testing.T) {
	cases := []struct {
		name, pub, priv, subject string
	}{
		{"short private", testPublic, base64.RawURLEncoding.EncodeToString([]byte("nope")), testSubject},
		{"not base64", testPublic, "!!!!", testSubject},
		{"public not a point", base64.RawURLEncoding.EncodeToString(make([]byte, pointLen)), testPrivate, testSubject},
		{"empty subject", testPublic, testPrivate, ""},
		{"subject not a URL", testPublic, testPrivate, "someone@example.org"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if _, err := NewKeys(c.pub, c.priv, c.subject); err == nil {
				t.Fatal("want an error")
			}
		})
	}
}

// Padded base64url is what some tools emit, and the value is pasted by hand.
func TestPaddedBase64IsAccepted(t *testing.T) {
	padded := base64.URLEncoding.EncodeToString(mustDecode(t, testPrivate))
	if !strings.HasSuffix(padded, "=") {
		t.Skip("this key happens not to need padding")
	}
	if _, err := NewKeys(testPublic, padded, testSubject); err != nil {
		t.Fatalf("padded private key rejected: %v", err)
	}
}

func TestAudienceRejectsNonHTTPSEndpoints(t *testing.T) {
	k := mustKeys(t)
	for _, endpoint := range []string{"http://push.example.org/x", "file:///etc/passwd", "not a url", ""} {
		if _, err := k.authorization(endpoint, time.Now()); err == nil {
			t.Errorf("endpoint %q should be rejected", endpoint)
		}
	}
}

func mustDecode(t *testing.T, s string) []byte {
	t.Helper()
	b, err := decodeB64(s)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// TestASubjectErrorDoesNotRepeatTheValue: the private key pasted into the
// subject by mistake used to be copied into the error, and main logs that
// error on the way down.
func TestASubjectErrorDoesNotRepeatTheValue(t *testing.T) {
	_, err := NewKeys(testPublic, testPrivate, testPrivate)
	if err == nil {
		t.Fatal("a key as the subject was accepted")
	}
	if strings.Contains(err.Error(), testPrivate) {
		t.Fatalf("the error carries the value it refused: %v", err)
	}
}
