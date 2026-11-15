package push

import (
	"context"
	"crypto"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func fcmCredentials(t *testing.T, key any, uri string) []byte {
	t.Helper()
	der, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	b, err := json.Marshal(map[string]string{"type": "service_account", "project_id": "test-project", "private_key": string(pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: der})), "client_email": "test@example.com", "token_uri": uri})
	if err != nil {
		t.Fatal(err)
	}
	return b
}
func fcmKey(t *testing.T) *rsa.PrivateKey {
	t.Helper()
	k, e := rsa.GenerateKey(rand.Reader, 2048)
	if e != nil {
		t.Fatal(e)
	}
	return k
}

func TestFCMJWTBodyAndCache(t *testing.T) {
	key := fcmKey(t)
	now := time.Unix(1800000000, 0)
	var tokens, sends atomic.Int32
	var endpoint string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != "POST" {
			t.Error("expected POST")
		}
		if r.URL.Path == "/token" {
			tokens.Add(1)
			if err := r.ParseForm(); err != nil {
				t.Error(err)
			}
			if r.Form.Get("grant_type") != "urn:ietf:params:oauth:grant-type:jwt-bearer" {
				t.Error("grant")
			}
			parts := strings.Split(r.Form.Get("assertion"), ".")
			if len(parts) != 3 {
				t.Error("JWT shape")
				return
			}
			dec := base64.RawURLEncoding
			header, _ := dec.DecodeString(parts[0])
			if string(header) != `{"alg":"RS256","typ":"JWT"}` {
				t.Error("header")
			}
			raw, _ := dec.DecodeString(parts[1])
			var c map[string]any
			_ = json.Unmarshal(raw, &c)
			if c["iss"] != "test@example.com" || c["aud"] != endpoint+"/token" || c["scope"] != "https://www.googleapis.com/auth/firebase.messaging" || c["iat"] != float64(now.Unix()) || c["exp"] != float64(now.Unix()+3600) {
				t.Errorf("claims %v", c)
			}
			sig, _ := dec.DecodeString(parts[2])
			hash := sha256.Sum256([]byte(parts[0] + "." + parts[1]))
			if err := rsa.VerifyPKCS1v15(&key.PublicKey, crypto.SHA256, hash[:], sig); err != nil {
				t.Error(err)
			}
			fmt.Fprintf(w, `{"access_token":"access-%d","expires_in":3600}`, tokens.Load())
			return
		}
		sends.Add(1)
		if r.Header.Get("Authorization") != fmt.Sprintf("Bearer access-%d", tokens.Load()) {
			t.Error("authorization")
		}
		raw, _ := io.ReadAll(r.Body)
		var got map[string]any
		_ = json.Unmarshal(raw, &got)
		m := got["message"].(map[string]any)
		a := m["android"].(map[string]any)
		if len(got) != 1 || len(m) != 3 || m["token"] != "device-secret" || len(a) != 3 || a["priority"] != "HIGH" || a["ttl"] != "60s" || a["collapse_key"] != "ring" || m["data"].(map[string]any)["k"] != "ring" {
			t.Errorf("body %s", raw)
		}
		_, _ = io.WriteString(w, `{}`)
	}))
	defer srv.Close()
	endpoint = srv.URL
	f, err := NewFCM(fcmCredentials(t, key, srv.URL+"/token"))
	if err != nil {
		t.Fatal(err)
	}
	f.SetTransportForTest(srv.Client(), srv.URL+"/send")
	f.now = func() time.Time { return now }
	send := func() {
		if err := f.Send(context.Background(), FCMMessage{Token: "device-secret", Data: map[string]string{"k": "ring"}, TTL: time.Minute, CollapseKey: "ring"}); err != nil {
			t.Error(err)
		}
	}
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() { defer wg.Done(); send() }()
	}
	wg.Wait()
	if tokens.Load() != 1 || sends.Load() != 8 {
		t.Fatalf("tokens=%d sends=%d", tokens.Load(), sends.Load())
	}
	now = now.Add(56 * time.Minute)
	send()
	if tokens.Load() != 2 {
		t.Error("token was not refreshed")
	}
}

func TestFCMErrorsAndRetry(t *testing.T) {
	key := fcmKey(t)
	for _, tc := range []struct {
		name   string
		status int
		body   string
		gone   bool
		retry  bool
	}{
		{"404", 404, `{}`, true, false}, {"unregistered", 400, `{"error":{"details":[{"errorCode":"UNREGISTERED"}]}}`, true, false},
		{"mismatch", 403, `{"error":{"details":[{"errorCode":"SENDER_ID_MISMATCH"}]}}`, true, false},
		{"forbidden", 403, `device-secret`, false, false}, {"retry", 401, `device-secret`, false, true}, {"unauthorized", 401, `device-secret`, false, false}, {"server", 500, `device-secret`, false, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var tokens, sends int
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path == "/token" {
					tokens++
					fmt.Fprintf(w, `{"access_token":"access-%d","expires_in":3600}`, tokens)
					return
				}
				sends++
				if tc.retry && sends == 2 {
					_, _ = io.WriteString(w, `{}`)
					return
				}
				w.WriteHeader(tc.status)
				_, _ = io.WriteString(w, tc.body)
			}))
			defer srv.Close()
			f, e := NewFCM(fcmCredentials(t, key, srv.URL+"/token"))
			if e != nil {
				t.Fatal(e)
			}
			f.SetTransportForTest(srv.Client(), srv.URL+"/send")
			err := f.Send(context.Background(), FCMMessage{Token: "device-secret"})
			if tc.retry {
				if err != nil {
					t.Fatal(err)
				}
			} else if err == nil || errors.Is(err, ErrGone) != tc.gone {
				t.Fatalf("error %v", err)
			}
			if err != nil && strings.Contains(err.Error(), "device-secret") {
				t.Fatal("secret in error")
			}
			want := 1
			if tc.status == 401 {
				want = 2
			}
			if sends != want || tokens != want {
				t.Fatalf("tokens=%d sends=%d", tokens, sends)
			}
		})
	}
}

func TestFCMInvalidCredentials(t *testing.T) {
	key := fcmKey(t)
	valid := fcmCredentials(t, key, "https://example.com/token")
	for _, field := range []string{"type", "project_id", "private_key", "client_email", "token_uri"} {
		var c map[string]string
		_ = json.Unmarshal(valid, &c)
		delete(c, field)
		raw, _ := json.Marshal(c)
		if _, err := NewFCM(raw); err == nil {
			t.Errorf("accepted missing %s", field)
		}
	}
	ec, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	for _, raw := range [][]byte{[]byte(`{`), []byte(`null`), fcmCredentials(t, ec, "https://example.com/token"), []byte(`{"type":"service_account","project_id":"p","private_key":"secret-invalid","client_email":"e","token_uri":"https://example.com"}`)} {
		if _, err := NewFCM(raw); err == nil {
			t.Error("accepted invalid credentials")
		} else if strings.Contains(err.Error(), "secret-invalid") {
			t.Error("key leaked")
		}
	}
}
