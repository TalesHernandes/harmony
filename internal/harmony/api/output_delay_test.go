package api

import (
	"net/http"
	"testing"
)

func TestVoiceOutputDelayOwnershipAndBounds(t *testing.T) {
	c := newVoiceCast(t)
	sid := c.alice.mustJoinVoice(c.owner.voiceID())["session_id"].(string)
	const path = "/api/voice/output-delay"
	for _, ms := range []int{-500, 0, 250, 500, 1000} {
		body := map[string]any{"session_id": sid, "ms": ms}
		if r := c.alice.do(http.MethodPost, path, body); r.status != http.StatusNoContent {
			t.Fatalf("own delay %d: %d", ms, r.status)
		}
		denied := c.bo.do(http.MethodPost, path, body)
		fake := c.bo.do(http.MethodPost, path, map[string]any{"session_id": "absent", "ms": ms})
		if denied.status != http.StatusNotFound || denied.status != fake.status || denied.raw != fake.raw {
			t.Fatal("another user's session was accessible or disclosed")
		}
	}
	for _, ms := range []any{-501, 1001, 100000, 1.5, "100", nil} {
		if r := c.alice.do(http.MethodPost, path, map[string]any{"session_id": sid, "ms": ms}); r.status != http.StatusBadRequest {
			t.Fatalf("invalid delay %v accepted: %d", ms, r.status)
		}
	}
	if r := c.alice.do(http.MethodPost, path, map[string]any{"session_id": sid}); r.status != http.StatusBadRequest {
		t.Fatal("missing delay accepted")
	}
}
