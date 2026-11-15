package store

import (
	"database/sql"
	"time"
)

// Account includes the owner's audit information, without hashes or tokens.
type Account struct {
	User
	DisabledAt time.Time
	InvitedBy  string
	LastSeenAt time.Time
}

func (s *Store) ListAccounts() ([]Account, error) {
	rows, err := s.db.Query(`SELECT u.id, u.username, u.display_name, u.avatar_version,
	 u.is_admin, u.created_at, u.disabled_at, COALESCE(c.username, ''),
	 (SELECT MAX(last_seen_at) FROM sessions WHERE user_id = u.id),
	 u.email_verified_at IS NOT NULL, u.twofa
	 FROM users u LEFT JOIN invites i ON i.used_by = u.id
	 LEFT JOIN users c ON c.id = i.created_by ORDER BY u.created_at, u.id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Account{}
	for rows.Next() {
		var a Account
		var created int64
		var disabled, seen sql.NullInt64
		if err := rows.Scan(&a.ID, &a.Username, &a.DisplayName, &a.Avatar,
			&a.IsAdmin, &created, &disabled, &a.InvitedBy, &seen, &a.EmailVerified, &a.TwoFA); err != nil {
			return nil, err
		}
		a.CreatedAt = time.Unix(created, 0).UTC()
		a.Disabled = disabled.Valid
		a.DisabledAt, a.LastSeenAt = fromUnix(disabled), fromUnix(seen)
		out = append(out, a)
	}
	return out, rows.Err()
}

// DisableAccount cuts off logins and revokes devices in one transaction.
// Repeating it preserves the first disable time.
func (s *Store) DisableAccount(userID int64) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	res, err := tx.Exec(`UPDATE users SET disabled_at = COALESCE(disabled_at, ?) WHERE id = ? AND is_admin = 0`, s.now().Unix(), userID)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	if _, err := tx.Exec(`UPDATE sessions SET revoked_at = ? WHERE user_id = ? AND revoked_at IS NULL`, s.now().Unix(), userID); err != nil {
		return err
	}
	if _, err := tx.Exec(`DELETE FROM push_subscriptions WHERE user_id = ?`, userID); err != nil {
		return err
	}
	if _, err := tx.Exec(`DELETE FROM fcm_tokens WHERE user_id = ?`, userID); err != nil {
		return err
	}
	return tx.Commit()
}

func (s *Store) EnableAccount(userID int64) error {
	res, err := s.db.Exec(`UPDATE users SET disabled_at = NULL WHERE id = ?`, userID)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

// RandomPassword is shared by the CLI and owner's panel. The alphabet and
// grouping match the original CLI generator, including its entropy source.
func RandomPassword() (string, error) {
	const alphabet = "abcdefghijkmnopqrstuvwxyz23456789" // no l, no 1, no 0, no o
	b, err := randomBytes(20)
	if err != nil {
		return "", err
	}
	out := make([]byte, 0, 23)
	for i, v := range b {
		if i > 0 && i%5 == 0 {
			out = append(out, '-')
		}
		out = append(out, alphabet[int(v)%len(alphabet)])
	}
	return string(out), nil
}
