package auth

import (
	"errors"
	"strings"
	"testing"
	"time"
)

func TestTokenRoundTrip(t *testing.T) {
	tk := NewTokens([]byte("secret"))
	tok := tk.Mint()
	if err := tk.Verify(tok); err != nil {
		t.Fatalf("verify fresh token: %v", err)
	}
}

func TestTokenExpires(t *testing.T) {
	tk := NewTokens([]byte("secret"))
	base := time.Unix(1_700_000_000, 0)
	tk.now = func() time.Time { return base }
	tok := tk.Mint()

	tk.now = func() time.Time { return base.Add(TokenTTL - time.Second) }
	if err := tk.Verify(tok); err != nil {
		t.Fatalf("should still be valid: %v", err)
	}
	tk.now = func() time.Time { return base.Add(TokenTTL) }
	if err := tk.Verify(tok); !errors.Is(err, ErrExpired) {
		t.Fatalf("want ErrExpired, got %v", err)
	}
}

func TestTokenTamper(t *testing.T) {
	tk := NewTokens([]byte("secret"))
	tok := tk.Mint()
	head, sig, _ := strings.Cut(tok, ".")

	// Different exp, same signature.
	if err := tk.Verify("OTk5OTk5OTk5OTk" + "." + sig); !errors.Is(err, ErrBadSig) {
		t.Fatalf("forged exp: want ErrBadSig, got %v", err)
	}
	// Same exp, different signature. Swapping the first character for
	// another one from the base64url alphabet, rather than XOR-ing the byte:
	// `c ^ 3` lands outside the alphabet for several valid characters ('9',
	// '8', '-', '_', 'X', 'x'), and Verify then fails at decoding with
	// ErrMalformed instead of at the signature check. Mint() picks a fresh
	// exp every run, so which character starts the signature is effectively
	// random — that made this assertion fail roughly one run in ten.
	flipped := []byte(sig)
	if flipped[0] == 'A' {
		flipped[0] = 'B'
	} else {
		flipped[0] = 'A'
	}
	if err := tk.Verify(head + "." + string(flipped)); !errors.Is(err, ErrBadSig) {
		t.Fatalf("flipped sig: want ErrBadSig, got %v", err)
	}
	// Other secret.
	if err := NewTokens([]byte("other")).Verify(tok); !errors.Is(err, ErrBadSig) {
		t.Fatalf("other secret: want ErrBadSig, got %v", err)
	}
	for _, bad := range []string{"", "nodot", "a.b.c", "!!!.x", head + ".!!!"} {
		if err := tk.Verify(bad); err == nil {
			t.Fatalf("%q verified", bad)
		}
	}
}

func TestPassphrase(t *testing.T) {
	if !CheckPassphrase("abc", "abc") || CheckPassphrase("abc", "abd") || CheckPassphrase("abc", "") {
		t.Fatal("CheckPassphrase mismatch")
	}
}

func TestRateLimiter(t *testing.T) {
	base := time.Unix(1_700_000_000, 0)
	rl := NewRateLimiter(5, time.Minute)
	rl.now = func() time.Time { return base }

	for i := 0; i < 5; i++ {
		if !rl.Allow("1.2.3.4") {
			t.Fatalf("hit %d should be allowed", i+1)
		}
	}
	if rl.Allow("1.2.3.4") {
		t.Fatal("6th hit should be blocked")
	}
	if !rl.Allow("5.6.7.8") {
		t.Fatal("other IP should be independent")
	}
	rl.now = func() time.Time { return base.Add(time.Minute) }
	if !rl.Allow("1.2.3.4") {
		t.Fatal("new window should reset")
	}
	if _, stale := rl.buckets["5.6.7.8"]; stale {
		t.Fatal("expired bucket should have been swept")
	}
}
