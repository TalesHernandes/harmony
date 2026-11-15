// The direct call: one person ringing another, peer to peer.
//
// This is the D4b sprint, and the thing that makes it different from every
// other call in harmony is where the sound goes. A voice channel is a room and
// its audio goes through this machine's own media server (internal/harmony/sfu)
// because eight people sending to seven others each is what a server is for. A
// direct call is two people, and two people do not need a middleman: the audio
// and the picture go **browser to browser**, and this file never sees a byte of
// either. What it carries is the introduction — who is calling whom, and the
// handful of messages the two browsers need to exchange to find each other.
//
// Three things follow from that, and they are the whole design:
//
//   - **Nothing is stored.** A call is live state, like presence and like who
//     is inside a voice room: it dies with the process, and that is the honest
//     shape of it — nobody is on a call after a restart, and a table saying
//     otherwise would be a lie that survives reboots. So no migration, and no
//     record in any backup of who called whom.
//   - **The server does not read what it relays.** An offer, an answer and a
//     candidate go through as the bytes they arrived as (json.RawMessage). The
//     contract is docs/PROTOCOL.md, written for the 1:1 app that has been in
//     the air since the first sprints, and the rule there is the rule here:
//     the server never interprets SDP.
//   - **Signalling rides on ordinary requests**, not on the live socket. The
//     socket has carried no writes since D1 and still does not, so it has no
//     authorisation surface of its own: everything that changes anything comes
//     back through a POST with the same cookie check and the same "is this
//     person in this call" check.
//
// Authorisation here is small enough to say in one sentence: you may act on a
// call you are one of the two people in, and a call you are not part of answers
// exactly what an invented id answers — the same bytes, so a 404 never tells
// anybody whether a call exists.
package api

import (
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"errors"
	"net/http"
	"strconv"
	"sync"
	"time"

	"call/internal/turn"
)

// callID names one call in a URL. It is not a secret and authorises nothing by
// itself — every endpoint below asks the book whether the person is in the call
// — but it is random anyway, because a guessable id is a way to knock on doors.
func callID() (string, error) {
	b := make([]byte, 12)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}

// How long a call may sit in each state before the book forgets it.
const (
	// ringFor is how long an unanswered call keeps ringing. It matches the life
	// the push is given (push.RingTTL) and the box on the screen (RingBox), so
	// that all three stop asking at the same moment.
	ringFor = 45 * time.Second
	// keepAnswered is a backstop and nothing more. A call ends when somebody
	// hangs up; this is for the pair of browsers that both vanished — a closed
	// laptop lid and a phone in a tunnel — and it only exists so the book
	// cannot grow for ever.
	keepAnswered = 12 * time.Hour
)

// Errors this file answers with. Busy is separate from not-found on purpose:
// "that person is on another call" is something the caller is entitled to know,
// because they are allowed to call that person at all.
var (
	errNoCall    = errors.New("api: no such call")
	errBusy      = errors.New("api: already on a call")
	errNotAnswer = errors.New("api: call has not been answered")
)

// directCall is one call in flight.
//
// toSess is empty until it is answered, and that is the point of it: a call
// rings on **every** device the person has open, because the phone in a pocket
// and the laptop on the desk are equally good places to answer. The moment one
// of them does, that device becomes the other end — and the rest are told to
// stop ringing.
type directCall struct {
	id         string
	from       int64
	fromSess   string
	to         int64
	toSess     string
	at         time.Time
	answered   bool
	suppressed bool
}

// party reports whether this person is in this call, and if so which end.
func (c *directCall) party(userID int64) bool { return c.from == userID || c.to == userID }

// otherEnd is who a message from userID should be delivered to, and on which
// device. It is only meaningful once the call has been answered.
func (c *directCall) otherEnd(userID int64) (int64, string) {
	if userID == c.from {
		return c.to, c.toSess
	}
	return c.from, c.fromSess
}

// callBook is every call in flight, by id.
//
// It sweeps itself on every operation rather than from a goroutine of its own.
// With twenty friends the map holds one or two entries, and a timer that exists
// only to delete from an empty map is a moving part with nothing to do.
type callBook struct {
	mu  sync.Mutex
	by  map[string]*directCall
	now func() time.Time
	// lost is the calls the sweep dropped because they rang out with nobody
	// answering, kept until somebody takes them. They are not dealt with here
	// because the line one of them leaves in a conversation is a database write,
	// and a database write under a mutex is how a small lock becomes a big one.
	lost []directCall
}

func newCallBook() *callBook {
	return &callBook{by: map[string]*directCall{}, now: time.Now}
}

// sweepLocked drops what has stopped being true. An unanswered call is over
// when it has rung for long enough; an answered one is kept until it is
// obviously stale, because the only honest end for it is somebody hanging up.
func (b *callBook) sweepLocked() {
	now := b.now()
	for id, c := range b.by {
		if !c.answered && now.Sub(c.at) > ringFor {
			// Somebody was called and never picked up. That is the one thing a
			// call leaves behind (store.PostNote), and it has to be noticed here
			// as well as in hangup: the caller's browser might have been closed
			// along with the tab, and then nobody is left to report it.
			b.lost = append(b.lost, *c)
			delete(b.by, id)
		}
		if c.answered && now.Sub(c.at) > keepAnswered {
			delete(b.by, id)
		}
	}
}

// start puts a new call in the book, unless either end is on one already.
//
// "Busy" is checked for both people and not only for the person being called.
// A device that lost its network mid-call leaves a call behind in this book,
// and the person sitting in front of it pressing "ligar" again would otherwise
// be told they cannot — so the check is what the sweep above is really for.
func (b *callBook) start(c *directCall) error {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.sweepLocked()
	for _, other := range b.by {
		reserves := func(user int64) bool { return other.from == user || (!other.suppressed && other.to == user) }
		if reserves(c.from) || (!c.suppressed && reserves(c.to)) {
			return errBusy
		}
	}
	c.at = b.now()
	b.by[c.id] = c
	return nil
}

// get returns the call, but only to somebody who is in it. Both failures are
// one failure to the caller above, which is what keeps a 404 from being a way
// to find out that a call is happening.
func (b *callBook) get(id string, userID int64) (directCall, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.sweepLocked()
	c, ok := b.by[id]
	if !ok || !c.party(userID) || (c.suppressed && c.from != userID) {
		return directCall{}, errNoCall
	}
	return *c, nil
}

// answer marks which device picked up. A second accept — the other phone, a
// second later — is not an error to anybody: it is answered by the call it
// asked about, unchanged, and the device learns it was not the one.
func (b *callBook) answer(id string, userID int64, sessID string) (directCall, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.sweepLocked()
	c, ok := b.by[id]
	if !ok || c.to != userID || c.suppressed {
		// Only the person being called can answer. The person who *made* the
		// call asking to accept it is told what a stranger is told.
		return directCall{}, errNoCall
	}
	if !c.answered {
		c.answered, c.toSess, c.at = true, sessID, b.now()
	}
	return *c, nil
}

// end takes the call out of the book and reports what it was.
func (b *callBook) end(id string, userID int64) (directCall, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.sweepLocked()
	c, ok := b.by[id]
	if !ok || !c.party(userID) || (c.suppressed && c.from != userID) {
		return directCall{}, errNoCall
	}
	delete(b.by, id)
	return *c, nil
}

// takeLost sweeps the book and hands back the calls that rang out, once each.
func (b *callBook) takeLost() []directCall {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.sweepLocked()
	out := b.lost
	b.lost = nil
	return out
}

// ---------------------------------------------------------------- ICE

// iceServers is what a browser is told to use to find a path to the other one.
//
// STUN is a browser asking "what does my address look like from outside?", and
// it is enough for most pairs of people. TURN is the fallback for the ones it is
// not enough for — two telephones on mobile networks, typically — where the
// only way through is a relay both sides can reach. That relay is the one the
// 1:1 app has used since its fourth sprint, on the machine that also keeps this
// service's backups.
//
// The credential is **ephemeral and minted per call**, in coturn's REST scheme:
// the username is an expiry and a name, and the password is an HMAC of it with
// a secret only the two servers know. A credential that leaks dies on its own,
// instead of turning the relay into an open proxy for whoever found it. The
// secret itself never leaves the machine and is never in this repository — it
// comes from the environment, like every other secret here.
func (s *Server) iceServers(who string) []map[string]any {
	out := []map[string]any{}
	if len(s.cfg.ICE.STUNURLs) > 0 {
		out = append(out, map[string]any{"urls": s.cfg.ICE.STUNURLs})
	}
	if len(s.cfg.ICE.TURNURLs) > 0 && len(s.cfg.ICE.TURNSecret) > 0 {
		user := strconv.FormatInt(time.Now().Add(turn.TTL).Unix(), 10) + ":" + who
		out = append(out, map[string]any{
			"urls":       s.cfg.ICE.TURNURLs,
			"username":   user,
			"credential": turn.Sign(s.cfg.ICE.TURNSecret, user),
		})
	}
	return out
}

// ---------------------------------------------------------------- handlers

// callError answers what the asker is entitled to know.
func (s *Server) callError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, errBusy):
		fail(w, http.StatusConflict, "busy")
	case errors.Is(err, errNotAnswer):
		fail(w, http.StatusConflict, "not_answered")
	default:
		// Everything else is "there is no such call, as far as you are
		// concerned" — an id that never existed and an id belonging to two
		// other people are one answer, byte for byte.
		fail(w, http.StatusNotFound, "not_found")
	}
}

func callJSON(c directCall, from, to userCard, ice []map[string]any, polite bool) map[string]any {
	return map[string]any{
		"id":          c.id,
		"from":        from.json(),
		"to":          to.json(),
		"ice_servers": ice,
		// Who gives way when both browsers speak at once. The server decides it
		// and the browser never guesses (PROTOCOL.md §5): the one who placed
		// the call is impolite, because they are the one who was there first.
		"polite": polite,
	}
}

// userCard is the little bit of a person that travels with a call.
type userCard struct {
	Username string
	Display  string
	Avatar   string
}

func (u userCard) json() map[string]any {
	return map[string]any{"username": u.Username, "display_name": u.Display, "avatar": u.Avatar}
}

// handleCallStart rings somebody.
//
// Who may call whom is the same rule as who may open a conversation with whom:
// anybody with an account here, because this is a server of twenty friends and
// there is no other membership to check. A name that does not exist answers
// exactly what a name that does answers when it cannot be called, which is how
// nobody uses this to find out who has an account.
func (s *Server) handleCallStart(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		Username string `json:"username"`
	}
	if !decode(w, r, &req) {
		return
	}
	target, _, err := s.st.UserByUsername(req.Username)
	if err != nil || target.Disabled || target.ID == cu.user.ID {
		// Calling yourself is not a thing, and it answers like a name that is
		// not a thing — the same choice handleOpenDM makes.
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	// The same cooldown the doorbell has, and for the same reason: what this
	// button does is make a box appear on somebody else's screen, which is the
	// rudest thing anything in this app can do.
	if !s.rings.take(cu.user.Username + ">" + target.Username + "@call") {
		fail(w, http.StatusTooManyRequests, "ring_limit")
		return
	}
	id, err := callID()
	if err != nil {
		s.log.Error("call id", "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	c := &directCall{id: id, from: cu.user.ID, fromSess: cu.sess.ID, to: target.ID}
	c.suppressed = s.blockedPair(c.from, c.to)
	if err := s.callBook.start(c); err != nil {
		s.callError(w, err)
		return
	}
	if c.suppressed {
		writeJSON(w, http.StatusCreated, callJSON(*c, userCard{cu.user.Username, cu.user.DisplayName, cu.user.Avatar}, userCard{target.Username, target.DisplayName, target.Avatar}, s.iceServers(cu.user.Username), false))
		return
	}
	me := userCard{cu.user.Username, cu.user.DisplayName, cu.user.Avatar}
	them := userCard{target.Username, target.DisplayName, target.Avatar}
	// Never a name in the log line beyond the ids, like everywhere else here.
	s.log.Info("call", "from", cu.user.ID, "to", target.ID, "call_id", id)

	// On every screen they have open…
	s.hub.publish([]int64{target.ID}, "call", map[string]any{
		"id":   id,
		"from": me.json(),
	})
	// …and in the pocket of whoever has none. This is the D4 doorbell, reused
	// whole: it is the one notification urgent enough to wake a phone whose
	// owner is merely away, because a call that waits to be noticed is not a
	// call.
	s.notifyCall(target, cu.user, id)

	writeJSON(w, http.StatusCreated,
		callJSON(*c, me, them, s.iceServers(cu.user.Username), false))
}

// handleCallGet is "am I still being called?".
//
// It exists for one moment and it is an important one: the app opened from the
// notification, with the telephone in somebody's hand, ten seconds after the
// ring. The live socket told every screen that was already open, and this
// screen was not one of them — it was not running yet. Without this, answering
// a call from the notification would open the app on a conversation with no
// call in it.
func (s *Server) handleCallGet(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	c, err := s.callBook.get(r.PathValue("id"), cu.user.ID)
	if err != nil {
		s.callError(w, err)
		return
	}
	from, to, ok := s.cards(c)
	if !ok {
		s.callError(w, errNoCall)
		return
	}
	out := callJSON(c, from, to, nil, c.to == cu.user.ID)
	// Whether somebody has already picked up, which is how a second device
	// knows it arrived too late.
	out["answered"] = c.answered
	writeJSON(w, http.StatusOK, out)
}

// handleCallAccept is somebody picking up.
//
// The device that accepts becomes the other end of the call, and the person's
// other devices are told to stop ringing. That is the only reason the accept
// carries a session at all: a call is between two browsers, not between two
// accounts, and the browser that answered is the one the audio has to go to.
func (s *Server) handleCallAccept(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	pending, checkErr := s.callBook.get(r.PathValue("id"), cu.user.ID)
	if checkErr != nil || s.blockedPair(pending.from, pending.to) {
		s.callError(w, errNoCall)
		return
	}
	c, err := s.callBook.answer(r.PathValue("id"), cu.user.ID, cu.sess.ID)
	if err != nil {
		s.callError(w, err)
		return
	}
	from, to, ok := s.cards(c)
	if !ok {
		s.callError(w, errNoCall)
		return
	}
	if c.toSess != cu.sess.ID {
		// Another of this person's devices got there first. Not an error, and
		// not a call for this one either.
		fail(w, http.StatusConflict, "taken")
		return
	}
	s.hub.publishSession(c.from, c.fromSess, "call_accepted", map[string]any{
		"id": c.id, "peer": to.json(),
	})
	// The other phones in the house stop ringing. It goes to every device of
	// this person; the one that answered ignores its own id.
	s.hub.publish([]int64{c.to}, "call_taken", map[string]any{"id": c.id, "by": cu.sess.ID})
	s.pending.clearKind(c.to, NotifyRing)
	writeJSON(w, http.StatusOK, callJSON(c, from, to, s.iceServers(cu.user.Username), true))
}

// handleCallHangup ends a call, from either end and at any point: refusing it,
// cancelling it before it is answered, and putting the telephone down half an
// hour later are the same thing to everybody else — the call is over.
func (s *Server) handleCallHangup(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		Reason string `json:"reason"`
	}
	// The body is optional: a beacon sent by a closing tab has none.
	_ = json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<12)).Decode(&req)
	reason := "hangup"
	if req.Reason == "rejected" || req.Reason == "missed" {
		reason = req.Reason
	}
	c, err := s.callBook.end(r.PathValue("id"), cu.user.ID)
	if err != nil {
		s.callError(w, err)
		return
	}
	// Both ends hear it, including every device that is still ringing.
	recipients := []int64{c.from, c.to}
	if c.suppressed {
		recipients = []int64{c.from}
	}
	s.hub.publish(recipients, "call_ended", map[string]any{
		"id": c.id, "reason": reason,
	})
	// And the line in the conversation, for a call that never got answered.
	s.noteCall(c, reason)
	// A call that was actually answered leaves no cooldown behind: see
	// ringLimiter.forget. Everything else keeps it — and "actually answered"
	// is the server's own record, not the word the caller sent. It used to be
	// the word: hanging up with no reason cleared the twenty seconds, so the
	// caller controlled the brake that exists against them, and ringing
	// somebody over and over was cancel, call, cancel, call.
	if c.toSess != "" {
		if from, to, ok := s.cards(c); ok {
			s.rings.forget(from.Username + ">" + to.Username + "@call")
		}
	}
	s.pending.clearKind(c.to, NotifyRing)
	writeJSON(w, http.StatusOK, map[string]any{"ended": true})
}

// CallSweep writes the line for calls that rang out with both browsers gone.
//
// Ten seconds because the only reader is a person who will look at the
// conversation later; being a few seconds late to say "you missed a call" is
// not something anybody can perceive.
func (s *Server) CallSweep(stop <-chan struct{}) {
	t := time.NewTicker(10 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-stop:
			return
		case <-t.C:
			for _, c := range s.callBook.takeLost() {
				s.noteCall(c, "missed")
			}
		}
	}
}

// noteCall leaves the one line a call is allowed to leave behind.
//
// **Only calls that were never answered.** A call that happened leaves nothing
// anywhere, which is the promise the D4b made and this keeps: no row, no
// backup, no record that two people spoke or for how long. What was missing was
// something else entirely — somebody whose telephone was face down, or out of
// signal, or shut, had no way of ever finding out that they had been called.
// That is not a record of a conversation; it is a message saying "I tried", and
// it belongs in the conversation like any other message.
//
// It is written as the caller, into the conversation the two of them already
// have — and if they have none yet, the one it opens. Nobody is pushed about
// it: the telephone already rang, and being told twice about the same call is
// worse than being told once.
func (s *Server) noteCall(c directCall, reason string) {
	if c.answered || c.suppressed || s.blockedPair(c.from, c.to) {
		return
	}
	if reason != "rejected" {
		reason = "missed"
	}
	ch, err := s.st.EnsureDM(c.from, c.to)
	if err != nil {
		s.log.Error("call note", "err", err)
		return
	}
	msg, err := s.st.PostNote(c.from, ch.ID, "call", reason)
	if err != nil {
		s.log.Error("call note", "err", err)
		return
	}
	s.notifyChannel(ch.ID, "message", s.messageJSON(msg))
}

// handleCallSignal carries one message from one browser to the other.
//
// **The payload is not read.** It arrives as bytes and leaves as the same
// bytes: an offer, an answer or an ICE candidate, which are the browsers'
// business and not this server's (PROTOCOL.md §6). Whatever a future client
// puts in one reaches the other client intact.
//
// It is refused before the call is answered, and that is not pedantry: until
// somebody picks up there is no second device to send it to — the call is
// ringing on all of them.
func (s *Server) handleCallSignal(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		Kind    string          `json:"kind"`
		Payload json.RawMessage `json:"payload"`
	}
	if !decode(w, r, &req) {
		return
	}
	switch req.Kind {
	case "offer", "answer", "ice":
	default:
		fail(w, http.StatusBadRequest, "bad_signal")
		return
	}
	c, err := s.callBook.get(r.PathValue("id"), cu.user.ID)
	if err != nil {
		s.callError(w, err)
		return
	}
	if !c.answered {
		s.callError(w, errNotAnswer)
		return
	}
	user, sess := c.otherEnd(cu.user.ID)
	s.hub.publishSession(user, sess, "call_signal", map[string]any{
		"id": c.id, "kind": req.Kind, "payload": req.Payload,
	})
	w.WriteHeader(http.StatusNoContent)
}

// handleCallState is "my microphone is off" and "my camera is on".
//
// It is its own endpoint rather than something inside the media, because the
// other side has to be able to *draw* it: a muted microphone that only showed
// up as silence is indistinguishable from somebody who stopped talking.
func (s *Server) handleCallState(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		Mic    bool `json:"mic"`
		Cam    bool `json:"cam"`
		Screen bool `json:"screen"`
	}
	if !decode(w, r, &req) {
		return
	}
	c, err := s.callBook.get(r.PathValue("id"), cu.user.ID)
	if err != nil {
		s.callError(w, err)
		return
	}
	if !c.answered {
		s.callError(w, errNotAnswer)
		return
	}
	user, sess := c.otherEnd(cu.user.ID)
	s.hub.publishSession(user, sess, "call_state", map[string]any{
		"id": c.id, "mic": req.Mic, "cam": req.Cam, "screen": req.Screen,
	})
	w.WriteHeader(http.StatusNoContent)
}

// cards reads the two people's names for an answer that has to carry them.
func (s *Server) cards(c directCall) (from, to userCard, ok bool) {
	a, err := s.st.UserByID(c.from)
	if err != nil {
		return from, to, false
	}
	b, err := s.st.UserByID(c.to)
	if err != nil {
		return from, to, false
	}
	return userCard{a.Username, a.DisplayName, a.Avatar}, userCard{b.Username, b.DisplayName, b.Avatar}, true
}
