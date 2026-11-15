package api

import (
	"net/http"
	"testing"
)

func TestRestartCannotAccessAnotherUsersVoiceSession(t *testing.T) {
	c := newVoiceCast(t)
	offer := c.alice.mustJoinVoice(c.alice.voiceID())
	sid := offer["session_id"].(string)
	other := c.bo.do(http.MethodPost, "/api/voice/restart", map[string]any{"session_id": sid})
	fake := c.bo.do(http.MethodPost, "/api/voice/restart", map[string]any{"session_id": "unknown"})
	if other.status != http.StatusNotFound || other.status != fake.status || other.raw != fake.raw {
		t.Fatalf("restart leaks or accesses another user's session: %d %s / %d %s", other.status, other.raw, fake.status, fake.raw)
	}
	owned := c.alice.do(http.MethodPost, "/api/voice/restart", map[string]any{"session_id": sid})
	if owned.status != http.StatusOK || owned.body["session_id"] != sid || owned.body["sdp"] != offer["sdp"] {
		t.Fatalf("owner cannot replay unanswered offer: %d %s", owned.status, owned.raw)
	}
}
