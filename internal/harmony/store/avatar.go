package store

import (
	"database/sql"
	"errors"
	"time"
)

// SetAvatar keeps only the processed image, and books successful changes in
// the same transaction. The quota survives a restart and concurrent requests.
func (s *Store) SetAvatar(userID int64, jpeg []byte, version string) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	now := s.now().Unix()
	if _, err = tx.Exec(`DELETE FROM usage WHERE kind = 'avatar' AND at <= ?`, s.now().Add(-time.Hour).Unix()); err != nil {
		return err
	}
	var count int
	if err = tx.QueryRow(`SELECT count(*) FROM usage WHERE user_id = ? AND kind = 'avatar'`, userID).Scan(&count); err != nil {
		return err
	}
	if count >= 10 {
		return ErrRateLimited
	}
	result, err := tx.Exec(`UPDATE users SET avatar_blob = ?, avatar_version = ? WHERE id = ?`, jpeg, version, userID)
	if err != nil {
		return err
	}
	n, err := result.RowsAffected()
	if err != nil {
		return err
	}
	if n == 0 {
		return ErrNotFound
	}
	if _, err = tx.Exec(`INSERT INTO usage (user_id, kind, at) VALUES (?, 'avatar', ?)`, userID, now); err != nil {
		return err
	}
	return tx.Commit()
}

func (s *Store) Avatar(username string) ([]byte, error) {
	var data []byte
	err := s.db.QueryRow(`SELECT avatar_blob FROM users WHERE username = ? AND avatar_version != '' AND disabled_at IS NULL`, NormalizeUsername(username)).Scan(&data)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	return data, err
}

func (s *Store) AvatarVersion(username string) (string, error) {
	var version string
	err := s.db.QueryRow(`SELECT avatar_version FROM users WHERE username = ?`, NormalizeUsername(username)).Scan(&version)
	if errors.Is(err, sql.ErrNoRows) {
		return "", ErrNotFound
	}
	return version, err
}
