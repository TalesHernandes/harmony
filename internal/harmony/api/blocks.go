package api

import "net/http"

func (s *Server) handlePersonPreferences(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	items, err := s.st.PersonPreferences(cu.user.ID)
	if err != nil {
		s.chatError(w, err, "personal preferences")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"items": items})
}

func (s *Server) handlePersonPreference(block, enabled bool) func(http.ResponseWriter, *http.Request, ctxUser) {
	return func(w http.ResponseWriter, r *http.Request, cu ctxUser) {
		if err := s.st.SetPersonPreference(cu.user.ID, r.PathValue("username"), block, enabled); err != nil {
			s.chatError(w, err, "personal preference")
			return
		}
		// No actor, target, or names in diagnostics. Fan-out is strictly to self.
		s.log.Info("personal preference changed", "block", block, "enabled", enabled)
		s.hub.publish([]int64{cu.user.ID}, "person_preferences", map[string]any{})
		s.handlePersonPreferences(w, r, cu)
	}
}

// Database failures must fail closed for this privacy decision.
func (s *Server) blockedPair(a, b int64) bool {
	yes, err := s.st.BlockedPair(a, b)
	return err != nil || yes
}
func (s *Server) blocks(a, b int64) bool { yes, err := s.st.Blocks(a, b); return err != nil || yes }
