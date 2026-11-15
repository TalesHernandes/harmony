package store

import (
	"database/sql"
	"errors"
	"math"
	"strings"
)

// Roles (D12c), as on Discord, cut to what twenty friends use.
//
// The owner is not a role. They can do everything, sit above every role, and
// nobody can take anything from them. A role only gives: somebody with no role
// can do exactly what everybody could do before roles existed.
//
// The rule for Manage roles is Discord's: somebody who has it touches (edits,
// deletes, gives, takes, moves) only roles strictly **below** their own highest
// role, never gives a role a permission they do not have themselves, and never
// touches the owner. Without it a moderator promotes themself to anything.
//
// Every check here reads the asker's roles inside the same transaction as the
// write, so a role taken away a moment ago is already gone for the next
// request: nothing about a person's rank is cached anywhere.

// Permission is a set of things a role lets somebody do. Bits, so a fourth
// permission is a new constant and not a migration.
type Permission int64

const (
	// PermManageChannels creates, renames, orders and deletes channels and
	// folders. It is what `can_manage` in GET /api/channels has meant since
	// D12c.
	PermManageChannels Permission = 1 << iota
	// PermManageMessages removes other people's messages and pins messages in
	// text channels. In a conversation of two, both people still pin, as
	// before, and nobody but the owner removes the other's message.
	PermManageMessages
	// PermManageRoles creates, edits, orders, gives and takes roles below the
	// asker's own highest role.
	PermManageRoles

	// AllPermissions is what the owner has.
	AllPermissions = PermManageChannels | PermManageMessages | PermManageRoles
)

// permissionNames are the words the API speaks. The order is the order the
// settings screen lists them in.
var permissionNames = []struct {
	p    Permission
	name string
}{
	{PermManageChannels, "manage_channels"},
	{PermManageMessages, "manage_messages"},
	{PermManageRoles, "manage_roles"},
}

// Names lists the permissions in p, in the fixed order above. Never nil, so
// it is [] and not null in JSON.
func (p Permission) Names() []string {
	out := []string{}
	for _, n := range permissionNames {
		if p&n.p != 0 {
			out = append(out, n.name)
		}
	}
	return out
}

// ParsePermissions turns the API's words back into a set. A word it does not
// know is an error rather than ignored: a page that asks for a permission
// this server does not have should hear so.
func ParsePermissions(names []string) (Permission, bool) {
	var p Permission
	for _, name := range names {
		found := false
		for _, n := range permissionNames {
			if n.name == name {
				p |= n.p
				found = true
				break
			}
		}
		if !found {
			return 0, false
		}
	}
	return p, true
}

// MaxRoles is how many roles the server holds. Discord allows 250; twenty
// friends with more than fifty roles is a mistake, not a hierarchy.
const MaxRoles = 50

// MaxRoleNameRunes is a role's name length, the same as a folder's.
const MaxRoleNameRunes = MaxCategoryNameRunes

var (
	// ErrBadRoleName is an empty, over-long or control-character name.
	ErrBadRoleName = errors.New("store: bad role name")
	// ErrBadColor is a colour that is neither "" nor one of RoleColors.
	ErrBadColor = errors.New("store: bad color")
	// ErrRoleLimit is one role past MaxRoles.
	ErrRoleLimit = errors.New("store: role limit")
	// ErrStaleRoles is an order that does not name exactly the roles there
	// are now.
	ErrStaleRoles = errors.New("store: stale roles")
)

// Role is one role, with the usernames of the live accounts that have it.
type Role struct {
	ID       string
	Name     string
	Color    string
	Position int
	Hoist    bool
	Perms    Permission
	Members  []string
}

// Rank is where somebody stands: what they may do, and how high their highest
// role is. Top is that role's position (0 is the highest role); somebody with
// no role has Top = math.MaxInt, below every role.
type Rank struct {
	Owner bool
	Perms Permission
	Top   int
}

// Has reports whether the rank includes p. The owner has everything.
func (r Rank) Has(p Permission) bool {
	return r.Owner || r.Perms&p == p
}

// Above reports whether a role at position is strictly below this rank's
// highest role — the only roles a Manage roles holder may touch.
func (r Rank) Above(position int) bool {
	return r.Owner || position > r.Top
}

type queryRower interface {
	QueryRow(string, ...any) *sql.Row
	Query(string, ...any) (*sql.Rows, error)
}

// RankOf reads where somebody stands now. A disabled account has no rank: it
// cannot sign in, and a session that is still open is closed by the admin
// path, but the rank says so too.
func (s *Store) RankOf(userID int64) (Rank, error) {
	return rankOf(s.db, userID)
}

func rankOf(q queryRower, userID int64) (Rank, error) {
	var admin int
	var disabled sql.NullInt64
	err := q.QueryRow(`SELECT is_admin, disabled_at FROM users WHERE id = ?`, userID).Scan(&admin, &disabled)
	if errors.Is(err, sql.ErrNoRows) {
		return Rank{Top: math.MaxInt}, nil
	}
	if err != nil {
		return Rank{}, err
	}
	if disabled.Valid {
		return Rank{Top: math.MaxInt}, nil
	}
	if admin == 1 {
		return Rank{Owner: true, Perms: AllPermissions, Top: -1}, nil
	}
	// SQLite has no OR aggregate: the bits are joined one role at a time.
	rows, err := q.Query(`SELECT r.perms, r.position FROM user_roles ur JOIN roles r ON r.id = ur.role_id WHERE ur.user_id = ?`, userID)
	if err != nil {
		return Rank{}, err
	}
	defer rows.Close()
	r := Rank{Top: math.MaxInt}
	for rows.Next() {
		var bits int64
		var position int
		if err := rows.Scan(&bits, &position); err != nil {
			return Rank{}, err
		}
		r.Perms |= Permission(bits) & AllPermissions
		r.Top = min(r.Top, position)
	}
	return r, rows.Err()
}

// Roles lists every role, highest first, each with the usernames of the live
// accounts that have it. Everybody may read it: it is what colours the names
// and groups the member list.
func (s *Store) Roles() ([]Role, error) {
	return roles(s.db)
}

func roles(q queryRower) ([]Role, error) {
	rows, err := q.Query(`SELECT id, name, color, position, hoist, perms FROM roles ORDER BY position, id`)
	if err != nil {
		return nil, err
	}
	out := []Role{}
	index := map[string]int{}
	for rows.Next() {
		var r Role
		var hoist int
		var perms int64
		if err := rows.Scan(&r.ID, &r.Name, &r.Color, &r.Position, &hoist, &perms); err != nil {
			rows.Close()
			return nil, err
		}
		r.Hoist = hoist == 1
		r.Perms = Permission(perms) & AllPermissions
		r.Members = []string{}
		index[r.ID] = len(out)
		out = append(out, r)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}
	rows, err = q.Query(`SELECT ur.role_id, u.username FROM user_roles ur JOIN users u ON u.id = ur.user_id
		WHERE u.disabled_at IS NULL ORDER BY u.id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var roleID, username string
		if err := rows.Scan(&roleID, &username); err != nil {
			return nil, err
		}
		if i, ok := index[roleID]; ok {
			out[i].Members = append(out[i].Members, username)
		}
	}
	return out, rows.Err()
}

// RoleInput is what creating a role carries. Editing carries the same fields
// as pointers (RolePatch), so a page can change one thing without restating
// the others.
type RoleInput struct {
	Name  string
	Color string
	Hoist bool
	Perms Permission
}

// RolePatch is an edit; a nil field is left as it is.
type RolePatch struct {
	Name  *string
	Color *string
	Hoist *bool
	Perms *Permission
}

// RoleColors is the palette a role's colour is picked from — Discord's twenty
// swatches. A fixed list rather than any "#rrggbb" because the web client may
// not set a style at run time (security_test.go,
// TestTheClientSourceHasNoInlineStyle): each colour is a class in style.css,
// and a colour outside the list would be a name drawn in no colour at all.
// The apps read the hex as it is.
var RoleColors = []string{
	"#48e5c2", "#7bed9f", "#74b9ff", "#c39bd3", "#f78fb3", "#ffeaa7", "#f8c291", "#ff7675", "#dfe6e9", "#ffffff",
	"#1abc9c", "#2ecc71", "#3498db", "#9b59b6", "#e91e63", "#f1c40f", "#e67e22", "#e74c3c", "#95a5a6", "#607d8b",
	"#11806a", "#1f8b4c", "#206694", "#71368a", "#ad1457", "#c27c0e", "#a84300", "#992d22", "#979c9f", "#546e7a",
	"#00bcd4", "#a3e635", "#5865f2", "#7c4dff", "#e040fb", "#ffd700", "#ff7f50", "#dc143c", "#8d6e63", "#b0bec5",
	"#004d40", "#1b5e20", "#1a3a8a", "#4a148c", "#880e4f", "#8a6d00", "#6d3410", "#7f1d1d", "#4e342e", "#37474f",
}

// NormalizeColor lower-cases a "#RRGGBB" so two spellings of one colour are
// one value.
func NormalizeColor(c string) string {
	return strings.ToLower(strings.TrimSpace(c))
}

// ValidColor reports whether a normalised colour is "" (no colour: the name
// keeps the text colour, and the next role down that has one shows instead)
// or one of RoleColors.
func ValidColor(c string) bool {
	if c == "" {
		return true
	}
	for _, k := range RoleColors {
		if c == k {
			return true
		}
	}
	return false
}

func roleNameFree(tx *sql.Tx, name, except string) error {
	var taken int
	if err := tx.QueryRow(`SELECT COUNT(*) FROM roles WHERE name = ? COLLATE NOCASE AND id <> ?`,
		name, except).Scan(&taken); err != nil {
		return err
	}
	if taken > 0 {
		return ErrNameTaken
	}
	return nil
}

// CreateRole adds a role at the bottom of the list. The asker needs Manage
// roles, and may only put in it permissions they have.
func (s *Store) CreateRole(actor int64, in RoleInput) (Role, error) {
	id, err := newID()
	if err != nil {
		return Role{}, err
	}
	tx, err := s.db.Begin()
	if err != nil {
		return Role{}, err
	}
	defer tx.Rollback()
	rank, err := rankOf(tx, actor)
	if err != nil {
		return Role{}, err
	}
	if !rank.Has(PermManageRoles) || in.Perms&^rank.Perms != 0 || in.Perms&^AllPermissions != 0 {
		return Role{}, ErrNotAllowed
	}
	// The inputs after the permission, so somebody who may not create a role
	// hears 403 whatever they sent.
	name := NormalizeCategoryName(in.Name)
	if !validRoleName(name) {
		return Role{}, ErrBadRoleName
	}
	color := NormalizeColor(in.Color)
	if !ValidColor(color) {
		return Role{}, ErrBadColor
	}
	var count, position int
	if err := tx.QueryRow(`SELECT COUNT(*), COALESCE(MAX(position), -1) + 1 FROM roles`).Scan(&count, &position); err != nil {
		return Role{}, err
	}
	if count >= MaxRoles {
		return Role{}, ErrRoleLimit
	}
	if err := roleNameFree(tx, name, ""); err != nil {
		return Role{}, err
	}
	if _, err := tx.Exec(`INSERT INTO roles (id, name, color, position, hoist, perms, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)`,
		id, name, color, position, boolInt(in.Hoist), int64(in.Perms), s.now().UTC().Unix()); err != nil {
		return Role{}, err
	}
	if err := tx.Commit(); err != nil {
		return Role{}, err
	}
	return Role{ID: id, Name: name, Color: color, Position: position, Hoist: in.Hoist, Perms: in.Perms, Members: []string{}}, nil
}

func validRoleName(name string) bool {
	return ValidCategoryName(name)
}

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

// roleForChange reads a role inside tx and checks the asker may touch it: they
// have Manage roles and the role is strictly below their highest one. A role
// that does not exist is ErrNotFound — everybody can see the list, so there is
// nothing to hide and the two answers may differ.
func roleForChange(tx *sql.Tx, actor int64, id string) (Rank, int, Permission, error) {
	rank, err := rankOf(tx, actor)
	if err != nil {
		return Rank{}, 0, 0, err
	}
	if !rank.Has(PermManageRoles) {
		return Rank{}, 0, 0, ErrNotAllowed
	}
	var position int
	var perms int64
	err = tx.QueryRow(`SELECT position, perms FROM roles WHERE id = ?`, id).Scan(&position, &perms)
	if errors.Is(err, sql.ErrNoRows) {
		return Rank{}, 0, 0, ErrNotFound
	}
	if err != nil {
		return Rank{}, 0, 0, err
	}
	if !rank.Above(position) {
		return Rank{}, 0, 0, ErrNotAllowed
	}
	return rank, position, Permission(perms), nil
}

// UpdateRole edits a role below the asker. Of the permissions, the asker may
// switch on or off only the ones they have themselves; the others stay as they
// were (Discord: you may edit a role that has a permission you lack, but not
// that permission).
func (s *Store) UpdateRole(actor int64, id string, p RolePatch) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	rank, _, old, err := roleForChange(tx, actor, id)
	if err != nil {
		return err
	}
	if p.Name != nil {
		name := NormalizeCategoryName(*p.Name)
		if !validRoleName(name) {
			return ErrBadRoleName
		}
		if err := roleNameFree(tx, name, id); err != nil {
			return err
		}
		if _, err := tx.Exec(`UPDATE roles SET name = ? WHERE id = ?`, name, id); err != nil {
			return err
		}
	}
	if p.Color != nil {
		color := NormalizeColor(*p.Color)
		if !ValidColor(color) {
			return ErrBadColor
		}
		if _, err := tx.Exec(`UPDATE roles SET color = ? WHERE id = ?`, color, id); err != nil {
			return err
		}
	}
	if p.Hoist != nil {
		if _, err := tx.Exec(`UPDATE roles SET hoist = ? WHERE id = ?`, boolInt(*p.Hoist), id); err != nil {
			return err
		}
	}
	if p.Perms != nil {
		next := *p.Perms
		if next&^AllPermissions != 0 || (next^old)&^rank.Perms != 0 {
			return ErrNotAllowed
		}
		if _, err := tx.Exec(`UPDATE roles SET perms = ? WHERE id = ?`, int64(next), id); err != nil {
			return err
		}
	}
	return tx.Commit()
}

// DeleteRole removes a role below the asker, and with it every "who has it"
// row, in one transaction: the next request of anybody who had it is already
// answered without it.
func (s *Store) DeleteRole(actor int64, id string) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, _, _, err := roleForChange(tx, actor, id); err != nil {
		return err
	}
	if _, err := tx.Exec(`DELETE FROM user_roles WHERE role_id = ?`, id); err != nil {
		return err
	}
	if _, err := tx.Exec(`DELETE FROM roles WHERE id = ?`, id); err != nil {
		return err
	}
	// Close the gap, so positions stay 0..n-1 and "the place in the list" is
	// the same number on every page.
	if err := renumberRoles(tx); err != nil {
		return err
	}
	return tx.Commit()
}

func renumberRoles(tx *sql.Tx) error {
	rows, err := tx.Query(`SELECT id FROM roles ORDER BY position, id`)
	if err != nil {
		return err
	}
	var ids []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return err
		}
		ids = append(ids, id)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return err
	}
	for i, id := range ids {
		if _, err := tx.Exec(`UPDATE roles SET position = ? WHERE id = ?`, i, id); err != nil {
			return err
		}
	}
	return nil
}

// OrderRoles writes the whole order of the list at once, highest first. Like
// SaveLayout it has to name exactly the roles there are, none twice, or it is
// ErrStaleRoles and nothing is written.
//
// Somebody who is not the owner may only move roles below their own highest
// one, among themselves: every role from the top down to and including their
// own highest has to stay exactly where it is. So nobody lifts a role over
// their own, and nobody drops their own to pull one above it.
func (s *Store) OrderRoles(actor int64, ids []string) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	rank, err := rankOf(tx, actor)
	if err != nil {
		return err
	}
	if !rank.Has(PermManageRoles) {
		return ErrNotAllowed
	}
	rows, err := tx.Query(`SELECT id FROM roles ORDER BY position, id`)
	if err != nil {
		return err
	}
	var current []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return err
		}
		current = append(current, id)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return err
	}
	if len(ids) != len(current) {
		return ErrStaleRoles
	}
	known := map[string]bool{}
	for _, id := range current {
		known[id] = false
	}
	for _, id := range ids {
		seen, ok := known[id]
		if !ok || seen {
			return ErrStaleRoles
		}
		known[id] = true
	}
	if !rank.Owner {
		for i := 0; i <= rank.Top && i < len(current); i++ {
			if ids[i] != current[i] {
				return ErrNotAllowed
			}
		}
	}
	for i, id := range ids {
		if _, err := tx.Exec(`UPDATE roles SET position = ? WHERE id = ?`, i, id); err != nil {
			return err
		}
	}
	return tx.Commit()
}

// SetRoleMember gives a role to somebody (on) or takes it away. The role has to
// be below the asker; the person may be anybody, the asker included (Discord
// lets you give yourself a role below your own, and take one off), except that
// only the owner touches the owner's roles. A username nobody has is ErrNotFound. Giving twice, or taking a role
// somebody does not have, is not an error: the page says which way it should
// end up.
func (s *Store) SetRoleMember(actor int64, roleID, username string, on bool) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	rank, _, perms, err := roleForChange(tx, actor, roleID)
	if err != nil {
		return err
	}
	// Giving a role is giving what it carries: a Manage roles holder who could
	// hand out a role with a permission they lack would hand it to themself
	// (Codex's audit, 02/11/2026). Taking one away gives nothing.
	if on && perms&^rank.Perms != 0 {
		return ErrNotAllowed
	}
	var target int64
	var admin int
	err = tx.QueryRow(`SELECT id, is_admin FROM users WHERE username = ? COLLATE NOCASE`, username).Scan(&target, &admin)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return err
	}
	// The owner's roles are the owner's own business: they wear one for the
	// colour and the group (rankOf ignores it, so it neither gives nor takes),
	// and nobody else may put one on them or take it off.
	if admin == 1 && !rank.Owner {
		return ErrNotAllowed
	}
	if on {
		_, err = tx.Exec(`INSERT OR IGNORE INTO user_roles (user_id, role_id) VALUES (?, ?)`, target, roleID)
	} else {
		_, err = tx.Exec(`DELETE FROM user_roles WHERE user_id = ? AND role_id = ?`, target, roleID)
	}
	if err != nil {
		return err
	}
	return tx.Commit()
}
