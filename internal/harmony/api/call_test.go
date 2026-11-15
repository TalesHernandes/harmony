// Sprint D4b — the direct call, turned into tests.
//
// There is no audio anywhere in this file and there cannot be: the whole point
// of a direct call is that the sound never comes here. What can be tested from
// the outside is exactly what this server is responsible for — who may ring
// whom, who may act on a call, that what is relayed arrives unread and
// unchanged, and that a person is on one call at a time.
package api

import (
	"encoding/base64"
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"call/internal/turn"
)

// call rings somebody, as this device.
func (d *device) call(username string) reply {
	d.h.t.Helper()
	return d.do(http.MethodPost, "/api/calls", map[string]any{"username": username})
}

// waitFor reads events off a socket until one of the wanted kinds arrives.
// Presence events are skipped: since D4 every socket opening tells everybody
// listening that somebody arrived, and that is not what any of these tests are
// about.
func waitEvent(t *testing.T, ws *websocket.Conn, kinds ...string) gatewayEvent {
	t.Helper()
	want := map[string]bool{}
	for _, k := range kinds {
		want[k] = true
	}
	for {
		_ = ws.SetReadDeadline(time.Now().Add(5 * time.Second))
		var ev gatewayEvent
		if err := ws.ReadJSON(&ev); err != nil {
			t.Fatalf("waiting for %v: %v", kinds, err)
		}
		if want[ev.Type] {
			return ev
		}
	}
}

func eventData(t *testing.T, ev gatewayEvent) map[string]any {
	t.Helper()
	data, ok := ev.Data.(map[string]any)
	if !ok {
		t.Fatalf("event %q has no object in it: %v", ev.Type, ev.Data)
	}
	return data
}

// The rule of the house, on the newest surface: a call belongs to its two
// people, and to nobody else. Every way in is tried from the outside, and each
// one has to answer what an invented id answers — the same bytes, or a 404
// becomes a way of finding out that two other people are talking.
func TestOnePersonCannotReachAnotherPairsCall(t *testing.T) {
	c := newCast(t)

	res := c.owner.call("alice")
	if res.status != http.StatusCreated {
		t.Fatalf("calling alice: %d %s", res.status, res.raw)
	}
	id, _ := res.body["id"].(string)
	if id == "" {
		t.Fatalf("the call has no id: %s", res.raw)
	}

	ghost := "AAAAAAAAAAAAAAAA"
	for _, way := range []struct {
		what string
		send func(string) reply
	}{
		{"GET", func(id string) reply { return c.bo.get("/api/calls/" + id) }},
		{"accept", func(id string) reply {
			return c.bo.do(http.MethodPost, "/api/calls/"+id+"/accept", nil)
		}},
		{"hangup", func(id string) reply {
			return c.bo.do(http.MethodPost, "/api/calls/"+id+"/hangup", map[string]any{})
		}},
		{"signal", func(id string) reply {
			return c.bo.do(http.MethodPost, "/api/calls/"+id+"/signal",
				map[string]any{"kind": "offer", "payload": map[string]any{"sdp": "v=0"}})
		}},
		{"state", func(id string) reply {
			return c.bo.do(http.MethodPost, "/api/calls/"+id+"/state", map[string]any{"mic": true})
		}},
	} {
		real, fake := way.send(id), way.send(ghost)
		if real.status != http.StatusNotFound {
			t.Errorf("%s on somebody else's call: %d %s", way.what, real.status, real.raw)
		}
		if real.status != fake.status || real.raw != fake.raw {
			t.Errorf("%s: a real call answers %d %q and an invented one %d %q; they must be identical",
				way.what, real.status, real.raw, fake.status, fake.raw)
		}
	}

	// And the call is still there, untouched, for the two people it belongs to.
	if res := c.alice.do(http.MethodPost, "/api/calls/"+id+"/accept", nil); res.status != http.StatusOK {
		t.Fatalf("alice answering her own call: %d %s", res.status, res.raw)
	}
}

// Only the person who was called may answer, and the person who called may not
// answer their own call — which would be a way to put somebody into a call they
// never picked up.
func TestOnlyThePersonCalledCanAnswer(t *testing.T) {
	c := newCast(t)
	id, _ := c.owner.call("alice").body["id"].(string)

	mine := c.owner.do(http.MethodPost, "/api/calls/"+id+"/accept", nil)
	if mine.status != http.StatusNotFound {
		t.Errorf("the caller answered his own call: %d %s", mine.status, mine.raw)
	}
	if res := c.alice.do(http.MethodPost, "/api/calls/"+id+"/accept", nil); res.status != http.StatusOK {
		t.Fatalf("the person called could not answer: %d %s", res.status, res.raw)
	}
}

// What the server relays it does not read. This is the sentence that keeps
// harmony out of the middle of a call: an offer goes in as bytes and comes out
// as the same bytes, including a field this server has never heard of.
func TestSignallingIsRelayedWordForWordAndOnlyToTheOtherDevice(t *testing.T) {
	c := newCast(t)

	alice := c.alice.dial(t)
	defer alice.Close()
	bruno := c.bo.dial(t)
	defer bruno.Close()

	id, _ := c.owner.call("alice").body["id"].(string)
	ring := eventData(t, waitEvent(t, alice, "call"))
	if ring["id"] != id {
		t.Fatalf("the ring names call %v, want %s", ring["id"], id)
	}
	from, _ := ring["from"].(map[string]any)
	if from["username"] != "tales" {
		t.Errorf("the ring does not say who is calling: %v", ring["from"])
	}

	// Nothing may be relayed before somebody picks up: until then there is no
	// second device to relay it to.
	early := c.owner.do(http.MethodPost, "/api/calls/"+id+"/signal",
		map[string]any{"kind": "offer", "payload": map[string]any{"sdp": "v=0"}})
	if early.status != http.StatusConflict {
		t.Errorf("an offer before the call was answered: %d %s", early.status, early.raw)
	}

	if res := c.alice.do(http.MethodPost, "/api/calls/"+id+"/accept", nil); res.status != http.StatusOK {
		t.Fatalf("accept: %d %s", res.status, res.raw)
	}

	// A payload with something in it this server has no idea about.
	payload := map[string]any{
		"type":                   "offer",
		"sdp":                    "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\n",
		"uma-coisa-que-inventei": []any{float64(1), "dois"},
	}
	if res := c.owner.do(http.MethodPost, "/api/calls/"+id+"/signal",
		map[string]any{"kind": "offer", "payload": payload}); res.status != http.StatusNoContent {
		t.Fatalf("relaying an offer: %d %s", res.status, res.raw)
	}

	got := eventData(t, waitEvent(t, alice, "call_signal"))
	if got["kind"] != "offer" {
		t.Errorf("the relayed message is a %v", got["kind"])
	}
	mine, _ := json.Marshal(payload)
	theirs, _ := json.Marshal(got["payload"])
	if string(mine) != string(theirs) {
		t.Errorf("the payload changed on the way through:\n sent %s\n got  %s", mine, theirs)
	}

	// And Bruno, who is on the same server and has his socket open, heard none
	// of it: not the ring, not the offer.
	_ = bruno.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	for {
		var ev gatewayEvent
		if err := bruno.ReadJSON(&ev); err != nil {
			break // the deadline: nothing more arrived, which is the point
		}
		if strings.HasPrefix(ev.Type, "call") {
			t.Fatalf("a bystander received %q from somebody else's call", ev.Type)
		}
	}
}

// One call at a time, and the person who is busy is not the only one who may
// not be disturbed: ringing somebody who is already on a call has to say so
// rather than start a second one nobody can answer.
func TestAPersonIsOnOneCallAtATime(t *testing.T) {
	c := newCast(t)
	if res := c.owner.call("alice"); res.status != http.StatusCreated {
		t.Fatalf("the first call: %d %s", res.status, res.raw)
	}
	second := c.bo.call("alice")
	if second.status != http.StatusConflict {
		t.Errorf("alice was rung while already being called: %d %s", second.status, second.raw)
	}
	if code, _ := second.body["error"].(string); code != "busy" {
		t.Errorf("the second caller is told %q, want busy", code)
	}
}

// Calling somebody who does not exist, and calling yourself, answer the same
// thing — which is how nobody uses this to find out who has an account here.
func TestCallingNobodyAndCallingYourselfAnswerTheSame(t *testing.T) {
	c := newCast(t)
	ghost := c.owner.call("ninguem")
	self := c.owner.call("tales")
	if ghost.status != http.StatusNotFound {
		t.Errorf("calling a name that does not exist: %d %s", ghost.status, ghost.raw)
	}
	if ghost.status != self.status || ghost.raw != self.raw {
		t.Errorf("calling yourself answers %d %q and calling a ghost %d %q; they must be identical",
			self.status, self.raw, ghost.status, ghost.raw)
	}
}

// Hanging up ends it for both, and the call stops existing — so the two of them
// can call each other again straight away.
func TestHangingUpEndsItForBothAndFreesThemBoth(t *testing.T) {
	c := newCast(t)
	owner := c.owner.dial(t)
	defer owner.Close()

	id, _ := c.owner.call("alice").body["id"].(string)
	if res := c.alice.do(http.MethodPost, "/api/calls/"+id+"/accept", nil); res.status != http.StatusOK {
		t.Fatalf("accept: %d %s", res.status, res.raw)
	}
	if res := c.alice.do(http.MethodPost, "/api/calls/"+id+"/hangup",
		map[string]any{"reason": "hangup"}); res.status != http.StatusOK {
		t.Fatalf("hangup: %d %s", res.status, res.raw)
	}
	ended := eventData(t, waitEvent(t, owner, "call_ended"))
	if ended["id"] != id || ended["reason"] != "hangup" {
		t.Errorf("the caller was told %v", ended)
	}
	// Gone for good: the id answers like an id that never existed.
	if res := c.owner.get("/api/calls/" + id); res.status != http.StatusNotFound {
		t.Errorf("the call is still there after hanging up: %d %s", res.status, res.raw)
	}
	// And neither of them is busy.
	if res := c.owner.call("alice"); res.status != http.StatusCreated {
		t.Errorf("calling again right afterwards: %d %s", res.status, res.raw)
	}
}

// An unanswered call is over when it has rung long enough. Nobody is left busy
// because of a browser that was closed mid-ring.
func TestACallThatNobodyAnswersStopsExisting(t *testing.T) {
	c := newCast(t)
	// The two clocks, moved on rather than waited out. The cooldown on ringing
	// is one of them, and in real life it is never in the way here: it is twenty
	// seconds and a call rings for forty-five, so by the time a call has rung
	// out the caller may already try again.
	now := time.Now()
	c.h.api.callBook.now = func() time.Time { return now }
	c.h.api.rings.now = func() time.Time { return now }

	id, _ := c.owner.call("alice").body["id"].(string)
	now = now.Add(ringFor + time.Second)

	if res := c.alice.do(http.MethodPost, "/api/calls/"+id+"/accept", nil); res.status != http.StatusNotFound {
		t.Errorf("a call answered long after it stopped ringing: %d %s", res.status, res.raw)
	}
	if res := c.owner.call("alice"); res.status != http.StatusCreated {
		t.Errorf("the caller is still busy from a call nobody answered: %d %s", res.status, res.raw)
	}
}

// The credential for the relay is minted per call, expires on its own, and is
// what coturn will actually accept. The secret itself never leaves the machine.
func TestTheTurnCredentialIsEphemeralAndNeverExposesTheSecret(t *testing.T) {
	secret := "um-segredo-bem-comprido-de-teste"
	h := newHarnessTweaked(t, func(cfg *Config) {
		cfg.ICE = ICEConfig{
			STUNURLs:   []string{"stun:stun.example:3478"},
			TURNURLs:   []string{"turn:relay.example:3478?transport=udp"},
			TURNSecret: []byte(secret),
		}
	})
	owner := h.join(h.bootstrapInvite(), "tales", goodPass)
	inviteA, _ := owner.inviteFrom("alice")
	alice := h.join(inviteA, "alice", goodPass2)

	res := owner.call("alice")
	if res.status != http.StatusCreated {
		t.Fatalf("calling: %d %s", res.status, res.raw)
	}
	if strings.Contains(res.raw, secret) {
		t.Fatal("the answer carries the TURN secret itself")
	}
	list, _ := res.body["ice_servers"].([]any)
	if len(list) != 2 {
		t.Fatalf("want a STUN entry and a TURN entry, got %d: %s", len(list), res.raw)
	}
	relay, _ := list[1].(map[string]any)
	user, _ := relay["username"].(string)
	cred, _ := relay["credential"].(string)
	if !strings.HasSuffix(user, ":tales") {
		t.Errorf("the credential is not minted for this person: %q", user)
	}
	expiry, rest, ok := strings.Cut(user, ":")
	if !ok || rest != "tales" {
		t.Fatalf("username is not <expiry>:<user>: %q", user)
	}
	when, err := time.Parse(time.RFC3339, time.Unix(parseUnix(t, expiry), 0).UTC().Format(time.RFC3339))
	if err != nil {
		t.Fatal(err)
	}
	if when.Before(time.Now()) || when.After(time.Now().Add(turn.TTL+time.Minute)) {
		t.Errorf("the credential expires at %s, which is not within its lifetime", when)
	}
	// It is what coturn computes, which is the only definition of "correct"
	// that matters here.
	if want := turn.Sign([]byte(secret), user); cred != want {
		t.Errorf("the password is not the HMAC coturn will expect")
	}
	if _, err := base64.StdEncoding.DecodeString(cred); err != nil {
		t.Errorf("the password is not base64: %v", err)
	}

	// The person on the other end gets one of their own, minted for them.
	id, _ := res.body["id"].(string)
	got := alice.do(http.MethodPost, "/api/calls/"+id+"/accept", nil)
	if got.status != http.StatusOK {
		t.Fatalf("accept: %d %s", got.status, got.raw)
	}
	theirs, _ := got.body["ice_servers"].([]any)
	if len(theirs) != 2 {
		t.Fatalf("the person answering got %d ice servers", len(theirs))
	}
	relayB, _ := theirs[1].(map[string]any)
	if userB, _ := relayB["username"].(string); !strings.HasSuffix(userB, ":alice") {
		t.Errorf("the answerer's credential is not theirs: %q", userB)
	}
	if strings.Contains(got.raw, secret) {
		t.Fatal("the accept answer carries the TURN secret itself")
	}
}

// Without a relay configured — which is how a laptop runs — a call is still set
// up, with STUN alone and no credential to carry.
func TestWithoutARelayACallStillHappens(t *testing.T) {
	h := newHarnessTweaked(t, func(cfg *Config) {
		cfg.ICE = ICEConfig{STUNURLs: []string{"stun:stun.example:3478"}}
	})
	owner := h.join(h.bootstrapInvite(), "tales", goodPass)
	inviteA, _ := owner.inviteFrom("alice")
	h.join(inviteA, "alice", goodPass2)
	res := owner.call("alice")
	if res.status != http.StatusCreated {
		t.Fatalf("calling: %d %s", res.status, res.raw)
	}
	list, _ := res.body["ice_servers"].([]any)
	if len(list) != 1 {
		t.Fatalf("want the STUN entry alone, got %d: %s", len(list), res.raw)
	}
	only, _ := list[0].(map[string]any)
	if _, has := only["credential"]; has {
		t.Error("the STUN entry carries a credential, which it has no use for")
	}
}

func parseUnix(t *testing.T, s string) int64 {
	t.Helper()
	var n int64
	for _, r := range s {
		if r < '0' || r > '9' {
			t.Fatalf("expiry is not a number: %q", s)
		}
		n = n*10 + int64(r-'0')
	}
	return n
}
