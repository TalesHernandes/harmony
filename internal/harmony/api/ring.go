// A campainha: calling somebody who is not in the room.
//
// What this is, and what it is not. The `PLANO-DISCORD.md` D4 asks for a
// direct 1:1 call that rings — and a direct call, by the rule in CLAUDE.md,
// stays peer to peer with the contract in docs/PROTOCOL.md. That call does not
// exist in harmony yet, and building it is a sprint of its own (signalling,
// perfect negotiation, ephemeral TURN, answer and decline, the call screen).
// So what rings here is the call harmony *does* have: somebody in a voice room
// asking for a person who is not in it. Tapping the notification joins them to
// that room.
//
// The authorisation is the same one as everywhere else, twice. The caller has
// to be a member of the voice channel *and* actually inside it — you invite
// people to where you are, not to a room you merely know the id of. The person
// being called has to be a member of that channel too, and a channel they do
// not belong to answers exactly what an invented id answers, so a ring cannot
// be used to find out what rooms exist.
package api

import (
	"net/http"
	"sync"
	"time"
)

// ringCooldown is per pair per room. It is not about the push — the notifier
// has its own cooldown for that — but about the modal that pops up on somebody
// else's screen, which is the rudest thing a button in this app can do.
const ringCooldown = 20 * time.Second

type ringLimiter struct {
	mu   sync.Mutex
	last map[string]time.Time
	now  func() time.Time
}

func newRingLimiter() *ringLimiter {
	return &ringLimiter{last: map[string]time.Time{}, now: time.Now}
}

func (r *ringLimiter) take(key string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	now := r.now()
	if at, seen := r.last[key]; seen && now.Sub(at) < ringCooldown {
		return false
	}
	// Kept small by hand: this map only ever holds the pairs that rang in the
	// last twenty seconds, and a twenty-person server cannot make it big.
	for k, at := range r.last {
		if now.Sub(at) > 10*ringCooldown {
			delete(r.last, k)
		}
	}
	r.last[key] = now
	return true
}

// forget drops the cooldown on one pair. A call that actually happened is not
// what this limiter is for: it exists so that pressing a button twice cannot
// make a box appear on somebody's screen twice, and hanging up after talking
// for half an hour is not that. Ringing back straight away is what people do.
//
// A *refusal* keeps its twenty seconds, and that is the distinction: "not now"
// answered by an immediate second ring is exactly the rudeness this guards
// against.
func (r *ringLimiter) forget(key string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.last, key)
}

func (s *Server) handleVoiceRing(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	id := r.PathValue("id")
	var req struct {
		Username string `json:"username"`
	}
	if !decode(w, r, &req) {
		return
	}
	if !s.voiceRoom(w, cu, id) {
		return
	}
	channel, err := s.st.ChannelFor(cu.user.ID, id)
	if err != nil {
		s.chatError(w, err, "ring")
		return
	}
	target, _, err := s.st.UserByUsername(req.Username)
	if err != nil || target.Disabled || target.ID == cu.user.ID {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	// The person being called has to belong to this room as well. Same answer
	// as an invented name, for the same reason.
	if _, err := s.st.ChannelFor(target.ID, id); err != nil {
		fail(w, http.StatusNotFound, "not_found")
		return
	}

	media := s.media.Load()
	if media == nil {
		fail(w, http.StatusServiceUnavailable, "voice_unavailable")
		return
	}
	peers := media.Rooms([]string{id})[id]
	callerInside, targetInside := false, false
	for _, p := range peers {
		if p.Username == cu.user.Username {
			callerInside = true
		}
		if p.Username == target.Username {
			targetInside = true
		}
	}
	if !callerInside {
		// Ringing somebody into a room you are not in is a way to make
		// somebody else's phone buzz from anywhere. It is refused.
		fail(w, http.StatusConflict, "not_in_room")
		return
	}
	if targetInside {
		fail(w, http.StatusConflict, "already_here")
		return
	}
	if !s.rings.take(cu.user.Username + ">" + target.Username + "@" + id) {
		fail(w, http.StatusTooManyRequests, "ring_limit")
		return
	}

	if s.blockedPair(cu.user.ID, target.ID) {
		writeJSON(w, http.StatusOK, map[string]any{"ringing": true})
		return
	}
	s.log.Info("ring", "from", cu.user.ID, "to", target.ID, "channel", id)
	// On the screen of whoever has the app open…
	s.hub.publish([]int64{target.ID}, "ring", map[string]any{
		"channel_id":   channel.ID,
		"channel_name": channel.Name,
		"from":         s.personJSON(cu.user.Username, cu.user.DisplayName),
	})
	// …and in the pocket of whoever does not.
	s.notifyRing(target, cu.user, channel)
	writeJSON(w, http.StatusOK, map[string]any{"ringing": true})
}
