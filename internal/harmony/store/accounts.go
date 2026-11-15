package store

import (
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/base32"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"regexp"
	"strings"
	"time"
)

// SessionTokenBytes is 32: the 256-bit random session token CLAUDE.md asks for.
const SessionTokenBytes = 32

// InviteCodeBytes is 16. An invite lives for days, is single use and is rate
// limited, so 128 bits is already far past guessable.
const InviteCodeBytes = 16

// SessionTTL and InviteTTL are the defaults. Both are absolute: a session does
// not become immortal by being used, it is re-issued.
const (
	SessionTTL = 30 * 24 * time.Hour
	InviteTTL  = 7 * 24 * time.Hour
)

// User is an account.
type User struct {
	SecurityVersion int64
	Email           string
	EmailVerified   bool
	TwoFA           string
	ID              int64
	Username        string
	DisplayName     string
	Avatar          string
	IsAdmin         bool
	CreatedAt       time.Time
	Disabled        bool
}

// Invite is one single-use signup ticket. The code itself exists only in the
// return value of CreateInvite; it is never read back out of the database.
type Invite struct {
	ID        string
	CreatedBy int64
	Note      string
	CreatedAt time.Time
	ExpiresAt time.Time
	UsedAt    time.Time
	UsedBy    int64
	// The two names are filled in by the list queries, which join the users
	// table. They are the record of who let whom in, and they outlive the
	// invite's usefulness on purpose: a spent invite is never purged.
	// CreatedByName is empty for the bootstrap invite, the one minted from the
	// command line before any account existed.
	CreatedByName string
	UsedByName    string
}

// Used reports whether the invite has been spent.
func (i Invite) Used() bool { return !i.UsedAt.IsZero() }

// Session is one signed-in device.
type Session struct {
	ID         string
	UserID     int64
	CreatedAt  time.Time
	ExpiresAt  time.Time
	LastSeenAt time.Time
	Revoked    bool
	UserAgent  string
}

var usernameRE = regexp.MustCompile(`^[a-z0-9](?:[a-z0-9._-]{1,30}[a-z0-9])$`)

// NormalizeUsername lowercases and trims. Two people cannot end up as "Tales"
// and "tales"; the column is COLLATE NOCASE for the same reason.
func NormalizeUsername(s string) string { return strings.ToLower(strings.TrimSpace(s)) }

// ValidUsername reports whether s is an acceptable username: 3 to 32 characters
// of lowercase letters, digits, dot, dash and underscore, not at the edges.
func ValidUsername(s string) bool { return usernameRE.MatchString(s) }

// NormalizeInviteCode makes codes tolerant of how they arrive: pasted from a
// message with spaces or dashes, typed in lower case.
func NormalizeInviteCode(s string) string {
	s = strings.ToUpper(strings.TrimSpace(s))
	return strings.NewReplacer("-", "", " ", "", "\t", "").Replace(s)
}

var inviteEncoding = base32.StdEncoding.WithPadding(base32.NoPadding)

func randomBytes(n int) ([]byte, error) {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return nil, err
	}
	return b, nil
}

// newID mints a public handle for a row. It names a thing in a URL; it is not
// a secret and never authorises anything by itself.
func newID() (string, error) {
	b, err := randomBytes(12)
	if err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}

func hashSecret(s string) string {
	sum := sha256.Sum256([]byte(s))
	return hex.EncodeToString(sum[:])
}

func fromUnix(n sql.NullInt64) time.Time {
	if !n.Valid || n.Int64 == 0 {
		return time.Time{}
	}
	return time.Unix(n.Int64, 0).UTC()
}

// ---------------------------------------------------------------- invites

// CreateInvite mints a single-use code valid for ttl and returns the plaintext
// code exactly once. createdBy may be 0 for the bootstrap invite, the one made
// from the command line before any account exists.
func (s *Store) CreateInvite(createdBy int64, note string, ttl time.Duration) (string, Invite, error) {
	raw, err := randomBytes(InviteCodeBytes)
	if err != nil {
		return "", Invite{}, err
	}
	code := inviteEncoding.EncodeToString(raw)
	id, err := newID()
	if err != nil {
		return "", Invite{}, err
	}
	now := s.now().UTC()
	inv := Invite{
		ID:        id,
		CreatedBy: createdBy,
		Note:      note,
		CreatedAt: now,
		ExpiresAt: now.Add(ttl),
	}
	var creator any
	if createdBy != 0 {
		creator = createdBy
	}
	_, err = s.db.Exec(
		`INSERT INTO invites (id, code_hash, created_by, note, created_at, expires_at)
		 VALUES (?, ?, ?, ?, ?, ?)`,
		inv.ID, hashSecret(code), creator, note, now.Unix(), inv.ExpiresAt.Unix())
	if err != nil {
		return "", Invite{}, err
	}
	return code, inv, nil
}

// InviteUsable reports whether code names an invite that exists, has not been
// spent and has not expired. It is what makes the signup page exist at all.
func (s *Store) InviteUsable(code string) bool {
	code = NormalizeInviteCode(code)
	if code == "" {
		return false
	}
	var usedAt sql.NullInt64
	var expiresAt int64
	err := s.db.QueryRow(
		`SELECT used_at, expires_at FROM invites WHERE code_hash = ?`,
		hashSecret(code)).Scan(&usedAt, &expiresAt)
	if err != nil {
		return false
	}
	return !usedAt.Valid && s.now().Unix() < expiresAt
}

// inviteColumns is shared by the two list queries so they cannot drift apart.
const inviteColumns = `SELECT i.id, i.created_by, i.note, i.created_at, i.expires_at,
	                          i.used_at, i.used_by, c.username, u.username
	                     FROM invites i
	                LEFT JOIN users c ON c.id = i.created_by
	                LEFT JOIN users u ON u.id = i.used_by`

// ListInvites returns the invites a user created, newest first.
func (s *Store) ListInvites(createdBy int64) ([]Invite, error) {
	return s.scanInvites(s.db.Query(
		inviteColumns+` WHERE i.created_by = ? ORDER BY i.created_at DESC`, createdBy))
}

// ListAllInvites returns every invite ever minted, including the bootstrap one
// that has no creator. It is the owner's view: who was let in, by whom, and
// when. Only an admin caller reaches it.
func (s *Store) ListAllInvites() ([]Invite, error) {
	return s.scanInvites(s.db.Query(inviteColumns + ` ORDER BY i.created_at DESC`))
}

func (s *Store) scanInvites(rows *sql.Rows, err error) ([]Invite, error) {
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Invite
	for rows.Next() {
		var inv Invite
		var by, usedAt, usedBy sql.NullInt64
		var createdName, usedName sql.NullString
		var created, expires int64
		if err := rows.Scan(&inv.ID, &by, &inv.Note, &created, &expires,
			&usedAt, &usedBy, &createdName, &usedName); err != nil {
			return nil, err
		}
		inv.CreatedBy = by.Int64
		inv.CreatedAt = time.Unix(created, 0).UTC()
		inv.ExpiresAt = time.Unix(expires, 0).UTC()
		inv.UsedAt = fromUnix(usedAt)
		inv.UsedBy = usedBy.Int64
		inv.CreatedByName = createdName.String
		inv.UsedByName = usedName.String
		out = append(out, inv)
	}
	return out, rows.Err()
}

// ListUsers returns every account, oldest first. The owner's view again, and
// what `harmony -audit` prints.
func (s *Store) ListUsers() ([]User, error) {
	rows, err := s.db.Query(
		`SELECT id, username, display_name, is_admin, created_at, disabled_at, avatar_version
		   FROM users ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []User
	for rows.Next() {
		var u User
		var admin int
		var created int64
		var disabled sql.NullInt64
		if err := rows.Scan(&u.ID, &u.Username, &u.DisplayName, &admin, &created, &disabled, &u.Avatar); err != nil {
			return nil, err
		}
		u.IsAdmin = admin == 1
		u.CreatedAt = time.Unix(created, 0).UTC()
		u.Disabled = disabled.Valid
		out = append(out, u)
	}
	return out, rows.Err()
}

// RevokeInvite deletes an unused invite. The user id is not a filter bolted on
// afterwards: it is part of the WHERE, so asking for somebody elses invite id
// matches nothing and reports ErrNotFound, the same answer a made-up id gets.
func (s *Store) RevokeInvite(userID int64, id string) error {
	res, err := s.db.Exec(
		`DELETE FROM invites WHERE id = ? AND created_by = ? AND used_at IS NULL`, id, userID)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

// RevokeAnyInvite cancels an unused invite whoever made it, including the
// bootstrap one that has no creator. Admin only, and it is a separate method
// rather than a flag on the one above so that no ordinary call can reach it by
// passing the wrong argument.
func (s *Store) RevokeAnyInvite(id string) error {
	res, err := s.db.Exec(`DELETE FROM invites WHERE id = ? AND used_at IS NULL`, id)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

// ---------------------------------------------------------------- signup

// SignUp consumes code and creates the account in one transaction. The invite
// is spent by an UPDATE that requires it to still be unused, so two people
// racing on the same code produce exactly one account: whoever loses sees
// ErrInviteBad, not a second free membership.
//
// The first account ever created is the owner's and becomes admin.
func (s *Store) SignUp(code, username, displayName, passwordHash string, emails ...string) (User, error) {
	username = NormalizeUsername(username)
	if !ValidUsername(username) {
		return User{}, ErrNotFound
	}
	code = NormalizeInviteCode(code)
	now := s.now().UTC()

	tx, err := s.db.Begin()
	if err != nil {
		return User{}, err
	}
	defer tx.Rollback()

	res, err := tx.Exec(
		`UPDATE invites SET used_at = ? WHERE code_hash = ? AND used_at IS NULL AND expires_at > ?`,
		now.Unix(), hashSecret(code), now.Unix())
	if err != nil {
		return User{}, err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return User{}, ErrInviteBad
	}

	var existing int
	if err := tx.QueryRow(`SELECT count(*) FROM users`).Scan(&existing); err != nil {
		return User{}, err
	}
	isAdmin := 0
	if existing == 0 {
		isAdmin = 1
	}

	var email any
	if len(emails) > 0 {
		email = NormalizeEmail(emails[0])
	}
	res, err = tx.Exec(
		`INSERT INTO users (username, display_name, password_hash, is_admin, created_at, email)
		 VALUES (?, ?, ?, ?, ?, ?)`,
		username, displayName, passwordHash, isAdmin, now.Unix(), email)
	if err != nil {
		if isUniqueViolation(err) {
			return User{}, ErrNameTaken
		}
		return User{}, err
	}
	id, err := res.LastInsertId()
	if err != nil {
		return User{}, err
	}
	if _, err := tx.Exec(`UPDATE invites SET used_by = ? WHERE code_hash = ?`, id, hashSecret(code)); err != nil {
		return User{}, err
	}
	// Join every text and voice channel, in the same transaction as the account
	// itself. A channel is "everybody's" only because everybody has a row here;
	// an account created without them would see an empty app and there would
	// be no way in from the outside to fix it.
	if _, err := tx.Exec(
		`INSERT INTO channel_members (channel_id, user_id, added_at, last_read_message_id)
		 SELECT id, ?, ?, 0 FROM channels WHERE kind IN ('text', 'voice') AND deleted_at IS NULL`, id, now.Unix()); err != nil {
		return User{}, err
	}
	if err := tx.Commit(); err != nil {
		return User{}, err
	}
	return User{
		ID: id, Username: username, DisplayName: displayName,
		IsAdmin: isAdmin == 1, CreatedAt: now,
		Email: func() string {
			if email == nil {
				return ""
			}
			return email.(string)
		}(),
	}, nil
}

func isUniqueViolation(err error) bool {
	// The pure-Go driver reports this as a plain error string; matching it is
	// ugly but the alternative is a pre-check with a race in the gap.
	return err != nil && strings.Contains(strings.ToLower(err.Error()), "unique constraint")
}

// UserByUsername looks an account up for login, returning the stored hash.
func (s *Store) UserByUsername(username string) (User, string, error) {
	return s.scanUser(s.db.QueryRow(
		`SELECT id, username, display_name, password_hash, is_admin, created_at, disabled_at, avatar_version, COALESCE(email,''), email_verified_at IS NOT NULL, twofa, security_version
		   FROM users WHERE username = ?`, NormalizeUsername(username)))
}

// UserByID looks an account up by id.
func (s *Store) UserByID(id int64) (User, error) {
	u, _, err := s.scanUser(s.db.QueryRow(
		`SELECT id, username, display_name, password_hash, is_admin, created_at, disabled_at, avatar_version, COALESCE(email,''), email_verified_at IS NOT NULL, twofa, security_version
		   FROM users WHERE id = ?`, id))
	return u, err
}

func (s *Store) scanUser(row *sql.Row) (User, string, error) {
	var u User
	var hash string
	var admin int
	var created int64
	var disabled sql.NullInt64
	err := row.Scan(&u.ID, &u.Username, &u.DisplayName, &hash, &admin, &created, &disabled, &u.Avatar, &u.Email, &u.EmailVerified, &u.TwoFA, &u.SecurityVersion)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, "", ErrNotFound
	}
	if err != nil {
		return User{}, "", err
	}
	u.IsAdmin = admin == 1
	u.CreatedAt = time.Unix(created, 0).UTC()
	u.Disabled = disabled.Valid
	return u, hash, nil
}

// CountUsers reports how many accounts exist.
func (s *Store) CountUsers() (int, error) {
	var n int
	err := s.db.QueryRow(`SELECT count(*) FROM users`).Scan(&n)
	return n, err
}

// SetPasswordHash replaces a password and revokes every session the user has,
// including the one asking: changing a password is the move you make when you
// think somebody else has it.
func (s *Store) SetPasswordHash(userID int64, hash string) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`UPDATE users SET password_hash = ?, security_version=security_version+1,totp_pending='',totp_pending_until=0 WHERE id = ?`, hash, userID); err != nil {
		return err
	}
	if _, err := tx.Exec(
		`UPDATE sessions SET revoked_at = ? WHERE user_id = ? AND revoked_at IS NULL`,
		s.now().Unix(), userID); err != nil {
		return err
	}
	return tx.Commit()
}

// ---------------------------------------------------------------- sessions

// CreateSession issues a 256-bit token and returns it once. Only its hash is
// stored.
func (s *Store) CreateSession(userID int64, userAgent string, ttl time.Duration) (string, Session, error) {
	return s.CreateSessionForHash(userID, "", userAgent, ttl)
}

// CreateSessionForHash is CreateSession with a condition: when hash is not
// empty, the row is only written while the account still has that exact
// password hash, and ErrCredentials comes back when it does not.
//
// Verifying a password takes a tenth of a second, and a password reset ends
// every session of the account. Without this condition a sign-in that started
// before the reset and finished after it would insert its session afterwards —
// the one device the reset was meant to cut off would be the one left standing.
func (s *Store) CreateSessionForHash(userID int64, hash, userAgent string, ttl time.Duration, versions ...int64) (string, Session, error) {
	version := int64(-1)
	if len(versions) > 0 {
		version = versions[0]
	}
	raw, err := randomBytes(SessionTokenBytes)
	if err != nil {
		return "", Session{}, err
	}
	token := base64.RawURLEncoding.EncodeToString(raw)
	id, err := newID()
	if err != nil {
		return "", Session{}, err
	}
	now := s.now().UTC()
	sess := Session{
		ID: id, UserID: userID, CreatedAt: now,
		ExpiresAt: now.Add(ttl), LastSeenAt: now,
		UserAgent: truncate(userAgent, 200),
	}
	// One statement, so the check and the insert cannot be separated: the row
	// appears only if that password hash is still the account's.
	res, err := s.db.Exec(
		`INSERT INTO sessions (id, token_hash, user_id, created_at, expires_at, last_seen_at, user_agent)
		 SELECT ?, ?, ?, ?, ?, ?, ? FROM users WHERE id = ? AND disabled_at IS NULL AND (? = '' OR password_hash = ?) AND (? < 0 OR security_version = ?)`,
		sess.ID, hashSecret(token), userID, now.Unix(), sess.ExpiresAt.Unix(), now.Unix(), sess.UserAgent,
		userID, hash, hash, version, version)
	if err != nil {
		return "", Session{}, err
	}
	if n, err := res.RowsAffected(); err == nil && n == 0 {
		return "", Session{}, ErrCredentials
	}
	return token, sess, nil
}

// Authenticate resolves a token to its session and user, refusing expired,
// revoked and disabled ones. It also stamps last_seen_at, which is what makes
// the device list useful.
func (s *Store) Authenticate(token string) (Session, User, error) {
	var sess Session
	var created, expires, lastSeen int64
	var revoked sql.NullInt64
	err := s.db.QueryRow(
		`SELECT id, user_id, created_at, expires_at, last_seen_at, revoked_at, user_agent
		   FROM sessions WHERE token_hash = ?`, hashSecret(token)).
		Scan(&sess.ID, &sess.UserID, &created, &expires, &lastSeen, &revoked, &sess.UserAgent)
	if errors.Is(err, sql.ErrNoRows) {
		return Session{}, User{}, ErrSessionStale
	}
	if err != nil {
		return Session{}, User{}, err
	}
	now := s.now().UTC()
	if revoked.Valid || now.Unix() >= expires {
		return Session{}, User{}, ErrSessionStale
	}
	sess.CreatedAt = time.Unix(created, 0).UTC()
	sess.ExpiresAt = time.Unix(expires, 0).UTC()
	sess.LastSeenAt = time.Unix(lastSeen, 0).UTC()

	user, err := s.UserByID(sess.UserID)
	if err != nil || user.Disabled {
		return Session{}, User{}, ErrSessionStale
	}
	// A minute of granularity is plenty and keeps a chatty client from writing
	// on every single request.
	if now.Sub(sess.LastSeenAt) > time.Minute {
		if _, err := s.db.Exec(`UPDATE sessions SET last_seen_at = ? WHERE id = ?`, now.Unix(), sess.ID); err != nil {
			return Session{}, User{}, err
		}
		sess.LastSeenAt = now
	}
	return sess, user, nil
}

// ListSessions returns a user's live sessions, newest first.
func (s *Store) ListSessions(userID int64) ([]Session, error) {
	rows, err := s.db.Query(
		`SELECT id, user_id, created_at, expires_at, last_seen_at, user_agent
		   FROM sessions
		  WHERE user_id = ? AND revoked_at IS NULL AND expires_at > ?
		  ORDER BY created_at DESC`, userID, s.now().Unix())
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Session
	for rows.Next() {
		var sess Session
		var created, expires, lastSeen int64
		if err := rows.Scan(&sess.ID, &sess.UserID, &created, &expires, &lastSeen, &sess.UserAgent); err != nil {
			return nil, err
		}
		sess.CreatedAt = time.Unix(created, 0).UTC()
		sess.ExpiresAt = time.Unix(expires, 0).UTC()
		sess.LastSeenAt = time.Unix(lastSeen, 0).UTC()
		out = append(out, sess)
	}
	return out, rows.Err()
}

// RevokeSession logs one device out. Like RevokeInvite, ownership is in the
// WHERE clause, so another user's session id is indistinguishable from a
// nonexistent one.
func (s *Store) RevokeSession(userID int64, id string) error {
	res, err := s.db.Exec(
		`UPDATE sessions SET revoked_at = ?
		  WHERE id = ? AND user_id = ? AND revoked_at IS NULL`,
		s.now().Unix(), id, userID)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

// SessionAlive says whether a session is still good, by id rather than by
// token: the open sockets know which session they belong to but never hold the
// token itself. It is the same three conditions Authenticate applies — not
// revoked, not expired, account not disabled — and it stamps nothing, because
// a socket being asked about is not the person doing something.
func (s *Store) SessionAlive(id string) (bool, error) {
	var n int
	err := s.db.QueryRow(
		`SELECT count(*) FROM sessions s JOIN users u ON u.id = s.user_id
		  WHERE s.id = ? AND s.revoked_at IS NULL AND s.expires_at > ? AND u.disabled_at IS NULL`,
		id, s.now().Unix()).Scan(&n)
	if err != nil {
		return false, err
	}
	return n > 0, nil
}

// RevokeAllSessions is "log out of every device". Pass the current session id
// as keep to stay signed in here, or "" to be logged out too.
func (s *Store) RevokeAllSessions(userID int64, keep string) (int, error) {
	res, err := s.db.Exec(
		`UPDATE sessions SET revoked_at = ?
		  WHERE user_id = ? AND revoked_at IS NULL AND id <> ?`,
		s.now().Unix(), userID, keep)
	if err != nil {
		return 0, err
	}
	n, err := res.RowsAffected()
	return int(n), err
}

// PurgeExpired drops sessions and invites nobody can use any more, and failure
// rows older than the widest window. Housekeeping, run daily.
func (s *Store) PurgeExpired(failureWindow time.Duration) error {
	now := s.now().Unix()
	hourWindow := failureWindow
	if hourWindow < time.Hour {
		hourWindow = time.Hour
	}
	keepRevoked := now - int64((30 * 24 * time.Hour).Seconds())
	stmts := []struct {
		q    string
		args []any
	}{
		{`DELETE FROM security_tokens WHERE expires_at <= ? OR used_at IS NOT NULL`, []any{now}},
		{`UPDATE users SET totp_pending='', totp_pending_until=0 WHERE totp_pending_until <= ?`, []any{now}},
		{`DELETE FROM sessions WHERE expires_at < ? OR revoked_at < ?`, []any{now, keepRevoked}},
		{`DELETE FROM invites WHERE expires_at < ? AND used_at IS NULL`, []any{now}},
		{`DELETE FROM auth_failures WHERE
 (kind IN ('signup','email_change','email_resend_hour','forgot_ip','forgot_account') AND at <= ?) OR
 (kind NOT IN ('signup','email_change','email_resend_hour','forgot_ip','forgot_account') AND at < ?)`,
			[]any{now - int64(hourWindow.Seconds()), now - int64(failureWindow.Seconds())}},
		// The usage log only ever answers "how many in the last window"; past
		// the longest window a row is dead weight in every backup.
		{`DELETE FROM usage WHERE at < ?`, []any{now - int64(UsageRetention.Seconds())}},
		// An upload that was never put on a message is a picture the sender
		// chose and then changed their mind about. The file itself is swept by
		// the same pass in the API layer, which is the half that knows the
		// directory.
		{`DELETE FROM attachments WHERE message_id IS NULL AND created_at < ?`, []any{now - int64((24 * time.Hour).Seconds())}},
	}
	for _, st := range stmts {
		if _, err := s.db.Exec(st.q, st.args...); err != nil {
			return err
		}
	}
	return nil
}

// ---------------------------------------------------------------- rate limit

// Failure kinds. Counting the account and the address separately is the point:
// one stops a stranger grinding a password list against one friend, the other
// stops a bot spraying every name it can think of.
const (
	FailAccount = "account"
	FailIP      = "ip"
	FailSignup  = "signup"
)

// RecordFailure logs one failed attempt.
func (s *Store) RecordFailure(kind, key string) error {
	_, err := s.db.Exec(`INSERT INTO auth_failures (kind, key, at) VALUES (?, ?, ?)`,
		kind, key, s.now().Unix())
	return err
}

// Failures counts attempts for kind/key inside window.
func (s *Store) Failures(kind, key string, window time.Duration) (int, error) {
	var n int
	err := s.db.QueryRow(
		`SELECT count(*) FROM auth_failures WHERE kind = ? AND key = ? AND at > ?`,
		kind, key, s.now().Add(-window).Unix()).Scan(&n)
	return n, err
}

// ReserveAttempt is Failures and RecordFailure in one step, and the step is
// the point: it counts and writes inside a single transaction, so two attempts
// that arrive together cannot both read "four so far" and both be let through.
//
// Counting and then deciding is what the login used to do, with argon2id — a
// tenth of a second and 64 MiB — sitting between the two. Twenty attempts sent
// at once all read the same count, all passed, and all hashed: the limit of
// five was a limit on attempts that waited their turn. Whoever calls this
// spends the attempt before doing the expensive work and clears it on success,
// which is the opposite order and the safe one.
//
// It reports whether the attempt is allowed. The row is written either way:
// somebody knocking on a locked door is still knocking.
func (s *Store) ReserveAttempt(kind, key string, window time.Duration, limit int) (bool, error) {
	ok, _, err := s.ReserveAttemptID(kind, key, window, limit)
	return ok, err
}

type AttemptReceipt struct {
	id    int64
	nonce string
}

// ReserveAttemptID returns a receipt so a correct password awaiting a second
// factor can refund only its own reservation, preserving all failed guesses.
func (s *Store) ReserveAttemptID(kind, key string, window time.Duration, limit int) (bool, AttemptReceipt, error) {
	tx, err := s.db.Begin()
	if err != nil {
		return false, AttemptReceipt{}, err
	}
	defer tx.Rollback()

	now := s.now()
	var n int
	if err := tx.QueryRow(
		`SELECT count(*) FROM auth_failures WHERE kind = ? AND key = ? AND at > ?`,
		kind, key, now.Add(-window).Unix()).Scan(&n); err != nil {
		return false, AttemptReceipt{}, err
	}
	nonce, err := newID()
	if err != nil {
		return false, AttemptReceipt{}, err
	}
	res, err := tx.Exec(`INSERT INTO auth_failures (kind, key, at, reservation) VALUES (?, ?, ?, ?)`,
		kind, key, now.Unix(), nonce)
	if err != nil {
		return false, AttemptReceipt{}, err
	}
	id, err := res.LastInsertId()
	if err != nil {
		return false, AttemptReceipt{}, err
	}
	if err := tx.Commit(); err != nil {
		return false, AttemptReceipt{}, err
	}
	return n < limit, AttemptReceipt{id: id, nonce: nonce}, nil
}

func (s *Store) RefundAttempt(kind, key string, receipt AttemptReceipt) error {
	_, err := s.db.Exec(`DELETE FROM auth_failures WHERE rowid=? AND reservation=? AND kind=? AND key=?`, receipt.id, receipt.nonce, kind, key)
	return err
}

// ClearFailures forgets a key's attempts, called after a successful login so a
// friend who mistyped four times is not locked out by their own success.
func (s *Store) ClearFailures(kind, key string) error {
	_, err := s.db.Exec(`DELETE FROM auth_failures WHERE kind = ? AND key = ?`, kind, key)
	return err
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}

// MaxNicknameRunes is the length of a name somebody gives somebody else. The
// same as a display name: it goes in the same places and has to fit the same
// column.
const MaxNicknameRunes = 32

// ErrBadNickname is a name too long to put where a name goes.
var ErrBadNickname = errors.New("store: bad nickname")

// SetNickname records what one person calls another, or clears it when the name
// is empty. It is the owner's own view and is never shown to anybody else.
func (s *Store) SetNickname(ownerID int64, about string, name string) error {
	user, _, err := s.UserByUsername(about)
	if err != nil {
		return err
	}
	name = strings.TrimSpace(name)
	if len([]rune(name)) > MaxNicknameRunes {
		return ErrBadNickname
	}
	if name == "" {
		_, err := s.db.Exec(`DELETE FROM nicknames WHERE owner_id = ? AND about_id = ?`,
			ownerID, user.ID)
		return err
	}
	_, err = s.db.Exec(`
INSERT INTO nicknames (owner_id, about_id, name, set_at) VALUES (?, ?, ?, ?)
ON CONFLICT (owner_id, about_id) DO UPDATE SET name = excluded.name, set_at = excluded.set_at`,
		ownerID, user.ID, name, s.now().UTC().Unix())
	return err
}

// Nicknames is every name this person has given, by username. There is no way
// to ask for somebody else's: the only id this query takes is the asker's.
func (s *Store) Nicknames(ownerID int64) (map[string]string, error) {
	rows, err := s.db.Query(`
SELECT u.username, n.name FROM nicknames n
  JOIN users u ON u.id = n.about_id
 WHERE n.owner_id = ?`, ownerID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[string]string{}
	for rows.Next() {
		var username, name string
		if err := rows.Scan(&username, &name); err != nil {
			return nil, err
		}
		out[username] = name
	}
	return out, rows.Err()
}
