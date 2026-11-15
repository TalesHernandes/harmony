package push

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"syscall"
	"time"
)

// How long the push service may hold a message before giving up on it
// (RFC 8030 §5.2). This is the whole staleness story, and it is why the push
// needs no payload to be safe: a radio that was asleep hands the queued message
// over when it wakes, and without a TTL that can be minutes later.
//
// A ring is worthless after a minute — the person gave up and the room moved
// on — so it expires with the call. A mention is not: reading it an hour later
// is exactly what a mention is for.
const (
	RingTTL    = 45 * time.Second
	MessageTTL = 12 * time.Hour
)

// ErrGone means the push service says the subscription is dead: the browser
// dropped its registration, or the app was uninstalled. The endpoint should be
// forgotten (RFC 8030 §7.3).
var ErrGone = errors.New("push: subscription gone")

// Message is one bodyless push. Topic is what makes a second one *replace* a
// first that is still queued instead of stacking on the phone; it is per kind,
// so a ring never buries a mention.
type Message struct {
	Endpoint string
	TTL      time.Duration
	// Urgency is "high" for something the phone should wake up out of doze for
	// (a call), "normal" for the rest (RFC 8030 §5.3).
	Urgency string
	Topic   string
}

// Sender posts pushes to a push service.
type Sender struct {
	keys   *Keys
	client *http.Client
	now    func() time.Time
}

// ErrBlockedAddress means the endpoint resolved to an address inside this
// machine or its network. It is not a delivery failure, it is a refusal.
var ErrBlockedAddress = errors.New("push: endpoint resolves to a private address")

// NewSender returns a Sender using keys.
//
// Two restrictions on the client, both for the same reason: the destination of
// this request is a URL a member chose. A browser hands out its own push
// endpoint and we store whatever it says, so an outbound request to an
// arbitrary host is by design — but only to a host on the public internet, and
// only to the one that was named.
//
//   - Redirects are not followed. RFC 8030 push services do not need them, and
//     following one hands the choice of destination to the previous
//     destination, which can point at 127.0.0.1 and downgrade to plain http on
//     the way. Go also keeps the Authorization header on a redirect that stays
//     on the same host, so the VAPID token would travel with it.
//   - The socket refuses to connect to a private address. This is checked on
//     the resolved address, at dial time, which is the only place a name that
//     resolves to 10.0.0.1 can be caught.
func NewSender(keys *Keys) *Sender {
	dialer := &net.Dialer{
		Timeout:   5 * time.Second,
		KeepAlive: 30 * time.Second,
		Control:   refusePrivateAddress,
	}
	return &Sender{
		keys: keys,
		// Bounded on purpose: delivery happens on a goroutine of its own, and a
		// push service having a slow day must not be able to pile those up.
		client: &http.Client{
			Timeout:       10 * time.Second,
			CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
			Transport: &http.Transport{
				Proxy:                 http.ProxyFromEnvironment,
				DialContext:           dialer.DialContext,
				ForceAttemptHTTP2:     true,
				MaxIdleConns:          10,
				IdleConnTimeout:       90 * time.Second,
				TLSHandshakeTimeout:   10 * time.Second,
				ExpectContinueTimeout: time.Second,
			},
		},
		now: time.Now,
	}
}

// refusePrivateAddress is the dialer's last word on where a push may go. It
// sees the address the name actually resolved to, so a host that answers with
// 127.0.0.1 — on purpose, or on a second lookup — is stopped here rather than
// at the URL, which only ever showed a name.
func refusePrivateAddress(network, address string, _ syscall.RawConn) error {
	if !strings.HasPrefix(network, "tcp") {
		return fmt.Errorf("%w: %s", ErrBlockedAddress, network)
	}
	host, _, err := net.SplitHostPort(address)
	if err != nil {
		return err
	}
	ip := net.ParseIP(host)
	if ip == nil {
		return fmt.Errorf("%w: %s", ErrBlockedAddress, host)
	}
	if ip.IsLoopback() || ip.IsPrivate() || ip.IsLinkLocalUnicast() ||
		ip.IsLinkLocalMulticast() || ip.IsMulticast() || ip.IsUnspecified() ||
		ip.IsInterfaceLocalMulticast() {
		return fmt.Errorf("%w: %s", ErrBlockedAddress, ip)
	}
	return nil
}

// SetClientForTest replaces the HTTP client. It exists so the tests can point
// the sender at a fake push service over its own self-signed TLS — the only way
// to assert on a request that leaves this machine and whose reply nobody waits
// for.
func (s *Sender) SetClientForTest(c *http.Client) { s.client = c }

// Send delivers one bodyless push. It returns ErrGone when the subscription
// should be dropped.
//
// No body and no Content-Encoding: a push with no payload is valid, and the
// service worker needs no data to know what to do — it asks the server (see the
// package comment).
func (s *Sender) Send(ctx context.Context, msg Message) error {
	auth, err := s.keys.authorization(msg.Endpoint, s.now())
	if err != nil {
		return err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, msg.Endpoint, http.NoBody)
	if err != nil {
		return err
	}
	ttl := msg.TTL
	if ttl <= 0 {
		ttl = MessageTTL
	}
	urgency := msg.Urgency
	if urgency == "" {
		urgency = "normal"
	}
	req.Header.Set("Authorization", auth)
	req.Header.Set("TTL", strconv.Itoa(int(ttl.Seconds())))
	req.Header.Set("Urgency", urgency)
	if msg.Topic != "" {
		// RFC 8030 §5.4. The header has a narrow alphabet (base64url, at most
		// 32 characters), so the callers keep the topics short and constant.
		req.Header.Set("Topic", msg.Topic)
	}
	req.Header.Set("Content-Length", "0")

	resp, err := s.client.Do(req)
	if err != nil {
		return scrub(err)
	}
	defer resp.Body.Close()
	// Drained and thrown away: draining lets the connection be reused, and the
	// body is a stranger's text — it can repeat the endpoint back at us, or
	// the headers we sent — so it does not go into an error that will be
	// logged. The status says everything a log line needs.
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 4<<10))

	switch {
	case resp.StatusCode == http.StatusNotFound || resp.StatusCode == http.StatusGone:
		return ErrGone
	case resp.StatusCode >= 200 && resp.StatusCode < 300:
		return nil
	default:
		// The number, never resp.Status: over HTTP/1.1 the status line carries
		// a reason phrase the remote server writes, and it can say anything —
		// the path it was sent, the Authorization header. The body was
		// thrown away above for the same reason; the phrase is the same
		// stranger's text one line higher.
		return fmt.Errorf("push: status %d", resp.StatusCode)
	}
}

// scrub takes the endpoint out of a transport error before anybody can log it.
//
// The endpoint is a capability URL: whoever holds it can make that person's
// telephone buzz, so this project treats it as a secret and promises it never
// reaches a log. But an error from net/http is a *url.Error, and a *url.Error
// prints the whole URL — path, query and all. Leaving the endpoint out of the
// log line is not enough when the error carries it.
//
// What survives is what a log needs: the operation, and the cause with its
// sentinel intact, so errors.Is still works on what comes back.
func scrub(err error) error {
	var ue *url.Error
	if errors.As(err, &ue) {
		return fmt.Errorf("push: %s: %w", ue.Op, scrubHost(ue.Err))
	}
	return fmt.Errorf("push: %w", scrubHost(err))
}

func scrubHost(err error) error {
	// A DNS error names the host it looked up, which for a push endpoint is
	// part of the address. The kind of failure is the useful half.
	var de *net.DNSError
	if errors.As(err, &de) {
		if de.IsTimeout {
			return errors.New("dns lookup timed out")
		}
		return errors.New("dns lookup failed")
	}
	// An OpError prints its address too; its own cause does not.
	var oe *net.OpError
	if errors.As(err, &oe) && oe.Err != nil {
		return fmt.Errorf("%s: %w", oe.Op, oe.Err)
	}
	return err
}
