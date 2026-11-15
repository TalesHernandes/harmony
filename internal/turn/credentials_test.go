package turn

import (
	"crypto/hmac"
	"crypto/sha1"
	"encoding/base64"
	"testing"
	"time"
)

func TestICEServerMatchesCoturnRESTScheme(t *testing.T) {
	secret := []byte("turn-secret-turn-secret")
	urls := []string{"turn:turn.example.org:3478?transport=udp", "turn:turn.example.org:3478?transport=tcp"}
	c := New(secret, urls)
	base := time.Unix(1_700_000_000, 0)
	c.now = func() time.Time { return base }

	s := c.ICEServer("ab12cd34")

	if len(s.URLs) != 2 || s.URLs[0] != urls[0] || s.URLs[1] != urls[1] {
		t.Fatalf("urls not forwarded verbatim: %v", s.URLs)
	}
	if want := "1700007200:ab12cd34"; s.Username != want {
		t.Fatalf("username: want %q got %q", want, s.Username)
	}
	m := hmac.New(sha1.New, secret)
	m.Write([]byte(s.Username))
	if want := base64.StdEncoding.EncodeToString(m.Sum(nil)); s.Credential != want {
		t.Fatalf("credential: want %q got %q", want, s.Credential)
	}
	if Sign(secret, s.Username) != s.Credential {
		t.Fatal("Sign must agree with ICEServer")
	}
}

func TestICEServerIsFreshPerCall(t *testing.T) {
	c := New([]byte("s"), []string{"turn:x"})
	now := time.Unix(1_700_000_000, 0)
	c.now = func() time.Time { return now }
	a := c.ICEServer("u")
	now = now.Add(time.Second)
	b := c.ICEServer("u")
	if a.Username == b.Username || a.Credential == b.Credential {
		t.Fatalf("credentials must follow the clock: %+v vs %+v", a, b)
	}
	if x, y := c.ICEServer("u1"), c.ICEServer("u2"); x.Username == y.Username {
		t.Fatal("different users must get different usernames")
	}
}

func TestURLsAreCopied(t *testing.T) {
	urls := []string{"turn:x"}
	c := New([]byte("s"), urls)
	urls[0] = "mutated"
	s := c.ICEServer("u")
	if s.URLs[0] != "turn:x" {
		t.Fatalf("New must copy urls, got %v", s.URLs)
	}
	s.URLs[0] = "mutated"
	if c.ICEServer("u").URLs[0] != "turn:x" {
		t.Fatal("ICEServer must return a fresh slice")
	}
}
