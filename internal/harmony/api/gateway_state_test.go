package api

import (
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// dialHidden is the Android app opening its socket with no screen in front of
// it: woken by a notification, or reconnecting in a pocket.
func (d *device) dialHidden(t *testing.T) *websocket.Conn {
	t.Helper()
	dialer := websocket.Dialer{Jar: d.c.Jar, HandshakeTimeout: 5 * time.Second}
	url := "ws" + strings.TrimPrefix(d.h.srv.URL, "http") + "/ws?state=hidden"
	ws, _, err := dialer.Dial(url, http.Header{"Origin": {baseURL}})
	if err != nil {
		t.Fatalf("dial /ws?state=hidden: %v", err)
	}
	return ws
}

// The phone woken by a notification opens its socket at once. Had it arrived
// as "looking", the next message would be swallowed as seen — the exact moment
// the notification exists for.
func TestASocketOpenedBehindTheScreenStillGetsWokenUp(t *testing.T) {
	c := newPushCast(t)
	if res := c.alice.subscribePush(c.service.endpoint("alice-phone")); res.status != http.StatusOK {
		t.Fatalf("subscribe: %d %s", res.status, res.raw)
	}
	sock := c.alice.dialHidden(t)
	defer sock.Close()
	waitFor(t, "alice to show up", func() bool { return c.owner.presence()["alice"] == PresenceOnline })

	c.owner.mustSay(c.owner.generalID(), "@alice acorda")
	c.service.wait(t)

	// Opening the app is the POST that says so, and from then on she is here.
	c.alice.setPresence("active")
	c.owner.mustSay(c.owner.generalID(), "@alice agora voce ve")
	c.service.quiet(t)
}

// Anything else in the parameter is the web's socket, which is looking.
func TestAnUnknownStateOnTheSocketIsTheOrdinaryOne(t *testing.T) {
	c := newPushCast(t)
	if res := c.alice.subscribePush(c.service.endpoint("alice-phone")); res.status != http.StatusOK {
		t.Fatalf("subscribe: %d %s", res.status, res.raw)
	}
	dialer := websocket.Dialer{Jar: c.alice.c.Jar, HandshakeTimeout: 5 * time.Second}
	url := "ws" + strings.TrimPrefix(c.alice.h.srv.URL, "http") + "/ws?state=away"
	sock, _, err := dialer.Dial(url, http.Header{"Origin": {baseURL}})
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer sock.Close()
	waitFor(t, "alice online", func() bool { return c.owner.presence()["alice"] == PresenceOnline })

	c.owner.mustSay(c.owner.generalID(), "@alice oi")
	c.service.quiet(t)
}

// The handshake reaches the phone before the server has put the socket in the
// hub. A "hidden" posted in that gap must still land on the socket that
// follows, not be lost with nobody to mark (Codex audit, 26/09/2026).
func TestAReportThatBeatsItsSocketIsAppliedWhenTheSocketArrives(t *testing.T) {
	h := newHub()
	clock := time.Now()
	h.now = func() time.Time { return clock }

	h.setReported(7, "sess-a", PresenceHidden, false)
	c := &gatewayConn{userID: 7, sessID: "sess-a", send: make(chan []byte, 1)}
	h.add(c)
	if h.visible(7) {
		t.Fatal("the socket arrived as looking although its device had just said it was hidden")
	}

	// Another session's report is not this socket's.
	h.setReported(8, "sess-b", PresenceHidden, false)
	d := &gatewayConn{userID: 8, sessID: "sess-other", send: make(chan []byte, 1)}
	h.add(d)
	if !h.visible(8) {
		t.Fatal("a report from one session was applied to another")
	}

	// A report whose socket never came is forgotten, not applied an hour later.
	h.setReported(9, "sess-c", PresenceHidden, false)
	clock = clock.Add(time.Minute)
	e := &gatewayConn{userID: 9, sessID: "sess-c", send: make(chan []byte, 1)}
	h.add(e)
	if !h.visible(9) {
		t.Fatal("a stale early report was applied")
	}
}
