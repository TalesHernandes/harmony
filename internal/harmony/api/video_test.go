// Sprint D3 turned into tests: the two new doors in front of the media server,
// and the chat that a voice channel now has.
//
// The new doors are `publish` and `unpublish` — turning a camera or a screen on
// and off in the middle of a call. They take a session identifier and no channel
// identifier, so the only thing that could be forged is the session, and the
// only question worth asking of them is the one this file asks over and over:
// what happens when Bruno holds Alice's session.
//
// The chat is not a new door at all, and that is the point. A voice channel is
// a channel, in the same table with the same membership rows, so its messages
// go through the same handler and the same query as every other channel's. The
// tests below prove that from the outside: what works in a text channel works
// here, and what is refused in a conversation that is not yours is refused the
// same way, down to the bytes.
package api

import (
	"net/http"
	"testing"
)

func (d *device) voicePublish(session string, sources ...string) reply {
	return d.do(http.MethodPost, "/api/voice/publish",
		map[string]any{"session_id": session, "sources": sources})
}

func (d *device) voiceUnpublish(session string, sources ...string) reply {
	return d.do(http.MethodPost, "/api/voice/unpublish",
		map[string]any{"session_id": session, "sources": sources})
}

// ---------------------------------------------------------------------------
// turning a screen on is not something you can do to somebody else
// ---------------------------------------------------------------------------

// TestBrunoCannotPublishOnAliceSession is the D3 version of the test that has
// mattered in every sprint. He has her session identifier and it buys him
// nothing: he cannot turn her camera on, cannot turn her screen off, and the
// refusal he gets is the one an invented session gets.
func TestBrunoCannotPublishOnAliceSession(t *testing.T) {
	c := newVoiceCast(t)
	room := c.owner.voiceID()
	session, _ := c.alice.mustJoinVoice(room)["session_id"].(string)
	if session == "" {
		t.Fatal("join gave no session")
	}

	for _, tc := range []struct {
		what string
		call func(*device, string) reply
	}{
		{"publish", func(d *device, s string) reply { return d.voicePublish(s, "cam", "screen") }},
		{"unpublish", func(d *device, s string) reply { return d.voiceUnpublish(s, "screen") }},
	} {
		hers := tc.call(c.bo, session)
		invented := tc.call(c.bo, "sessao-inventada")
		if hers.status != http.StatusNotFound {
			t.Fatalf("%s answered %d to somebody else's session: %s", tc.what, hers.status, hers.raw)
		}
		if hers.raw != invented.raw || hers.status != invented.status {
			t.Fatalf("%s tells a real session from an invented one: %d %q vs %d %q",
				tc.what, hers.status, hers.raw, invented.status, invented.raw)
		}
	}

	// And Alice is exactly as she was: in the room, sharing nothing.
	peers := roomPeers(t, c.alice.voiceRooms(), room)
	if len(peers) != 1 {
		t.Fatalf("bruno moved alice: %v", peers)
	}
	if pub, _ := peers[0]["publishing"].([]any); len(pub) != 0 {
		t.Fatalf("bruno turned something on for alice: %v", pub)
	}
}

// TestAlicePublishesOnHerOwnSession is the other half: the same calls, made by
// the person they belong to, are accepted. A refusal that refuses everybody
// would prove nothing at all.
func TestAlicePublishesOnHerOwnSession(t *testing.T) {
	c := newVoiceCast(t)
	room := c.owner.voiceID()
	session, _ := c.alice.mustJoinVoice(room)["session_id"].(string)

	if res := c.alice.voicePublish(session, "screen", "screen_audio"); res.status != http.StatusNoContent {
		t.Fatalf("alice could not open her own screen: %d %s", res.status, res.raw)
	}
	if res := c.alice.voiceUnpublish(session, "screen", "screen_audio"); res.status != http.StatusNoContent {
		t.Fatalf("alice could not close her own screen: %d %s", res.status, res.raw)
	}
	// Twice in a row is not an error: pressing a button twice is a thing
	// people do, and the second press finds the slot already as it should be.
	if res := c.alice.voiceUnpublish(session, "screen"); res.status != http.StatusNoContent {
		t.Fatalf("closing a screen that is already closed was an error: %d %s", res.status, res.raw)
	}
}

// TestPublishingNeedsASessionAndTheRightSite: signed out, nothing; from another
// site, nothing. The same two rules every write in this service has had since
// D0 — a page on another domain must not open your camera with your cookie.
func TestPublishingNeedsASessionAndTheRightSite(t *testing.T) {
	c := newVoiceCast(t)
	stranger := c.h.device("stranger")
	for _, path := range []string{"/api/voice/publish", "/api/voice/unpublish"} {
		body := map[string]any{"session_id": "x", "sources": []string{"cam"}}
		if res := stranger.do(http.MethodPost, path, body); res.status != http.StatusUnauthorized {
			t.Fatalf("%s answered %d to a stranger: %s", path, res.status, res.raw)
		}
		res := c.owner.do(http.MethodPost, path, body,
			func(r *http.Request) { r.Header.Set("Origin", "https://outro-site.example") })
		if res.status != http.StatusForbidden || res.errorCode() != "bad_origin" {
			t.Fatalf("%s accepted a cross-site write: %d %s", path, res.status, res.raw)
		}
	}
}

// TestWhoIsSharingIsPartOfTheRoom: the list of who is inside says who has a
// screen or a camera on, so the interface can say "fulano está com a tela
// ligada" without anybody having to subscribe to a picture to find out. It is
// empty until a picture actually arrives — the list reports what the server is
// receiving, not what somebody said they would send.
func TestWhoIsSharingIsPartOfTheRoom(t *testing.T) {
	c := newVoiceCast(t)
	room := c.owner.voiceID()
	session, _ := c.alice.mustJoinVoice(room)["session_id"].(string)
	if res := c.alice.voicePublish(session, "screen"); res.status != http.StatusNoContent {
		t.Fatalf("publish: %d %s", res.status, res.raw)
	}
	peers := roomPeers(t, c.bo.voiceRooms(), room)
	if len(peers) != 1 {
		t.Fatalf("want one person in the room: %v", peers)
	}
	pub, ok := peers[0]["publishing"].([]any)
	if !ok {
		t.Fatalf("the room list has no publishing field: %v", peers[0])
	}
	if len(pub) != 0 {
		t.Fatalf("a slot that is open but empty is not a screen anybody can watch: %v", pub)
	}
	// And it still says nothing about anybody's connection.
	for _, forbidden := range []string{"sdp", "session_mids", "candidate"} {
		if _, leaked := peers[0][forbidden]; leaked {
			t.Fatalf("the room list carries %s: %v", forbidden, peers[0])
		}
	}
}

// TestUnknownSourcesAreIgnoredNotRefused: a newer client asking to publish
// something this build has never heard of should get a working voice channel
// rather than an error, because the alternative is an app that stops working
// the day it is updated.
func TestUnknownSourcesAreIgnoredNotRefused(t *testing.T) {
	c := newVoiceCast(t)
	session, _ := c.alice.mustJoinVoice(c.owner.voiceID())["session_id"].(string)
	if res := c.alice.voicePublish(session, "holograma"); res.status != http.StatusNoContent {
		t.Fatalf("an unknown source was refused: %d %s", res.status, res.raw)
	}
}

// ---------------------------------------------------------------------------
// the chat of a voice channel
// ---------------------------------------------------------------------------

// TestAVoiceChannelHasAChat: the sprint's second half. A voice channel is a
// channel, so it has history, unread counts and mentions like any other, and
// none of that needed a second implementation to get.
func TestAVoiceChannelHasAChat(t *testing.T) {
	c := newVoiceCast(t)
	room := c.owner.voiceID()

	c.alice.mustSay(room, "entrei, alguém vem?")
	c.owner.mustSay(room, "@alice já vou")

	history := c.bo.history(room)
	if len(history) != 2 {
		t.Fatalf("bruno sees %d messages in the voice channel's chat, want 2", len(history))
	}
	if history[0]["body"] != "entrei, alguém vem?" {
		t.Fatalf("the history is not the history: %v", history[0])
	}

	// Unread and mentions are counted for a voice channel exactly as for a
	// text one, because it is the same query.
	ch, ok := c.alice.channelByID(room)
	if !ok {
		t.Fatalf("the voice channel is not in alice's list")
	}
	if unread, _ := ch["unread"].(float64); unread != 1 {
		t.Fatalf("alice has %v unread in the voice channel, want 1 (the owner's reply)", ch["unread"])
	}
	if mentions, _ := ch["mentions"].(float64); mentions != 1 {
		t.Fatalf("alice was mentioned once and the channel says %v", ch["mentions"])
	}
}

// TestAVoiceChannelChatIsNotEverybodysEither: the door in front of the chat is
// the same door as ever. A conversation Bruno has no row in answers him what an
// invented id answers, whether he asks for its history or tries to write in it —
// and the voice channel he *is* in answers normally, so the refusal is about
// membership rather than about everything.
func TestAVoiceChannelChatIsNotEverybodysEither(t *testing.T) {
	c := newVoiceCast(t)
	dm := c.owner.openDM("alice")
	room := c.bo.voiceID()

	theirs := c.bo.get("/api/channels/" + dm + "/messages")
	invented := c.bo.get("/api/channels/nao-existe/messages")
	if theirs.status != http.StatusNotFound || theirs.raw != invented.raw {
		t.Fatalf("history of a conversation that is not bruno's: %d %q vs invented %d %q",
			theirs.status, theirs.raw, invented.status, invented.raw)
	}

	wrote := c.bo.say(dm, "oi")
	inventedWrite := c.bo.say("nao-existe", "oi")
	if wrote.status != http.StatusNotFound || wrote.raw != inventedWrite.raw {
		t.Fatalf("bruno wrote into a conversation that is not his: %d %q vs invented %d %q",
			wrote.status, wrote.raw, inventedWrite.status, inventedWrite.raw)
	}

	// And the room he belongs to takes his message, so what was refused above
	// was the membership and not the endpoint.
	c.bo.mustSay(room, "aqui eu posso")
}

// TestVoiceChatSurvivesTheMediaServerBeingOff: the chat of a voice channel is
// text in a database and has nothing to do with the media server. A machine
// with no UDP port open still has the conversation.
func TestVoiceChatSurvivesTheMediaServerBeingOff(t *testing.T) {
	c := newCast(t) // deliberately not newVoiceCast
	room := c.owner.voiceID()
	c.alice.mustSay(room, "sem voz, com texto")
	if got := c.owner.history(room); len(got) != 1 {
		t.Fatalf("the voice channel's chat needs a media server: %v", got)
	}
}

// ---------------------------------------------------------------------------
// not watching somebody's screen
// ---------------------------------------------------------------------------

func (d *device) voiceWatch(session, who string, on bool) reply {
	return d.do(http.MethodPost, "/api/voice/watch",
		map[string]any{"session_id": session, "username": who, "watching": on})
}

// TestBrunoCannotDecideWhatAliceWatches is the same test as every other door
// here. Being able to stop your own screen arriving is a comfort; being able to
// stop somebody else's is a way to blind them.
func TestBrunoCannotDecideWhatAliceWatches(t *testing.T) {
	c := newVoiceCast(t)
	room := c.owner.voiceID()
	session, _ := c.alice.mustJoinVoice(room)["session_id"].(string)
	if session == "" {
		t.Fatal("join gave no session")
	}

	hers := c.bo.voiceWatch(session, "alice", false)
	invented := c.bo.voiceWatch("sessao-inventada", "alice", false)
	if hers.status != http.StatusNotFound {
		t.Fatalf("watch answered %d to somebody else's session: %s", hers.status, hers.raw)
	}
	if hers.raw != invented.raw || hers.status != invented.status {
		t.Fatalf("watch tells a real session from an invented one: %d %q vs %d %q",
			hers.status, hers.raw, invented.status, invented.raw)
	}

	// And Alice is where she was.
	if peers := roomPeers(t, c.alice.voiceRooms(), room); len(peers) != 1 {
		t.Fatalf("bruno moved alice: %v", peers)
	}
}

// TestAliceDecidesWhatSheWatches is the other half: on her own session it is
// allowed, both ways, and twice in a row is not an error — pressing a button
// twice is a thing people do.
func TestAliceDecidesWhatSheWatches(t *testing.T) {
	c := newVoiceCast(t)
	room := c.owner.voiceID()
	session, _ := c.alice.mustJoinVoice(room)["session_id"].(string)

	for _, on := range []bool{false, false, true, true} {
		if res := c.alice.voiceWatch(session, "bo", on); res.status != http.StatusNoContent {
			t.Fatalf("alice could not set watching=%v: %d %s", on, res.status, res.raw)
		}
	}
	// A name nobody in the room has is not an error either: somebody may have
	// left between the press and the request.
	if res := c.alice.voiceWatch(session, "ninguem", false); res.status != http.StatusNoContent {
		t.Fatalf("watching nobody was an error: %d %s", res.status, res.raw)
	}
	// An empty name is a bug in the caller, and is refused as one.
	if res := c.alice.voiceWatch(session, "", false); res.status != http.StatusBadRequest {
		t.Fatalf("an empty name answered %d: %s", res.status, res.raw)
	}
}

func TestWatchNeedsASessionAndTheRightSite(t *testing.T) {
	c := newVoiceCast(t)
	stranger := c.h.device("stranger")
	body := map[string]any{"session_id": "x", "username": "alice", "watching": false}
	if res := stranger.do(http.MethodPost, "/api/voice/watch", body); res.status != http.StatusUnauthorized {
		t.Fatalf("watch answered %d to a stranger: %s", res.status, res.raw)
	}
	res := c.owner.do(http.MethodPost, "/api/voice/watch", body,
		func(r *http.Request) { r.Header.Set("Origin", "https://outro-site.example") })
	if res.status != http.StatusForbidden {
		t.Fatalf("watch answered %d to another site: %s", res.status, res.raw)
	}
}
