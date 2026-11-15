package store

import (
	"database/sql"
	"errors"
	"time"
)

// Push subscriptions: one row per browser that agreed to be woken up.
//
// A subscription belongs to a login session, not only to a person. That is the
// D4 decision and the reason is mechanical: a push endpoint is issued by the
// browser, so it *is* a device, and the session is the only row in this
// database that already means "this device, signed in as this person". Hanging
// it there gives three things for free — logging a device out stops its
// notifications the same second, an expired session takes its registration with
// it (ON DELETE CASCADE), and the phone of somebody who signed in on a friend's
// computer once cannot go on ringing for them.
//
// Delivery, on the other hand, is to the *person*: every live session of theirs
// gets the push, because "which of my devices is in my pocket right now" is not
// something this server can know.

// MaxSubscriptionsPerUser bounds the table. A person with a phone, a computer
// and a tablet is three; the rest are reinstalls that the browser gave a new
// endpoint to and never came back for.
const MaxSubscriptionsPerUser = 8

// PushSubscription is one registered device.
type PushSubscription struct {
	Endpoint  string
	P256dh    string
	Auth      string
	SessionID string
	UserAgent string
	CreatedAt time.Time
}

// AddPushSubscription registers (or refreshes) one endpoint for one session.
//
// The endpoint is the primary key, so a browser that re-subscribes with the
// same endpoint updates its row instead of making a second one.
//
// It does **not** change hands. Naming somebody else's endpoint used to move
// the registration to whoever asked, which needs nothing but the endpoint
// string: their device stopped being rung and the registration showed up under
// the other account. Knowing an address is not owning the device, so a
// conflict with another person's row is refused here. (A person legitimately
// re-registering is always the same account, and a browser that really did
// change hands registers a new endpoint.)
func (s *Store) AddPushSubscription(userID int64, sessionID, endpoint, p256dh, auth, userAgent string) error {
	if endpoint == "" || sessionID == "" {
		return ErrNotFound
	}
	now := s.now().Unix()
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	res, err := tx.Exec(
		`INSERT INTO push_subscriptions (endpoint, user_id, session_id, p256dh, auth, user_agent, created_at)
		 VALUES (?, ?, ?, ?, ?, ?, ?)
		 ON CONFLICT(endpoint) DO UPDATE SET
		   session_id = excluded.session_id,
		   p256dh = excluded.p256dh, auth = excluded.auth,
		   user_agent = excluded.user_agent, created_at = excluded.created_at
		 WHERE push_subscriptions.user_id = excluded.user_id`,
		endpoint, userID, sessionID, p256dh, auth, truncate(userAgent, 200), now)
	if err != nil {
		return err
	}
	// Nothing written means the row is somebody else's: the conflict happened
	// and the WHERE refused it.
	if n, err := res.RowsAffected(); err == nil && n == 0 {
		return ErrEndpointTaken
	}
	// Oldest first out, and only this person's: a full table is one person's
	// reinstalls, and the newest registration is the one that can actually ring.
	if _, err := tx.Exec(
		`DELETE FROM push_subscriptions
		  WHERE user_id = ? AND endpoint NOT IN (
		        SELECT endpoint FROM push_subscriptions WHERE user_id = ?
		         ORDER BY created_at DESC, endpoint LIMIT ?)`,
		userID, userID, MaxSubscriptionsPerUser); err != nil {
		return err
	}
	return tx.Commit()
}

// RemovePushSubscription forgets one endpoint of one person. The ownership is
// in the WHERE clause, so somebody else's endpoint answers exactly what an
// invented one answers: nothing happened, and no error either — a page
// unsubscribing twice is normal.
func (s *Store) RemovePushSubscription(userID int64, endpoint string) error {
	_, err := s.db.Exec(`DELETE FROM push_subscriptions WHERE endpoint = ? AND user_id = ?`,
		endpoint, userID)
	return err
}

// DropPushSubscription forgets an endpoint the push service itself declared
// dead (RFC 8030 §7.3). No user id: this is not somebody asking, it is the
// push service being authoritative, and keeping the row would make the count
// lie about whether the doorbell can ring.
func (s *Store) DropPushSubscription(endpoint string) error {
	_, err := s.db.Exec(`DELETE FROM push_subscriptions WHERE endpoint = ?`, endpoint)
	return err
}

// PushSubscriptionOwner answers which account and session an endpoint belongs
// to. It is what lets the service worker re-register from
// `pushsubscriptionchange`, where the only thing it can prove is that it knows
// the endpoint being replaced.
func (s *Store) PushSubscriptionOwner(endpoint string) (userID int64, sessionID string, err error) {
	err = s.db.QueryRow(`SELECT user_id, session_id FROM push_subscriptions WHERE endpoint = ?`,
		endpoint).Scan(&userID, &sessionID)
	if errors.Is(err, sql.ErrNoRows) {
		return 0, "", ErrNotFound
	}
	return userID, sessionID, err
}

// HasPushSubscription reports whether this person registered this endpoint. It
// is what the settings screen reads to say "este aparelho está ligado", which
// is the antidote to a doorbell that is off and looks fine.
func (s *Store) HasPushSubscription(userID int64, endpoint string) (bool, error) {
	var one int
	err := s.db.QueryRow(`SELECT 1 FROM push_subscriptions WHERE endpoint = ? AND user_id = ?`,
		endpoint, userID).Scan(&one)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	return err == nil, err
}

// LivePushSubscriptions lists the devices of one person that may still be woken
// up: the session behind each one has to be neither revoked nor expired. A
// device that was logged out stops ringing the same second, without anything
// having to remember to delete its row.
func (s *Store) LivePushSubscriptions(userID int64) ([]PushSubscription, error) {
	rows, err := s.db.Query(
		`SELECT p.endpoint, p.p256dh, p.auth, p.session_id, p.user_agent, p.created_at
		   FROM push_subscriptions p
		   JOIN sessions s ON s.id = p.session_id
		  WHERE p.user_id = ? AND s.revoked_at IS NULL AND s.expires_at > ?
		  ORDER BY p.created_at`,
		userID, s.now().Unix())
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []PushSubscription
	for rows.Next() {
		var sub PushSubscription
		var created int64
		if err := rows.Scan(&sub.Endpoint, &sub.P256dh, &sub.Auth, &sub.SessionID, &sub.UserAgent, &created); err != nil {
			return nil, err
		}
		sub.CreatedAt = time.Unix(created, 0)
		out = append(out, sub)
	}
	return out, rows.Err()
}

// CountPushSubscriptions is how many devices of this person are registered and
// still signed in.
func (s *Store) CountPushSubscriptions(userID int64) (int, error) {
	var n int
	err := s.db.QueryRow(
		`SELECT count(*) FROM push_subscriptions p
		   JOIN sessions s ON s.id = p.session_id
		  WHERE p.user_id = ? AND s.revoked_at IS NULL AND s.expires_at > ?`,
		userID, s.now().Unix()).Scan(&n)
	return n, err
}
