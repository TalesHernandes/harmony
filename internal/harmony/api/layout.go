// The shape of the channel column (D12a): renaming rooms, folders, the order of
// everything, and pinned messages.
//
// Everything here except pinning in a conversation of two is the owner's, for
// the reason handleCreateChannel gives: on a server that exists because one
// person decided who is in it, the shape of the place is that person's call
// (until roles, D12c–e). A member asking gets 403 — they can see the column, so
// there is nothing for a 404 to hide. Pinning in somebody else's conversation
// is the other case, and answers 404 exactly like an invented id
// (store.MessageFor).
package api

import (
	"errors"
	"net/http"
	"strconv"

	"call/internal/harmony/store"
)

func categoryJSON(c store.Category) map[string]any {
	return map[string]any{"id": c.ID, "name": c.Name, "position": c.Position}
}

// layoutJSON is the whole column's shape: the folders in order and where every
// room sits. It is sent whole on every change rather than as a diff, because
// a drag rewrites the order of a dozen rows at once and twenty people's pages
// have to end up with the same picture, not with the same sequence of edits.
func (s *Server) layoutJSON() (map[string]any, error) {
	cats, places, err := s.st.Layout()
	if err != nil {
		return nil, err
	}
	cs := make([]map[string]any, 0, len(cats))
	for _, c := range cats {
		cs = append(cs, categoryJSON(c))
	}
	ps := make([]map[string]any, 0, len(places))
	for _, p := range places {
		ps = append(ps, map[string]any{"id": p.ID, "category_id": p.CategoryID, "position": p.Position})
	}
	return map[string]any{"categories": cs, "channels": ps}, nil
}

// publishLayout tells every open page the new shape. Every page, because every
// account is a member of every room and every folder is everybody's.
func (s *Server) publishLayout() {
	data, err := s.layoutJSON()
	if err != nil {
		s.log.Error("layout", "err", err)
		return
	}
	s.hub.publishAll("layout", data)
}

func (s *Server) layoutError(w http.ResponseWriter, err error, what string) {
	switch {
	case errors.Is(err, store.ErrBadCategoryName):
		fail(w, http.StatusBadRequest, "bad_category_name")
	case errors.Is(err, store.ErrStaleLayout):
		fail(w, http.StatusConflict, "stale_layout")
	case errors.Is(err, store.ErrPinLimit):
		fail(w, http.StatusConflict, "pin_limit")
	case errors.Is(err, store.ErrNotAllowed):
		fail(w, http.StatusForbidden, "not_allowed")
	default:
		s.chatError(w, err, what)
	}
}

// ---------------------------------------------------------------- rename

// handleRenameChannel renames a text channel or a voice room.
//
// The new name goes out as the `channel` event every app already understands
// — the Android and Windows apps of today included — and it goes out **per
// person**, with that person's own unread count and read mark. The event
// replaces the whole row on the other side, and a row with the counters of
// nobody would have put everybody's unread line back at the top.
func (s *Server) handleRenameChannel(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	if !s.can(w, cu, store.PermManageChannels) {
		return
	}
	var req struct {
		Name string `json:"name"`
	}
	if !decode(w, r, &req) {
		return
	}
	id := r.PathValue("id")
	// Through the owner's own membership first, so a conversation of two
	// other people is the same 404 as an id nobody has.
	if _, err := s.st.ChannelFor(cu.user.ID, id); err != nil {
		s.chatError(w, err, "rename channel")
		return
	}
	if err := s.st.RenameRoom(id, req.Name); err != nil {
		s.chatError(w, err, "rename channel")
		return
	}
	s.log.Info("channel renamed", "user_id", cu.user.ID, "channel_id", id)
	s.republishChannel(id)
	ch, err := s.st.ChannelFor(cu.user.ID, id)
	if err != nil {
		s.chatError(w, err, "rename channel")
		return
	}
	writeJSON(w, http.StatusOK, s.channelJSON(ch))
}

// republishChannel sends each member of a channel their own row of it.
func (s *Server) republishChannel(id string) {
	ids, err := s.st.ChannelMemberIDs(id)
	if err != nil {
		s.log.Error("fan-out", "err", err)
		return
	}
	for _, uid := range ids {
		list, err := s.st.ListChannels(uid)
		if err != nil {
			s.log.Error("fan-out", "err", err)
			continue
		}
		for _, c := range list {
			if c.ID == id {
				s.hub.publish([]int64{uid}, "channel", s.channelJSON(c))
				break
			}
		}
	}
}

// ---------------------------------------------------------------- folders

func (s *Server) handleCreateCategory(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	if !s.can(w, cu, store.PermManageChannels) {
		return
	}
	var req struct {
		Name string `json:"name"`
	}
	if !decode(w, r, &req) {
		return
	}
	c, err := s.st.CreateCategory(req.Name)
	if err != nil {
		s.layoutError(w, err, "create category")
		return
	}
	s.log.Info("category created", "user_id", cu.user.ID, "category_id", c.ID)
	s.publishLayout()
	writeJSON(w, http.StatusCreated, categoryJSON(c))
}

func (s *Server) handleRenameCategory(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	if !s.can(w, cu, store.PermManageChannels) {
		return
	}
	var req struct {
		Name string `json:"name"`
	}
	if !decode(w, r, &req) {
		return
	}
	id := r.PathValue("id")
	if err := s.st.RenameCategory(id, req.Name); err != nil {
		s.layoutError(w, err, "rename category")
		return
	}
	s.publishLayout()
	w.WriteHeader(http.StatusNoContent)
}

// handleDeleteCategory removes a folder and leaves every room that was in it,
// with every message, at the top of the column.
func (s *Server) handleDeleteCategory(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	if !s.can(w, cu, store.PermManageChannels) {
		return
	}
	id := r.PathValue("id")
	if err := s.st.DeleteCategory(id); err != nil {
		s.layoutError(w, err, "delete category")
		return
	}
	s.log.Info("category deleted", "user_id", cu.user.ID, "category_id", id)
	s.publishLayout()
	w.WriteHeader(http.StatusNoContent)
}

// handleSaveLayout writes the order of the whole column at once — what a drag
// ends in. See store.SaveLayout for why it has to name everything.
func (s *Server) handleSaveLayout(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	if !s.can(w, cu, store.PermManageChannels) {
		return
	}
	var req struct {
		Categories []string `json:"categories"`
		Channels   []struct {
			ID         string `json:"id"`
			CategoryID string `json:"category_id"`
		} `json:"channels"`
	}
	if !decode(w, r, &req) {
		return
	}
	rooms := make([]store.Placement, 0, len(req.Channels))
	for _, c := range req.Channels {
		rooms = append(rooms, store.Placement{ID: c.ID, CategoryID: c.CategoryID})
	}
	if err := s.st.SaveLayout(req.Categories, rooms); err != nil {
		s.layoutError(w, err, "save layout")
		return
	}
	s.log.Info("layout saved", "user_id", cu.user.ID)
	s.publishLayout()
	data, err := s.layoutJSON()
	if err != nil {
		s.chatError(w, err, "layout")
		return
	}
	writeJSON(w, http.StatusOK, data)
}

// ---------------------------------------------------------------- pins

// handlePin fixes a message to its channel or unfixes it. Like a reaction, the
// page says which way it should end up, and what goes out to the room is the
// whole message on the `message_changed` every page already handles — with
// `pinned` on it, which an app that does not know the word simply ignores.
//
// Nobody's telephone buzzes: a pin is a bookmark for the channel. A new pin
// does leave a line in it (kind 'pin', since 30/10/2026), delivered like any
// message to the pages that are open, so the others find out it is there.
func (s *Server) handlePin(on bool) func(http.ResponseWriter, *http.Request, ctxUser) {
	return func(w http.ResponseWriter, r *http.Request, cu ctxUser) {
		id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
		if err != nil {
			fail(w, http.StatusNotFound, "not_found")
			return
		}
		msg, note, err := s.st.Pin(cu.user.ID, id, on, s.permissionsOf(cu.user.ID)&store.PermManageMessages != 0)
		if err != nil {
			s.layoutError(w, err, "pin")
			return
		}
		s.notifyChannel(msg.ChannelID, "message_changed", s.messageJSON(msg))
		if note.ID != 0 {
			s.notifyChannel(note.ChannelID, "message", s.messageJSON(note))
		}
		writeJSON(w, http.StatusOK, s.messageJSON(msg))
	}
}

func (s *Server) handlePins(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	list, err := s.st.Pins(cu.user.ID, r.PathValue("id"))
	if err != nil {
		s.chatError(w, err, "pins")
		return
	}
	cache := s.cachedPreviews(list)
	out := make([]map[string]any, 0, len(list))
	for _, m := range list {
		out = append(out, s.messageJSON(m, cache))
	}
	writeJSON(w, http.StatusOK, map[string]any{"messages": out, "limit": store.MaxPinsPerChannel})
}
