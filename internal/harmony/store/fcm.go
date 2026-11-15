package store

// AddFCMToken attaches an Android installation to its current login. Unlike a
// Web Push endpoint, an FCM token survives account switches: the native app has
// only one session at a time, so registration deliberately transfers ownership.
func (s *Store) AddFCMToken(userID int64, sessionID, token string) error {
	if token == "" || sessionID == "" {
		return ErrNotFound
	}
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	_, err = tx.Exec(`INSERT INTO fcm_tokens(token,user_id,session_id,created_at) VALUES(?,?,?,?)
 ON CONFLICT(token) DO UPDATE SET user_id=excluded.user_id, session_id=excluded.session_id, created_at=excluded.created_at`, token, userID, sessionID, s.now().Unix())
	if err != nil {
		return err
	}
	_, err = tx.Exec(`DELETE FROM fcm_tokens WHERE user_id=? AND token NOT IN
 (SELECT token FROM fcm_tokens WHERE user_id=? ORDER BY created_at DESC, token LIMIT ?)`, userID, userID, MaxSubscriptionsPerUser)
	if err != nil {
		return err
	}
	return tx.Commit()
}

func (s *Store) RemoveFCMToken(userID int64, token string) error {
	_, err := s.db.Exec(`DELETE FROM fcm_tokens WHERE user_id=? AND token=?`, userID, token)
	return err
}

func (s *Store) DropFCMToken(token string) error {
	_, err := s.db.Exec(`DELETE FROM fcm_tokens WHERE token=?`, token)
	return err
}

func (s *Store) LiveFCMTokens(userID int64) ([]string, error) {
	rows, err := s.db.Query(`SELECT p.token FROM fcm_tokens p JOIN sessions s ON s.id=p.session_id
 WHERE p.user_id=? AND s.revoked_at IS NULL AND s.expires_at>? ORDER BY p.created_at,p.token`, userID, s.now().Unix())
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []string
	for rows.Next() {
		var token string
		if err := rows.Scan(&token); err != nil {
			return nil, err
		}
		out = append(out, token)
	}
	return out, rows.Err()
}

func (s *Store) CountFCMTokens(userID int64) (int, error) {
	var count int
	err := s.db.QueryRow(`SELECT count(*) FROM fcm_tokens p JOIN sessions s ON s.id=p.session_id
 WHERE p.user_id=? AND s.revoked_at IS NULL AND s.expires_at>?`, userID, s.now().Unix()).Scan(&count)
	return count, err
}
