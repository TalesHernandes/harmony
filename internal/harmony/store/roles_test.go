package store

import (
	"errors"
	"testing"
)

// rolesFixture is the owner, a moderator holding "Mod" (Manage roles and
// Manage messages), and two roles below Mod: "Ajudante" and "Amigos".
func rolesFixture(t *testing.T) (st *Store, owner, mod, bia User, modRole, helper, friends Role) {
	t.Helper()
	st = memStore(t)
	owner = signUp(t, st, "owner")
	mod = signUp(t, st, "mod")
	bia = signUp(t, st, "bia")
	if !owner.IsAdmin || mod.IsAdmin {
		t.Fatalf("the first account is the owner: %v %v", owner.IsAdmin, mod.IsAdmin)
	}
	var err error
	modRole, err = st.CreateRole(owner.ID, RoleInput{Name: "Mod", Color: "#2ECC71", Perms: PermManageRoles | PermManageMessages})
	if err != nil {
		t.Fatal(err)
	}
	helper, err = st.CreateRole(owner.ID, RoleInput{Name: "Ajudante", Perms: PermManageChannels})
	if err != nil {
		t.Fatal(err)
	}
	friends, err = st.CreateRole(owner.ID, RoleInput{Name: "Amigos"})
	if err != nil {
		t.Fatal(err)
	}
	if err := st.SetRoleMember(owner.ID, modRole.ID, "mod", true); err != nil {
		t.Fatal(err)
	}
	return
}

func TestRoleRankIsReadFresh(t *testing.T) {
	st, owner, mod, bia, modRole, _, friends := rolesFixture(t)
	r, err := st.RankOf(mod.ID)
	if err != nil || !r.Has(PermManageRoles) || r.Has(PermManageChannels) || r.Top != modRole.Position {
		t.Fatalf("mod rank = %+v, %v", r, err)
	}
	if r, _ := st.RankOf(bia.ID); r.Perms != 0 || r.Has(PermManageMessages) {
		t.Fatalf("somebody with no role has nothing: %+v", r)
	}
	if r, _ := st.RankOf(owner.ID); !r.Owner || !r.Has(AllPermissions) {
		t.Fatalf("owner rank = %+v", r)
	}
	// The owner may wear a role, for the colour: it takes nothing away, and
	// nobody else may put one on them or take it off.
	if err := st.SetRoleMember(owner.ID, friends.ID, "owner", true); err != nil {
		t.Fatalf("the owner giving themself a role: %v", err)
	}
	if r, _ := st.RankOf(owner.ID); !r.Owner || !r.Has(AllPermissions) || r.Top != -1 {
		t.Fatalf("a role changed the owner's rank: %+v", r)
	}
	// "Amigos" is below the moderator, so only the owner rule stops this.
	if err := st.SetRoleMember(mod.ID, friends.ID, "owner", false); !errors.Is(err, ErrNotAllowed) {
		t.Fatalf("somebody else touching the owner's roles: %v", err)
	}
	// Deleting the role takes the permission with it on the very next read.
	if err := st.DeleteRole(owner.ID, modRole.ID); err != nil {
		t.Fatal(err)
	}
	if r, _ := st.RankOf(mod.ID); r.Has(PermManageRoles) || r.Has(PermManageMessages) {
		t.Fatalf("deleted role still counts: %+v", r)
	}
}

func TestManageRolesTouchesOnlyRolesBelowItsOwn(t *testing.T) {
	st, owner, mod, _, modRole, helper, friends := rolesFixture(t)
	other, err := st.CreateRole(owner.ID, RoleInput{Name: "Igual"})
	if err != nil {
		t.Fatal(err)
	}
	// Lift "Igual" above Mod: now it outranks the moderator.
	if err := st.OrderRoles(owner.ID, []string{other.ID, modRole.ID, helper.ID, friends.ID}); err != nil {
		t.Fatal(err)
	}
	name := "x"
	for what, err := range map[string]error{
		"edit own":         st.UpdateRole(mod.ID, modRole.ID, RolePatch{Name: &name}),
		"edit above":       st.UpdateRole(mod.ID, other.ID, RolePatch{Name: &name}),
		"delete own":       st.DeleteRole(mod.ID, modRole.ID),
		"delete above":     st.DeleteRole(mod.ID, other.ID),
		"give own":         st.SetRoleMember(mod.ID, modRole.ID, "bia", true),
		"give above":       st.SetRoleMember(mod.ID, other.ID, "bia", true),
		"take own from me": st.SetRoleMember(mod.ID, modRole.ID, "mod", false),
		"touch the owner":  st.SetRoleMember(mod.ID, friends.ID, "owner", true),
		"lift over mine":   st.OrderRoles(mod.ID, []string{other.ID, friends.ID, modRole.ID, helper.ID}),
		"drop mine":        st.OrderRoles(mod.ID, []string{other.ID, helper.ID, modRole.ID, friends.ID}),
	} {
		if !errors.Is(err, ErrNotAllowed) {
			t.Errorf("%s: got %v, want ErrNotAllowed", what, err)
		}
	}
	// Below: allowed.
	if err := st.UpdateRole(mod.ID, friends.ID, RolePatch{Name: &name}); err != nil {
		t.Fatal(err)
	}
	if err := st.SetRoleMember(mod.ID, friends.ID, "bia", true); err != nil {
		t.Fatal(err)
	}
	if err := st.SetRoleMember(mod.ID, friends.ID, "mod", true); err != nil {
		t.Fatalf("a role below one's own may be given to oneself: %v", err)
	}
	if err := st.OrderRoles(mod.ID, []string{other.ID, modRole.ID, friends.ID, helper.ID}); err != nil {
		t.Fatalf("reordering below one's own: %v", err)
	}
	if err := st.DeleteRole(mod.ID, friends.ID); err != nil {
		t.Fatal(err)
	}
	list, _ := st.Roles()
	for i, r := range list {
		if r.Position != i {
			t.Fatalf("positions are 0..n-1 after a delete: %+v", list)
		}
	}
}

func TestManageRolesNeverGivesWhatItLacks(t *testing.T) {
	st, owner, mod, _, _, helper, friends := rolesFixture(t)
	// Mod has Manage roles and Manage messages, not Manage channels.
	if _, err := st.CreateRole(mod.ID, RoleInput{Name: "Novo", Perms: PermManageChannels}); !errors.Is(err, ErrNotAllowed) {
		t.Fatalf("create with a permission one lacks: %v", err)
	}
	r, err := st.CreateRole(mod.ID, RoleInput{Name: "Novo", Perms: PermManageMessages})
	if err != nil {
		t.Fatal(err)
	}
	if r.Position != 3 {
		t.Fatalf("a new role goes to the bottom: %d", r.Position)
	}
	add := PermManageChannels
	if err := st.UpdateRole(mod.ID, friends.ID, RolePatch{Perms: &add}); !errors.Is(err, ErrNotAllowed) {
		t.Fatalf("switch on a permission one lacks: %v", err)
	}
	// Ajudante has Manage channels, which Mod lacks: Mod may still rename it
	// and switch Manage messages, but not take Manage channels off.
	name := "Ajudantes"
	if err := st.UpdateRole(mod.ID, helper.ID, RolePatch{Name: &name}); err != nil {
		t.Fatal(err)
	}
	keep := PermManageChannels | PermManageMessages
	if err := st.UpdateRole(mod.ID, helper.ID, RolePatch{Perms: &keep}); err != nil {
		t.Fatalf("toggling only one's own bits: %v", err)
	}
	drop := PermManageMessages
	if err := st.UpdateRole(mod.ID, helper.ID, RolePatch{Perms: &drop}); !errors.Is(err, ErrNotAllowed) {
		t.Fatalf("switch off a permission one lacks: %v", err)
	}
	// Nor give a role that carries one, to herself or anybody (the hole the
	// Codex audit found on 02/11/2026: Mod giving herself Ajudante).
	if err := st.SetRoleMember(mod.ID, helper.ID, "mod", true); !errors.Is(err, ErrNotAllowed) {
		t.Fatalf("giving oneself a role with a permission one lacks: %v", err)
	}
	if err := st.SetRoleMember(mod.ID, helper.ID, "bia", true); !errors.Is(err, ErrNotAllowed) {
		t.Fatalf("giving somebody a role with a permission one lacks: %v", err)
	}
	if r, _ := st.RankOf(mod.ID); r.Has(PermManageChannels) {
		t.Fatal("the moderator gained Manage channels")
	}
	// Taking it off somebody gives nothing, so it is allowed.
	if err := st.SetRoleMember(owner.ID, helper.ID, "bia", true); err != nil {
		t.Fatal(err)
	}
	if err := st.SetRoleMember(mod.ID, helper.ID, "bia", false); err != nil {
		t.Fatalf("taking a role off: %v", err)
	}
	// The owner may do all of it.
	if err := st.UpdateRole(owner.ID, helper.ID, RolePatch{Perms: &drop}); err != nil {
		t.Fatal(err)
	}
}

func TestNoRoleNoManagingAndInputsAreChecked(t *testing.T) {
	st, owner, _, bia, _, _, friends := rolesFixture(t)
	name := "x"
	for what, err := range map[string]error{
		"create": func() error { _, err := st.CreateRole(bia.ID, RoleInput{Name: "B"}); return err }(),
		"edit":   st.UpdateRole(bia.ID, friends.ID, RolePatch{Name: &name}),
		"delete": st.DeleteRole(bia.ID, friends.ID),
		"give":   st.SetRoleMember(bia.ID, friends.ID, "bia", true),
		"order":  st.OrderRoles(bia.ID, nil),
	} {
		if !errors.Is(err, ErrNotAllowed) {
			t.Errorf("%s without Manage roles: %v", what, err)
		}
	}
	if _, err := st.CreateRole(owner.ID, RoleInput{Name: "C", Color: "#123456"}); !errors.Is(err, ErrBadColor) {
		t.Fatalf("a colour outside the palette: %v", err)
	}
	if _, err := st.CreateRole(owner.ID, RoleInput{Name: "  "}); !errors.Is(err, ErrBadRoleName) {
		t.Fatalf("an empty name: %v", err)
	}
	if _, err := st.CreateRole(owner.ID, RoleInput{Name: "amigos"}); !errors.Is(err, ErrNameTaken) {
		t.Fatalf("a taken name: %v", err)
	}
	if err := st.OrderRoles(owner.ID, []string{friends.ID}); !errors.Is(err, ErrStaleRoles) {
		t.Fatalf("an order that leaves roles out: %v", err)
	}
	if err := st.SetRoleMember(owner.ID, friends.ID, "ninguem", true); !errors.Is(err, ErrNotFound) {
		t.Fatalf("an unknown person: %v", err)
	}
	if err := st.DeleteRole(owner.ID, "nope"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("an unknown role: %v", err)
	}
}

func TestModeratorRemovesOthersOnlyInTextChannels(t *testing.T) {
	st, _, mod, bia, _, _, _ := rolesFixture(t)
	inGeral, err := st.PostMessage(bia.ID, "geral", "oi", nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := st.DeleteMessage(mod.ID, inGeral.ID, OwnOnly); !errors.Is(err, ErrNotFound) {
		t.Fatalf("without moderation: %v", err)
	}
	if _, err := st.DeleteMessage(mod.ID, inGeral.ID, OthersInText); err != nil {
		t.Fatalf("a moderator in a text channel: %v", err)
	}
	dm, err := st.EnsureDM(mod.ID, bia.ID)
	if err != nil {
		t.Fatal(err)
	}
	private, err := st.PostMessage(bia.ID, dm.ID, "só nós", nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := st.DeleteMessage(mod.ID, private.ID, OthersInText); !errors.Is(err, ErrNotFound) {
		t.Fatalf("a role does not reach into a conversation of two: %v", err)
	}
}
