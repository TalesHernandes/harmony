package push

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// The endpoint of a push is a URL a member's browser chose, so the sender is
// the one place in harmony where an outbound request goes wherever somebody
// else said. Audit of 2026-09-20, F3: it used to follow a redirect, which
// hands that choice on again — to an address inside this machine, over plain
// http, carrying the VAPID token when the host stayed the same.
func TestPushDoesNotFollowARedirect(t *testing.T) {
	reached := make(chan struct{}, 1)
	inside := httptest.NewServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		reached <- struct{}{}
	}))
	defer inside.Close()

	front := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, inside.URL+"/whatever", http.StatusFound)
	}))
	defer front.Close()

	// Only the certificate is test-only. The redirect policy is the real one:
	// the same sender built by NewSender, with its client's trust widened.
	s := NewSender(mustKeys(t))
	client := front.Client()
	client.CheckRedirect = s.client.CheckRedirect
	s.SetClientForTest(client)

	err := s.Send(context.Background(), Message{Endpoint: front.URL + "/subscription"})
	if err == nil {
		t.Error("a redirect was treated as a delivered push")
	}
	select {
	case <-reached:
		t.Fatalf("the sender followed the redirect and reached %s", inside.URL)
	case <-time.After(time.Second):
	}
}

// And the other half, which a redirect policy alone does not cover: a name
// that simply resolves to an address inside the network. The check is on the
// dialled address, so it holds however the name got there.
func TestPushRefusesAPrivateAddress(t *testing.T) {
	inside := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		t.Error("the request reached a loopback address")
	}))
	defer inside.Close()

	s := NewSender(mustKeys(t))
	// The test server's own trust, but this sender's dialler — the thing
	// under test — so the refusal is the real one and not a TLS complaint.
	client := front(inside, s)
	s.SetClientForTest(client)

	err := s.Send(context.Background(), Message{Endpoint: inside.URL + "/subscription"})
	if !errors.Is(err, ErrBlockedAddress) && !strings.Contains(errString(err), ErrBlockedAddress.Error()) {
		t.Fatalf("a push to 127.0.0.1 was not refused: %v", err)
	}
}

// front borrows the test server's certificate pool while keeping the sender's
// own transport, which is where the address check lives.
func front(srv *httptest.Server, s *Sender) *http.Client {
	trusted := srv.Client()
	ours := s.client.Transport.(*http.Transport).Clone()
	ours.TLSClientConfig = trusted.Transport.(*http.Transport).TLSClientConfig
	return &http.Client{Transport: ours, Timeout: 10 * time.Second, CheckRedirect: s.client.CheckRedirect}
}

func errString(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

// The endpoint is a capability URL: whoever holds it can make that person's
// telephone buzz. The project treats it as a secret and promises it never
// reaches a log — but a transport error from net/http is a *url.Error, which
// prints the whole URL, and that error was being logged whole. Audit of
// 2026-09-20, F9.
func TestAFailedPushDoesNotCarryTheEndpoint(t *testing.T) {
	const marker = "marcador-secreto-do-dono"

	// A name that cannot resolve, by standard (RFC 6761): the request fails
	// in the transport, which is where the error that named the endpoint was
	// built. A closed port on 127.0.0.1 would not do — the dialler refuses
	// that address before it ever becomes a *url.Error, so the test would
	// pass without the scrubbing being involved at all.
	s := NewSender(mustKeys(t))
	err := s.Send(context.Background(), Message{Endpoint: "https://push.invalid/" + marker})
	if err == nil {
		t.Fatal("a push to a closed port succeeded")
	}
	if strings.Contains(err.Error(), marker) {
		t.Errorf("the error carries the endpoint: %v", err)
	}
	t.Logf("what a failure says now: %v", err)
}

// And the other source of text: the remote service's own body, which can
// repeat back the URL or the headers it was sent.
func TestAPushErrorDoesNotCarryTheRemoteBody(t *testing.T) {
	const marker = "texto-do-servidor-remoto"
	service := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusBadRequest)
		_, _ = w.Write([]byte(marker))
	}))
	defer service.Close()

	s := NewSender(mustKeys(t))
	s.SetClientForTest(service.Client())
	err := s.Send(context.Background(), Message{Endpoint: service.URL + "/x"})
	if err == nil {
		t.Fatal("a 400 was treated as delivered")
	}
	if strings.Contains(err.Error(), marker) {
		t.Errorf("the error repeats the remote body: %v", err)
	}
}

// The body was scrubbed in round 9, and resp.Status went into the error in its
// place — but over HTTP/1.1 the status line has a reason phrase, and the remote
// server writes it. This one writes the marker there, where a well-behaved
// server writes "Bad Request".
func TestAPushErrorDoesNotCarryTheRemoteReasonPhrase(t *testing.T) {
	const marker = "frase-do-servidor-remoto"
	service := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		conn, buf, err := w.(http.Hijacker).Hijack()
		if err != nil {
			t.Error(err)
			return
		}
		defer conn.Close()
		_, _ = buf.WriteString("HTTP/1.1 400 " + marker + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
		_ = buf.Flush()
	}))
	defer service.Close()

	s := NewSender(mustKeys(t))
	s.SetClientForTest(service.Client())
	err := s.Send(context.Background(), Message{Endpoint: service.URL + "/x"})
	if err == nil {
		t.Fatal("a 400 was treated as delivered")
	}
	if strings.Contains(err.Error(), marker) {
		t.Errorf("the error repeats the remote reason phrase: %v", err)
	}
	if !strings.Contains(err.Error(), "400") {
		t.Errorf("the error lost the status code, which is the useful half: %v", err)
	}
}
