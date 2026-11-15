// Package turn mints the ephemeral TURN credentials described in
// docs/historico/PLANO.md §8: coturn's REST-API scheme (use-auth-secret), where the
// username is "<expiry-unix>:<user>" and the password is
// base64(HMAC-SHA1(secret, username)). A leaked credential dies with its
// expiry instead of turning the relay into an open proxy.
package turn

import (
	"crypto/hmac"
	"crypto/sha1"
	"encoding/base64"
	"strconv"
	"time"

	"call/internal/protocol"
)

// TTL is how long a minted credential stays valid. Long enough to outlive a
// call plus an ICE restart, short enough that leaking one is boring.
const TTL = 2 * time.Hour

// Credentials mints per-connection TURN credentials for a fixed set of URLs.
type Credentials struct {
	secret []byte
	urls   []string
	now    func() time.Time
}

// New returns a minter keyed by coturn's static-auth-secret, advertising urls
// (turn:/turns: URIs, delivered verbatim to the client).
func New(secret []byte, urls []string) *Credentials {
	return &Credentials{secret: secret, urls: append([]string(nil), urls...), now: time.Now}
}

// ICEServer returns an RTCIceServer entry for user with a fresh expiry.
func (c *Credentials) ICEServer(user string) protocol.ICEServer {
	exp := c.now().Add(TTL).Unix()
	username := strconv.FormatInt(exp, 10) + ":" + user
	return protocol.ICEServer{
		URLs:       append([]string(nil), c.urls...),
		Username:   username,
		Credential: Sign(c.secret, username),
	}
}

// Sign computes coturn's password for username: base64(HMAC-SHA1(secret, username)).
// Exported so tests elsewhere can verify a minted credential without
// duplicating the scheme.
func Sign(secret []byte, username string) string {
	m := hmac.New(sha1.New, secret)
	m.Write([]byte(username))
	return base64.StdEncoding.EncodeToString(m.Sum(nil))
}
