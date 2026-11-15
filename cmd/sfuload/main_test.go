package main

import (
	"net/http"
	"net/http/cookiejar"
	"net/url"
	"testing"
)

// The session cookie is Secure. On this machine over plain http it must come
// back, whatever the Go version; to any other http address it must not.
func TestLoopbackJarSendsTheSecureSessionOnlyLocally(t *testing.T) {
	cases := []struct {
		base string
		want bool
	}{
		{"http://127.0.0.1:8081", true},
		{"http://localhost:8081", true},
		{"http://[::1]:8081", true},
		{"https://harmony.example.com", true},
		{"http://harmony.example.com", false},
		{"http://203.0.113.10:8081", false},
	}
	for _, tc := range cases {
		inner, err := cookiejar.New(nil)
		if err != nil {
			t.Fatal(err)
		}
		jar := loopbackJar{inner}
		u, err := url.Parse(tc.base + "/api/login")
		if err != nil {
			t.Fatal(err)
		}
		jar.SetCookies(u, []*http.Cookie{{
			Name: "harmony_session", Value: "token", Path: "/",
			HttpOnly: true, Secure: true, SameSite: http.SameSiteLaxMode,
		}})
		got := len(jar.Cookies(u)) == 1
		if got != tc.want {
			t.Errorf("%s: cookie sent = %v, want %v", tc.base, got, tc.want)
		}
	}
}
