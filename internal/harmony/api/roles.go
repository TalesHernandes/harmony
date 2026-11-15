// Roles (D12c): the list, creating, editing, ordering, deleting, and who has
// which. The rules themselves — who may touch which role — live in
// store/roles.go, inside the transaction that writes, so this file only turns
// requests into calls and answers into JSON.
//
// The three permissions open what used to be the owner's alone: channels and
// folders (Manage channels), other people's messages and pins (Manage
// messages), and roles (Manage roles). What stays the owner's for good —
// invites, accounts, resetting somebody's password, the voice statistics —
// still asks cu.user.IsAdmin and never a role.
package api

import (
	"errors"
	"math"
	"net/http"

	"call/internal/harmony/store"
)

func roleJSON(r store.Role) map[string]any {
	return map[string]any{
		"id":          r.ID,
		"name":        r.Name,
		"color":       r.Color,
		"position":    r.Position,
		"hoist":       r.Hoist,
		"permissions": r.Perms.Names(),
		"members":     r.Members,
	}
}

// rolesJSON is the whole list, highest first, each role with who has it. Like
// the channel column (layoutJSON) it is sent whole on every change: a drag
// rewrites every position, and a page that applies a diff to a list it got a
// second ago ends up with a picture nobody else has.
func (s *Server) rolesJSON() (map[string]any, error) {
	list, err := s.st.Roles()
	if err != nil {
		return nil, err
	}
	out := make([]map[string]any, 0, len(list))
	for _, r := range list {
		out = append(out, roleJSON(r))
	}
	return map[string]any{"roles": out}, nil
}

// publishRoles tells every open page the new list. Everybody, because a role
// colours names in every page and moves people in every member list.
func (s *Server) publishRoles() {
	data, err := s.rolesJSON()
	if err != nil {
		s.log.Error("roles", "err", err)
		return
	}
	s.hub.publishAll("roles", data)
}

// can answers whether somebody may do something now, reading their roles from
// the database on this request — a role taken a second ago is already gone.
// A read error is a 500 and false, so the caller just returns.
func (s *Server) can(w http.ResponseWriter, cu ctxUser, p store.Permission) bool {
	rank, err := s.st.RankOf(cu.user.ID)
	if err != nil {
		s.log.Error("rank", "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
		return false
	}
	if !rank.Has(p) {
		fail(w, http.StatusForbidden, "not_allowed")
		return false
	}
	return true
}

// permissionsOf is the asker's own set, for /api/me and the role list. A read
// error leaves it empty, which only hides buttons the server would refuse.
func (s *Server) permissionsOf(userID int64) store.Permission {
	rank, err := s.st.RankOf(userID)
	if err != nil {
		s.log.Error("rank", "err", err)
		return 0
	}
	if rank.Owner {
		return store.AllPermissions
	}
	return rank.Perms
}

func (s *Server) rolesError(w http.ResponseWriter, err error, what string) {
	switch {
	case errors.Is(err, store.ErrBadRoleName):
		fail(w, http.StatusBadRequest, "bad_role_name")
	case errors.Is(err, store.ErrBadColor):
		fail(w, http.StatusBadRequest, "bad_color")
	case errors.Is(err, store.ErrRoleLimit):
		fail(w, http.StatusConflict, "role_limit")
	case errors.Is(err, store.ErrStaleRoles):
		fail(w, http.StatusConflict, "stale_roles")
	case errors.Is(err, store.ErrNotAllowed):
		fail(w, http.StatusForbidden, "not_allowed")
	default:
		s.chatError(w, err, what)
	}
}

// handleRoles is the list. Everybody may read it, as on Discord: the roles are
// on everybody's profile anyway. my_permissions is the asker's own set, so a
// page knows which buttons to draw without working it out; my_top is the
// position of their highest role (-1 for the owner, null for nobody), so the
// settings screen can grey out the roles they cannot touch.
func (s *Server) handleRoles(w http.ResponseWriter, _ *http.Request, cu ctxUser) {
	data, err := s.rolesJSON()
	if err != nil {
		s.chatError(w, err, "roles")
		return
	}
	rank, err := s.st.RankOf(cu.user.ID)
	if err != nil {
		s.chatError(w, err, "roles")
		return
	}
	data["my_permissions"] = s.permissionsOf(cu.user.ID).Names()
	data["colors"] = store.RoleColors
	if rank.Owner {
		data["my_top"] = -1
	} else if rank.Top == math.MaxInt {
		data["my_top"] = nil
	} else {
		data["my_top"] = rank.Top
	}
	writeJSON(w, http.StatusOK, data)
}

type roleRequest struct {
	Name        *string   `json:"name"`
	Color       *string   `json:"color"`
	Hoist       *bool     `json:"hoist"`
	Permissions *[]string `json:"permissions"`
}

func (s *Server) handleCreateRole(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req roleRequest
	if !decode(w, r, &req) {
		return
	}
	in := store.RoleInput{}
	if req.Name != nil {
		in.Name = *req.Name
	}
	if req.Color != nil {
		in.Color = *req.Color
	}
	if req.Hoist != nil {
		in.Hoist = *req.Hoist
	}
	if req.Permissions != nil {
		p, ok := store.ParsePermissions(*req.Permissions)
		if !ok {
			fail(w, http.StatusBadRequest, "bad_permission")
			return
		}
		in.Perms = p
	}
	role, err := s.st.CreateRole(cu.user.ID, in)
	if err != nil {
		s.log.Info("role refused", "user_id", cu.user.ID, "action", "create", "err", err)
		s.rolesError(w, err, "create role")
		return
	}
	s.log.Info("role created", "user_id", cu.user.ID, "role_id", role.ID)
	s.publishRoles()
	writeJSON(w, http.StatusCreated, roleJSON(role))
}

func (s *Server) handleUpdateRole(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req roleRequest
	if !decode(w, r, &req) {
		return
	}
	patch := store.RolePatch{Name: req.Name, Color: req.Color, Hoist: req.Hoist}
	if req.Permissions != nil {
		p, ok := store.ParsePermissions(*req.Permissions)
		if !ok {
			fail(w, http.StatusBadRequest, "bad_permission")
			return
		}
		patch.Perms = &p
	}
	id := r.PathValue("id")
	if err := s.st.UpdateRole(cu.user.ID, id, patch); err != nil {
		s.log.Info("role refused", "user_id", cu.user.ID, "action", "update", "role_id", id, "err", err)
		s.rolesError(w, err, "update role")
		return
	}
	s.log.Info("role updated", "user_id", cu.user.ID, "role_id", id)
	s.publishRoles()
	w.WriteHeader(http.StatusNoContent)
}

// handleDeleteRole removes a role. Whoever had it loses what it gave on their
// next request: nothing caches a rank.
func (s *Server) handleDeleteRole(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	id := r.PathValue("id")
	if err := s.st.DeleteRole(cu.user.ID, id); err != nil {
		s.log.Info("role refused", "user_id", cu.user.ID, "action", "delete", "role_id", id, "err", err)
		s.rolesError(w, err, "delete role")
		return
	}
	s.log.Info("role deleted", "user_id", cu.user.ID, "role_id", id)
	s.publishRoles()
	w.WriteHeader(http.StatusNoContent)
}

// handleOrderRoles writes the whole order at once, highest first — what a drag
// ends in. 409 stale_roles means the list changed meanwhile: reload and let
// the person try again.
func (s *Server) handleOrderRoles(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	var req struct {
		Roles []string `json:"roles"`
	}
	if !decode(w, r, &req) {
		return
	}
	if err := s.st.OrderRoles(cu.user.ID, req.Roles); err != nil {
		s.log.Info("role refused", "user_id", cu.user.ID, "action", "order", "err", err)
		s.rolesError(w, err, "order roles")
		return
	}
	s.log.Info("roles ordered", "user_id", cu.user.ID)
	s.publishRoles()
	data, err := s.rolesJSON()
	if err != nil {
		s.chatError(w, err, "roles")
		return
	}
	writeJSON(w, http.StatusOK, data)
}

// handleRoleMember gives a role to somebody (PUT) or takes it (DELETE).
func (s *Server) handleRoleMember(on bool) func(http.ResponseWriter, *http.Request, ctxUser) {
	return func(w http.ResponseWriter, r *http.Request, cu ctxUser) {
		id := r.PathValue("id")
		if err := s.st.SetRoleMember(cu.user.ID, id, r.PathValue("username"), on); err != nil {
			s.log.Info("role refused", "user_id", cu.user.ID, "action", "member", "role_id", id, "err", err)
			s.rolesError(w, err, "role member")
			return
		}
		s.log.Info("role member", "user_id", cu.user.ID, "role_id", id, "on", on)
		s.publishRoles()
		w.WriteHeader(http.StatusNoContent)
	}
}
