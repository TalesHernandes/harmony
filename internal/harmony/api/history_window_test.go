package api

import (
	"fmt"
	"net/http"
	"reflect"
	"testing"
)

func TestHistoryWindows(t *testing.T) {
	c := newCast(t)
	channel := c.alice.generalID()
	alice, _, err := c.h.st.UserByUsername("alice")
	if err != nil {
		t.Fatal(err)
	}
	ids := make([]int64, 207)
	for i := range ids {
		m, err := c.h.st.PostMessage(alice.ID, channel, fmt.Sprintf("line %d @bruno", i), nil, 0)
		if err != nil {
			t.Fatal(err)
		}
		ids[i] = m.ID
	}
	for _, tc := range []struct {
		name, cursor, limit string
		anchor, first, last int
		more, newer         bool
	}{
		{"middle", "around", "4", 103, 101, 105, true, true},
		{"start", "around", "4", 0, 0, 2, false, true},
		{"end", "around", "4", 206, 204, 206, true, false},
		{"exact start", "around", "4", 2, 0, 4, false, true},
		{"exact end", "around", "4", 204, 202, 206, true, false},
		{"odd limit", "around", "3", 103, 102, 104, true, true},
		{"anchor only", "around", "1", 103, 103, 103, true, true},
		{"default", "around", "", 103, 78, 128, true, true},
		{"invalid limit", "around", "201", 103, 78, 128, true, true},
		{"maximum", "around", "200", 103, 3, 203, true, true},
		{"forward", "after", "4", 103, 104, 107, false, true},
		{"forward exact end", "after", "4", 202, 203, 206, false, false},
		{"forward short end", "after", "4", 204, 205, 206, false, false},
		{"forward empty", "after", "4", 206, 207, 206, false, false},
		{"forward default", "after", "", 103, 104, 153, false, true},
		{"forward maximum", "after", "200", 0, 1, 200, false, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			url := fmt.Sprintf("/api/channels/%s/messages?%s=%d", channel, tc.cursor, ids[tc.anchor])
			if tc.limit != "" {
				url += "&limit=" + tc.limit
			}
			res := c.alice.get(url)
			if res.status != http.StatusOK {
				t.Fatalf("history: %d %s", res.status, res.raw)
			}
			if res.body["has_more"] != tc.more || res.body["has_newer"] != tc.newer {
				t.Fatalf("flags: %s", res.raw)
			}
			messages := res.body["messages"].([]any)
			if len(messages) != tc.last-tc.first+1 {
				t.Fatalf("got %d messages, want %d", len(messages), tc.last-tc.first+1)
			}
			for i, raw := range messages {
				m := raw.(map[string]any)
				if int64(m["id"].(float64)) != ids[tc.first+i] || m["channel_id"] != channel {
					t.Fatalf("unexpected message at %d: %v", i, m)
				}
				if m["author"] == nil || m["attachments"] == nil || m["reactions"] == nil || m["mentions"] == nil {
					t.Fatalf("missing history fields: %v", m)
				}
			}
		})
	}
	// Existing clients still receive the original keys and the newest page.
	legacy := c.alice.get("/api/channels/" + channel + "/messages")
	if _, found := legacy.body["has_newer"]; found || len(legacy.body["messages"].([]any)) != HistoryPageSize {
		t.Fatalf("legacy contract changed: %s", legacy.raw)
	}
}

func TestHistoryWindowAuthorizationAndMissingAnchors(t *testing.T) {
	c := newCast(t)
	general := c.alice.generalID()
	private := c.bo.openDM("tales")
	secret := c.bo.mustSay(private, "private message")["id"]
	deleted := c.alice.mustSay(general, "removed anchor")["id"]
	res := c.alice.do(http.MethodDelete, fmt.Sprintf("/api/messages/%.0f", deleted), nil)
	if res.status != http.StatusOK {
		t.Fatalf("delete: %d %s", res.status, res.raw)
	}
	for _, cursor := range []string{"around", "after"} {
		missing := c.alice.get("/api/channels/" + general + "/messages?" + cursor + "=99999999")
		for _, tc := range []struct{ channel, anchor string }{
			{private, fmt.Sprintf("%.0f", secret)}, // A cannot read B's channel.
			{general, fmt.Sprintf("%.0f", secret)}, // Nor smuggle its id into A's channel.
			{general, "0"}, {general, "-1"}, {general, "bad"}, {general, ""},
		} {
			got := c.alice.get("/api/channels/" + tc.channel + "/messages?" + cursor + "=" + tc.anchor)
			if got.status != http.StatusNotFound || !reflect.DeepEqual(got.body, missing.body) {
				t.Fatalf("%s must answer the same 404: %d %s", cursor, got.status, got.raw)
			}
		}
		// A removed message cannot be jumped to, but reading on past it works.
		gone := c.alice.get(fmt.Sprintf("/api/channels/%s/messages?%s=%.0f", general, cursor, deleted))
		if want := map[string]int{"around": http.StatusNotFound, "after": http.StatusOK}[cursor]; gone.status != want {
			t.Fatalf("%s from a removed anchor: %d %s", cursor, gone.status, gone.raw)
		}
		// Even a member of both channels cannot use an anchor in the wrong one.
		wrong := c.owner.get(fmt.Sprintf("/api/channels/%s/messages?%s=%.0f", general, cursor, secret))
		if wrong.status != http.StatusNotFound || !reflect.DeepEqual(wrong.body, missing.body) {
			t.Fatalf("wrong-channel anchor: %d %s", wrong.status, wrong.raw)
		}
	}
}

func TestHistoryWindowPreservesMessageFormat(t *testing.T) {
	c := newCast(t)
	channel := c.alice.generalID()
	file := c.alice.upload(channel, "history.png", pngBytes(t, 2, 2))
	if file.status != http.StatusCreated {
		t.Fatalf("upload: %d %s", file.status, file.raw)
	}
	first := c.alice.mustSay(channel, "picture @bruno", file.body["id"].(string))["id"]
	reply := c.alice.do(http.MethodPost, "/api/channels/"+channel+"/messages",
		map[string]any{"body": "reply", "reply_to": first})
	if reply.status != http.StatusCreated {
		t.Fatalf("reply: %d %s", reply.status, reply.raw)
	}
	reaction := c.bo.do(http.MethodPost, fmt.Sprintf("/api/messages/%.0f/reactions", first),
		map[string]any{"emoji": "🔥", "on": true})
	if reaction.status != http.StatusOK {
		t.Fatalf("reaction: %d %s", reaction.status, reaction.raw)
	}
	deleted := c.alice.mustSay(channel, "tombstone")["id"]
	if res := c.alice.do(http.MethodDelete, fmt.Sprintf("/api/messages/%.0f", deleted), nil); res.status != http.StatusOK {
		t.Fatalf("delete: %d %s", res.status, res.raw)
	}
	legacy := c.alice.get("/api/channels/" + channel + "/messages").body["messages"].([]any)
	for _, tc := range []struct {
		cursor string
		id     any
		want   []any
	}{
		{"around", reply.body["id"], legacy},
		{"after", first, legacy[1:]},
	} {
		got := c.alice.get(fmt.Sprintf("/api/channels/%s/messages?%s=%.0f&limit=4", channel, tc.cursor, tc.id))
		if got.status != http.StatusOK || !reflect.DeepEqual(got.body["messages"], tc.want) {
			t.Fatalf("%s changed message format: %d %s", tc.cursor, got.status, got.raw)
		}
	}
}
