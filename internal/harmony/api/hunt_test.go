// The bug hunt of 03/10/2026, after D10, turned into tests. Each one failed
// before its fix; the comment says what used to happen.
package api

import (
	"fmt"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"
)

// TestAnUnpostedUploadIsOnlyItsOwners: an image chosen but not yet sent is a
// draft. Bruno, in the same channel, used to be able to read Alice's draft by
// its id, because the gate only asked about channel membership.
func TestAnUnpostedUploadIsOnlyItsOwners(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	up := c.alice.upload(general, "rascunho.png", pngBytes(t, 8, 8))
	if up.status != http.StatusCreated {
		t.Fatalf("upload: %d %s", up.status, up.raw)
	}
	fileID, _ := up.body["id"].(string)

	draft := c.bo.get("/api/files/" + fileID)
	fake := c.bo.get("/api/files/AAAAAAAAAAAAAAAA")
	if draft.status != http.StatusNotFound || draft.raw != fake.raw {
		t.Fatalf("bruno read alice's unposted upload: %d %q (an invented id gives %d %q)",
			draft.status, draft.raw, fake.status, fake.raw)
	}
	if got := c.alice.get("/api/files/" + fileID); got.status != http.StatusOK {
		t.Fatalf("alice cannot see her own draft: %d %s", got.status, got.raw)
	}
	c.alice.mustSay(general, "olha", fileID)
	if got := c.bo.get("/api/files/" + fileID); got.status != http.StatusOK {
		t.Fatalf("once posted, the channel reads it: %d %s", got.status, got.raw)
	}
}

// TestAnAuthorCannotRewriteADeletedChannel: deleting a channel keeps its
// history and drops its members. The edit used to commit first and check
// membership afterwards, so the author got a 404 and the history changed anyway.
func TestAnAuthorCannotRewriteADeletedChannel(t *testing.T) {
	c := newCast(t)
	created := c.owner.do(http.MethodPost, "/api/channels", map[string]string{"name": "efemero"})
	if created.status != http.StatusCreated {
		t.Fatalf("create: %d %s", created.status, created.raw)
	}
	channel := created.body["id"].(string)
	posted := c.alice.mustSay(channel, "texto original")
	id := fmt.Sprintf("%.0f", posted["id"].(float64))
	if res := c.owner.do(http.MethodDelete, "/api/channels/"+channel, nil); res.status != http.StatusNoContent {
		t.Fatalf("delete: %d %s", res.status, res.raw)
	}

	if res := c.alice.do(http.MethodPatch, "/api/messages/"+id, map[string]any{"body": "reescrito"}); res.status != http.StatusNotFound {
		t.Fatalf("edit after the channel was deleted: %d %s, want 404", res.status, res.raw)
	}
	var body string
	var edited any
	if err := c.h.st.DB().QueryRow(`SELECT body, edited_at FROM messages WHERE id = ?`, id).Scan(&body, &edited); err != nil {
		t.Fatal(err)
	}
	if body != "texto original" || edited != nil {
		t.Fatalf("a refused edit changed the history: body=%q edited_at=%v", body, edited)
	}
}

// TestASuccessfulLoginDoesNotWipeTheAddressCounter: a member who knows one
// password used to clear the address's counter with a good login and go on
// guessing at everybody else's. The good login now only gets its own attempt
// back.
func TestASuccessfulLoginDoesNotWipeTheAddressCounter(t *testing.T) {
	c := newCast(t)
	d := c.h.device("spray")
	d.ip = "192.0.2.77"
	wrong := func(i int) reply {
		return d.do(http.MethodPost, "/api/login", map[string]string{
			"username": "alvo" + strings.Repeat("x", i), "password": "chute errado",
		})
	}
	for i := 0; i < IPLimit-1; i++ {
		if res := wrong(i); res.status != http.StatusUnauthorized {
			t.Fatalf("guess %d: %d %s", i+1, res.status, res.raw)
		}
	}
	if res := d.do(http.MethodPost, "/api/login", map[string]string{"username": "bruno", "password": goodPass2}); res.status != http.StatusOK {
		t.Fatalf("the attacker's own login: %d %s", res.status, res.raw)
	}
	// The good login did not count, so one guess is left — and only one.
	if res := wrong(100); res.status != http.StatusUnauthorized {
		t.Fatalf("the last guess under the limit: %d %s", res.status, res.raw)
	}
	if res := wrong(101); res.status != http.StatusTooManyRequests {
		t.Fatalf("a good login reset the address counter: %d %s, want 429", res.status, res.raw)
	}
}

// TestAStalledAvatarUploadDoesNotBlockEverybodyElse: the upload used to take
// the global avatar lock before reading its body, so a connection that sent the
// headers and then sat still held every other avatar change hostage.
func TestAStalledAvatarUploadDoesNotBlockEverybodyElse(t *testing.T) {
	c := newCast(t)
	conn, err := net.Dial("tcp", strings.TrimPrefix(c.h.srv.URL, "http://"))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	fmt.Fprintf(conn, "PUT /api/me/avatar HTTP/1.1\r\nHost: x\r\nCookie: %s=%s\r\nContent-Type: image/png\r\nContent-Length: 1024\r\n\r\npartial",
		SessionCookie, c.alice.sessionCookie())
	time.Sleep(200 * time.Millisecond) // let the stalled request reach its handler

	done := make(chan int, 1)
	go func() { done <- c.bo.do(http.MethodDelete, "/api/me/avatar", nil).status }()
	select {
	case status := <-done:
		if status != http.StatusNoContent {
			t.Fatalf("bruno's avatar removal: %d", status)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("bruno's avatar removal waited on alice's stalled upload")
	}
}

// TestADifferentCallIsNotSwallowedByTheRingCooldown: the ring cooldown was per
// person, so a new call within ten seconds of one that had already ended rang
// nowhere on a phone with the app closed. Repeats of one call still fold.
func TestADifferentCallIsNotSwallowedByTheRingCooldown(t *testing.T) {
	n := newNotifier(&Server{}, nil, nil)
	if !n.take(7, NotifyRing, "first") {
		t.Fatal("the first ring was held back")
	}
	if n.take(7, NotifyRing, "first") {
		t.Fatal("a repeat of the same call was not folded")
	}
	if !n.take(7, NotifyRing, "second") {
		t.Fatal("a different call was swallowed by the previous call's cooldown")
	}
	if !n.take(7, NotifyMention, "") || n.take(7, NotifyMention, "") {
		t.Fatal("mentions are still folded per person")
	}
}

// TestAStalledFileUploadIsCutOff: a byte limit is not a time limit. An upload
// whose body stopped arriving used to hold its handler for as long as the
// client kept the connection open.
func TestAStalledFileUploadIsCutOff(t *testing.T) {
	c := newCast(t)
	old := uploadReadTime
	uploadReadTime = 300 * time.Millisecond
	t.Cleanup(func() { uploadReadTime = old })

	conn, err := net.Dial("tcp", strings.TrimPrefix(c.h.srv.URL, "http://"))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	fmt.Fprintf(conn, "POST /api/channels/%s/uploads HTTP/1.1\r\nHost: x\r\nCookie: %s=%s\r\nContent-Type: multipart/form-data; boundary=x\r\nContent-Length: 4096\r\n\r\n--x\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.png\"\r\n\r\npartial",
		c.alice.generalID(), SessionCookie, c.alice.sessionCookie())

	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	buf := make([]byte, 256)
	_, err = conn.Read(buf)
	if ne, ok := err.(net.Error); ok && ne.Timeout() {
		t.Fatal("a stalled upload was still being waited on after its deadline")
	}
}
