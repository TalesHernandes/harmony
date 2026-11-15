// Notifications with the app closed: who gets woken up, and what they are told.
//
// The shape of this file is one decision, and the rest follows from it: **the
// Web Push carries nothing**. It is an empty POST that means "harmony has something
// for you". The service worker it wakes then asks this server what to show,
// over the session cookie it already has, and gets the sentence back from
// /api/notifications/pending.
//
// Three things that buys, none of which is cleverness for its own sake:
//
//  1. The text never passes through a push service. Google (Chrome) and Mozilla
//     (Firefox) relay every push on the internet; an encrypted payload would be
//     opaque to them, but the simplest way to keep somebody's name and somebody's
//     channel off a third party's wire is not to put it there.
//  2. No RFC 8291. Encrypting a payload is ECDH plus HKDF plus AES-GCM and a
//     record layer, and it is the half of Web Push that goes wrong silently.
//     What is left here is the half that is just a signed JWT and a POST.
//  3. What the notification says is decided when it is *read*, not when it was
//     sent. A mention already read on the computer is dropped before the phone
//     ever shows it (see handlePendingNotifications), which no payload sitting
//     in a push queue could do.
//
// The price, said plainly: the phone has to reach this server to learn what the
// notification says. It was going to have to anyway to be useful — tapping it
// opens the app — and a phone with no network gets the generic "algo novo no
// harmony" that the service worker falls back to.
//
// **What the lock screen shows, and why.** Who and where: "@tales te mencionou
// em #geral", "@tales te mandou uma mensagem", "@tales está te chamando em
// 🔊 sala". Never the text of the message. Two reasons, and the first is the
// one that decides it: a lock screen is read by whoever is holding the phone,
// including the person sitting next to its owner, and a private message that
// announces itself in full on a café table is not private. The second is that
// the same rule already governs this server's logs (CLAUDE.md), and having one
// rule is worth more than the few seconds a preview would save.
//
// Android uses a second, data-only FCM path: identifiers and time only.
// It fetches any display text here using its own authenticated session.
package api

import (
	"context"
	"errors"
	"sort"
	"strconv"
	"sync"
	"time"

	"call/internal/harmony/push"
	"call/internal/harmony/store"
)

// The kinds of thing that can wake a phone. They double as the push Topic, so
// a second mention replaces a first one still queued at the push service
// instead of the two stacking up on the phone — and a ring never buries a
// mention, because the topics differ.
const (
	NotifyMention = "mention"
	NotifyDM      = "dm"
	NotifyRing    = "ring"
)

// How long a pending notification is worth showing. A ring dies with the call
// it was announcing: nobody wants to be told at 23:40 that somebody called at
// 23:05. A message does not — reading it an hour later is what a mention is
// for — and twelve hours matches the TTL the push itself is sent with, so the
// two expire together.
const (
	pendingRingTTL    = 60 * time.Second
	pendingMessageTTL = 12 * time.Hour
	// maxPendingPerUser bounds one person's list. Twenty is far more than a
	// phone will ever draw; the cap is here so that a runaway loop is a bounded
	// runaway loop.
	maxPendingPerUser = 20
)

// pushCooldown throttles per person and per kind. Five mentions in a row is one
// buzz, which is the behaviour anybody would ask for, and a client stuck in a
// retry loop cannot turn into a burst at the push service.
var pushCooldown = map[string]time.Duration{
	NotifyMention: 15 * time.Second,
	NotifyDM:      15 * time.Second,
	NotifyRing:    10 * time.Second,
}

// notification is one thing to show. It never holds what anybody wrote.
type notification struct {
	AuthorID  int64  `json:"-"`
	MessageID int64  `json:"message_id,omitempty"`
	CallID    string `json:"-"`
	// Tag is the notification's identity on the phone: a second notification
	// with the same tag replaces the first instead of stacking. One per kind
	// per channel, so a conversation is one line that keeps updating.
	Tag       string `json:"tag"`
	Kind      string `json:"kind"`
	Title     string `json:"title"`
	Body      string `json:"body"`
	URL       string `json:"url"`
	ChannelID string `json:"-"`
	At        int64  `json:"at"`
	expires   time.Time
}

// pendingStore is what the woken service worker comes back to read. In memory,
// not in the database: a table of "things we were about to tell you" is a table
// of who talked to whom, sitting in every backup, for something that is over in
// seconds. A deploy empties it, and the cost of that is one generic
// notification instead of a specific one.
type pendingStore struct {
	mu  sync.Mutex
	now func() time.Time
	by  map[int64][]notification
}

func newPendingStore() *pendingStore {
	return &pendingStore{now: time.Now, by: map[int64][]notification{}}
}

func (p *pendingStore) add(userID int64, n notification) {
	p.mu.Lock()
	defer p.mu.Unlock()
	list := p.live(p.by[userID])
	for i, old := range list {
		if old.Tag == n.Tag {
			list = append(list[:i], list[i+1:]...)
			break
		}
	}
	list = append(list, n)
	if len(list) > maxPendingPerUser {
		list = list[len(list)-maxPendingPerUser:]
	}
	p.by[userID] = list
}

// take returns what is still worth showing, newest last. It does not consume:
// a person with a phone and a tablet has two service workers that both wake up,
// and the second one arriving to an empty list would be the one that says
// nothing.
func (p *pendingStore) take(userID int64) []notification {
	p.mu.Lock()
	defer p.mu.Unlock()
	list := p.live(p.by[userID])
	if len(list) == 0 {
		delete(p.by, userID)
		return nil
	}
	p.by[userID] = list
	out := make([]notification, len(list))
	copy(out, list)
	sort.SliceStable(out, func(i, j int) bool { return out[i].At < out[j].At })
	return out
}

// clear forgets one channel's notifications for one person. Called when they
// read it somewhere else, so the phone in the pocket stops being told about
// something already dealt with.
func (p *pendingStore) clear(userID int64, channelID string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	list := p.live(p.by[userID])
	kept := list[:0]
	for _, n := range list {
		if n.ChannelID != channelID {
			kept = append(kept, n)
		}
	}
	if len(kept) == 0 {
		delete(p.by, userID)
		return
	}
	p.by[userID] = kept
}

// clearKind forgets every notification of one kind for one person. The
// doorbell uses it: once a call has been answered or is over, a phone that
// wakes up later must not still be told somebody is calling.
func (p *pendingStore) clearKind(userID int64, kind string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	list := p.live(p.by[userID])
	kept := list[:0]
	for _, n := range list {
		if n.Kind != kind {
			kept = append(kept, n)
		}
	}
	if len(kept) == 0 {
		delete(p.by, userID)
		return
	}
	p.by[userID] = kept
}

func (p *pendingStore) live(list []notification) []notification {
	now := p.now()
	kept := make([]notification, 0, len(list))
	for _, n := range list {
		if n.expires.After(now) {
			kept = append(kept, n)
		}
	}
	return kept
}

// notifier turns "something happened to this person" into a push, when there is
// a reason to. It is deliberately split from the sending: deciding is
// synchronous and cheap, and the HTTP to a third party happens on a goroutine
// where it cannot hold up a message being posted.
type notifier struct {
	srv    *Server
	sender *push.Sender
	fcm    *push.FCM

	mu   sync.Mutex
	last map[string]time.Time // "userID:kind" -> when a push last went out
	now  func() time.Time

	// sendTimeout is how long one device gets. It matches the sender's own
	// client timeout; tests shorten it.
	sendTimeout time.Duration
}

func newNotifier(s *Server, keys *push.Keys, fcm *push.FCM) *notifier {
	var sender *push.Sender
	if keys != nil {
		sender = push.NewSender(keys)
	}
	return &notifier{srv: s, sender: sender, fcm: fcm, last: map[string]time.Time{}, now: time.Now,
		sendTimeout: 10 * time.Second}
}

// take reports whether a push of this kind may go out to this person now.
// A direct call is throttled per call, not per person: the cooldown is there to
// fold repeats of one ring into one buzz, and it used to swallow the push of a
// different call placed within ten seconds of one that had already ended.
func (n *notifier) take(userID int64, kind, callID string) bool {
	cooldown, ok := pushCooldown[kind]
	if !ok {
		cooldown = 15 * time.Second
	}
	key := kind + ":" + strconv.FormatInt(userID, 10)
	if callID != "" {
		key += ":" + callID
	}
	n.mu.Lock()
	defer n.mu.Unlock()
	now := n.now()
	if at, seen := n.last[key]; seen && now.Sub(at) < cooldown {
		return false
	}
	n.last[key] = now
	// Per-call keys would otherwise pile up forever. Every cooldown is well
	// under a minute, so anything older is dead weight.
	if len(n.last) > 256 {
		for k, at := range n.last {
			if now.Sub(at) > time.Minute {
				delete(n.last, k)
			}
		}
	}
	return true
}

// wake pushes to every live device of one person and forgets the dead ones.
func (n *notifier) wake(userID int64, notice notification) {
	var subs []store.PushSubscription
	var tokens []string
	var err error
	if n.sender != nil {
		subs, err = n.srv.st.LivePushSubscriptions(userID)
		if err != nil {
			n.srv.log.Error("push subscriptions", "err", err)
		}
	}
	if n.fcm != nil {
		tokens, err = n.srv.st.LiveFCMTokens(userID)
		if err != nil {
			n.srv.log.Error("fcm subscriptions", "err", err)
		}
	}
	if len(subs)+len(tokens) == 0 {
		return
	}
	kind := notice.Kind
	if !n.take(userID, kind, notice.CallID) {
		n.srv.log.Info("push suppressed by cooldown", "user_id", userID, "kind", kind)
		return
	}
	msg := push.Message{TTL: push.MessageTTL, Urgency: "normal", Topic: kind}
	if kind == NotifyRing {
		// Preserve Web Push's existing urgency and lifetime.
		msg.TTL, msg.Urgency = push.RingTTL, "high"
	}
	if len(subs) > 0 {
		go n.deliver(subs, msg, kind)
	}
	data := map[string]string{"k": kind, "at": strconv.FormatInt(notice.At, 10)}
	if notice.CallID != "" {
		data["k"] = "call"
		data["call"] = notice.CallID
	} else {
		data["c"] = notice.ChannelID
	}
	if kind == NotifyDM || kind == NotifyMention {
		data["m"] = strconv.FormatInt(notice.MessageID, 10)
	}
	ttl := push.MessageTTL
	if kind == NotifyRing {
		// Android's contract is 60 seconds; Web Push RingTTL is 45 seconds.
		ttl = 60 * time.Second
	}
	for _, token := range tokens {
		go func(token string) {
			ctx, cancel := context.WithTimeout(context.Background(), n.sendTimeout)
			defer cancel()
			err := n.fcm.Send(ctx, push.FCMMessage{Token: token, Data: data, TTL: ttl, CollapseKey: data["k"]})
			switch {
			case err == nil:
				n.srv.log.Info("fcm push sent", "kind", data["k"])
			case errors.Is(err, push.ErrGone):
				n.srv.log.Info("fcm token gone, dropping", "kind", data["k"])
				if err := n.srv.st.DropFCMToken(token); err != nil {
					n.srv.log.Error("fcm store", "err", err)
				}
			default:
				n.srv.log.Error("fcm push failed", "err", err, "kind", data["k"])
			}
		}(token)
	}
}

func (n *notifier) deliver(subs []store.PushSubscription, msg push.Message, kind string) {
	// One deadline per device, and all of them at once. A single deadline for
	// the whole list, sent in order, let the first two devices spend it: two
	// push services that accept the connection and never answer used ten
	// seconds and five, and the third device — often the new phone, since the
	// list is oldest first — got a context that was already over. Sent side by
	// side, the whole delivery still ends within one sendTimeout.
	var wg sync.WaitGroup
	for _, sub := range subs {
		wg.Add(1)
		go func(sub store.PushSubscription) {
			defer wg.Done()
			ctx, cancel := context.WithTimeout(context.Background(), n.sendTimeout)
			defer cancel()
			m := msg
			m.Endpoint = sub.Endpoint
			err := n.sender.Send(ctx, m)
			switch {
			case err == nil:
				// No endpoint in the line: it is a capability URL, and it is a
				// secret like any other (CLAUDE.md).
				n.srv.log.Info("push sent", "kind", kind)
			case errors.Is(err, push.ErrGone):
				n.srv.log.Info("push subscription gone, dropping", "kind", kind)
				if err := n.srv.st.DropPushSubscription(sub.Endpoint); err != nil {
					n.srv.log.Error("push store", "err", err)
				}
			default:
				n.srv.log.Error("push failed", "err", err, "kind", kind)
			}
		}(sub)
	}
	wg.Wait()
}

// ---------------------------------------------------------------- the rules

// notifyMessage decides who, if anybody, should be woken by a message that was
// just posted. The two cases are the two the owner asked for: a mention in a
// channel, and a direct message.
//
// Nobody who has the app in front of them is pushed. They already see the badge
// and the channel go bold, and a phone buzzing about a message on the screen in
// front of you is the single most annoying thing this feature can do.
func (s *Server) notifyMessage(msg store.Message, channel store.Channel) {
	if s.notify == nil {
		return // no push provider configured: the doorbell is off, everything else works
	}
	author := msg.AuthorLabel
	if author == "" {
		author = msg.AuthorName
	}

	switch {
	case channel.Kind == store.ChannelDM:
		ids, err := s.st.ChannelMemberIDs(msg.ChannelID)
		if err != nil {
			s.log.Error("notify dm", "err", err)
			return
		}
		for _, id := range ids {
			if id == msg.AuthorID || s.silenced(id, msg.ChannelID) {
				continue
			}
			s.raise(id, notification{
				AuthorID:  msg.AuthorID,
				Tag:       NotifyDM + ":" + msg.ChannelID,
				Kind:      NotifyDM,
				Title:     author,
				Body:      "te mandou uma mensagem",
				URL:       "/?c=" + msg.ChannelID,
				ChannelID: msg.ChannelID,
				MessageID: msg.ID,
			})
		}

	case len(msg.Mentions) > 0:
		for _, username := range msg.Mentions {
			user, _, err := s.st.UserByUsername(username)
			if err != nil {
				continue
			}
			if user.ID == msg.AuthorID || user.Disabled {
				continue
			}
			// The membership check is the authorisation, and it is the same one
			// that decides whether they may read the channel at all: a channel
			// they are not in answers ErrNotFound here exactly as it would
			// anywhere else, so a mention cannot be used to tell somebody that
			// a channel exists.
			if _, err := s.st.ChannelFor(user.ID, msg.ChannelID); err != nil {
				continue
			}
			// And a channel they have switched off says nothing, even about
			// them. That is what silencing one means; somebody who wants to be
			// reachable in a channel they silenced is reachable in a private
			// conversation, which is a different channel with its own switch.
			if s.silenced(user.ID, msg.ChannelID) {
				continue
			}
			s.raise(user.ID, notification{
				AuthorID:  msg.AuthorID,
				Tag:       NotifyMention + ":" + msg.ChannelID,
				Kind:      NotifyMention,
				Title:     author,
				Body:      "te mencionou em #" + channel.Name,
				URL:       "/?c=" + msg.ChannelID,
				ChannelID: msg.ChannelID,
				MessageID: msg.ID,
			})
		}
	}
}

// notifyRing is the doorbell: somebody in a voice room asking for a person who
// is not in it. It is the one notification that is urgent, and the only one
// that is worth a push to somebody whose app is merely *away* rather than
// closed — a phone in a pocket is away, and a call that waits for the app to be
// opened is not a call.
func (s *Server) notifyRing(target store.User, caller store.User, channel store.Channel) {
	if s.notify == nil {
		return
	}
	name := caller.DisplayName
	if name == "" {
		name = caller.Username
	}
	s.raise(target.ID, notification{
		AuthorID:  caller.ID,
		Tag:       NotifyRing + ":" + channel.ID,
		Kind:      NotifyRing,
		Title:     name + " está te chamando",
		Body:      "em 🔊 " + channel.Name,
		URL:       "/?join=" + channel.ID,
		ChannelID: channel.ID,
	})
}

// notifyCall is the doorbell for a direct call — the same machinery as
// notifyRing, pointed at a person instead of at a room. It is deliberately the
// same kind (NotifyRing), because it is the same urgency and should replace the
// same line on the phone rather than stack a second one under it.
//
// The URL opens the app *on the call*: with the app closed, tapping the
// notification is how somebody answers.
func (s *Server) notifyCall(target store.User, caller store.User, callID string) {
	if s.notify == nil {
		return
	}
	name := caller.DisplayName
	if name == "" {
		name = caller.Username
	}
	s.raise(target.ID, notification{
		AuthorID: caller.ID,
		Tag:      NotifyRing + ":call",
		Kind:     NotifyRing,
		Title:    name + " está te ligando",
		Body:     "chamada direta",
		URL:      "/?call=" + callID,
		CallID:   callID,
	})
}

// raise records what to show and, when the person is not looking at the app,
// wakes their devices.
// silenced is the one question a notification has to ask that is not about the
// message: has this person switched this channel off? It is asked here rather
// than inside raise() because raise also carries the doorbell, and a doorbell
// is a person calling you, not a channel talking.
func (s *Server) silenced(userID int64, channelID string) bool {
	muted, err := s.st.Muted(userID, channelID)
	if err != nil {
		// A database that will not answer must not turn into silence: a
		// notification too many is a nuisance, a notification missing is the
		// feature not working.
		s.log.Error("mute check", "err", err)
		return false
	}
	return muted
}

func (s *Server) raise(userID int64, n notification) {
	if n.AuthorID != 0 && s.blocks(userID, n.AuthorID) {
		return
	}
	n.At = time.Now().UnixMilli()
	n.expires = time.Now().Add(pendingMessageTTL)
	if n.Kind == NotifyRing {
		n.expires = time.Now().Add(pendingRingTTL)
	}
	s.pending.add(userID, n)
	// The question is not "are they online", it is "is this app on a screen in
	// front of them right now". Those came apart on 13/09/2026, when a hidden
	// tab stopped meaning away: somebody who switched to another window two
	// minutes ago still shows online to their friends, and still wants to be
	// told that a message arrived. A pocket, a closed app and another window
	// are all the same thing from here.
	if s.hub.visible(userID) {
		s.log.Info("push skipped, person is here", "user_id", userID, "kind", n.Kind)
		return
	}
	s.notify.wake(userID, n)
}
