// Chat: channels, history, direct messages, mentions and read state.
//
// Every handler in this file follows the same shape, and the shape is the
// security rule: it takes the signed-in person's id from requireUser and hands
// it to a store method that joins channel_members. There is no "is this channel
// public" test anywhere above the database, so there is nowhere to forget one.
// An id from a conversation somebody is not part of comes back as not_found —
// the same answer an invented id gets, so the 404 tells a stranger nothing.
package api

import (
	"errors"
	"io/fs"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"

	"call/internal/harmony/store"
)

// Per-person limits, PLANO-DISCORD.md §5: "so that a bug or a compromised
// account cannot take the rest down". Twenty a minute is far above how fast
// anybody types and far below what a loop does.
const (
	MessagesPerMinute = 20
	messageWindow     = time.Minute
	// HistoryPageSize is what the client gets per scroll-back.
	HistoryPageSize = 50
)

// ---------------------------------------------------------------- JSON

func (s *Server) channelJSON(c store.Channel) map[string]any {
	out := map[string]any{
		"id":              c.ID,
		"kind":            c.Kind,
		"name":            c.Name,
		"topic":           c.Topic,
		"muted":           c.Muted,
		"unread":          c.Unread,
		"mentions":        c.Mentions,
		"last_message_id": c.LastMessageID,
		"last_read_id":    c.LastReadID,
	}
	if c.Kind == store.ChannelDM {
		out["peer"] = s.personJSON(c.PeerName, c.PeerDisplayName)
	} else {
		// Where the room sits (D12a). An app that draws no folders ignores
		// both and still gets the rooms in the owner's order, because that is
		// the order the list comes in.
		out["category_id"] = c.CategoryID
		out["position"] = c.Position
	}
	return out
}

func (s *Server) attachmentJSON(a store.Attachment) map[string]any {
	out := map[string]any{
		"id":     a.ID,
		"name":   a.Name,
		"mime":   a.MIME,
		"size":   a.Size,
		"width":  a.Width,
		"height": a.Height,
		// The only address the picture has. It is not a path into a folder;
		// it is an id that the server checks membership on.
		"url": "/api/files/" + a.ID,
	}
	// A row whose bytes are gone says so, instead of handing out an address
	// that answers 404. It happened for real: the move off the Contabo on
	// 18/09/2026 carried the database and not the upload folder, and nine
	// pictures and files from before it became a bare file name in the
	// conversation. The row stays — the message still had something attached,
	// and saying what is more honest than pretending it never did.
	if s.cfg.FilesDir != "" {
		if path, err := s.filePath(a.ID); err == nil {
			if _, err := os.Stat(path); errors.Is(err, fs.ErrNotExist) {
				out["missing"] = true
			}
		}
	}
	return out
}

func (s *Server) messageJSON(m store.Message, pageCache ...map[string]store.Preview) map[string]any {
	var cache map[string]store.Preview
	if len(pageCache) > 0 {
		cache = pageCache[0]
	} else {
		cache = s.cachedPreviews([]store.Message{m})
	}
	files := make([]map[string]any, 0, len(m.Attachments))
	for _, a := range m.Attachments {
		files = append(files, s.attachmentJSON(a))
	}
	mentions := m.Mentions
	if mentions == nil {
		mentions = []string{}
	}
	// Who pressed what, in the order each one was first pressed. The names go
	// out, not a count and a "yours": the same message is sent to the whole
	// room at once, so anything meaning "yours" would have to be a different
	// answer per reader — and "quem foi que riu disso" is most of why anybody
	// wants this at all.
	reactions := make([]map[string]any, 0, len(m.Reactions))
	for _, r := range m.Reactions {
		reactions = append(reactions, map[string]any{"emoji": r.Emoji, "users": r.Users})
	}
	out := map[string]any{
		"id":          m.ID,
		"channel_id":  m.ChannelID,
		"author":      s.personJSON(m.AuthorName, m.AuthorLabel),
		"body":        m.Body,
		"created_at":  m.CreatedAt.Format(time.RFC3339),
		"attachments": files,
		"mentions":    mentions,
		"reactions":   reactions,
		"deleted":     m.Deleted(),
	}
	// Only when there is one, so that every message a person wrote goes over the
	// wire exactly as it did before the D4i.
	if m.Kind != "" {
		out["kind"] = m.Kind
	}
	// Only when it is, like kind: an unpinned message goes over the wire as
	// it did before the D12a.
	if m.Pinned {
		out["pinned"] = true
	}
	if !m.EditedAt.IsZero() {
		out["edited_at"] = m.EditedAt.Format(time.RFC3339)
	}
	if m.Deleted() {
		out["deleted_at"] = m.DeletedAt.Format(time.RFC3339)
		out["deleted_by"] = m.DeletedBy
	}
	// The line being answered, as it is *now*. Sent alongside rather than left
	// for the page to look up: the message being answered is very often older
	// than the page is holding, and asking for it would be one request per
	// reply on every scroll-back.
	if m.ReplyTo > 0 && m.ReplyToAuthor != "" {
		out["reply_to"] = map[string]any{
			"id":      m.ReplyTo,
			"author":  s.personJSON(m.ReplyToAuthor, m.ReplyToLabel),
			"body":    m.ReplyToBody,
			"deleted": m.ReplyToDeleted,
		}
	}
	if embeds := previewEmbeds(m, cache); len(embeds) > 0 {
		out["embeds"] = embeds
	}
	return out
}

// chatError turns a store error into the answer the asker is entitled to.
// ErrNotFound covers both "no such thing" and "not yours", on purpose.
// handleNickname is the name *you* call somebody. Nobody else is ever shown it,
// and that is not a rule the interface keeps — it is the shape of the table
// (store migration 7), where every row carries whose opinion it is.
//
// An empty name clears it and answers the same 200, because "no nickname" is a
// perfectly good answer to "what do you call them".
func (s *Server) handleNickname(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		Name string `json:"name"`
	}
	if !decode(w, r, &req) {
		return
	}
	if err := s.st.SetNickname(cu.user.ID, r.PathValue("username"), req.Name); err != nil {
		if errors.Is(err, store.ErrBadNickname) {
			fail(w, http.StatusBadRequest, "bad_nickname")
			return
		}
		// A name that does not exist answers the same as anything else that
		// does not exist, which is how nobody finds out who has an account.
		s.chatError(w, err, "nickname")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"name": strings.TrimSpace(req.Name)})
}

// handleTyping says that this person is writing something in this channel.
//
// **Nothing is stored, and nothing can be.** It is a fact that is true for four
// seconds, like presence, and a table of "who was typing" would be a table of
// who talks to whom sitting in every backup for a feature that is over before
// anybody reads it. It goes straight out to the other members and is forgotten.
//
// It is a POST rather than something sent up the socket because the socket has
// carried no writes since D1: everything that changes anything goes through the
// same door, with the same origin check and the same membership check.
//
// The answer is 204 whatever happens — a person who has left the channel in the
// half second since the keystroke is not an error worth a page telling anybody
// about, and the membership check above is what decides whether it is sent.
func (s *Server) handleTyping(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	channelID := r.PathValue("id")
	if _, err := s.st.ChannelFor(cu.user.ID, channelID); err != nil {
		s.chatError(w, err, "typing")
		return
	}
	ids, err := s.st.ChannelMemberIDs(channelID)
	if err != nil {
		s.chatError(w, err, "typing")
		return
	}
	others := make([]int64, 0, len(ids))
	for _, id := range ids {
		if id != cu.user.ID {
			others = append(others, id)
		}
	}
	if len(others) > 0 {
		s.hub.publish(others, "typing", map[string]any{
			"channel_id":   channelID,
			"username":     cu.user.Username,
			"display_name": cu.user.DisplayName,
			"avatar":       cu.user.Avatar,
		})
	}
	w.WriteHeader(http.StatusNoContent)
}

// handleMute silences a channel for the person asking, or stops silencing it.
// It is their own opinion about a shared thing, so it is stored on their
// membership and changes nothing for anybody else.
func (s *Server) handleMute(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		Muted bool `json:"muted"`
	}
	if !decode(w, r, &req) {
		return
	}
	if err := s.st.SetMuted(cu.user.ID, r.PathValue("id"), req.Muted); err != nil {
		s.chatError(w, err, "mute")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"muted": req.Muted})
}

// handleHide closes a conversation for the person asking. Nothing is deleted:
// see store.HideChannel, and note that it refuses anything that is not a
// conversation — a shared channel one person could make vanish is a channel
// they would have no way back into.
func (s *Server) handleHide(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	if err := s.st.HideChannel(cu.user.ID, r.PathValue("id")); err != nil {
		s.chatError(w, err, "hide")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"hidden": true})
}

func (s *Server) chatError(w http.ResponseWriter, err error, what string) {
	switch {
	case errors.Is(err, store.ErrDelivery):
		fail(w, http.StatusConflict, "delivery_failed")
	case errors.Is(err, store.ErrNotFound):
		fail(w, http.StatusNotFound, "not_found")
	case errors.Is(err, store.ErrBadMessage):
		fail(w, http.StatusBadRequest, "bad_message")
	case errors.Is(err, store.ErrBadAttachments):
		fail(w, http.StatusBadRequest, "bad_attachments")
	case errors.Is(err, store.ErrBadReaction):
		fail(w, http.StatusBadRequest, "bad_reaction")
	case errors.Is(err, store.ErrBadChannelName):
		fail(w, http.StatusBadRequest, "bad_channel_name")
	case errors.Is(err, store.ErrNameTaken):
		fail(w, http.StatusConflict, "name_taken")
	case errors.Is(err, store.ErrEndpointTaken):
		fail(w, http.StatusConflict, "endpoint_taken")
	default:
		s.log.Error(what, "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
	}
}

// ---------------------------------------------------------------- channels

func (s *Server) handleListChannels(w http.ResponseWriter, _ *http.Request, cu ctxUser) {
	list, err := s.st.ListChannels(cu.user.ID)
	if err != nil {
		s.chatError(w, err, "channels")
		return
	}
	out := make([]map[string]any, 0, len(list))
	for _, c := range list {
		out = append(out, s.channelJSON(c))
	}
	cats, err := s.st.Categories()
	if err != nil {
		s.chatError(w, err, "categories")
		return
	}
	folders := make([]map[string]any, 0, len(cats))
	for _, c := range cats {
		folders = append(folders, categoryJSON(c))
	}
	writeJSON(w, http.StatusOK, map[string]any{"channels": out, "categories": folders, "can_manage": s.permissionsOf(cu.user.ID)&store.PermManageChannels != 0})
}

// handleCreateChannel is owner-only, for the same reason invites are: on a
// server that exists because one person decided who is in it, the shape of the
// place is that person's call too.
func (s *Server) handleCreateChannel(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	if !s.can(w, cu, store.PermManageChannels) {
		return
	}
	var req struct {
		Name  string `json:"name"`
		Topic string `json:"topic"`
		// Kind is "text" (the default, so an older client keeps working) or
		// "voice". Nothing else: the two other kinds a channel row can have —
		// a direct message — are made by their own endpoint and never by name.
		Kind string `json:"kind"`
	}
	if !decode(w, r, &req) {
		return
	}
	var ch store.Channel
	var err error
	switch req.Kind {
	case "", store.ChannelText:
		ch, err = s.st.CreateTextChannel(cu.user.ID, req.Name, req.Topic)
	case store.ChannelVoice:
		ch, err = s.st.CreateVoiceChannel(cu.user.ID, req.Name, req.Topic)
	default:
		fail(w, http.StatusBadRequest, "bad_request")
		return
	}
	if err != nil {
		s.chatError(w, err, "create channel")
		return
	}
	s.log.Info("channel created", "user_id", cu.user.ID, "channel_id", ch.ID)
	// Everybody is a member of a text channel, so everybody's channel column
	// needs to grow a line.
	s.notifyChannel(ch.ID, "channel", s.channelJSON(ch))
	writeJSON(w, http.StatusCreated, s.channelJSON(ch))
}

func (s *Server) handleDeleteChannel(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	if !s.can(w, cu, store.PermManageChannels) {
		return
	}
	id := r.PathValue("id")
	ch, err := s.st.ChannelFor(cu.user.ID, id)
	if err != nil {
		s.chatError(w, err, "delete channel")
		return
	}
	if ch.Kind == store.ChannelVoice {
		if media := s.media.Load(); media != nil && len(media.Rooms([]string{id})[id]) > 0 {
			fail(w, http.StatusConflict, "room_busy")
			return
		}
	}
	// Save recipients before revoking membership, and only notify after commit.
	ids, err := s.st.ChannelMemberIDs(id)
	if err != nil {
		s.chatError(w, err, "delete channel members")
		return
	}
	if err := s.st.DeleteRoom(cu.user.ID, id); err != nil {
		if errors.Is(err, store.ErrLastChannel) {
			fail(w, http.StatusConflict, "last_channel")
			return
		}
		s.chatError(w, err, "delete channel")
		return
	}
	s.hub.publish(ids, "channel_removed", map[string]string{"id": id})
	s.log.Info("channel deleted", "user_id", cu.user.ID, "channel_id", id)
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleGetChannel(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	ch, err := s.st.ChannelFor(cu.user.ID, r.PathValue("id"))
	if err != nil {
		s.chatError(w, err, "channel")
		return
	}
	writeJSON(w, http.StatusOK, s.channelJSON(ch))
}

// handleOpenDM opens (or reopens) the conversation with one person. It is
// idempotent: pressing the button twice, or on two devices at once, lands in
// the same conversation, because dm_key is unique in the database.
func (s *Server) handleOpenDM(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		Username string `json:"username"`
	}
	if !decode(w, r, &req) {
		return
	}
	peer, _, err := s.st.UserByUsername(req.Username)
	if err != nil || peer.Disabled || peer.ID == cu.user.ID {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	ch, err := s.st.EnsureDM(cu.user.ID, peer.ID)
	if err != nil {
		s.chatError(w, err, "open dm")
		return
	}
	// Both sides get the new line in their column; each sees the other's name,
	// so the payload is built per person rather than broadcast as one blob.
	for _, uid := range []int64{cu.user.ID, peer.ID} {
		if view, err := s.st.ChannelFor(uid, ch.ID); err == nil {
			s.hub.publish([]int64{uid}, "channel", s.channelJSON(view))
		}
	}
	// And the answer is the asker's view of it, for the same reason: a
	// conversation is named after *the other person*, so it has no name at all
	// until it is looked at from one side or the other.
	//
	// It used to answer with what EnsureDM returned, which is the row in the
	// channels table and has nobody in it. Nothing showed, because the page
	// almost always had the conversation in its column already and kept the
	// copy it had. Closing one and opening it again is the case where it does
	// not — and there the conversation came back with no name on it, which is
	// exactly what the owner saw.
	mine, err := s.st.ChannelFor(cu.user.ID, ch.ID)
	if err != nil {
		s.chatError(w, err, "open dm")
		return
	}
	writeJSON(w, http.StatusOK, s.channelJSON(mine))
}

// handleSearch is "where did I see that?".
//
// The one thing worth saying about it is what is *not* here: no index of its
// own, no list of channels to pass in, and no way to ask about somebody else's
// conversations. The question is answered against the same membership rows
// every other read uses (store.SearchMessages), so a word written in a private
// conversation this person is not part of is not findable — and not *absent*
// either, which is the same thing said more carefully: the answer looks exactly
// like the answer for a word nobody ever wrote.
func (s *Server) handleSearch(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	q := r.URL.Query().Get("q")
	if len([]rune(q)) > 200 {
		q = string([]rune(q)[:200])
	}
	list, err := s.st.SearchMessages(cu.user.ID, q, store.MaxSearchResults)
	if err != nil {
		s.chatError(w, err, "search")
		return
	}
	out := make([]map[string]any, 0, len(list))
	previewCache := s.cachedPreviews(list)
	for _, m := range list {
		one := s.messageJSON(m, previewCache)
		// Where it was said, so the answer can be read without opening every
		// conversation in it. The channel is one this person belongs to, by the
		// query above, so its name is theirs to know.
		if ch, err := s.st.ChannelFor(cu.user.ID, m.ChannelID); err == nil {
			one["channel"] = s.channelJSON(ch)
		}
		out = append(out, one)
	}
	// Never the words: a log line is the easiest place in the world to leak what
	// somebody was looking for.
	s.log.Info("search", "user_id", cu.user.ID, "found", len(out))
	writeJSON(w, http.StatusOK, map[string]any{"messages": out})
}

// handleMembers is the list the mention box completes from and the DM list is
// started from. It is the membership of a twenty-person private server, which
// everybody in it can already see; it carries no address and no session.
func (s *Server) handleMembers(w http.ResponseWriter, _ *http.Request, cu ctxUser) {
	users, err := s.st.ListUsers()
	if err != nil {
		s.chatError(w, err, "members")
		return
	}
	// The names this person has given people, which are theirs alone. They ride
	// along with the list rather than having a request of their own because
	// they are useless without it — a nickname is a label on a name.
	nicks, err := s.st.Nicknames(cu.user.ID)
	if err != nil {
		s.chatError(w, err, "members")
		return
	}
	out := make([]map[string]any, 0, len(users))
	for _, u := range users {
		if u.Disabled {
			continue
		}
		person := map[string]any{
			"username": u.Username, "display_name": u.DisplayName, "is_admin": u.IsAdmin, "avatar": u.Avatar,
		}
		if nick := nicks[u.Username]; nick != "" {
			person["nickname"] = nick
		}
		out = append(out, person)
	}
	writeJSON(w, http.StatusOK, map[string]any{"members": out})
}

// ---------------------------------------------------------------- messages

func (s *Server) handleHistory(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	before, _ := strconv.ParseInt(r.URL.Query().Get("before"), 10, 64)
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	if limit <= 0 {
		limit = HistoryPageSize
	}
	var list []store.Message
	var err error
	var hasMore, hasNewer bool
	window := r.URL.Query().Has("around") || r.URL.Query().Has("after")
	if window {
		around := r.URL.Query().Has("around")
		key := "after"
		if around {
			key = "around"
		}
		anchor, parseErr := strconv.ParseInt(r.URL.Query().Get(key), 10, 64)
		if parseErr != nil || anchor <= 0 {
			fail(w, http.StatusNotFound, "not_found")
			return
		}
		list, hasMore, hasNewer, err = s.st.MessagesWindow(cu.user.ID, r.PathValue("id"), anchor, limit, around)
	} else {
		list, err = s.st.Messages(cu.user.ID, r.PathValue("id"), before, limit)
		hasMore = len(list) == limit
	}
	if err != nil {
		s.chatError(w, err, "history")
		return
	}
	out := make([]map[string]any, 0, len(list))
	previewCache := s.cachedPreviews(list)
	for _, m := range list {
		out = append(out, s.messageJSON(m, previewCache))
	}
	// has_more without a second count: a full page means there is probably
	// more above it, and the client finds out for certain by asking.
	response := map[string]any{"messages": out, "has_more": hasMore}
	if window {
		response["has_newer"] = hasNewer
	}
	writeJSON(w, http.StatusOK, response)
}

func (s *Server) handlePostMessage(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	n, err := s.st.RecordedUsage(cu.user.ID, store.UsageMessage, messageWindow)
	if err != nil {
		s.chatError(w, err, "usage")
		return
	}
	if n >= MessagesPerMinute {
		fail(w, http.StatusTooManyRequests, "message_limit")
		return
	}
	var req struct {
		Body        string   `json:"body"`
		Attachments []string `json:"attachments"`
		ReplyTo     int64    `json:"reply_to"`
	}
	if !decode(w, r, &req) {
		return
	}
	channelID := r.PathValue("id")
	// The channel is read before the message is posted, for its kind and its
	// name: a notification says "em #geral" or "te mandou uma mensagem", and
	// this is the read that already checks the author belongs there.
	channel, err := s.st.ChannelFor(cu.user.ID, channelID)
	if err != nil {
		s.chatError(w, err, "post message")
		return
	}
	// The check at the top is the cheap no for somebody already over it; this
	// one, inside the transaction that writes the message, is the one that
	// decides.
	msg, err := s.st.PostMessageWithin(cu.user.ID, channelID, req.Body, req.Attachments, req.ReplyTo,
		&store.MessageQuota{Window: messageWindow, Max: MessagesPerMinute})
	if errors.Is(err, store.ErrMessageLimit) {
		fail(w, http.StatusTooManyRequests, "message_limit")
		return
	}
	if err != nil {
		s.chatError(w, err, "post message")
		return
	}
	// Never the body, never the mentions: a log line is the easiest place in
	// the world to leak what somebody wrote.
	s.log.Info("message", "user_id", cu.user.ID, "message_id", msg.ID, "files", len(msg.Attachments))
	s.notifyChannel(msg.ChannelID, "message", s.messageJSON(msg))
	// And, for whoever is not looking at the app, the phone in their pocket.
	s.notifyMessage(msg, channel)
	writeJSON(w, http.StatusCreated, s.messageJSON(msg))
	s.startPreviews(cu.user.ID, msg)
}

func (s *Server) handleEditMessage(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	var req struct {
		Body string `json:"body"`
	}
	if !decode(w, r, &req) {
		return
	}
	msg, err := s.st.EditMessage(cu.user.ID, id, req.Body)
	if err != nil {
		s.chatError(w, err, "edit message")
		return
	}
	s.notifyChannel(msg.ChannelID, "message_changed", s.messageJSON(msg))
	writeJSON(w, http.StatusOK, s.messageJSON(msg))
	s.startPreviews(cu.user.ID, msg)
}

func (s *Server) handleDeleteMessage(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	mod := store.OwnOnly
	switch {
	case cu.user.IsAdmin:
		mod = store.OthersAnywhere
	case s.permissionsOf(cu.user.ID)&store.PermManageMessages != 0:
		mod = store.OthersInText
	}
	msg, err := s.st.DeleteMessage(cu.user.ID, id, mod)
	if err != nil {
		s.chatError(w, err, "delete message")
		return
	}
	s.log.Info("message removed", "user_id", cu.user.ID, "message_id", id)
	s.notifyChannel(msg.ChannelID, "message_changed", s.messageJSON(msg))
	writeJSON(w, http.StatusOK, s.messageJSON(msg))
}

// handleReact puts one emoji on a message, or takes it off again.
//
// One endpoint and not two, because it is one gesture: the button under a
// message is a switch. The page says which way it should end up rather than
// "toggle it", so that two devices of the same person pressing at the same
// moment agree instead of taking turns undoing each other.
//
// What comes back, and what goes out to the room, is the whole message — the
// same `message_changed` an edit sends. That is not laziness: a reaction is a
// change to a message, every page in the channel already knows how to replace
// one, and a new kind of event would be a second path to keep in step with the
// first.
func (s *Server) handleReact(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	var req struct {
		Emoji string `json:"emoji"`
		On    bool   `json:"on"`
	}
	if !decode(w, r, &req) {
		return
	}
	msg, err := s.st.React(cu.user.ID, id, req.Emoji, req.On)
	if err != nil {
		s.chatError(w, err, "react")
		return
	}
	s.notifyChannel(msg.ChannelID, "message_changed", s.messageJSON(msg))
	// No notification of any kind leaves the building for this. A reaction is
	// the lightest thing anybody does in a chat, and a telephone that buzzes
	// for one is a telephone people turn off.
	writeJSON(w, http.StatusOK, s.messageJSON(msg))
}

// handleMarkRead moves the read mark. It is what makes the unread badge follow
// a person from the phone to the desktop instead of living in one browser.
func (s *Server) handleMarkRead(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		LastReadID int64 `json:"last_read_id"`
	}
	if !decode(w, r, &req) {
		return
	}
	channelID := r.PathValue("id")
	if err := s.st.MarkRead(cu.user.ID, channelID, req.LastReadID); err != nil {
		s.chatError(w, err, "mark read")
		return
	}
	// Reading it here is reading it everywhere: the phone in the pocket stops
	// being told about a conversation that has already been dealt with.
	s.pending.clear(cu.user.ID, channelID)
	// Only this person's own devices: what somebody has read is theirs.
	s.hub.publish([]int64{cu.user.ID}, "read", map[string]any{
		"channel_id": channelID, "last_read_id": req.LastReadID,
	})
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}
