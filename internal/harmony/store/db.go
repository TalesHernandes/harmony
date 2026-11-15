// Package store is every read and write harmony makes to its SQLite file.
// Nothing above it builds SQL, so "who may see this" is decided in one place.
package store

import (
	"database/sql"
	"errors"
	"fmt"
	"net/url"
	"time"

	_ "modernc.org/sqlite" // pure-Go driver, no cgo (see CLAUDE.md)
)

// Store owns the database handle.
type Store struct {
	db  *sql.DB
	now func() time.Time
}

// Errors the layers above turn into HTTP status codes. They are deliberately
// coarse: the API must not tell a stranger whether a username exists or
// whether an invite was merely used, only that it cannot proceed.
var (
	ErrNotFound     = errors.New("store: not found")
	ErrInviteBad    = errors.New("store: invite unusable")
	ErrNameTaken    = errors.New("store: username taken")
	ErrCredentials  = errors.New("store: bad credentials")
	ErrRateLimited  = errors.New("store: too many attempts")
	ErrSessionStale = errors.New("store: session expired or revoked")
	// ErrEndpointTaken is a push endpoint that already belongs to somebody
	// else. It is not "already registered": re-registering your own is the
	// normal case and succeeds.
	ErrEndpointTaken = errors.New("store: push endpoint belongs to another account")
	// The two halves of the daily upload ceiling, separate because the person
	// is told which one they reached.
	ErrUploadFiles = errors.New("store: too many uploads today")
	ErrUploadBytes = errors.New("store: too many bytes uploaded today")
	// ErrMessageLimit is the per-minute message ceiling, reached.
	ErrMessageLimit = errors.New("store: too many messages this minute")
)

// Open opens (creating it if needed) the SQLite file at path and applies the
// schema. WAL is not a tuning knob here: it is what lets a reader run while a
// writer commits, and it is what the daily integrity check assumes.
func Open(path string) (*Store, error) {
	dsn := "file:" + url.PathEscape(path) +
		"?_pragma=journal_mode(WAL)" +
		"&_pragma=busy_timeout(5000)" +
		"&_pragma=foreign_keys(ON)" +
		"&_pragma=synchronous(NORMAL)"
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, err
	}
	// One connection. Every write in this service is sub-millisecond and the
	// expensive part of a login (argon2) happens outside the database, so
	// serialising removes a whole class of "database is locked" bugs at a cost
	// nobody with twenty friends can measure. Revisit only with a measurement.
	db.SetMaxOpenConns(1)
	db.SetConnMaxLifetime(0)

	s := &Store{db: db, now: time.Now}
	if err := s.migrate(); err != nil {
		db.Close()
		return nil, err
	}
	return s, nil
}

// OpenMemory returns a private in-memory database, for tests.
func OpenMemory() (*Store, error) {
	db, err := sql.Open("sqlite", "file::memory:?_pragma=foreign_keys(ON)")
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	s := &Store{db: db, now: time.Now}
	if err := s.migrate(); err != nil {
		db.Close()
		return nil, err
	}
	return s, nil
}

// Close releases the handle.
func (s *Store) Close() error { return s.db.Close() }

// DB exposes the handle for maintenance commands (backup, integrity check).
func (s *Store) DB() *sql.DB { return s.db }

// SetClock replaces the clock; tests use it to age sessions and invites.
func (s *Store) SetClock(now func() time.Time) { s.now = now }

// JournalMode reports the journal mode actually in force.
func (s *Store) JournalMode() (string, error) {
	var mode string
	err := s.db.QueryRow(`PRAGMA journal_mode`).Scan(&mode)
	return mode, err
}

// IntegrityCheck runs SQLite's own consistency check. It returns nil only when
// SQLite answers exactly "ok"; a backup that fails this is not a backup.
func (s *Store) IntegrityCheck() error {
	var result string
	if err := s.db.QueryRow(`PRAGMA integrity_check`).Scan(&result); err != nil {
		return err
	}
	if result != "ok" {
		return fmt.Errorf("store: integrity check failed: %s", result)
	}
	return nil
}

// BackupTo writes a consistent copy of the whole database to path using
// VACUUM INTO, which SQLite performs inside a read transaction: the copy is a
// single point in time even while the service keeps writing. Copying the file
// with cp cannot promise that.
func (s *Store) BackupTo(path string) error {
	_, err := s.db.Exec(`VACUUM INTO ?`, path)
	return err
}

// migrate applies every schema step that has not run yet. user_version is
// SQLite's own integer in the file header, so the migration state travels with
// the database, including into a restored backup.
func (s *Store) migrate() error {
	var version int
	if err := s.db.QueryRow(`PRAGMA user_version`).Scan(&version); err != nil {
		return err
	}
	for i := version; i < len(migrations); i++ {
		tx, err := s.db.Begin()
		if err != nil {
			return err
		}
		if _, err := tx.Exec(migrations[i]); err != nil {
			tx.Rollback()
			return fmt.Errorf("store: migration %d: %w", i+1, err)
		}
		// PRAGMA does not take a bind parameter; i+1 is a loop counter, not input.
		if _, err := tx.Exec(fmt.Sprintf("PRAGMA user_version = %d", i+1)); err != nil {
			tx.Rollback()
			return err
		}
		if err := tx.Commit(); err != nil {
			return err
		}
	}
	return nil
}

// migrations is append-only. Never edit a shipped entry: a database that has
// already run it would silently disagree with a fresh one.
var migrations = []string{
	// 1 — accounts, invites, sessions, and the failure log the rate limiter reads.
	`
CREATE TABLE users (
	id            INTEGER PRIMARY KEY AUTOINCREMENT,
	username      TEXT    NOT NULL UNIQUE COLLATE NOCASE,
	display_name  TEXT    NOT NULL,
	password_hash TEXT    NOT NULL,
	is_admin      INTEGER NOT NULL DEFAULT 0,
	created_at    INTEGER NOT NULL,
	disabled_at   INTEGER
);

-- The code itself is never stored, only its SHA-256. Someone who reads the
-- database file still cannot sign up with an invite that is sitting unused.
CREATE TABLE invites (
	id         TEXT    PRIMARY KEY,
	code_hash  TEXT    NOT NULL UNIQUE,
	created_by INTEGER REFERENCES users(id),
	note       TEXT    NOT NULL DEFAULT '',
	created_at INTEGER NOT NULL,
	expires_at INTEGER NOT NULL,
	used_at    INTEGER,
	used_by    INTEGER REFERENCES users(id)
);
CREATE INDEX invites_by_creator ON invites(created_by, created_at DESC);

-- Same for session tokens: the file holds a hash, so a stolen backup does not
-- hand over live sessions. The id is a separate public handle, so that naming a
-- session in a URL ("log this device out") never puts the secret in a URL.
CREATE TABLE sessions (
	id           TEXT    PRIMARY KEY,
	token_hash   TEXT    NOT NULL UNIQUE,
	user_id      INTEGER NOT NULL REFERENCES users(id),
	created_at   INTEGER NOT NULL,
	expires_at   INTEGER NOT NULL,
	last_seen_at INTEGER NOT NULL,
	revoked_at   INTEGER,
	user_agent   TEXT    NOT NULL DEFAULT ''
);
CREATE INDEX sessions_by_user ON sessions(user_id, created_at DESC);

-- One row per failed attempt, kept only as long as the window needs it. In the
-- database rather than in memory so that restarting the server is not a way to
-- clear a lockout.
CREATE TABLE auth_failures (
	kind TEXT    NOT NULL,
	key  TEXT    NOT NULL,
	at   INTEGER NOT NULL
);
CREATE INDEX auth_failures_lookup ON auth_failures(kind, key, at);
`,

	// 2 — the text side: channels (a direct message is a channel with two
	// members), messages, image attachments, mentions, and the per-person
	// usage log the message and upload limits read.
	//
	// Membership is the whole authorisation story. There is no "is this
	// channel public?" flag to forget: a person reads a channel if and only if
	// channel_members has their row, and every query below joins it. A text
	// channel simply has a row for everybody.
	`
CREATE TABLE channels (
	id         TEXT    PRIMARY KEY,
	kind       TEXT    NOT NULL,              -- 'text' or 'dm'
	name       TEXT    NOT NULL DEFAULT '',
	topic      TEXT    NOT NULL DEFAULT '',
	position   INTEGER NOT NULL DEFAULT 0,
	created_by INTEGER REFERENCES users(id),
	created_at INTEGER NOT NULL,
	-- For a direct message: the two user ids, smaller first, as 'a:b'. UNIQUE,
	-- so two people cannot end up with two different private conversations
	-- because they both pressed the button at the same moment.
	dm_key     TEXT    UNIQUE
);

CREATE TABLE channel_members (
	channel_id           TEXT    NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
	user_id              INTEGER NOT NULL REFERENCES users(id),
	added_at             INTEGER NOT NULL,
	-- Read state lives here, per person per channel, so it follows you from
	-- the phone to the desktop instead of living in one browser.
	last_read_message_id INTEGER NOT NULL DEFAULT 0,
	PRIMARY KEY (channel_id, user_id)
);
CREATE INDEX channel_members_by_user ON channel_members(user_id);

-- The id is the order. AUTOINCREMENT (not plain rowid) so a deleted row's
-- number is never handed to a later message: "everything after id 40" has to
-- keep meaning the same thing forever, on every device.
CREATE TABLE messages (
	id         INTEGER PRIMARY KEY AUTOINCREMENT,
	channel_id TEXT    NOT NULL REFERENCES channels(id),
	author_id  INTEGER NOT NULL REFERENCES users(id),
	body       TEXT    NOT NULL,
	created_at INTEGER NOT NULL,
	edited_at  INTEGER,
	-- A removed message is a tombstone, not a missing row: who removed it and
	-- when stays on the record, and the ids around it keep their order.
	deleted_at INTEGER,
	deleted_by INTEGER REFERENCES users(id)
);
CREATE INDEX messages_by_channel ON messages(channel_id, id DESC);

-- The file on disk is named by this id, never by anything the uploader typed.
-- name is only what to show and what to call it on the way out.
CREATE TABLE attachments (
	id         TEXT    PRIMARY KEY,
	message_id INTEGER REFERENCES messages(id),
	channel_id TEXT    NOT NULL REFERENCES channels(id),
	owner_id   INTEGER NOT NULL REFERENCES users(id),
	name       TEXT    NOT NULL,
	mime       TEXT    NOT NULL,
	size       INTEGER NOT NULL,
	width      INTEGER NOT NULL DEFAULT 0,
	height     INTEGER NOT NULL DEFAULT 0,
	created_at INTEGER NOT NULL
);
CREATE INDEX attachments_by_message ON attachments(message_id);

CREATE TABLE mentions (
	message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
	user_id    INTEGER NOT NULL REFERENCES users(id),
	channel_id TEXT    NOT NULL REFERENCES channels(id),
	PRIMARY KEY (message_id, user_id)
);
CREATE INDEX mentions_by_user ON mentions(user_id, channel_id);

-- One row per message sent and per file uploaded, kept only as long as the
-- longest window needs it. In the database, like auth_failures, so restarting
-- the server is not a way to reset somebody's quota.
CREATE TABLE usage (
	user_id INTEGER NOT NULL REFERENCES users(id),
	kind    TEXT    NOT NULL,
	at      INTEGER NOT NULL
);
CREATE INDEX usage_lookup ON usage(user_id, kind, at);

-- The one channel that has to exist for the service to be usable on first
-- start. Everybody who already has an account joins it here; SignUp adds the
-- rows for everybody who comes later.
INSERT INTO channels (id, kind, name, topic, position, created_at)
VALUES ('geral', 'text', 'geral', 'O canal de todo mundo', 0, strftime('%s','now'));
INSERT INTO channel_members (channel_id, user_id, added_at, last_read_message_id)
SELECT 'geral', id, strftime('%s','now'), 0 FROM users;
`,
	// 3 — voice. A voice channel is a row in the same channels table, with the
	// same membership rows, so nothing about who may be in it is new. What is
	// new lives nowhere near the database: who is inside *right now* is held in
	// memory by the media server and dies with the process, which is exactly
	// what "who is in the voice channel" means — nobody is in a room after the
	// server restarts, and pretending otherwise in a table would be a lie that
	// survives reboots.
	//
	// Only adds: one channel, and a membership row in it for everybody who
	// already has an account. Nothing existing is altered and nothing is
	// deleted, so it runs the same way on a database with people in it.
	`
INSERT INTO channels (id, kind, name, topic, position, created_at)
VALUES ('voz-geral', 'voice', 'geral', 'A sala de voz de todo mundo', 0, strftime('%s','now'));
INSERT INTO channel_members (channel_id, user_id, added_at, last_read_message_id)
SELECT 'voz-geral', id, strftime('%s','now'), 0 FROM users;
`,
	// 4 — the doorbell: one row per browser that agreed to be woken up with the
	// app closed. See push.go for why a subscription hangs off a login session
	// and not only off a person.
	//
	// What is *not* here is as deliberate as what is: no notification is ever
	// stored. What a notification says is decided at the moment it is sent and
	// kept in memory for a minute (internal/harmony/api/notify.go), because a
	// table of "things we were about to tell you" is a table of who talked to
	// whom, sitting in every backup, for a feature that is over in seconds.
	//
	// Only adds one table, so it runs the same way on a database with people in
	// it. Presence adds nothing at all: who is online is a question about open
	// sockets, and a table pretending to answer it would be a lie that survives
	// a reboot.
	`
CREATE TABLE push_subscriptions (
	endpoint   TEXT    PRIMARY KEY,
	user_id    INTEGER NOT NULL REFERENCES users(id),
	session_id TEXT    NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
	-- Stored, unused: nothing here encrypts a payload, because nothing here
	-- sends one (the woken service worker asks the server what to show). They
	-- are kept so that adding RFC 8291 later would be a server change alone,
	-- instead of something every device has to subscribe again for.
	p256dh     TEXT    NOT NULL DEFAULT '',
	auth       TEXT    NOT NULL DEFAULT '',
	user_agent TEXT    NOT NULL DEFAULT '',
	created_at INTEGER NOT NULL
);
CREATE INDEX push_subscriptions_by_user ON push_subscriptions(user_id);
CREATE INDEX push_subscriptions_by_session ON push_subscriptions(session_id);
`,
	// 5 — replying to one particular message.
	//
	// One nullable column and nothing else: the quotation shown above a reply is
	// not stored anywhere, it is read from the message being answered at the
	// moment the page asks. That is what makes an edit of the original show
	// through in every reply to it, and a removal turn every quotation of it
	// into "mensagem removida" without a single row being rewritten.
	//
	// It is not ON DELETE CASCADE and it must never be: this server does not
	// delete message rows (a removed message is a tombstone, migration 2), and
	// a cascade here would mean removing one line silently removes the answers
	// to it.
	`
ALTER TABLE messages ADD COLUMN reply_to INTEGER REFERENCES messages(id);
CREATE INDEX messages_by_reply ON messages(reply_to);
`,
	// 6 — silencing a channel, and closing a conversation.
	//
	// Both are about one person's relationship with one channel, so both are
	// columns on the membership row rather than tables of their own: they
	// travel with the membership, and a person removed from a channel takes
	// their opinion of it with them.
	//
	// **Neither one deletes anything, and that is the whole point of the
	// second.** Closing a conversation is closing a tab: the messages stay
	// exactly where they are, both people still have them, and writing in it
	// again brings it straight back (store.EnsureDM clears this, and so does a
	// message arriving — a closed conversation that swallowed messages would be
	// a blocklist wearing the wrong name).
	`
ALTER TABLE channel_members ADD COLUMN muted_at  INTEGER;
ALTER TABLE channel_members ADD COLUMN hidden_at INTEGER;
`,
	// 7 — the name you call somebody, which only you see.
	//
	// Two ids and a word. `owner_id` is whose opinion it is and `about_id` is
	// who it is about, and the pair is the key: one person has one name for
	// another, and nobody else is ever shown it. That last part is not a
	// promise made in the interface — it is the shape of the table, because
	// there is no query that could hand somebody a row that is not theirs
	// without saying `owner_id = <them>`.
	//
	// It is here rather than in the browser's storage, where the volumes live,
	// because a volume is about the machine and a name is about the person: it
	// has to be the same on the telephone as on the desktop.
	`
CREATE TABLE nicknames (
	owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
	about_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
	name     TEXT    NOT NULL,
	set_at   INTEGER NOT NULL,
	PRIMARY KEY (owner_id, about_id)
);
`,
	// 8 — reacting to a message.
	//
	// Three columns, and the key is all three of them: one person, one message,
	// one emoji. "You cannot react twice with the same thing" is therefore not
	// a rule anybody has to remember to check — it is the shape of the table,
	// and a second press is an ordinary delete of the row that is already
	// there.
	//
	// created_at is not decoration: it is the order the buttons are drawn in.
	// Whoever reacted first put that emoji on the message, and the row of
	// reactions has to stay still while people press them — a count that
	// reorders itself as it grows moves the button out from under the next
	// finger.
	//
	// The emoji is stored as the text it is, not as a number into a list of
	// allowed ones. A list would mean the telephone's own keyboard could not
	// be used, which is where most of these are going to be typed. What
	// protects the column instead is a check on the *shape* of what arrives
	// (ValidReaction, chat.go): symbols only, no letters, no digits, no spaces
	// — because a free-text column on a message is a second message body, and
	// this one would have no rate limit and no tombstone.
	//
	// ON DELETE CASCADE is the honest answer here, unlike on reply_to: a
	// reaction is *about* a message and means nothing without it. It never
	// fires today — this server does not delete message rows, it tombstones
	// them (migration 2) — and a tombstone drops its reactions on the way out
	// of the database instead (store.fill).
	`
CREATE TABLE reactions (
	message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
	user_id    INTEGER NOT NULL REFERENCES users(id),
	emoji      TEXT    NOT NULL,
	created_at INTEGER NOT NULL,
	PRIMARY KEY (message_id, user_id, emoji)
);
CREATE INDEX reactions_by_message ON reactions(message_id, created_at);
`,

	// 9 — the line a call leaves behind.
	//
	// Everything in the messages table until now was written by a person. This
	// column is how a row says it was not: kind is '' for everything anybody
	// typed, and 'call' for the one line this server writes on its own — "nobody
	// answered". The client draws it as a notice and not as somebody speaking.
	//
	// It is a column and not a table because it *is* a message in every way that
	// matters: it sits in the conversation in its place in time, it is what the
	// unread line counts up to, and the page that already draws a conversation
	// draws it with no second query.
	//
	// What this deliberately does **not** become is a call log. Only calls that
	// were never answered leave a row; a call that happened leaves nothing, which
	// keeps the D4b promise that no backup anywhere holds a record of who spoke
	// to whom for how long. A missed call is not a record of a conversation — it
	// is a message saying "I tried", and that is exactly the thing the owner said
	// was disappearing.
	`
ALTER TABLE messages ADD COLUMN kind TEXT NOT NULL DEFAULT '';
`,
	// 10 — keep deleted rooms and their history, but revoke every membership.
	`
ALTER TABLE channels ADD COLUMN deleted_at INTEGER;
ALTER TABLE channels ADD COLUMN deleted_by INTEGER REFERENCES users(id);
`,
	// 11 — the Android app's Firebase addresses. They hang off a login session
	// for the same reason Web Push does (migration 4): logging out stops the
	// phone ringing without anything having to remember to delete a row.
	`
CREATE TABLE fcm_tokens (
 token TEXT PRIMARY KEY,
 user_id INTEGER NOT NULL REFERENCES users(id),
 session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
 created_at INTEGER NOT NULL
);
CREATE INDEX fcm_tokens_by_user ON fcm_tokens(user_id);
CREATE INDEX fcm_tokens_by_session ON fcm_tokens(session_id);
`,
	// 12 — processed profile photos travel with the database backup.
	`
ALTER TABLE users ADD COLUMN avatar_blob BLOB;
ALTER TABLE users ADD COLUMN avatar_version TEXT NOT NULL DEFAULT '';
`,
	// 13 — email ownership and single-use security proofs.
	`ALTER TABLE auth_failures ADD COLUMN reservation TEXT NOT NULL DEFAULT '';
ALTER TABLE users ADD COLUMN security_version INTEGER NOT NULL DEFAULT 0;
ALTER TABLE users ADD COLUMN email TEXT;
ALTER TABLE users ADD COLUMN email_verified_at INTEGER;
ALTER TABLE users ADD COLUMN twofa TEXT NOT NULL DEFAULT '' CHECK(twofa IN ('','email','totp'));
ALTER TABLE users ADD COLUMN totp_secret TEXT NOT NULL DEFAULT '';
ALTER TABLE users ADD COLUMN totp_last_step INTEGER NOT NULL DEFAULT -1;
ALTER TABLE users ADD COLUMN totp_pending TEXT NOT NULL DEFAULT '';
ALTER TABLE users ADD COLUMN totp_pending_until INTEGER NOT NULL DEFAULT 0;
CREATE UNIQUE INDEX users_email_unique ON users(email) WHERE email IS NOT NULL;
CREATE TABLE security_tokens (
 id TEXT PRIMARY KEY,
 user_id INTEGER NOT NULL REFERENCES users(id),
 purpose TEXT NOT NULL,
 secret_hash TEXT NOT NULL UNIQUE,
 code_hash TEXT NOT NULL DEFAULT '',
 target_email TEXT NOT NULL DEFAULT '',
 password_hash TEXT NOT NULL,
 security_version INTEGER NOT NULL,
 method TEXT NOT NULL DEFAULT '',
 expires_at INTEGER NOT NULL,
 used_at INTEGER,
 attempts INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX security_tokens_user ON security_tokens(user_id,purpose);
CREATE TABLE recovery_codes (
 user_id INTEGER NOT NULL REFERENCES users(id),
 code_hash TEXT NOT NULL,
 used_at INTEGER,
 PRIMARY KEY(user_id,code_hash)
);`,
	// 14 - cache remote metadata and retain current message ownership for images.
	`CREATE TABLE IF NOT EXISTS previews (
 url TEXT PRIMARY KEY,
 result TEXT NOT NULL,
 success INTEGER NOT NULL,
 fetched_at INTEGER NOT NULL,
 image_id TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS previews_image ON previews(image_id);
CREATE TABLE IF NOT EXISTS message_preview_links (
 message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
 url TEXT NOT NULL,
 body_hash TEXT NOT NULL,
 PRIMARY KEY(message_id,url)
);
CREATE INDEX IF NOT EXISTS message_preview_links_url ON message_preview_links(url);`,
	// 15 - which reader of the sites wrote a preview (store.PreviewVersion);
	// rows from before it are version 1 and are fetched again.
	`ALTER TABLE previews ADD COLUMN version INTEGER NOT NULL DEFAULT 1;`,
	// 16 — the shape of the channel column (D12a): folders, and pinned
	// messages.
	//
	// Only adds. A folder is a row of its own with a heading and a place; a
	// room points at the folder it is in, or at nothing for the top of the
	// column, which is where every room that exists today stays. Deleting a
	// folder never deletes a room: store.DeleteCategory moves them out in the
	// same transaction. The column has no REFERENCES on purpose — SQLite
	// cannot drop a column that declares one, and the store tests rewind the
	// schema to test a migration on a database with people in it.
	//
	// A pin is one row per message, so a message is pinned once or not at
	// all, and the channel is carried along so the fifty-per-channel ceiling
	// is one indexed count. Who pinned it and when are kept because the list
	// is drawn newest pin first.
	`
CREATE TABLE categories (
	id         TEXT    PRIMARY KEY,
	name       TEXT    NOT NULL,
	position   INTEGER NOT NULL DEFAULT 0,
	created_at INTEGER NOT NULL
);
ALTER TABLE channels ADD COLUMN category_id TEXT;
CREATE TABLE pins (
	message_id INTEGER PRIMARY KEY REFERENCES messages(id) ON DELETE CASCADE,
	channel_id TEXT    NOT NULL REFERENCES channels(id),
	pinned_by  INTEGER NOT NULL REFERENCES users(id),
	pinned_at  INTEGER NOT NULL
);
CREATE INDEX pins_by_channel ON pins(channel_id, pinned_at);
`,
	// 17 — roles (D12c): a name, a colour, a place in the list, "show apart in
	// the member list", and a set of permissions; and who has which.
	//
	// Only adds. Nobody has a role on the day this runs, so nobody can do
	// anything they could not do the day before. Position 0 is the highest
	// role. perms is a bit set (store.Permission) so a fourth permission is a
	// new bit and not a migration. colour is "" for none or "#rrggbb".
	// A role that is deleted takes its rows in user_roles with it, in the same
	// transaction (store.DeleteRole) — the cascade below is the second lock.
	`
CREATE TABLE roles (
	id         TEXT    PRIMARY KEY,
	name       TEXT    NOT NULL,
	color      TEXT    NOT NULL DEFAULT '',
	position   INTEGER NOT NULL,
	hoist      INTEGER NOT NULL DEFAULT 0,
	perms      INTEGER NOT NULL DEFAULT 0,
	created_at INTEGER NOT NULL
);
CREATE TABLE user_roles (
	user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
	role_id TEXT    NOT NULL REFERENCES roles(id) ON DELETE CASCADE,
	PRIMARY KEY (user_id, role_id)
);
CREATE INDEX user_roles_by_role ON user_roles(role_id);
`,
	// 18 — personal preferences, readable only through the owner's session.
	`CREATE TABLE person_preferences (
 owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 about_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 muted INTEGER NOT NULL DEFAULT 0,
 blocked INTEGER NOT NULL DEFAULT 0,
 PRIMARY KEY(owner_id,about_id),
 CHECK(owner_id <> about_id)
 );`,
}
