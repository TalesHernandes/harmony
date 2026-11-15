package store

import "errors"

var ErrDelivery = errors.New("delivery_failed")

type PersonPreference struct {
	Username string `json:"username"`
	Muted    bool   `json:"muted"`
	Blocked  bool   `json:"blocked"`
}

func (s *Store) PersonPreferences(owner int64) ([]PersonPreference, error) {
	rows, err := s.db.Query(`SELECT u.username,p.muted,p.blocked FROM person_preferences p JOIN users u ON u.id=p.about_id WHERE p.owner_id=? AND (p.muted=1 OR p.blocked=1) ORDER BY u.username`, owner)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []PersonPreference{}
	for rows.Next() {
		var p PersonPreference
		if err := rows.Scan(&p.Username, &p.Muted, &p.Blocked); err != nil {
			return nil, err
		}
		out = append(out, p)
	}
	return out, rows.Err()
}

func (s *Store) SetPersonPreference(owner int64, username string, block, enabled bool) error {
	target, _, err := s.UserByUsername(username)
	if err != nil || target.ID == owner {
		return ErrNotFound
	}
	column := "muted"
	if block {
		column = "blocked"
	}
	_, err = s.db.Exec(`INSERT INTO person_preferences(owner_id,about_id,`+column+`) VALUES(?,?,?) ON CONFLICT(owner_id,about_id) DO UPDATE SET `+column+`=excluded.`+column, owner, target.ID, enabled)
	return err
}

func (s *Store) Blocks(owner, about int64) (bool, error) {
	var yes bool
	err := s.db.QueryRow(`SELECT EXISTS(SELECT 1 FROM person_preferences WHERE owner_id=? AND about_id=? AND blocked=1)`, owner, about).Scan(&yes)
	return yes, err
}

func (s *Store) BlockedPair(a, b int64) (bool, error) {
	var yes bool
	err := s.db.QueryRow(`SELECT EXISTS(SELECT 1 FROM person_preferences WHERE blocked=1 AND ((owner_id=? AND about_id=?) OR (owner_id=? AND about_id=?)))`, a, b, b, a).Scan(&yes)
	return yes, err
}
