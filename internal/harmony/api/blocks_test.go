package api

import (
	"github.com/gorilla/websocket"
	"net/http"
	"strings"
	"testing"
)

func TestPersonalListsArePrivateEvenFromOwner(t *testing.T) {
	c := newCast(t)
	for _, kind := range []string{"blocks", "mutes"} {
		if r := c.alice.do(http.MethodPut, "/api/me/"+kind+"/bruno", nil); r.status != 200 {
			t.Fatal(r.status, r.raw)
		}
	}
	if r := c.alice.get("/api/me/blocks"); !strings.Contains(r.raw, `"blocked":true`) || !strings.Contains(r.raw, `"muted":true`) {
		t.Fatal(r.raw)
	}
	for _, d := range []*device{c.bo, c.owner} {
		for _, suffix := range []string{"", "?username=alice", "?user_id=2", "?owner=alice"} {
			if r := d.get("/api/me/blocks" + suffix); strings.Contains(r.raw, "bruno") {
				t.Fatalf("private list leaked: %s", r.raw)
			}
		}
		for _, path := range []string{"/api/people/alice/blocks", "/api/admin/users/alice/blocks", "/api/users/alice/blocks"} {
			for _, method := range []string{http.MethodGet, http.MethodPut, http.MethodDelete} {
				if r := d.do(method, path, nil); r.status == 200 {
					t.Fatalf("unexpected private route: %s %s", method, path)
				}
			}
		}
		for _, path := range []string{"/api/me", "/api/members"} {
			r := d.get(path)
			if strings.Contains(r.raw, `"blocked"`) || strings.Contains(r.raw, `"muted"`) {
				t.Fatalf("preferences leaked: %s", r.raw)
			}
		}
	}
	// Naming another owner in the query/body still only changes the caller's list.
	if r := c.bo.do(http.MethodDelete, "/api/me/blocks/bruno?owner=alice", map[string]any{"owner": "alice"}); r.status != 404 {
		t.Fatal(r.status, r.raw)
	}
	if r := c.owner.do(http.MethodPut, "/api/me/blocks/bruno?owner=alice", nil); r.status != 200 {
		t.Fatal(r.status, r.raw)
	}
	if r := c.owner.do(http.MethodDelete, "/api/me/blocks/bruno", nil); r.status != 200 {
		t.Fatal(r.status, r.raw)
	}
	if r := c.alice.get("/api/me/blocks"); !strings.Contains(r.raw, `"blocked":true`) {
		t.Fatal(r.raw)
	}
	for _, kind := range []string{"blocks", "mutes"} {
		if r := c.alice.do(http.MethodPut, "/api/me/"+kind+"/alice", nil); r.status != 404 {
			t.Fatal(r.status, r.raw)
		}
	}
	if r := c.alice.do(http.MethodPut, "/api/me/blocks/tales", nil); r.status != 200 {
		t.Fatal("owner must be blockable", r.status, r.raw)
	}
}

func TestBlockRefusesDMAndCallBothWaysWithoutDisclosure(t *testing.T) {
	c := newCast(t)
	dm := c.alice.openDM("bruno")
	c.alice.mustSay(dm, "history remains")
	c.alice.do(http.MethodPut, "/api/me/blocks/bruno", nil)
	for _, d := range []*device{c.alice, c.bo} {
		r := d.say(dm, "must not deliver")
		if r.status != http.StatusConflict || !strings.Contains(r.raw, `delivery_failed`) || strings.Contains(r.raw, "block") {
			t.Fatal(r.status, r.raw)
		}
		if r := d.get("/api/channels/" + dm + "/messages"); !strings.Contains(r.raw, "history remains") {
			t.Fatal(r.status, r.raw)
		}
	}
	for _, pair := range []struct {
		from, to *device
		name     string
	}{{c.alice, c.bo, "bruno"}, {c.bo, c.alice, "alice"}} {
		r := pair.from.call(pair.name)
		if r.status != 201 {
			t.Fatal(r.status, r.raw)
		}
		id := r.body["id"].(string)
		if r := pair.to.do(http.MethodPost, "/api/calls/"+id+"/accept", nil); r.status != 404 {
			t.Fatal(r.status, r.raw)
		}
		pair.from.do(http.MethodPost, "/api/calls/"+id+"/hangup", nil)
	}
	c.alice.do(http.MethodDelete, "/api/me/blocks/bruno", nil)
	if r := c.bo.say(dm, "delivers again"); r.status != 201 {
		t.Fatal(r.status, r.raw)
	}
}

func TestBlockEventOnlyReachesOwnSessionsAndSuppressesUnreadAndPush(t *testing.T) {
	c := newPushCast(t)
	if r := c.alice.subscribePush(c.service.endpoint("blocked-device")); r.status != 200 {
		t.Fatal(r.status, r.raw)
	}
	c.alice.setPresence("away")
	a := c.alice.dial(t)
	defer a.Close()
	second := c.h.device("second-session")
	if r := second.do(http.MethodPost, "/api/login", map[string]string{"username": "alice", "password": goodPass2}); r.status != 200 {
		t.Fatal(r.status, r.raw)
	}
	a2 := second.dial(t)
	defer a2.Close()
	b := c.bo.dial(t)
	defer b.Close()
	o := c.owner.dial(t)
	defer o.Close()
	c.alice.do(http.MethodPut, "/api/me/blocks/bruno", nil)
	waitEvent(t, a, "person_preferences")
	waitEvent(t, a2, "person_preferences")
	c.alice.setPresence("away")
	ch := c.bo.generalID()
	c.bo.mustSay(ch, "@alice blocked mention")
	channel, _ := c.alice.channelByID(ch)
	if channel["unread"] != float64(0) || channel["mentions"] != float64(0) {
		t.Fatal(channel)
	}
	if r := c.alice.get("/api/notifications/pending"); strings.Contains(r.raw, "mention") {
		t.Fatal(r.raw)
	}
	// A channel message acts as a barrier: every earlier private event would be
	// observed before it if it had been sent to another account.
	for _, ws := range []*websocket.Conn{b, o} {
		for {
			ev := waitEvent(t, ws, "person_preferences", "message")
			if ev.Type == "person_preferences" {
				t.Fatal("private event leaked")
			}
			break
		}
	}
	c.service.quiet(t)
}

func TestBlockedMentionAndCallNeverWakeFCM(t *testing.T) {
	c := newFCMCast(t)
	c.register(t)
	c.alice.do(http.MethodPut, "/api/me/blocks/bruno", nil)
	c.bo.mustSay(c.bo.generalID(), "@alice must stay quiet")
	if r := c.bo.call("alice"); r.status != 201 {
		t.Fatal(r.status, r.raw)
	}
	c.quiet(t)
	if r := c.alice.get("/api/notifications/pending"); strings.Contains(r.raw, "mention") || strings.Contains(r.raw, "ring") {
		t.Fatal(r.raw)
	}
}

func TestPersonalPreferenceRoutesRequireLogin(t *testing.T) {
	h := newHarness(t)
	d := h.device("privacy-test")
	for _, route := range []struct{ method, path string }{
		{http.MethodGet, "/api/me/blocks"},
		{http.MethodPut, "/api/me/mutes/alice"},
		{http.MethodDelete, "/api/me/mutes/alice"},
		{http.MethodPut, "/api/me/blocks/alice"},
		{http.MethodDelete, "/api/me/blocks/alice"},
	} {
		r := d.do(route.method, route.path, nil)
		if r.status != http.StatusUnauthorized {
			t.Fatal(route.method, route.path, r.status, r.raw)
		}
	}
}

func TestSuppressedCallDoesNotReserveOrRevealRecipient(t *testing.T) {
	book := newCallBook()
	if err := book.start(&directCall{id: "silent", from: 1, to: 2, suppressed: true}); err != nil {
		t.Fatal(err)
	}
	if _, err := book.get("silent", 2); err != errNoCall {
		t.Fatal("recipient can inspect a suppressed call", err)
	}
	if err := book.start(&directCall{id: "ordinary", from: 3, to: 2}); err != nil {
		t.Fatal("a suppressed call must not make its recipient busy", err)
	}
}
