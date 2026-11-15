package api

// D12c: roles. Each route with the "A cannot do what only B may" test, and the
// promise the owner made when he asked for roles: invites, accounts, resetting
// a password and the voice statistics stay his, whatever role anybody has.

import (
	"fmt"
	"net/http"
	"testing"
)

func (d *device) createRole(name, color string, perms ...string) reply {
	d.h.t.Helper()
	if perms == nil {
		perms = []string{}
	}
	return d.do(http.MethodPost, "/api/roles", map[string]any{"name": name, "color": color, "permissions": perms})
}

func (d *device) mustCreateRole(name, color string, perms ...string) string {
	d.h.t.Helper()
	r := d.createRole(name, color, perms...)
	if r.status != http.StatusCreated {
		d.h.t.Fatalf("create role %s: %d %s", name, r.status, r.raw)
	}
	return r.body["id"].(string)
}

func (d *device) giveRole(roleID, username string, on bool) reply {
	d.h.t.Helper()
	method := http.MethodPut
	if !on {
		method = http.MethodDelete
	}
	return d.do(method, "/api/roles/"+roleID+"/members/"+username, nil)
}

func (d *device) roles() []map[string]any {
	d.h.t.Helper()
	r := d.get("/api/roles")
	if r.status != http.StatusOK {
		d.h.t.Fatalf("GET /api/roles: %d %s", r.status, r.raw)
	}
	var out []map[string]any
	for _, x := range r.body["roles"].([]any) {
		out = append(out, x.(map[string]any))
	}
	return out
}

func TestNobodyWithoutManageRolesTouchesRoles(t *testing.T) {
	c := newCast(t)
	id := c.owner.mustCreateRole("Moderador", "#2ecc71", "manage_messages")
	before := fmt.Sprint(c.owner.roles())
	for what, r := range map[string]reply{
		"create": c.bo.createRole("Bo", ""),
		"edit":   c.bo.do(http.MethodPatch, "/api/roles/"+id, map[string]any{"name": "x"}),
		"delete": c.bo.do(http.MethodDelete, "/api/roles/"+id, nil),
		"give":   c.bo.giveRole(id, "bruno", true),
		"order":  c.bo.do(http.MethodPut, "/api/roles/order", map[string]any{"roles": []string{id}}),
	} {
		if r.status != http.StatusForbidden || r.errorCode() != "not_allowed" {
			t.Errorf("%s: %d %s", what, r.status, r.raw)
		}
	}
	if after := fmt.Sprint(c.bo.roles()); after != before {
		t.Fatalf("a refused request changed the roles:\n%s\n%s", before, after)
	}
	// Everybody reads the list: it colours names.
	if r := c.bo.get("/api/roles"); r.status != http.StatusOK || len(r.body["my_permissions"].([]any)) != 0 {
		t.Fatalf("GET /api/roles as a member: %d %s", r.status, r.raw)
	}
}

func TestManageRolesStopsAtItsOwnRoleOverHTTP(t *testing.T) {
	c := newCast(t)
	mod := c.owner.mustCreateRole("Moderador", "#2ecc71", "manage_roles", "manage_messages")
	low := c.owner.mustCreateRole("Amigos", "")
	requireReply(t, c.owner.giveRole(mod, "alice", true), http.StatusNoContent, "")
	// Her own role, the owner, and a permission she lacks: refused.
	requireReply(t, c.alice.do(http.MethodPatch, "/api/roles/"+mod, map[string]any{"color": "#e74c3c"}), http.StatusForbidden, "not_allowed")
	requireReply(t, c.alice.giveRole(mod, "bruno", true), http.StatusForbidden, "not_allowed")
	requireReply(t, c.alice.giveRole(low, "tales", true), http.StatusForbidden, "not_allowed")
	requireReply(t, c.alice.createRole("Canais", "", "manage_channels"), http.StatusForbidden, "not_allowed")
	requireReply(t, c.alice.do(http.MethodPatch, "/api/roles/"+low, map[string]any{"permissions": []string{"manage_channels"}}), http.StatusForbidden, "not_allowed")
	canais := c.owner.mustCreateRole("Canais", "", "manage_channels")
	requireReply(t, c.alice.giveRole(canais, "alice", true), http.StatusForbidden, "not_allowed")
	requireReply(t, c.alice.do(http.MethodPost, "/api/channels", map[string]any{"name": "novo"}), http.StatusForbidden, "not_allowed")
	requireReply(t, c.bo.createRole("", ""), http.StatusForbidden, "not_allowed")
	// Below her, with what she has: allowed.
	requireReply(t, c.alice.giveRole(low, "bruno", true), http.StatusNoContent, "")
	requireReply(t, c.alice.do(http.MethodPatch, "/api/roles/"+low, map[string]any{"color": "#3498db", "permissions": []string{"manage_messages"}}), http.StatusNoContent, "")
	// Inputs.
	requireReply(t, c.owner.createRole("Cor", "#123456"), http.StatusBadRequest, "bad_color")
	requireReply(t, c.owner.createRole("", ""), http.StatusBadRequest, "bad_role_name")
	requireReply(t, c.owner.createRole("Perm", "", "ban_members"), http.StatusBadRequest, "bad_permission")
	requireReply(t, c.owner.do(http.MethodPut, "/api/roles/order", map[string]any{"roles": []string{low}}), http.StatusConflict, "stale_roles")
}

func TestNoRoleOpensWhatStaysTheOwners(t *testing.T) {
	c := newCast(t)
	all := c.owner.mustCreateRole("Tudo", "#e91e63", "manage_channels", "manage_messages", "manage_roles")
	requireReply(t, c.owner.giveRole(all, "alice", true), http.StatusNoContent, "")
	if r := c.alice.get("/api/me"); len(r.body["permissions"].([]any)) != 3 {
		t.Fatalf("/api/me permissions: %s", r.raw)
	}
	requireReply(t, c.alice.do(http.MethodPost, "/api/invites", map[string]any{"note": "x"}), http.StatusForbidden, "")
	if r := c.alice.get("/api/invites"); r.body["can_invite"] != false {
		t.Fatalf("can_invite with every role: %s", r.raw)
	}
	requireReply(t, c.alice.get("/api/admin/accounts"), http.StatusForbidden, "not_allowed")
	for _, action := range []string{"disable", "enable", "reset-password"} {
		requireReply(t, adminPost(c.alice, "bruno", action), http.StatusForbidden, "not_allowed")
	}
	requireReply(t, c.alice.get("/api/voice/stats"), http.StatusForbidden, "not_allowed")
	// And the owner cannot be given or stripped of anything.
	requireReply(t, c.alice.giveRole(all, "tales", true), http.StatusForbidden, "not_allowed")
}

func TestManageMessagesComesAndGoesWithTheRole(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	mod := c.owner.mustCreateRole("Moderador", "#2ecc71", "manage_messages")
	first := c.bo.mustSay(general, "primeira")
	// No role: Alice cannot remove Bruno's line, nor pin in a channel.
	requireReply(t, c.alice.do(http.MethodDelete, fmt.Sprintf("/api/messages/%v", first["id"]), nil), http.StatusNotFound, "")
	requireReply(t, c.alice.do(http.MethodPost, fmt.Sprintf("/api/messages/%v/pin", first["id"]), nil), http.StatusForbidden, "not_allowed")

	requireReply(t, c.owner.giveRole(mod, "alice", true), http.StatusNoContent, "")
	requireReply(t, c.alice.do(http.MethodPost, fmt.Sprintf("/api/messages/%v/pin", first["id"]), nil), http.StatusOK, "")
	requireReply(t, c.alice.do(http.MethodDelete, fmt.Sprintf("/api/messages/%v", first["id"]), nil), http.StatusOK, "")
	// Not in a conversation of two she is part of.
	dm := c.alice.openDM("bruno")
	private := c.bo.mustSay(dm, "só nós")
	requireReply(t, c.alice.do(http.MethodDelete, fmt.Sprintf("/api/messages/%v", private["id"]), nil), http.StatusNotFound, "")

	// Taken away: the very next request is refused.
	second := c.bo.mustSay(general, "segunda")
	requireReply(t, c.owner.giveRole(mod, "alice", false), http.StatusNoContent, "")
	requireReply(t, c.alice.do(http.MethodDelete, fmt.Sprintf("/api/messages/%v", second["id"]), nil), http.StatusNotFound, "")

	// Deleted with the role still on her: the same.
	requireReply(t, c.owner.giveRole(mod, "alice", true), http.StatusNoContent, "")
	requireReply(t, c.owner.do(http.MethodDelete, "/api/roles/"+mod, nil), http.StatusNoContent, "")
	requireReply(t, c.alice.do(http.MethodDelete, fmt.Sprintf("/api/messages/%v", second["id"]), nil), http.StatusNotFound, "")
}

func TestManageChannelsIsWhatCanManageMeans(t *testing.T) {
	c := newCast(t)
	role := c.owner.mustCreateRole("Canais", "", "manage_channels")
	if r := c.alice.get("/api/channels"); r.body["can_manage"] != false {
		t.Fatalf("can_manage without the role: %s", r.raw)
	}
	requireReply(t, c.alice.do(http.MethodPost, "/api/categories", map[string]any{"name": "Jogos"}), http.StatusForbidden, "not_allowed")
	requireReply(t, c.alice.do(http.MethodPost, "/api/channels", map[string]any{"name": "minecraft"}), http.StatusForbidden, "not_allowed")

	requireReply(t, c.owner.giveRole(role, "alice", true), http.StatusNoContent, "")
	if r := c.alice.get("/api/channels"); r.body["can_manage"] != true {
		t.Fatalf("can_manage with the role: %s", r.raw)
	}
	cat := c.alice.do(http.MethodPost, "/api/categories", map[string]any{"name": "Jogos"})
	requireReply(t, cat, http.StatusCreated, "")
	ch := c.alice.do(http.MethodPost, "/api/channels", map[string]any{"name": "minecraft"})
	requireReply(t, ch, http.StatusCreated, "")
	requireReply(t, c.alice.do(http.MethodPatch, "/api/channels/"+ch.body["id"].(string), map[string]any{"name": "mine"}), http.StatusOK, "")

	requireReply(t, c.owner.do(http.MethodDelete, "/api/roles/"+role, nil), http.StatusNoContent, "")
	requireReply(t, c.alice.do(http.MethodDelete, "/api/categories/"+cat.body["id"].(string), nil), http.StatusForbidden, "not_allowed")
	requireReply(t, c.alice.do(http.MethodDelete, "/api/channels/"+ch.body["id"].(string), nil), http.StatusForbidden, "not_allowed")
	if r := c.alice.get("/api/channels"); r.body["can_manage"] != false {
		t.Fatalf("can_manage after the role was deleted: %s", r.raw)
	}
}
