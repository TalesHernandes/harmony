// Sprint D2 turned into tests. The media server has its own tests, over real
// UDP, in internal/harmony/sfu; these are about the door in front of it.
//
// The rule being proved is the same one the whole service is built on and it
// has not changed for voice: a room is a channel, and a channel is somebody's
// only if there is a row for them in channel_members. So the interesting cases
// are all of the form "Bruno asks about something that is not his", and the
// answer he gets has to be the answer an invented id gets — the same status and
// the same bytes, because a different answer is itself an answer.
package api

import (
	"net/http"
	"strings"
	"testing"

	"call/internal/turn"
)

// voiceCast is the ordinary cast with the media server switched on. Port zero:
// the operating system picks a free one, so the tests do not fight over 40000
// and do not need the machine's firewall to cooperate.
func newVoiceCast(t *testing.T) *cast {
	t.Helper()
	c := newCast(t)
	if err := c.h.api.StartVoice(VoiceOptions{UDPPort: 0, PublicIP: "127.0.0.1", MaxPerRoom: 8}); err != nil {
		t.Fatalf("start voice: %v", err)
	}
	t.Cleanup(func() { c.h.api.StopVoice() })
	return c
}

// voiceID is the room every account belongs to from migration 3 onwards.
func (d *device) voiceID() string {
	d.h.t.Helper()
	for _, c := range d.channels() {
		if c["kind"] == "voice" {
			return c["id"].(string)
		}
	}
	d.h.t.Fatal("no voice channel in the channel list")
	return ""
}

func (d *device) voiceJoin(channelID string) reply {
	return d.do(http.MethodPost, "/api/voice/"+channelID+"/join", map[string]any{})
}

func (d *device) mustJoinVoice(channelID string) map[string]any {
	d.h.t.Helper()
	res := d.voiceJoin(channelID)
	if res.status != http.StatusOK {
		d.h.t.Fatalf("join voice %s: %d %s", channelID, res.status, res.raw)
	}
	return res.body
}

func (d *device) voiceRooms() map[string]any {
	d.h.t.Helper()
	res := d.get("/api/voice/rooms")
	if res.status != http.StatusOK {
		d.h.t.Fatalf("GET /api/voice/rooms: %d %s", res.status, res.raw)
	}
	rooms, _ := res.body["rooms"].(map[string]any)
	return rooms
}

// ---------------------------------------------------------------------------
// the room is a channel, and a channel is not everybody's
// ---------------------------------------------------------------------------

// TestBrunoCannotJoinSomebodyElsesRoom is the sprint's version of the D1 test
// that mattered most. The conversation between the owner and Alice is a channel
// Bruno has no row in; asking to open a microphone in it has to answer exactly
// what an invented id answers, down to the bytes.
func TestBrunoCannotJoinSomebodyElsesRoom(t *testing.T) {
	c := newVoiceCast(t)
	dm := c.owner.openDM("alice")

	theirs := c.bo.voiceJoin(dm)
	invented := c.bo.voiceJoin("nao-existe-este-canal")

	if theirs.status != http.StatusNotFound {
		t.Fatalf("bruno got %d joining a conversation that is not his: %s", theirs.status, theirs.raw)
	}
	if theirs.raw != invented.raw || theirs.status != invented.status {
		t.Fatalf("a real id and an invented one answer differently:\n real: %d %s\n fake: %d %s",
			theirs.status, theirs.raw, invented.status, invented.raw)
	}
	// And the room does not exist as far as he is concerned.
	if _, listed := c.bo.voiceRooms()[dm]; listed {
		t.Fatalf("bruno's room list names a conversation he is not in: %v", c.bo.voiceRooms())
	}
}

// TestATextChannelIsNotAVoiceRoom: everybody is a member of #geral, so this is
// not about membership. A voice join into a text channel is refused because the
// kind is wrong, and refused with the same answer, so the shape of the refusal
// never says which of the two reasons applied.
func TestATextChannelIsNotAVoiceRoom(t *testing.T) {
	c := newVoiceCast(t)
	res := c.owner.voiceJoin(c.owner.generalID())
	invented := c.owner.voiceJoin("nao-existe-este-canal")
	if res.status != http.StatusNotFound || res.raw != invented.raw {
		t.Fatalf("text channel answered %d %s, invented id answered %d %s",
			res.status, res.raw, invented.status, invented.raw)
	}
}

// TestADirectMessageDoesNotGoThroughTheMediaServer: even for the two people who
// are in it. A 1:1 call stays peer to peer (PROTOCOL.md), and the way to be
// sure the media server never quietly becomes the path for one is to refuse it
// at the door.
func TestADirectMessageDoesNotGoThroughTheMediaServer(t *testing.T) {
	c := newVoiceCast(t)
	dm := c.owner.openDM("alice")
	if res := c.owner.voiceJoin(dm); res.status != http.StatusNotFound {
		t.Fatalf("a direct message opened a room on the media server: %d %s", res.status, res.raw)
	}
}

// ---------------------------------------------------------------------------
// a session id is not a capability
// ---------------------------------------------------------------------------

// TestBrunoCannotUseAliceSession: he has her session identifier — the strongest
// thing he could have got hold of — and it buys him nothing. Not answering for
// her, not muting her, not hanging up on her.
func TestBrunoCannotUseAliceSession(t *testing.T) {
	c := newVoiceCast(t)
	room := c.owner.voiceID()
	joined := c.alice.mustJoinVoice(room)
	session, _ := joined["session_id"].(string)
	if session == "" {
		t.Fatalf("join gave no session: %v", joined)
	}

	cases := []struct {
		path string
		body map[string]any
	}{
		{"/api/voice/answer", map[string]any{"session_id": session, "sdp": "v=0"}},
		{"/api/voice/state", map[string]any{"session_id": session, "muted": true, "deafened": true}},
		{"/api/voice/leave", map[string]any{"session_id": session}},
		{"/api/voice/candidate", map[string]any{
			"session_id": session, "candidate": "candidate:0 1 udp 1 127.0.0.1 1 typ host",
			"sdp_mid": "0", "sdp_mline_index": 0,
		}},
		// The negotiation the browser starts (simulcast, D4r). It is the one
		// call where somebody else's session would let a stranger reshape a
		// live connection, so it belongs in this table before it belongs
		// anywhere else.
		{"/api/voice/offer", map[string]any{
			"session_id": session, "sources": []string{"screen"}, "sdp": "v=0",
		}},
		// Which layer this device receives (D4s). Through somebody else's
		// session this would be "put that person's screen on the smallest
		// picture there is" — a way to spoil what a stranger is watching.
		{"/api/voice/layer", map[string]any{
			"session_id": session, "username": "alice", "layer": "baixa",
		}},
	}
	for _, tc := range cases {
		his := tc.body
		res := c.bo.do(http.MethodPost, tc.path, his)
		fake := map[string]any{}
		for k, v := range his {
			fake[k] = v
		}
		fake["session_id"] = "sessao-inventada"
		invented := c.bo.do(http.MethodPost, tc.path, fake)

		if res.status != http.StatusNotFound {
			t.Fatalf("%s answered %d to somebody else's session: %s", tc.path, res.status, res.raw)
		}
		if res.raw != invented.raw {
			t.Fatalf("%s tells a real session from an invented one: %q vs %q", tc.path, res.raw, invented.raw)
		}
	}

	// And after all of that, Alice is still in the room, unmuted.
	peers := roomPeers(t, c.alice.voiceRooms(), room)
	if len(peers) != 1 {
		t.Fatalf("alice was moved by bruno: %d people in the room", len(peers))
	}
	if peers[0]["muted"] == true {
		t.Fatalf("bruno muted alice: %v", peers[0])
	}
}

// TestOwnSessionWorks is the other half of the test above: the same calls, made
// by the person they belong to, are accepted. A refusal that refuses everybody
// proves nothing.
func TestOwnSessionWorks(t *testing.T) {
	c := newVoiceCast(t)
	room := c.owner.voiceID()
	session, _ := c.alice.mustJoinVoice(room)["session_id"].(string)

	res := c.alice.do(http.MethodPost, "/api/voice/state",
		map[string]any{"session_id": session, "muted": true, "deafened": false})
	if res.status != http.StatusNoContent {
		t.Fatalf("alice could not mute herself: %d %s", res.status, res.raw)
	}
	peers := roomPeers(t, c.owner.voiceRooms(), room)
	if len(peers) != 1 || peers[0]["muted"] != true || peers[0]["username"] != "alice" {
		t.Fatalf("the room does not show alice muted: %v", peers)
	}

	res = c.alice.do(http.MethodPost, "/api/voice/leave", map[string]any{"session_id": session})
	if res.status != http.StatusNoContent {
		t.Fatalf("alice could not leave: %d %s", res.status, res.raw)
	}
	if peers := roomPeers(t, c.owner.voiceRooms(), room); len(peers) != 0 {
		t.Fatalf("alice left and is still there: %v", peers)
	}
}

// ---------------------------------------------------------------------------
// who is inside
// ---------------------------------------------------------------------------

func roomPeers(t *testing.T, rooms map[string]any, id string) []map[string]any {
	t.Helper()
	raw, ok := rooms[id].([]any)
	if !ok {
		t.Fatalf("room %s missing from %v", id, rooms)
	}
	out := make([]map[string]any, 0, len(raw))
	for _, p := range raw {
		out = append(out, p.(map[string]any))
	}
	return out
}

// TestEverybodySeesWhoIsInTheRoom: the point of a voice channel is that you can
// see it is worth joining before you join it.
func TestEverybodySeesWhoIsInTheRoom(t *testing.T) {
	c := newVoiceCast(t)
	room := c.owner.voiceID()

	if peers := roomPeers(t, c.bo.voiceRooms(), room); len(peers) != 0 {
		t.Fatalf("an empty room is not empty: %v", peers)
	}
	c.alice.mustJoinVoice(room)
	c.owner.mustJoinVoice(room)

	peers := roomPeers(t, c.bo.voiceRooms(), room)
	if len(peers) != 2 {
		t.Fatalf("bruno sees %d people, want 2: %v", len(peers), peers)
	}
	names := map[string]bool{}
	for _, p := range peers {
		names[p["username"].(string)] = true
		// The list says who and how, and nothing about the connection itself.
		if _, leaked := p["sdp"]; leaked {
			t.Fatalf("the room list carries connection details: %v", p)
		}
	}
	if !names["alice"] || !names["tales"] {
		t.Fatalf("the room list is missing somebody: %v", names)
	}
}

// TestRoomListOnlyHasMyRooms: the answer is built from the channels this person
// belongs to, so a room they have no row for cannot appear even by accident.
func TestRoomListOnlyHasMyRooms(t *testing.T) {
	c := newVoiceCast(t)
	rooms := c.bo.voiceRooms()
	if len(rooms) != 1 {
		t.Fatalf("bruno sees %d rooms, want the one everybody is in: %v", len(rooms), rooms)
	}
	if _, ok := rooms[c.bo.voiceID()]; !ok {
		t.Fatalf("the one room bruno should see is not there: %v", rooms)
	}
}

// ---------------------------------------------------------------------------
// the owner's things
// ---------------------------------------------------------------------------

// TestOnlyTheOwnerMakesVoiceChannels, like text channels since D1.
func TestOnlyTheOwnerMakesVoiceChannels(t *testing.T) {
	c := newVoiceCast(t)
	res := c.alice.do(http.MethodPost, "/api/channels",
		map[string]any{"name": "jogatina", "topic": "", "kind": "voice"})
	if res.status != http.StatusForbidden || res.errorCode() != "not_allowed" {
		t.Fatalf("alice made a voice channel: %d %s", res.status, res.raw)
	}

	res = c.owner.do(http.MethodPost, "/api/channels",
		map[string]any{"name": "jogatina", "topic": "", "kind": "voice"})
	if res.status != http.StatusCreated {
		t.Fatalf("the owner could not make a voice channel: %d %s", res.status, res.raw)
	}
	id, _ := res.body["id"].(string)
	// And it is a room everybody can be in, straight away.
	if _, ok := c.alice.channelByID(id); !ok {
		t.Fatalf("the new voice channel did not reach alice: %v", c.alice.channels())
	}
	if r := c.alice.voiceJoin(id); r.status != http.StatusOK {
		t.Fatalf("alice could not join the new room: %d %s", r.status, r.raw)
	}
}

// TestOnlyTheOwnerReadsTheMeasurement. The numbers say nothing about what
// anybody said, but they do say how many people are on the server right now.
func TestOnlyTheOwnerReadsTheMeasurement(t *testing.T) {
	c := newVoiceCast(t)
	if res := c.alice.get("/api/voice/stats"); res.status != http.StatusForbidden {
		t.Fatalf("alice read the measurement: %d %s", res.status, res.raw)
	}
	res := c.owner.get("/api/voice/stats")
	if res.status != http.StatusOK {
		t.Fatalf("the owner could not read the measurement: %d %s", res.status, res.raw)
	}
	for _, field := range []string{"bytes_in", "bytes_out", "peers", "cpu_seconds", "rss_bytes"} {
		if _, ok := res.body[field]; !ok {
			t.Fatalf("the measurement has no %s: %s", field, res.raw)
		}
	}
}

// ---------------------------------------------------------------------------
// the rest of the rules, unchanged by this sprint
// ---------------------------------------------------------------------------

// TestVoiceNeedsASession: nothing here is readable, or doable, signed out.
func TestVoiceNeedsASession(t *testing.T) {
	c := newVoiceCast(t)
	stranger := c.h.device("stranger")
	room := c.owner.voiceID()
	for _, call := range []struct {
		method, path string
		body         any
	}{
		{http.MethodGet, "/api/voice/rooms", nil},
		{http.MethodGet, "/api/voice/stats", nil},
		{http.MethodPost, "/api/voice/" + room + "/join", map[string]any{}},
		{http.MethodPost, "/api/voice/answer", map[string]any{"session_id": "x", "sdp": "v=0"}},
		{http.MethodPost, "/api/voice/leave", map[string]any{"session_id": "x"}},
	} {
		res := stranger.do(call.method, call.path, call.body)
		if res.status != http.StatusUnauthorized {
			t.Fatalf("%s %s answered %d to a stranger: %s", call.method, call.path, res.status, res.raw)
		}
	}
}

// TestVoiceRefusesCrossSiteWrites, like every other write since D0: a page on
// another site cannot open your microphone with your cookie.
func TestVoiceRefusesCrossSiteWrites(t *testing.T) {
	c := newVoiceCast(t)
	room := c.owner.voiceID()
	res := c.owner.do(http.MethodPost, "/api/voice/"+room+"/join", map[string]any{},
		func(r *http.Request) { r.Header.Set("Origin", "https://outro-site.example") })
	if res.status != http.StatusForbidden || res.errorCode() != "bad_origin" {
		t.Fatalf("a cross-site join was accepted: %d %s", res.status, res.raw)
	}
}

// TestWithoutTheMediaServerNothingElseBreaks: harmony started with the media
// server switched off is D1, working, plus an honest refusal on the voice
// endpoints. That is what a machine with no UDP port open should do.
func TestWithoutTheMediaServerNothingElseBreaks(t *testing.T) {
	c := newCast(t) // deliberately not newVoiceCast
	room := c.owner.voiceID()
	if res := c.owner.voiceJoin(room); res.status != http.StatusServiceUnavailable {
		t.Fatalf("join answered %d without a media server: %s", res.status, res.raw)
	}
	// The room still lists, empty, so the channel column draws correctly.
	if peers := roomPeers(t, c.owner.voiceRooms(), room); len(peers) != 0 {
		t.Fatalf("somebody is in a room with no media server: %v", peers)
	}
	// And the chat is untouched.
	c.owner.mustSay(c.owner.generalID(), "texto continua")
}

// TestVoiceLogsCarryNoConnectionDetails. A session description holds the keys
// and addresses of a media connection; the same rule that keeps message bodies
// out of the log keeps these out too.
func TestVoiceLogsCarryNoConnectionDetails(t *testing.T) {
	c := newVoiceCast(t)
	room := c.owner.voiceID()
	joined := c.alice.mustJoinVoice(room)
	sdp, _ := joined["sdp"].(string)
	if !strings.Contains(sdp, "a=fingerprint") {
		t.Fatalf("the offer does not look like an offer: %q", sdp)
	}
	_ = c.alice.do(http.MethodPost, "/api/voice/answer",
		map[string]any{"session_id": joined["session_id"], "sdp": sdp})

	logs := c.h.logs.String()
	for _, forbidden := range []string{"a=fingerprint", "a=ice-pwd", "v=0", "candidate:"} {
		if strings.Contains(logs, forbidden) {
			t.Fatalf("the log carries %q", forbidden)
		}
	}
}

// The voice channel is told how to get out of the network, and the credential
// it is told is that person's own.
//
// This is the 18/09/2026 fix. The voice channel used to answer with the offer
// alone, on the reasoning that the media server has a public address and so a
// host candidate always reaches it. That reasoning holds until a browser
// declines to offer a host candidate — Firefox with `no_host` or `relay_only`
// set, which is what every "prevent WebRTC leaks" switch turns on — and then
// the search ends with nothing and the call cannot happen at all.
//
// What is proved here is the same thing the 1:1 call proves: the list is
// present, the relay credential is minted for the person asking and not for
// anybody else, and the secret that mints it does not travel.
func TestJoiningVoiceIsToldHowToLeaveTheNetwork(t *testing.T) {
	secret := "um-segredo-bem-comprido-de-teste"
	h := newHarnessTweaked(t, func(cfg *Config) {
		cfg.ICE = ICEConfig{
			STUNURLs:   []string{"stun:stun.example:3478"},
			TURNURLs:   []string{"turn:relay.example:3478?transport=udp"},
			TURNSecret: []byte(secret),
		}
	})
	if err := h.api.StartVoice(VoiceOptions{UDPPort: 0, PublicIP: "127.0.0.1", MaxPerRoom: 8}); err != nil {
		t.Fatalf("start voice: %v", err)
	}
	t.Cleanup(func() { h.api.StopVoice() })

	owner := h.join(h.bootstrapInvite(), "tales", goodPass)
	inviteA, _ := owner.inviteFrom("alice")
	alice := h.join(inviteA, "alice", goodPass2)

	res := owner.voiceJoin(owner.voiceID())
	if res.status != http.StatusOK {
		t.Fatalf("join voice: %d %s", res.status, res.raw)
	}
	if strings.Contains(res.raw, secret) {
		t.Fatal("the answer carries the TURN secret itself")
	}
	// The offer still has to be there. A list of relays is worth nothing if
	// adding it cost the thing the browser came for.
	if sdp, _ := res.body["sdp"].(string); sdp == "" {
		t.Errorf("the offer lost its sdp: %s", res.raw)
	}
	if id, _ := res.body["session_id"].(string); id == "" {
		t.Errorf("the offer lost its session id: %s", res.raw)
	}

	list, _ := res.body["ice_servers"].([]any)
	if len(list) != 2 {
		t.Fatalf("want a STUN entry and a TURN entry, got %d: %s", len(list), res.raw)
	}
	relay, _ := list[1].(map[string]any)
	user, _ := relay["username"].(string)
	if !strings.HasSuffix(user, ":tales") {
		t.Errorf("the credential is not minted for this person: %q", user)
	}
	if cred, _ := relay["credential"].(string); cred != turn.Sign([]byte(secret), user) {
		t.Errorf("the password is not the HMAC coturn will expect")
	}

	// Alice gets one of her own. Two people in the same room are still two
	// people, and a credential naming somebody else is a credential that
	// identifies the wrong person to the relay.
	hers := alice.voiceJoin(alice.voiceID())
	if hers.status != http.StatusOK {
		t.Fatalf("alice joining: %d %s", hers.status, hers.raw)
	}
	herList, _ := hers.body["ice_servers"].([]any)
	if len(herList) != 2 {
		t.Fatalf("alice got %d ice servers", len(herList))
	}
	herRelay, _ := herList[1].(map[string]any)
	herUser, _ := herRelay["username"].(string)
	if !strings.HasSuffix(herUser, ":alice") {
		t.Errorf("alice was handed a credential that is not hers: %q", herUser)
	}
	if herUser == user {
		t.Error("both people were handed the same credential")
	}
}

// With no relay configured the list is empty and joining still works, because
// that is the setup this service ran on until today and a host candidate
// reaches the media server on its own.
func TestVoiceWithoutARelayStillJoins(t *testing.T) {
	c := newVoiceCast(t)
	res := c.owner.voiceJoin(c.owner.voiceID())
	if res.status != http.StatusOK {
		t.Fatalf("join voice: %d %s", res.status, res.raw)
	}
	list, ok := res.body["ice_servers"].([]any)
	if !ok {
		t.Fatalf("ice_servers is missing entirely: %s", res.raw)
	}
	if len(list) != 0 {
		t.Errorf("want no ice servers, got %d: %s", len(list), res.raw)
	}
}

// TestARevocationFromAnotherProcessHangsUpTheVoice is `harmony -reset-2fa` and
// `harmony -reset`: they write the revocation to the database from a separate
// process, so nothing in the running server is told. Without the sweep, the
// revoked device stayed in the room, hearing and speaking, for as long as its
// media connection lasted — the very device the reset was run to cut off.
func TestARevocationFromAnotherProcessHangsUpTheVoice(t *testing.T) {
	c := newVoiceCast(t)
	room := c.owner.voiceID()
	c.alice.mustJoinVoice(room)
	alice, _, err := c.h.st.UserByUsername("alice")
	if err != nil {
		t.Fatal(err)
	}
	if err := c.h.st.ResetTwoFA(alice.ID); err != nil {
		t.Fatal(err)
	}
	if peers := roomPeers(t, c.bo.voiceRooms(), room); len(peers) != 1 {
		t.Fatalf("before the sweep alice should still be inside (the defect being fixed): %v", peers)
	}
	c.h.api.sweepRevokedVoiceOnce()
	if peers := roomPeers(t, c.bo.voiceRooms(), room); len(peers) != 0 {
		t.Fatalf("a revoked sign-in is still in the voice room: %v", peers)
	}

	// A sign-in that is still good is left alone.
	c.owner.mustJoinVoice(room)
	c.h.api.sweepRevokedVoiceOnce()
	if peers := roomPeers(t, c.bo.voiceRooms(), room); len(peers) != 1 {
		t.Fatalf("the sweep hung up a live sign-in: %v", peers)
	}
}
