// The endpoints behind the doorbell: registering a device, forgetting it,
// saying whether it is really registered, and telling a woken service worker
// what to show.
//
// Every one of them goes through requireUser, like everything else that touches
// data. That is worth a note because the app 1:1 could not do it: its service
// worker had no session to speak of and had to prove itself by knowing the
// endpoint it was replacing. Here the session is an HttpOnly cookie on the
// origin, and a service worker's fetch to its own origin carries it — so
// `pushsubscriptionchange`, which fires with no page anywhere, is still an
// authenticated request. A subscription is data belonging to the person who
// made it, and it is treated as such: the ownership is in the WHERE clause.
package api

import (
	"net/http"
	"net/url"
	"strings"
)

// maxEndpoint bounds what may be stored. An endpoint plus two base64 keys is
// well under a kilobyte in every push service in existence.
const maxEndpoint = 1024

// handlePushKey hands the page the application server key it needs before it
// can subscribe at all, and says whether the doorbell is configured on this
// server. A VAPID public key is public by definition; this is behind the
// session anyway, because on this server everything is.
func (s *Server) handlePushKey(w http.ResponseWriter, _ *http.Request, _ ctxUser) {
	if s.vapid == nil {
		writeJSON(w, http.StatusOK, map[string]any{"enabled": false})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"enabled": true, "key": s.vapid.PublicKey()})
}

func (s *Server) handlePushSubscribe(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	if s.vapid == nil {
		fail(w, http.StatusServiceUnavailable, "push_disabled")
		return
	}
	var req struct {
		Endpoint string `json:"endpoint"`
		Keys     struct {
			P256dh string `json:"p256dh"`
			Auth   string `json:"auth"`
		} `json:"keys"`
		// Replaces carries the endpoint being rotated out, when the browser
		// retires one on its own. It only ever swaps one of this person's
		// registrations for another — a stranger's endpoint is not theirs to
		// name, and RemovePushSubscription has the user id in the WHERE.
		Replaces string `json:"replaces"`
	}
	if !decode(w, r, &req) {
		return
	}
	if !validPushEndpoint(req.Endpoint) {
		fail(w, http.StatusBadRequest, "bad_endpoint")
		return
	}
	if req.Replaces != "" && req.Replaces != req.Endpoint {
		if err := s.st.RemovePushSubscription(cu.user.ID, req.Replaces); err != nil {
			s.chatError(w, err, "push unsubscribe")
			return
		}
	}
	if err := s.st.AddPushSubscription(
		cu.user.ID, cu.sess.ID, req.Endpoint, req.Keys.P256dh, req.Keys.Auth, r.UserAgent(),
	); err != nil {
		s.chatError(w, err, "push subscribe")
		return
	}
	// No endpoint in the log line: it is a capability URL — anybody holding it
	// can make this person's phone buzz — and secrets do not go in logs.
	s.log.Info("push subscribed", "user_id", cu.user.ID, "session", cu.sess.ID)
	s.pushStatus(w, cu, req.Endpoint)
}

func (s *Server) handlePushUnsubscribe(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		Endpoint string `json:"endpoint"`
	}
	if !decode(w, r, &req) {
		return
	}
	// Somebody else's endpoint, and one that never existed, do exactly the same
	// thing here: nothing, successfully. A page unsubscribing twice is normal,
	// and a different answer for a stranger's endpoint would confirm it exists.
	if err := s.st.RemovePushSubscription(cu.user.ID, req.Endpoint); err != nil {
		s.chatError(w, err, "push unsubscribe")
		return
	}
	s.log.Info("push unsubscribed", "user_id", cu.user.ID)
	s.pushStatus(w, cu, req.Endpoint)
}

// handlePushStatus is the antidote to the failure this whole feature is prone
// to: a doorbell that is off and looks fine. The settings screen shows it, so
// "o servidor não tem o registro deste aparelho" is something that can be read
// instead of something found out by not being called.
func (s *Server) handlePushStatus(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	s.pushStatus(w, cu, r.URL.Query().Get("endpoint"))
}

func (s *Server) pushStatus(w http.ResponseWriter, cu ctxUser, endpoint string) {
	subscribed := false
	if endpoint != "" {
		var err error
		subscribed, err = s.st.HasPushSubscription(cu.user.ID, endpoint)
		if err != nil {
			s.chatError(w, err, "push status")
			return
		}
	}
	count, err := s.st.CountPushSubscriptions(cu.user.ID)
	if err != nil {
		s.chatError(w, err, "push status")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"enabled":    s.vapid != nil,
		"subscribed": subscribed,
		"devices":    count,
	})
}

// handlePendingNotifications is what the woken service worker asks. It is the
// reason the push itself can be empty (see notify.go).
//
// The list is filtered here, at the last possible moment, against what is
// actually still unread — so a mention already answered on the computer never
// reaches the phone in the pocket. That is something no payload sitting in a
// push queue could do.
func (s *Server) handlePendingNotifications(w http.ResponseWriter, _ *http.Request, cu ctxUser) {
	items := s.pending.take(cu.user.ID)
	out := make([]notification, 0, len(items))
	if len(items) > 0 {
		unread, err := s.unreadByChannel(cu.user.ID)
		if err != nil {
			s.chatError(w, err, "pending notifications")
			return
		}
		for _, n := range items {
			if n.AuthorID != 0 && s.blocks(cu.user.ID, n.AuthorID) {
				continue
			}
			if n.Kind != NotifyRing && unread[n.ChannelID] == 0 {
				continue
			}
			out = append(out, n)
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{"items": out})
}

// unreadByChannel is how many unread messages this person has, per channel. It
// comes from the same query the channel list is drawn from, so "já li isso" on
// the phone means exactly what it means on the computer.
func (s *Server) unreadByChannel(userID int64) (map[string]int, error) {
	list, err := s.st.ListChannels(userID)
	if err != nil {
		return nil, err
	}
	out := make(map[string]int, len(list))
	for _, ch := range list {
		out[ch.ID] = ch.Unread
	}
	return out, nil
}

// validPushEndpoint keeps anything that is not an https URL out of the
// database. The stored value feeds an outbound POST from this server, so a
// stored `http://10.0.0.1/` would turn harmony into a request forwarder for
// whoever managed to write it.
func validPushEndpoint(endpoint string) bool {
	if endpoint == "" || len(endpoint) > maxEndpoint {
		return false
	}
	u, err := url.Parse(endpoint)
	return err == nil && u.Scheme == "https" && u.Host != "" && !strings.Contains(endpoint, " ")
}

// pushEnabled says whether this build can ring anybody at all. Used by /api/me
// so the client can tell "você não ligou as notificações" from "este servidor
// não tem notificação", which are two different sentences for the person.
func (s *Server) pushEnabled() bool { return s.vapid != nil }

// FCM tokens are installation secrets; never include them in logs or errors.
func validFCMToken(token string) bool {
	if len(token) == 0 || len(token) > 4096 {
		return false
	}
	for _, c := range token {
		if !(c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_' || c == ':' || c == '.' || c == '-') {
			return false
		}
	}
	return true
}

func (s *Server) handleFCMRegister(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	if s.fcm == nil {
		fail(w, http.StatusServiceUnavailable, "push_disabled")
		return
	}
	var req struct {
		Token string `json:"token"`
	}
	if !decode(w, r, &req) {
		return
	}
	if !validFCMToken(req.Token) {
		fail(w, http.StatusBadRequest, "bad_token")
		return
	}
	if err := s.st.AddFCMToken(cu.user.ID, cu.sess.ID, req.Token); err != nil {
		s.chatError(w, err, "fcm register")
		return
	}
	s.log.Info("fcm registered", "user_id", cu.user.ID)
	s.fcmStatus(w, cu, true)
}

func (s *Server) handleFCMRemove(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		Token string `json:"token"`
	}
	if !decode(w, r, &req) {
		return
	}
	// Removal remains available with FCM disabled and never reveals ownership.
	if err := s.st.RemoveFCMToken(cu.user.ID, req.Token); err != nil {
		s.chatError(w, err, "fcm remove")
		return
	}
	s.log.Info("fcm removed", "user_id", cu.user.ID)
	s.fcmStatus(w, cu, false)
}

func (s *Server) fcmStatus(w http.ResponseWriter, cu ctxUser, registered bool) {
	count, err := s.st.CountFCMTokens(cu.user.ID)
	if err != nil {
		s.chatError(w, err, "fcm status")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"enabled": s.fcm != nil, "registered": registered, "devices": count})
}
