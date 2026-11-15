// Voice channels: the HTTP half of the media server.
//
// The shape is the same as chat.go's, and for the same reason. Every handler
// takes the signed-in person from requireUser, hands the channel id to
// ChannelFor, and only then speaks to the media server. The media server itself
// holds no opinion about who may be where: it is told the room, and it is told
// which rooms to report on. A voice channel somebody does not belong to answers
// exactly what an invented id answers.
//
// Signalling rides on ordinary requests, not on the live socket. The socket
// stays receive-only, as it has since D1, so it still has no authorisation
// surface of its own: a renegotiation offer goes *out* on it, and every reply
// comes back through a checked POST.
package api

import (
	"errors"
	"net/http"

	"github.com/pion/webrtc/v4"

	"call/internal/harmony/sfu"
	"call/internal/harmony/store"
)

// voiceError answers what the asker is entitled to know. An unknown session and
// somebody else's session are one answer on purpose.
func (s *Server) voiceError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, sfu.ErrNoSession):
		fail(w, http.StatusNotFound, "not_found")
	case errors.Is(err, sfu.ErrRoomFull):
		fail(w, http.StatusConflict, "room_full")
	case errors.Is(err, sfu.ErrTooManySlots):
		fail(w, http.StatusConflict, "reconnect_needed")
	case errors.Is(err, sfu.ErrClosed):
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
	// The browser asked to negotiate while one of ours was in flight. Not an
	// error the person should ever see: it means "ask again in a moment", and
	// the moment is short because the offer in flight is about to settle.
	case errors.Is(err, sfu.ErrBusyNegotiating):
		fail(w, http.StatusConflict, "negotiating")
	// A malformed request rather than a server fault: an unknown source, or an
	// offer that opened no picture slot at all.
	case errors.Is(err, sfu.ErrBadSource), errors.Is(err, sfu.ErrNoNewSlot):
		fail(w, http.StatusBadRequest, "bad_offer")
	default:
		s.log.Error("voice", "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
	}
}

// voiceRoom checks that this person may be in this room, and that the room is a
// voice room at all. Both failures answer not_found: telling the two apart would
// say whether a given id exists, which is the thing a stranger is trying to find
// out. A direct message lands here too — a 1:1 call stays peer to peer
// (PROTOCOL.md) and never goes near the media server.
func (s *Server) voiceRoom(w http.ResponseWriter, cu ctxUser, id string) bool {
	if s.media.Load() == nil {
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
		return false
	}
	ch, err := s.st.ChannelFor(cu.user.ID, id)
	if err != nil || ch.Kind != store.ChannelVoice {
		fail(w, http.StatusNotFound, "not_found")
		return false
	}
	return true
}

func (s *Server) handleVoiceJoin(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	id := r.PathValue("id")
	var req struct {
		// Publish is what this device will send: ["audio"] for a voice
		// channel. The server has to open the slots before it writes the
		// offer, because the browser never offers.
		Publish []string `json:"publish"`
	}
	if !decode(w, r, &req) {
		return
	}
	if !s.voiceRoom(w, cu, id) {
		return
	}
	media := s.media.Load()
	if media == nil {
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
		return
	}
	offer, err := media.Join(id, sfu.Joiner{
		UserID:       cu.user.ID,
		LoginSession: cu.sess.ID,
		Username:     cu.user.Username,
		DisplayName:  cu.user.DisplayName,
		Publish:      req.Publish,
	})
	if err != nil {
		s.voiceError(w, err)
		return
	}
	// The offer travels with the list of ways out of this network, and until
	// 18/09/2026 it did not.
	//
	// The voice channel was built assuming a host candidate would always be
	// there: the media server answers on one public address, so the browser's
	// own address on the local network is enough to reach it, and neither STUN
	// nor a relay is needed. That is true right up until a browser declines to
	// offer its local address at all — which is exactly what the WebRTC
	// leak-protection switches do, in Firefox's own settings and in every VPN
	// extension that ships one.
	//
	// Then there is nothing. Not a slow connection, not a bad route: the search
	// ends with zero candidates and the column sits on "Conectando..." forever.
	// The owner's Firefox did it tonight, in 97 ms, while Edge on the same
	// machine found four paths and the telephone found ten.
	//
	// So the voice channel now gets what the 1:1 call has had since its fourth
	// sprint, from the same relay and the same per-user ephemeral credential.
	// A browser that hides its local address still finds its public one through
	// STUN, and a browser that cannot open a UDP socket at all still has
	// turn:...?transport=tcp. Neither costs anything when the host candidate
	// works, because ICE prefers it and never asks the relay.
	writeJSON(w, http.StatusOK, struct {
		sfu.Offer
		ICEServers []map[string]any `json:"ice_servers"`
	}{s.avatarOffer(offer), s.iceServers(cu.user.Username)})
}

type voiceSessionRequest struct {
	SessionID string `json:"session_id"`
}

func (s *Server) handleVoiceRestart(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req voiceSessionRequest
	if !decode(w, r, &req) {
		return
	}
	media := s.media.Load()
	if media == nil {
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
		return
	}
	offer, err := media.RestartICE(req.SessionID, cu.user.ID)
	if err != nil {
		s.voiceError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, offer)
}

func (s *Server) handleVoiceAnswer(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		SessionID string `json:"session_id"`
		SDP       string `json:"sdp"`
	}
	if !decode(w, r, &req) {
		return
	}
	media := s.media.Load()
	if media == nil {
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
		return
	}
	if err := media.Answer(req.SessionID, cu.user.ID, req.SDP); err != nil {
		s.voiceError(w, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// handleVoiceOffer takes the browser's own offer for a screen and answers it.
//
// It is the exception to "the server offers", and it is here because simulcast
// cannot be asked for any other way: Pion does not write `a=rid` into an offer
// it originates, and the layers have to be named in the offer for the browser
// to send them. The whole reasoning, with what was measured, is on
// sfu.ClientOffer.
//
// What is checked is the only thing that could be forged — that the session
// belongs to the person asking. A session that is not yours is answered exactly
// as one that never existed, which is the rule the rest of this file keeps.
func (s *Server) handleVoiceOffer(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		SessionID string `json:"session_id"`
		// Which of this device's pictures and sounds the offer opens slots
		// for, in the order they appear in it.
		Sources []string `json:"sources"`
		SDP     string   `json:"sdp"`
	}
	if !decode(w, r, &req) {
		return
	}
	media := s.media.Load()
	if media == nil {
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
		return
	}
	answer, err := media.ClientOffer(req.SessionID, cu.user.ID, req.Sources, req.SDP)
	if err != nil {
		s.voiceError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"type": "answer", "sdp": answer})
}

func (s *Server) handleVoiceCandidate(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		SessionID     string  `json:"session_id"`
		Candidate     string  `json:"candidate"`
		SDPMid        *string `json:"sdp_mid"`
		SDPMLineIndex *uint16 `json:"sdp_mline_index"`
	}
	if !decode(w, r, &req) {
		return
	}
	media := s.media.Load()
	if media == nil {
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
		return
	}
	err := media.Candidate(req.SessionID, cu.user.ID, webrtc.ICECandidateInit{
		Candidate:     req.Candidate,
		SDPMid:        req.SDPMid,
		SDPMLineIndex: req.SDPMLineIndex,
	})
	if err != nil {
		s.voiceError(w, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// handleVoicePublish and handleVoiceUnpublish are what turn a camera or a
// screen on and off in the middle of a call.
//
// They take a session id and no channel id, and that is on purpose: the room a
// session is in was decided when it joined, and it was checked then. Asking for
// the channel again here would be a second place where the same question is
// answered, and a second place to get it wrong. What is checked is the only
// thing that could be forged — that the session belongs to the person asking —
// and the media server answers a session that is not yours exactly as it
// answers one that never existed.
//
// The offer that follows does not come back in this response. It goes out on
// the live socket like every other renegotiation, so that turning a screen on
// while somebody else is joining cannot produce two offers racing down one
// connection.
func (s *Server) handleVoicePublish(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	s.voicePublish(w, r, cu, true)
}

func (s *Server) handleVoiceUnpublish(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	s.voicePublish(w, r, cu, false)
}

func (s *Server) voicePublish(w http.ResponseWriter, r *http.Request, cu ctxUser, on bool) {
	var req struct {
		SessionID string `json:"session_id"`
		// Sources are "cam", "screen" and "screen_audio". "mic" is accepted
		// and does nothing on the way out: a microphone is silenced by mute,
		// not by taking its slot away.
		Sources []string `json:"sources"`
	}
	if !decode(w, r, &req) {
		return
	}
	media := s.media.Load()
	if media == nil {
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
		return
	}
	var err error
	if on {
		err = media.Publish(req.SessionID, cu.user.ID, req.Sources)
	} else {
		err = media.Unpublish(req.SessionID, cu.user.ID, req.Sources)
	}
	if err != nil {
		s.voiceError(w, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleVoiceLeave(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req voiceSessionRequest
	if !decode(w, r, &req) {
		return
	}
	media := s.media.Load()
	if media == nil {
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
		return
	}
	if err := media.Leave(req.SessionID, cu.user.ID); err != nil {
		s.voiceError(w, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// handleVoiceWatch is "estou na chamada mas não quero ver a tela de fulano".
//
// It takes a session and a name and nothing else. The session is checked
// against the person asking, exactly like the others here, so knowing somebody
// else's session identifier does not let you decide what they watch. And it
// only ever changes what *this* session receives: there is no way through this
// door to stop somebody else's picture reaching the room.
func (s *Server) handleVoiceWatch(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		SessionID string `json:"session_id"`
		Username  string `json:"username"`
		Watching  bool   `json:"watching"`
	}
	if !decode(w, r, &req) {
		return
	}
	if req.Username == "" {
		fail(w, http.StatusBadRequest, "bad_request")
		return
	}
	media := s.media.Load()
	if media == nil {
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
		return
	}
	if err := media.Watch(req.SessionID, cu.user.ID, req.Username, req.Watching); err != nil {
		s.voiceError(w, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// handleVoiceLayer is "a tela de fulano está deste tamanho aqui na minha tela".
//
// Same door as the one above and the same check: the session is matched against
// the person asking, so knowing somebody else's session identifier does not let
// you decide what size THEY receive. And it only ever lowers what this session
// gets — there is no way through here to touch what reaches the room.
func (s *Server) handleVoiceLayer(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		SessionID string `json:"session_id"`
		Username  string `json:"username"`
		// "alta", "media", "baixa", or empty for "as big as you have".
		Layer string `json:"layer"`
	}
	if !decode(w, r, &req) {
		return
	}
	if req.Username == "" {
		fail(w, http.StatusBadRequest, "bad_request")
		return
	}
	media := s.media.Load()
	if media == nil {
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
		return
	}
	if err := media.Layer(req.SessionID, cu.user.ID, req.Username, req.Layer); err != nil {
		s.voiceError(w, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleVoiceState(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		SessionID string `json:"session_id"`
		Muted     bool   `json:"muted"`
		Deafened  bool   `json:"deafened"`
	}
	if !decode(w, r, &req) {
		return
	}
	media := s.media.Load()
	if media == nil {
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
		return
	}
	if err := media.SetState(req.SessionID, cu.user.ID, req.Muted, req.Deafened); err != nil {
		s.voiceError(w, err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// handleVoiceRooms answers who is inside, for the voice channels this person is
// a member of and no others. The list of rooms comes from the store, not from
// the media server, so the media server never has to be trusted with the
// question.
func (s *Server) handleVoiceRooms(w http.ResponseWriter, _ *http.Request, cu ctxUser) {
	ids, err := s.st.VoiceChannelIDs(cu.user.ID)
	if err != nil {
		s.chatError(w, err, "voice rooms")
		return
	}
	rooms := map[string][]sfu.Peer{}
	if media := s.media.Load(); media != nil {
		rooms = media.Rooms(ids)
		for id, peers := range rooms {
			rooms[id] = s.avatarPeers(peers)
		}
	} else {
		for _, id := range ids {
			rooms[id] = []sfu.Peer{}
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{"rooms": rooms})
}

// handleVoiceStats is the D2 instrument, and it is the owner's alone. It says
// nothing about who is talking or what was said — packet counts, byte counts,
// processor seconds — but it does say how many people are on the server right
// now, and that is not everybody's business.
func (s *Server) handleVoiceStats(w http.ResponseWriter, _ *http.Request, cu ctxUser) {
	if !cu.user.IsAdmin {
		fail(w, http.StatusForbidden, "not_allowed")
		return
	}
	media := s.media.Load()
	if media == nil {
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
		return
	}
	writeJSON(w, http.StatusOK, media.Stats())
}

// ---------------------------------------------------------------- wiring

// voiceSignal is how the media server reaches a browser. It goes to the one
// device that joined — named by its login session — and not to that person's
// other phone, because an offer carries the keys to a media connection and the
// phone has no business answering it.
func (s *Server) voiceSignal(userID int64, loginSession, kind string, data any) {
	if offer, ok := data.(sfu.Offer); ok {
		data = s.avatarOffer(offer)
	}
	s.hub.publishSession(userID, loginSession, kind, data)
}

// voiceHangUp ends any voice session belonging to one login, for when that
// login stops being valid.
func (s *Server) voiceHangUp(loginSession string) {
	if media := s.media.Load(); media != nil {
		media.LeaveLogin(loginSession)
	}
}

// voiceRoomChanged tells a voice channel's members that who-is-inside moved.
// The recipient list is the membership table, the same one that decides who may
// read the channel at all.
func (s *Server) voiceRoomChanged(roomID string) {
	media := s.media.Load()
	if media == nil {
		return
	}
	peers := media.Rooms([]string{roomID})[roomID]
	s.notifyChannel(roomID, "voice_room", map[string]any{
		"channel_id": roomID,
		"peers":      s.avatarPeers(peers),
	})
}
