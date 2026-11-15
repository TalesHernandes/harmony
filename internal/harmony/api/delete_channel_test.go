package api

import (
	"net/http"
	"testing"
)

func TestNonAdminCannotDeleteExistingOrInventedChannel(t *testing.T) {
	c := newCast(t)
	dm := c.owner.openDM("alice")
	for _, member := range []*device{c.alice, c.bo} {
		fake := member.do(http.MethodDelete, "/api/channels/invented", nil)
		if fake.status != http.StatusForbidden || fake.body["error"] != "not_allowed" {
			t.Fatalf("invented: %d %s", fake.status, fake.raw)
		}
		for _, id := range []string{c.owner.generalID(), c.owner.voiceID(), dm} {
			res := member.do(http.MethodDelete, "/api/channels/"+id, nil)
			if res.status != fake.status || res.raw != fake.raw {
				t.Fatalf("existing: %d %s; invented: %d %s", res.status, res.raw, fake.status, fake.raw)
			}
			if res := c.owner.get("/api/channels/" + id); res.status != http.StatusOK {
				t.Fatalf("channel changed: %d %s", res.status, res.raw)
			}
		}
	}
}

func TestAdminDeletesChannelAndNotifiesFormerMembers(t *testing.T) {
	c := newCast(t)
	created := c.owner.do(http.MethodPost, "/api/channels", map[string]string{"name": "test"})
	if created.status != http.StatusCreated {
		t.Fatalf("create: %d %s", created.status, created.raw)
	}
	id := created.body["id"].(string)
	c.owner.mustSay(id, "preserved history")
	ws := c.alice.dial(t)
	defer ws.Close()
	waitEvent(t, ws, "presence")
	res := c.owner.do(http.MethodDelete, "/api/channels/"+id, nil)
	if res.status != http.StatusNoContent || res.raw != "" {
		t.Fatalf("delete: %d %s", res.status, res.raw)
	}
	ev := eventData(t, waitEvent(t, ws, "channel_removed"))
	if ev["id"] != id || len(ev) != 1 {
		t.Fatalf("removal event: %v", ev)
	}
	for _, member := range []*device{c.owner, c.alice, c.bo} {
		for _, path := range []string{"/api/channels/" + id, "/api/channels/" + id + "/messages"} {
			res := member.get(path)
			if res.status != http.StatusNotFound || res.body["error"] != "not_found" {
				t.Fatalf("read: %d %s", res.status, res.raw)
			}
		}
		if _, listed := member.channelByID(id); listed {
			t.Fatal("deleted channel still listed")
		}
		if res := member.say(id, "denied"); res.status != http.StatusNotFound {
			t.Fatalf("send: %d %s", res.status, res.raw)
		}
	}
	for _, missing := range []string{id, "invented", c.owner.openDM("alice")} {
		res := c.owner.do(http.MethodDelete, "/api/channels/"+missing, nil)
		if res.status != http.StatusNotFound || res.body["error"] != "not_found" {
			t.Fatalf("delete missing or DM: %d %s", res.status, res.raw)
		}
	}
}

func TestLastChannelFailureDoesNotPublishRemoval(t *testing.T) {
	c := newCast(t)
	id := c.owner.generalID()
	ws := c.alice.dial(t)
	defer ws.Close()
	waitEvent(t, ws, "presence")
	res := c.owner.do(http.MethodDelete, "/api/channels/"+id, nil)
	if res.status != http.StatusConflict || res.body["error"] != "last_channel" {
		t.Fatalf("last channel: %d %s", res.status, res.raw)
	}
	// A later event is a barrier: a premature removal would precede it on this socket.
	c.owner.mustSay(id, "still usable")
	if ev := waitEvent(t, ws, "channel_removed", "message"); ev.Type != "message" {
		t.Fatalf("failed delete published %s", ev.Type)
	}
}

func TestBusyVoiceRoomCannotBeDeleted(t *testing.T) {
	c := newVoiceCast(t)
	id := c.owner.voiceID()
	session := c.alice.mustJoinVoice(id)["session_id"].(string)
	ws := c.bo.dial(t)
	defer ws.Close()
	waitEvent(t, ws, "presence")
	res := c.owner.do(http.MethodDelete, "/api/channels/"+id, nil)
	if res.status != http.StatusConflict || res.body["error"] != "room_busy" {
		t.Fatalf("busy room: %d %s", res.status, res.raw)
	}
	if peers := roomPeers(t, c.bo.voiceRooms(), id); len(peers) != 1 {
		t.Fatalf("occupants changed: %v", peers)
	}
	c.owner.mustSay(c.owner.generalID(), "barrier")
	if ev := waitEvent(t, ws, "channel_removed", "message"); ev.Type != "message" {
		t.Fatalf("busy delete published %s", ev.Type)
	}
	res = c.alice.do(http.MethodPost, "/api/voice/leave", map[string]string{"session_id": session})
	if res.status != http.StatusNoContent {
		t.Fatalf("leave: %d %s", res.status, res.raw)
	}
	res = c.owner.do(http.MethodDelete, "/api/channels/"+id, nil)
	if res.status != http.StatusNoContent {
		t.Fatalf("empty room: %d %s", res.status, res.raw)
	}
	if eventData(t, waitEvent(t, ws, "channel_removed"))["id"] != id {
		t.Fatal("wrong removed room")
	}
	if res := c.alice.voiceJoin(id); res.status != http.StatusNotFound {
		t.Fatalf("join deleted room: %d %s", res.status, res.raw)
	}
}

func TestVoiceRoomCanBeDeletedWithoutMediaServer(t *testing.T) {
	c := newCast(t)
	id := c.owner.voiceID()
	res := c.owner.do(http.MethodDelete, "/api/channels/"+id, nil)
	if res.status != http.StatusNoContent {
		t.Fatalf("delete without media: %d %s", res.status, res.raw)
	}
}
