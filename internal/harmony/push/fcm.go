package push

import (
	"bytes"
	"context"
	"crypto"
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
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

// FCM sends data-only wakeups. Credentials and responses never enter errors.
type FCM struct {
	project, email, tokenURI, endpoint string
	key                                *rsa.PrivateKey
	client                             *http.Client
	now                                func() time.Time
	mu                                 sync.Mutex
	access                             string
	expires                            time.Time
}

type FCMMessage struct {
	Token       string
	Data        map[string]string
	TTL         time.Duration
	CollapseKey string
}

func NewFCM(credentialsJSON []byte) (*FCM, error) {
	var c struct {
		Type    string `json:"type"`
		Project string `json:"project_id"`
		Key     string `json:"private_key"`
		Email   string `json:"client_email"`
		URI     string `json:"token_uri"`
	}
	if json.Unmarshal(credentialsJSON, &c) != nil {
		return nil, errors.New("fcm: invalid credentials JSON")
	}
	if c.Type != "service_account" {
		return nil, errors.New("fcm: type must be service_account")
	}
	for _, field := range []struct{ name, value string }{{"project_id", c.Project}, {"private_key", c.Key}, {"client_email", c.Email}, {"token_uri", c.URI}} {
		if strings.TrimSpace(field.value) == "" {
			return nil, fmt.Errorf("fcm: missing %s", field.name)
		}
	}
	u, err := url.Parse(c.URI)
	if err != nil || u.Host == "" || (u.Scheme != "https" && u.Scheme != "http") || u.User != nil {
		return nil, errors.New("fcm: invalid token_uri")
	}
	block, rest := pem.Decode([]byte(c.Key))
	if block == nil || block.Type != "PRIVATE KEY" || len(bytes.TrimSpace(rest)) != 0 {
		return nil, errors.New("fcm: private_key must be PKCS#8 PEM")
	}
	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return nil, errors.New("fcm: invalid PKCS#8 private_key")
	}
	key, ok := parsed.(*rsa.PrivateKey)
	if !ok {
		return nil, errors.New("fcm: private_key must be RSA")
	}
	if key.Validate() != nil {
		return nil, errors.New("fcm: invalid RSA private_key")
	}
	return &FCM{project: c.Project, email: c.Email, tokenURI: c.URI, key: key, endpoint: "https://fcm.googleapis.com/v1/projects/" + url.PathEscape(c.Project) + "/messages:send", client: &http.Client{Timeout: 10 * time.Second}, now: time.Now}, nil
}

func (f *FCM) ProjectID() string { return f.project }

// SetTransportForTest must be called before any sends, like Sender's test seam.
func (f *FCM) SetTransportForTest(client *http.Client, endpoint string) {
	f.client = client
	f.endpoint = endpoint
}

func (f *FCM) accessToken(ctx context.Context) (string, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	now := f.now()
	if f.access != "" && now.Before(f.expires) {
		return f.access, nil
	}
	claims, _ := json.Marshal(map[string]any{"iss": f.email, "scope": "https://www.googleapis.com/auth/firebase.messaging", "aud": f.tokenURI, "iat": now.Unix(), "exp": now.Unix() + 3600})
	enc := base64.RawURLEncoding
	input := enc.EncodeToString([]byte(`{"alg":"RS256","typ":"JWT"}`)) + "." + enc.EncodeToString(claims)
	hash := sha256.Sum256([]byte(input))
	sig, err := rsa.SignPKCS1v15(rand.Reader, f.key, crypto.SHA256, hash[:])
	if err != nil {
		return "", errors.New("fcm: JWT signing failed")
	}
	form := url.Values{"grant_type": {"urn:ietf:params:oauth:grant-type:jwt-bearer"}, "assertion": {input + "." + enc.EncodeToString(sig)}}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, f.tokenURI, strings.NewReader(form.Encode()))
	if err != nil {
		return "", errors.New("fcm: invalid token request")
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	resp, err := f.client.Do(req)
	if err != nil {
		return "", errors.New("fcm: token transport failed")
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("fcm: token status %d", resp.StatusCode)
	}
	var token struct {
		Access  string `json:"access_token"`
		Expires int64  `json:"expires_in"`
	}
	if json.NewDecoder(io.LimitReader(resp.Body, 64<<10)).Decode(&token) != nil || token.Access == "" || token.Expires <= 0 || token.Expires > 86400 {
		return "", errors.New("fcm: invalid token response")
	}
	f.access = token.Access
	// Short-lived test/provider tokens retain a proportional safety margin.
	margin := 300 * time.Second
	lifetime := time.Duration(token.Expires) * time.Second
	if lifetime <= margin {
		margin = lifetime / 10
	}
	f.expires = now.Add(lifetime - margin)
	return f.access, nil
}

func (f *FCM) Send(ctx context.Context, m FCMMessage) error {
	ttl := m.TTL
	if ttl <= 0 {
		ttl = MessageTTL
	}
	body, _ := json.Marshal(map[string]any{"message": map[string]any{"token": m.Token, "data": m.Data, "android": map[string]string{"priority": "HIGH", "ttl": strconv.FormatInt(int64(ttl/time.Second), 10) + "s", "collapse_key": m.CollapseKey}}})
	for attempt := 0; attempt < 2; attempt++ {
		token, err := f.accessToken(ctx)
		if err != nil {
			return err
		}
		req, err := http.NewRequestWithContext(ctx, http.MethodPost, f.endpoint, bytes.NewReader(body))
		if err != nil {
			return errors.New("fcm: invalid send request")
		}
		req.Header.Set("Authorization", "Bearer "+token)
		req.Header.Set("Content-Type", "application/json")
		resp, err := f.client.Do(req)
		if err != nil {
			return errors.New("fcm: send transport failed")
		}
		var result struct {
			Error struct {
				Details []struct {
					Code string `json:"errorCode"`
				} `json:"details"`
			} `json:"error"`
		}
		_ = json.NewDecoder(io.LimitReader(resp.Body, 64<<10)).Decode(&result)
		resp.Body.Close()
		if resp.StatusCode == http.StatusUnauthorized {
			f.mu.Lock()
			if f.access == token {
				f.access = ""
			}
			f.mu.Unlock()
			if attempt == 0 {
				continue
			}
		}
		if resp.StatusCode == http.StatusNotFound {
			return ErrGone
		}
		for _, d := range result.Error.Details {
			if d.Code == "UNREGISTERED" || (resp.StatusCode == http.StatusForbidden && d.Code == "SENDER_ID_MISMATCH") {
				return ErrGone
			}
		}
		if resp.StatusCode >= 200 && resp.StatusCode < 300 {
			return nil
		}
		return fmt.Errorf("fcm: send status %d", resp.StatusCode)
	}
	return errors.New("fcm: send status 401")
}
