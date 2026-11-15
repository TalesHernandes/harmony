package api

import (
	"net/http"

	"call/internal/harmony/sfu"
)

func (s *Server) handleVoiceOutputDelay(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		SessionID string `json:"session_id"`
		Ms        *int   `json:"ms"`
	}
	if !decode(w, r, &req) {
		return
	}
	if req.Ms == nil || *req.Ms < sfu.MinOutputDelayMs || *req.Ms > sfu.MaxOutputDelayMs {
		fail(w, http.StatusBadRequest, "bad_request")
		return
	}
	media := s.media.Load()
	if media == nil {
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
		return
	}
	if err := media.OutputDelay(req.SessionID, cu.user.ID, *req.Ms); err != nil {
		s.voiceError(w, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}
