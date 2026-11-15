package api

import (
	"encoding/json"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

// The gateway is the "without reloading the page" half of the chat, and
// nothing more than that. It carries no writes: everything a person does still
// goes through the JSON API, where requireUser and the store's membership
// joins already decide what may happen. A socket that only ever sends is a
// socket with no authorisation surface of its own.
//
// Fan-out is by user id, never "to everybody": the API asks the store who the
// members of a channel are and hands the gateway that list.

// Gateway timings. The ping is what keeps a phone's connection from being
// dropped by a mobile network that thinks an idle socket is a dead one.
const (
	writeWait  = 10 * time.Second
	pongWait   = 60 * time.Second
	pingPeriod = 25 * time.Second
	// sendQueue is how far behind one slow device may fall before it is cut
	// loose. It reconnects and refetches; the alternative is one bad phone
	// holding the server's memory.
	sendQueue = 64
)

type gatewayEvent struct {
	Type string `json:"type"`
	Data any    `json:"data"`
}

type gatewayConn struct {
	userID int64
	// username is carried so that a presence event can name the person without
	// a database round trip on every connect and disconnect.
	username string
	// sessID is the login session this socket belongs to, which is how a
	// message meant for one device — a voice renegotiation offer — reaches that
	// device and not every screen the same person has open.
	sessID string
	// what this device last said about itself: PresenceOnline (the tab is on
	// the screen), PresenceHidden (it is not) or PresenceAway (nobody is
	// there). Set by a POST, never by the socket: the socket carries no writes
	// (see the comment at the top of this file), and this is a write.
	//
	// since is when it last changed, and it exists for exactly one reason: a
	// hidden tab becomes away by itself after a while, and the server is the
	// only party still running to notice — a telephone freezes its page the
	// moment the screen goes off, so nothing on that side can report the
	// passage of five minutes. See presence.go.
	reported string
	since    time.Time
	// busy is this device saying it is in a call — a voice room or a direct
	// one. It keeps its owner online however long the window has been behind
	// something else, because being in a call is being in the app. It
	// deliberately does **not** make them look reachable to the notification
	// code: a telephone in a pocket with a call running should still buzz when
	// somebody writes.
	busy bool
	ws   *websocket.Conn
	send chan []byte
	once sync.Once
}

func (c *gatewayConn) close() {
	c.once.Do(func() { close(c.send) })
}

type hub struct {
	mu    sync.Mutex
	conns map[int64]map[*gatewayConn]struct{}
	// early is what a device said about itself before its socket was in the
	// hub. The handshake reaches the client before add() runs, so a phone that
	// went behind the screen during it can POST "hidden" to a hub that has no
	// socket to mark yet — and the socket would then arrive as "looking" and
	// swallow the next notification (found by the Codex audit, 26/09/2026).
	// Kept by login session, briefly, and applied by the socket that follows.
	early map[string]earlyReport
	// now is the clock, so a test can move five minutes without waiting them.
	now func() time.Time
}

type earlyReport struct {
	reported string
	busy     bool
	at       time.Time
}

// earlyReportTTL is how long a report may wait for its socket. The handshake
// it races is milliseconds; a report older than this belonged to a socket
// that never came.
const earlyReportTTL = 30 * time.Second

func newHub() *hub {
	return &hub{conns: map[int64]map[*gatewayConn]struct{}{}, early: map[string]earlyReport{}, now: time.Now}
}

func (h *hub) add(c *gatewayConn) {
	h.mu.Lock()
	defer h.mu.Unlock()
	now := h.now()
	for sess, e := range h.early {
		if now.Sub(e.at) >= earlyReportTTL {
			delete(h.early, sess)
		}
	}
	// Said after the client opened this socket, so newer than ?state was.
	if e, ok := h.early[c.sessID]; ok {
		delete(h.early, c.sessID)
		c.reported, c.since, c.busy = e.reported, now, e.busy
	}
	if h.conns[c.userID] == nil {
		h.conns[c.userID] = map[*gatewayConn]struct{}{}
	}
	h.conns[c.userID][c] = struct{}{}
}

func (h *hub) remove(c *gatewayConn) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if set, ok := h.conns[c.userID]; ok {
		delete(set, c)
		if len(set) == 0 {
			delete(h.conns, c.userID)
		}
	}
	c.close()
}

// dropSession closes every socket belonging to one login session, and
// dropAllExcept closes a person's other devices. They exist because revoking a
// device has to mean the device stops receiving, and a socket authenticated at
// the handshake keeps receiving forever otherwise: the cookie is refused at the
// door while the connection already inside goes on being handed every message.
//
// Closing the send channel is how a socket is ended from the outside: the
// writer notices, sends a close frame, and the reader's cleanup does the rest.
func (h *hub) dropSession(userID int64, sessID string) int {
	return h.drop(userID, func(c *gatewayConn) bool { return c.sessID == sessID })
}

func (h *hub) dropAllExcept(userID int64, keepSessID string) int {
	return h.drop(userID, func(c *gatewayConn) bool { return c.sessID != keepSessID })
}

func (h *hub) drop(userID int64, match func(*gatewayConn) bool) int {
	h.mu.Lock()
	defer h.mu.Unlock()
	n := 0
	for c := range h.conns[userID] {
		if !match(c) {
			continue
		}
		delete(h.conns[userID], c)
		c.close()
		n++
	}
	if len(h.conns[userID]) == 0 {
		delete(h.conns, userID)
	}
	return n
}

// publish sends one event to the devices of the given people, and only them.
func (h *hub) publish(userIDs []int64, kind string, data any) {
	payload, err := json.Marshal(gatewayEvent{Type: kind, Data: data})
	if err != nil {
		return
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, uid := range userIDs {
		for c := range h.conns[uid] {
			select {
			case c.send <- payload:
			default:
				// Full queue: this device is not keeping up. Drop it rather
				// than block every other reader behind it.
				delete(h.conns[uid], c)
				c.close()
			}
		}
	}
}

// publishAll sends one event to every open socket. Only for news that is
// the same for everybody and says nothing about anyone: a new app build.
func (h *hub) publishAll(kind string, data any) {
	h.mu.Lock()
	ids := make([]int64, 0, len(h.conns))
	for uid := range h.conns {
		ids = append(ids, uid)
	}
	h.mu.Unlock()
	h.publish(ids, kind, data)
}

// publishSession sends one event to a single device: the sockets belonging to
// one login session of one person. Voice signalling uses it, because an offer
// describes a media connection that only the device that asked for it can
// answer.
func (h *hub) publishSession(userID int64, sessID, kind string, data any) {
	payload, err := json.Marshal(gatewayEvent{Type: kind, Data: data})
	if err != nil {
		return
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	for c := range h.conns[userID] {
		if c.sessID != sessID {
			continue
		}
		select {
		case c.send <- payload:
		default:
			delete(h.conns[userID], c)
			c.close()
		}
	}
}

// ---------------------------------------------------------------- presence
//
// Presence asks the hub, and the hub already knows: it holds a socket per
// device. Nothing is stored anywhere, which is the honest shape of the
// question — after a restart nobody is online, and a table saying otherwise
// would be a lie that survives reboots.

// Presence states, as the client draws them. "in_call" is not here on purpose:
// who is inside a voice room is something the client already receives, for the
// rooms it belongs to, and deriving it there means the server never has to
// answer "is this person in a call" for a room the asker cannot see.
const (
	PresenceOnline = "online"
	PresenceAway   = "away"
	// PresenceHidden is a device reporting about itself, never a state anybody
	// is drawn in: a hidden tab still shows its owner as online until it has
	// been hidden long enough (presence.go).
	PresenceHidden  = "hidden"
	PresenceOffline = "offline"
)

// state is the person's state across every device they have open. One device
// that counts as here is enough to be online; away means every open device
// stopped counting.
func (h *hub) state(userID int64) string {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.stateLocked(userID)
}

func (h *hub) stateLocked(userID int64) string {
	set := h.conns[userID]
	if len(set) == 0 {
		return PresenceOffline
	}
	now := h.now()
	for c := range set {
		if c.here(now) {
			return PresenceOnline
		}
	}
	return PresenceAway
}

// here is whether this one device should still count its owner as around. A
// tab on the screen does. A tab that was put behind something else does too,
// for a while — flicking to another window for a moment is not leaving, and
// calling it leaving is what made the owner ask for this. After awayAfterHidden
// it stops counting.
func (c *gatewayConn) here(now time.Time) bool {
	if c.busy {
		return true
	}
	switch c.reported {
	case PresenceAway:
		return false
	case PresenceHidden:
		return now.Sub(c.since) < awayAfterHidden
	default:
		return true
	}
}

// visible is whether any of this person's devices has the app actually on the
// screen. It is a different question from state() and it is asked by exactly
// one caller: whether a notification is worth pushing. Somebody who alt-tabbed
// two minutes ago still shows as online — that is the point — but they are not
// looking at this app, so the push is worth sending.
func (h *hub) visible(userID int64) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	for c := range h.conns[userID] {
		if c.reported != PresenceHidden && c.reported != PresenceAway {
			return true
		}
	}
	return false
}

// states is everybody with the app open, by user id. Whoever is missing is
// offline; saying so for twenty accounts would be a longer answer with no more
// in it.
func (h *hub) states() map[int64]string {
	h.mu.Lock()
	defer h.mu.Unlock()
	out := make(map[int64]string, len(h.conns))
	for uid := range h.conns {
		out[uid] = h.stateLocked(uid)
	}
	return out
}

// setReported marks one login session's sockets with what that device just
// said about itself. It reports the person's state afterwards, so the caller
// can decide whether anything is worth telling.
func (h *hub) setReported(userID int64, sessID string, reported string, busy bool) string {
	h.mu.Lock()
	defer h.mu.Unlock()
	now := h.now()
	matched := false
	for c := range h.conns[userID] {
		if c.sessID != sessID {
			continue
		}
		matched = true
		// The clock restarts only when the answer about the window changed:
		// a call ending must not hand a tab that has been hidden for an hour
		// another five minutes.
		if c.reported != reported {
			c.reported = reported
			c.since = now
		}
		c.busy = busy
	}
	if !matched {
		h.early[sessID] = earlyReport{reported: reported, busy: busy, at: now}
	}
	return h.stateLocked(userID)
}

// watchers is everybody with the app open — the only people a presence change
// can possibly matter to. Presence is therefore the one fan-out in this file
// that does not ask the store who to send to, because "whoever is listening" is
// exactly the right list and costs nothing.
func (h *hub) watchers() []int64 {
	h.mu.Lock()
	defer h.mu.Unlock()
	out := make([]int64, 0, len(h.conns))
	for uid := range h.conns {
		out = append(out, uid)
	}
	return out
}

// people is everybody with the app open, by id and name. It exists so the
// presence sweep can name somebody without asking the database for something
// the socket has been carrying all along.
func (h *hub) people() map[int64]string {
	h.mu.Lock()
	defer h.mu.Unlock()
	out := make(map[int64]string, len(h.conns))
	for uid, set := range h.conns {
		for c := range set {
			out[uid] = c.username
			break
		}
	}
	return out
}

// connections reports how many sockets are open, for the tests and for a log
// line. It counts devices, not people.
func (h *hub) connections() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	n := 0
	for _, set := range h.conns {
		n += len(set)
	}
	return n
}

// handleGateway upgrades a signed-in request to a WebSocket.
//
// The Origin check is not optional here and is not the same rule as the cookie
// one: SameSite=Lax does not stop a WebSocket handshake from another site, so
// without this any page on the internet could open a socket with the visitor's
// cookie and read their conversations.
func (s *Server) handleGateway(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	up := websocket.Upgrader{
		ReadBufferSize:  1024,
		WriteBufferSize: 4096,
		CheckOrigin: func(r *http.Request) bool {
			origin := r.Header.Get("Origin")
			if s.cfg.BaseURL == "" {
				return true // development, no fixed origin to compare against
			}
			return strings.EqualFold(strings.TrimSuffix(origin, "/"), strings.TrimSuffix(s.cfg.BaseURL, "/"))
		},
	}
	ws, err := up.Upgrade(w, r, nil)
	if err != nil {
		return // Upgrade has already written the error
	}
	c := &gatewayConn{
		userID:   cu.user.ID,
		username: cu.user.Username,
		sessID:   cu.sess.ID,
		ws:       ws,
		send:     make(chan []byte, sendQueue),
	}
	// The Android app keeps its socket open behind the screen, and it can be
	// started with no screen at all — a notification waking it in a pocket.
	// Arriving as "online" there, even for the moment before it could POST
	// otherwise, would make the notification code think the person is looking
	// and swallow the next message. So it may say where it is as it connects.
	// The web never sends this and arrives exactly as before.
	if r.URL.Query().Get("state") == PresenceHidden {
		c.reported, c.since = PresenceHidden, s.hub.now()
	}
	s.hub.add(c)
	// A revocation may land between requireUser and registration in the hub.
	// Check after registration so either this check or the revoker cuts it off.
	if alive, err := s.st.SessionAlive(c.sessID); err != nil || !alive {
		s.hub.dropSession(c.userID, c.sessID)
	}
	// A socket opening is somebody arriving, which is half of what presence is.
	s.presenceChanged(c.userID, c.username)
	go s.gatewayWrite(c)
	s.gatewayRead(c)
}

// gatewayRead exists to notice that the socket died and to answer pings. The
// client sends nothing that changes anything, so anything that arrives is
// discarded — deliberately, and cheaply: a read limit keeps a client from
// pushing megabytes at a server that will not look at them.
func (s *Server) gatewayRead(c *gatewayConn) {
	defer func() {
		s.hub.remove(c)
		c.ws.Close()
		// The other half: the last socket of a person closing is that person
		// going offline, and everybody still listening is told.
		s.presenceChanged(c.userID, c.username)
	}()
	c.ws.SetReadLimit(1024)
	_ = c.ws.SetReadDeadline(time.Now().Add(pongWait))
	c.ws.SetPongHandler(func(string) error {
		return c.ws.SetReadDeadline(time.Now().Add(pongWait))
	})
	for {
		if _, _, err := c.ws.ReadMessage(); err != nil {
			return
		}
	}
}

func (s *Server) gatewayWrite(c *gatewayConn) {
	ticker := time.NewTicker(pingPeriod)
	defer func() {
		ticker.Stop()
		c.ws.Close()
	}()
	for {
		select {
		case payload, ok := <-c.send:
			_ = c.ws.SetWriteDeadline(time.Now().Add(writeWait))
			if !ok {
				_ = c.ws.WriteMessage(websocket.CloseMessage, nil)
				return
			}
			if err := c.ws.WriteMessage(websocket.TextMessage, payload); err != nil {
				return
			}
		case <-ticker.C:
			// The ping is also when this socket's login is checked again.
			// Dropping the sockets at the moment of a revocation covers the
			// revocations this process performs; it cannot cover a session
			// that simply expired, nor a password reset run as a command on
			// the server, which writes to the database from another process
			// entirely. One cheap query per socket per ping covers both, and
			// twenty friends make that nothing.
			if alive, err := s.st.SessionAlive(c.sessID); err == nil && !alive {
				s.log.Info("socket closed: its session is gone", "user_id", c.userID)
				// The revoked device's microphone goes with its socket: a
				// voice call outlives a dropped socket on purpose, not a
				// revoked sign-in.
				s.voiceHangUp(c.sessID)
				return
			}
			_ = c.ws.SetWriteDeadline(time.Now().Add(writeWait))
			if err := c.ws.WriteMessage(websocket.PingMessage, nil); err != nil {
				return
			}
		}
	}
}

// notifyChannel sends an event to everybody who belongs to a channel. It reads
// the membership table for the list, so a direct message reaches two people and
// nobody else, by the same rule that decides who may read it.
func (s *Server) notifyChannel(channelID, kind string, data any) {
	ids, err := s.st.ChannelMemberIDs(channelID)
	if err != nil {
		s.log.Error("fan-out", "err", err)
		return
	}
	s.hub.publish(ids, kind, data)
}
