package api

import (
	"net/http"
	"time"

	"call/internal/harmony/passwd"
	"call/internal/harmony/store"
)

func (s *Server) handleAdminAccounts(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	// Check before looking up any account, including invented usernames.
	if !cu.user.IsAdmin {
		fail(w, http.StatusForbidden, "not_allowed")
		return
	}
	if r.Method == http.MethodGet {
		list, err := s.st.ListAccounts()
		if err != nil {
			s.chatError(w, err, "admin accounts")
			return
		}
		out := make([]map[string]any, 0, len(list))
		for _, a := range list {
			item := userJSON(a.User)
			item["email_verified"], item["has_twofa"] = a.EmailVerified, a.TwoFA != ""
			item["disabled"], item["disabled_at"], item["invited_by"] = a.Disabled, "", a.InvitedBy
			if !a.DisabledAt.IsZero() {
				item["disabled_at"] = a.DisabledAt.Format(time.RFC3339)
			}
			if !a.LastSeenAt.IsZero() {
				item["last_seen_at"] = a.LastSeenAt.Format(time.RFC3339)
			}
			out = append(out, item)
		}
		w.Header().Set("Cache-Control", "no-store")
		writeJSON(w, http.StatusOK, map[string]any{"accounts": out})
		return
	}
	u, _, err := s.st.UserByUsername(r.PathValue("username"))
	if err != nil {
		s.chatError(w, err, "admin account")
		return
	}
	action := r.PathValue("action")
	var password string
	switch action {
	case "disable":
		if u.IsAdmin {
			fail(w, http.StatusBadRequest, "cannot_disable_owner")
			return
		}
		err = s.st.DisableAccount(u.ID)
	case "enable":
		err = s.st.EnableAccount(u.ID)
	case "reset-password":
		if u.ID == cu.user.ID {
			fail(w, http.StatusBadRequest, "use_own_settings")
			return
		}
		release, ok := s.holdHashSlot(r)
		if !ok {
			return
		}
		defer release()
		password, err = store.RandomPassword()
		if err == nil {
			var hash string
			hash, err = passwd.Hash(password, s.params)
			if err == nil {
				err = s.st.SetPasswordHash(u.ID, hash)
			}
		}
	default:
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	if err != nil {
		s.chatError(w, err, "admin account update")
		return
	}
	if action != "enable" {
		s.endAccountAccess(u.ID, u.Username)
	}
	s.log.Info("account updated", "user_id", u.ID, "actor_id", cu.user.ID, "action", action)
	if action != "reset-password" {
		// A disabled account drops out of every role's list (store.Roles) and
		// comes back on enable; the pages hear it like any role change.
		s.publishRoles()
	}
	w.Header().Set("Cache-Control", "no-store")
	if action == "reset-password" {
		writeJSON(w, http.StatusOK, map[string]string{"password": password})
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (s *Server) endAccountAccess(userID int64, username string) {
	if media := s.media.Load(); media != nil {
		media.LeaveAllExceptLogin(userID, "")
	}
	// Use the same events and conversation record as an ordinary hangup.
	s.callBook.mu.Lock()
	var calls []directCall
	for id, c := range s.callBook.by {
		if c.party(userID) {
			calls = append(calls, *c)
			delete(s.callBook.by, id)
		}
	}
	s.callBook.mu.Unlock()
	for _, c := range calls {
		s.hub.publish([]int64{c.from, c.to}, "call_ended", map[string]any{"id": c.id, "reason": "hangup"})
		s.noteCall(c, "hangup")
		s.pending.clearKind(c.to, NotifyRing)
		if c.toSess != "" {
			if from, to, ok := s.cards(c); ok {
				s.rings.forget(from.Username + ">" + to.Username + "@call")
			}
		}
	}
	s.hub.dropAllExcept(userID, "")
	s.presenceChanged(userID, username)
}
