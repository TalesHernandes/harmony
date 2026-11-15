package store

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"strings"
	"time"
)

// Embed contains only local image addresses; remote image URLs never reach clients.
type Embed struct {
	URL         string `json:"url"`
	Kind        string `json:"kind"`
	Site        string `json:"site,omitempty"`
	Title       string `json:"title,omitempty"`
	Description string `json:"description,omitempty"`
	Image       string `json:"image,omitempty"`
	Width       int    `json:"width,omitempty"`
	Height      int    `json:"height,omitempty"`
}

type Preview struct {
	Embed     Embed
	Success   bool
	FetchedAt time.Time
	// Stale: written by an older reader of the sites (PreviewVersion), so not
	// shown and fetched again. Version 1 kept YouTube's generic datacenter page
	// ("- YouTube") as a success for a week (30/10/2026).
	Stale bool
}

// PreviewVersion is the reader that wrote a row; raise it when what a row
// holds would change, and every older row is fetched again.
const PreviewVersion = 2

func (p Preview) Fresh(now time.Time) bool {
	if p.Stale {
		return false
	}
	ttl := time.Hour
	if p.Success {
		ttl = 7 * 24 * time.Hour
	}
	return now.Sub(p.FetchedAt) < ttl
}

// Previews reads a whole page's URLs in one query, including cached failures.
func (s *Store) Previews(urls []string) (map[string]Preview, error) {
	out := make(map[string]Preview)
	if len(urls) == 0 {
		return out, nil
	}
	args := make([]any, len(urls))
	for i, u := range urls {
		args[i] = u
	}
	rows, err := s.db.Query(`SELECT url, result, success, fetched_at, version FROM previews WHERE url IN (`+strings.TrimRight(strings.Repeat("?,", len(urls)), ",")+`)`, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var u, raw string
		var p Preview
		var at, version int64
		if err = rows.Scan(&u, &raw, &p.Success, &at, &version); err != nil {
			return nil, err
		}
		if err = json.Unmarshal([]byte(raw), &p.Embed); err != nil {
			return nil, err
		}
		p.FetchedAt = time.Unix(at, 0)
		p.Stale = version < PreviewVersion
		out[u] = p
	}
	return out, rows.Err()
}

func (s *Store) SavePreview(p Preview) error {
	raw, err := json.Marshal(p.Embed)
	if err != nil {
		return err
	}
	_, err = s.db.Exec(`INSERT INTO previews(url,result,success,fetched_at,image_id,version) VALUES(?,?,?,?,?,?)
	 ON CONFLICT(url) DO UPDATE SET result=excluded.result,success=excluded.success,fetched_at=excluded.fetched_at,image_id=excluded.image_id,version=excluded.version`, p.Embed.URL, string(raw), p.Success, p.FetchedAt.Unix(), strings.TrimPrefix(p.Embed.Image, "/api/previews/"), PreviewVersion)
	return err
}

// BindPreviewLinks cannot attach a stale edit or somebody else's message.
func (s *Store) BindPreviewLinks(userID int64, m Message, urls []string) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var body string
	err = tx.QueryRow(`SELECT body FROM messages WHERE id=? AND author_id=? AND deleted_at IS NULL AND kind='' AND channel_id IN (SELECT channel_id FROM channel_members WHERE user_id=?)`, m.ID, userID, userID).Scan(&body)
	if err != nil || body != m.Body {
		return ErrNotFound
	}
	if _, err = tx.Exec(`DELETE FROM message_preview_links WHERE message_id=?`, m.ID); err != nil {
		return err
	}
	for _, u := range urls {
		if _, err = tx.Exec(`INSERT INTO message_preview_links(message_id,url,body_hash) VALUES(?,?,?)`, m.ID, u, previewBodyHash(body)); err != nil {
			return err
		}
	}
	return tx.Commit()
}

// PreviewImageFor applies the same current channel membership as message reads.
func (s *Store) PreviewImageFor(userID int64, id string) error {
	rows, err := s.db.Query(`SELECT m.body,l.body_hash FROM previews p JOIN message_preview_links l ON l.url=p.url
 JOIN messages m ON m.id=l.message_id JOIN channel_members cm ON cm.channel_id=m.channel_id
 WHERE p.image_id=? AND p.success=1 AND cm.user_id=? AND m.deleted_at IS NULL AND m.kind=''`, id, userID)
	if err != nil {
		return err
	}
	defer rows.Close()
	for rows.Next() {
		var body, hash string
		if err := rows.Scan(&body, &hash); err != nil {
			return err
		}
		if previewBodyHash(body) == hash {
			return nil
		}
	}
	if err := rows.Err(); err != nil {
		return err
	}
	return ErrNotFound
}

// Keep a revision fingerprint, never a second copy of the private message.
func previewBodyHash(body string) string {
	sum := sha256.Sum256([]byte(body))
	return hex.EncodeToString(sum[:])
}
