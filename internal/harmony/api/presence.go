// Presence: who is online, who stepped away, and who is in a call.
//
// Three decisions are worth reading before the code.
//
// **Where it lives.** Nowhere. The gateway already holds one socket per device
// (gateway.go), so "quem está com o app aberto" is a question it can answer
// without anything being written down. That is also the only honest answer:
// after a restart nobody is online, and a table saying otherwise would be a lie
// that outlives the process.
//
// **How "ausente" is decided**, which the owner left to the sprint that built
// this. By the browser saying so, not by the server guessing. The page reports
// one of three things about itself: `active` (the tab is on the screen),
// `hidden` (it is not) and `away` (nobody is there). A socket that is alive
// with a person in front of it is never timed out, because that would be
// guessing at exactly the thing the browser knows for certain — somebody
// reading for twenty minutes is still there.
//
// **The one thing the server does age, and why** (13/09/2026, at the owner's
// request: "ausente no pc tem que ser 5 minutos fora da aba"). A hidden tab
// becomes away after awayAfterHidden. Flicking to another window for a moment
// is not leaving, and going grey the instant somebody alt-tabs is what made him
// ask. This is not the server guessing: the browser reported a fact — this tab
// is not on the screen, as of now — and the passage of time is applied to it
// here for a reason that has no way round it. **A telephone freezes its page
// the moment the screen goes off**, so nothing on that side can report five
// minutes later that five minutes went by. The clock has to be somewhere that
// is still running, and that is here.
//
// **Why the state arrives by POST and not on the socket.** The socket carries
// no writes, by a rule that has held since D1 and is the reason it has no
// authorisation surface of its own. An away flag is a write. It costs one tiny
// request a few times an evening.
//
// **Why "em chamada" is not in here.** The client already receives who is
// inside each voice room it belongs to (voice.go), so it can draw "em chamada"
// from what it has. Answering it here would mean this endpoint deciding what
// rooms the asker may know about — a second authorisation rule for a fact the
// first one already covers.
package api

import (
	"net/http"
	"sync"
	"time"
)

// presenceTracker remembers what was last announced about each person, so that
// a device reconnecting three times in ten seconds is not three events for
// everybody else. It is the only state presence has, and it is ordinary memory.
type presenceTracker struct {
	mu   sync.Mutex
	last map[int64]string

	// order is held across reading the state, recording it and publishing it.
	// Those were three steps under two different locks, and a call that had
	// read "online" could be overtaken by the last socket closing: offline was
	// published, then the late call published online — the last word anybody
	// heard, about somebody who had gone. The sweep only visits people who
	// still have a socket, so nothing ever corrected it. Twenty people and a
	// non-blocking publish make one lock for everybody cheap.
	order sync.Mutex
	// afterRead is a test seam: it runs between reading the state and
	// announcing it, which is the window the race lived in. Nil in production.
	afterRead func(userID int64)
}

func newPresenceTracker() *presenceTracker {
	return &presenceTracker{last: map[int64]string{}}
}

// changed reports whether state is news about this person, and records it.
func (p *presenceTracker) changed(userID int64, state string) bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.last[userID] == state {
		return false
	}
	if state == PresenceOffline {
		delete(p.last, userID)
	} else {
		p.last[userID] = state
	}
	return true
}

// presenceChanged tells everybody with the app open that somebody's state
// moved. It is called when a socket opens, when one closes, and when a device
// says it went away or came back.
func (s *Server) presenceChanged(userID int64, username string) {
	s.presence.order.Lock()
	defer s.presence.order.Unlock()
	state := s.hub.state(userID)
	if s.presence.afterRead != nil {
		s.presence.afterRead(userID)
	}
	if !s.presence.changed(userID, state) {
		return
	}
	s.hub.publish(s.hub.watchers(), "presence", map[string]any{
		"username": username, "state": state, "avatar": s.avatarVersion(username),
	})
}

// handleSetPresence is the browser reporting on itself. It sets the flag for
// the sockets of *this* login session only — the person's phone is not made
// away because their computer's screen went off, which is the whole point of
// aggregating per person afterwards.
func (s *Server) handleSetPresence(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		State string `json:"state"`
		// Busy is a call, and it is a separate question from the window: see
		// the comment on gatewayConn.busy.
		Busy bool `json:"busy"`
	}
	if !decode(w, r, &req) {
		return
	}
	reported := PresenceOnline
	switch req.State {
	case PresenceAway:
		reported = PresenceAway
	case PresenceHidden:
		reported = PresenceHidden
	case PresenceOnline, "active":
	default:
		fail(w, http.StatusBadRequest, "bad_request")
		return
	}
	s.hub.setReported(cu.user.ID, cu.sess.ID, reported, req.Busy)
	s.presenceChanged(cu.user.ID, cu.user.Username)
	w.WriteHeader(http.StatusNoContent)
}

// handlePresence is the list a page draws on arrival, before any event has had
// a chance to happen. Only people with something open are in it; anybody the
// client does not find here is offline.
//
// This is the membership of a twenty-person private server looking at itself,
// the same list /api/members already answers. It carries no address, no session
// and no device: a name and one of three words.
func (s *Server) handlePresence(w http.ResponseWriter, _ *http.Request, _ ctxUser) {
	states := s.hub.states()
	users, err := s.st.ListUsers()
	if err != nil {
		s.chatError(w, err, "presence")
		return
	}
	out := map[string]string{}
	avatars := map[string]string{}
	for _, u := range users {
		if state, ok := states[u.ID]; !u.Disabled && ok && state != PresenceOffline {
			out[u.Username] = state
			avatars[u.Username] = u.Avatar
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{"presence": out, "avatars": avatars})
}

// awayAfterHidden is how long a tab may sit behind another window before its
// owner stops counting as around. Five minutes, which the owner chose and which
// is what Discord uses for the same dot.
const awayAfterHidden = 5 * time.Minute

// presenceSweep is the other half of aging a hidden tab. stateLocked already
// gives the right answer to anybody who asks, but nobody asks on a quiet
// evening: without this, a dot would keep saying "online" on every open screen
// until some unrelated event happened to shake it. main starts this.
//
// It is a loop over the people who have something open — twenty, on this
// service — every half minute, and it publishes only what changed, because
// presenceChanged already refuses to repeat itself.
func (s *Server) PresenceSweep(stop <-chan struct{}) {
	t := time.NewTicker(30 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-stop:
			return
		case <-t.C:
			s.sweepPresenceOnce()
			s.sweepRevokedVoiceOnce()
			s.sweepAppReleaseOnce()
		}
	}
}

// sweepRevokedVoiceOnce hangs up the media of sign-ins that no longer exist.
// A revocation made by this process hangs up on the spot; one written by
// another process — `harmony -reset-2fa`, `-reset` — or an expiry is only seen
// here. The socket's ping catches the device whose socket is open; this
// catches the one in a voice room whose socket is down, which a voice call is
// allowed to outlive.
func (s *Server) sweepRevokedVoiceOnce() {
	media := s.media.Load()
	if media == nil {
		return
	}
	for _, login := range media.Logins() {
		if alive, err := s.st.SessionAlive(login); err == nil && !alive {
			s.log.Info("voice closed: its session is gone")
			media.LeaveLogin(login)
		}
	}
}

func (s *Server) sweepPresenceOnce() {
	for uid, name := range s.hub.people() {
		s.presenceChanged(uid, name)
	}
}
