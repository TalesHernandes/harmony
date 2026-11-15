package store

import (
	"database/sql"
	"errors"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"
)

// The shape of the channel column (D12a): renaming a room, folders
// ("categories") that hold text channels and voice rooms together, the order of
// all of it, and pinned messages.
//
// Who may do what is decided above this file, in the API, with one exception
// that belongs here because it is about membership: pinning reads the message
// through MessageFor, so a message from a conversation somebody is not in is
// ErrNotFound, exactly what an invented id is.

// MaxPinsPerChannel is the ceiling the owner set: fifty for D12a, raised to
// Discord's 250 on 30/10/2026 (Discord went from 50 to 250 in August 2025).
const MaxPinsPerChannel = 250

// PinNoteBody is what the line "somebody pinned a message" carries as text.
// The web draws its own words from the kind; the body is for an app that does
// not know kind 'pin' yet, which shows it as an ordinary reply from whoever
// pinned, quoting the pinned message, and that still reads right.
const PinNoteBody = "fixou uma mensagem."

// MaxCategoryNameRunes matches a channel name's length. A folder name is free
// text ("Jogos", "Só os íntimos"), not a slug: it is a heading, not an address.
const MaxCategoryNameRunes = 32

var (
	// ErrNotAllowed is a member asking for something only the owner may do in
	// a place they can see. It is not ErrNotFound because there is nothing to
	// hide: they can already read the channel.
	ErrNotAllowed = errors.New("store: not allowed")
	// ErrPinLimit is one pin past MaxPinsPerChannel in one channel.
	ErrPinLimit = errors.New("store: pin limit")
	// ErrStaleLayout is an order that does not name exactly the folders and
	// rooms that exist now — somebody created or deleted one since the page
	// drew its column. The page reloads and the drag is lost, which is better
	// than writing an order that silently drops a room.
	ErrStaleLayout = errors.New("store: stale layout")
	// ErrBadCategoryName is an empty, over-long or control-character name.
	ErrBadCategoryName = errors.New("store: bad category name")
)

// Category is one folder in the channel column.
type Category struct {
	ID       string
	Name     string
	Position int
}

// Placement is where one room sits: in which folder ("" is the top of the
// column, outside any folder) and at which place among the rooms of its own
// kind there.
type Placement struct {
	ID         string
	CategoryID string
	Position   int
}

// NormalizeCategoryName trims and squeezes the spaces of a folder name. Case is
// kept: the column draws it in capitals, but the name is what the owner typed.
func NormalizeCategoryName(s string) string {
	return strings.Join(strings.Fields(s), " ")
}

// ValidCategoryName reports whether a normalised name can be a folder heading.
func ValidCategoryName(s string) bool {
	if s == "" || utf8.RuneCountInString(s) > MaxCategoryNameRunes {
		return false
	}
	for _, r := range s {
		if unicode.IsControl(r) {
			return false
		}
	}
	return true
}

// ---------------------------------------------------------------- rename

// RenameRoom gives a text channel or a voice room a new name. The same rule as
// creating one: a slug, unique among live rooms of the same kind — which is
// why both rooms called `geral` can each be renamed on their own.
//
// A conversation is never renamed: it is called after the other person.
func (s *Store) RenameRoom(channelID, name string) error {
	name = NormalizeChannelName(name)
	if !ValidChannelName(name) {
		return ErrBadChannelName
	}
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var kind string
	err = tx.QueryRow(`SELECT kind FROM channels
		WHERE id = ? AND kind IN ('text','voice') AND deleted_at IS NULL`, channelID).Scan(&kind)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return err
	}
	var taken int
	if err := tx.QueryRow(`SELECT COUNT(*) FROM channels
		WHERE kind = ? AND name = ? COLLATE NOCASE AND deleted_at IS NULL AND id <> ?`,
		kind, name, channelID).Scan(&taken); err != nil {
		return err
	}
	if taken > 0 {
		return ErrNameTaken
	}
	if _, err := tx.Exec(`UPDATE channels SET name = ? WHERE id = ?`, name, channelID); err != nil {
		return err
	}
	return tx.Commit()
}

// ---------------------------------------------------------------- categories

// Categories is every folder, in the order the column draws them. Everybody
// sees the same folders: they hold rooms everybody is a member of.
func (s *Store) Categories() ([]Category, error) {
	return categories(s.db)
}

type layoutReader interface {
	Query(string, ...any) (*sql.Rows, error)
}

func categories(q layoutReader) ([]Category, error) {
	rows, err := q.Query(`SELECT id, name, position FROM categories ORDER BY position, name, id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Category{}
	for rows.Next() {
		var c Category
		if err := rows.Scan(&c.ID, &c.Name, &c.Position); err != nil {
			return nil, err
		}
		out = append(out, c)
	}
	return out, rows.Err()
}

// CreateCategory adds an empty folder at the bottom of the column. An empty
// folder is legitimate — it is how the owner starts one before dragging
// anything into it.
func (s *Store) CreateCategory(name string) (Category, error) {
	name = NormalizeCategoryName(name)
	if !ValidCategoryName(name) {
		return Category{}, ErrBadCategoryName
	}
	id, err := newID()
	if err != nil {
		return Category{}, err
	}
	tx, err := s.db.Begin()
	if err != nil {
		return Category{}, err
	}
	defer tx.Rollback()
	if err := categoryNameFree(tx, name, ""); err != nil {
		return Category{}, err
	}
	var position int
	if err := tx.QueryRow(`SELECT COALESCE(MAX(position), -1) + 1 FROM categories`).Scan(&position); err != nil {
		return Category{}, err
	}
	if _, err := tx.Exec(`INSERT INTO categories (id, name, position, created_at) VALUES (?, ?, ?, ?)`,
		id, name, position, s.now().UTC().Unix()); err != nil {
		return Category{}, err
	}
	if err := tx.Commit(); err != nil {
		return Category{}, err
	}
	return Category{ID: id, Name: name, Position: position}, nil
}

// RenameCategory changes a folder's heading.
func (s *Store) RenameCategory(id, name string) error {
	name = NormalizeCategoryName(name)
	if !ValidCategoryName(name) {
		return ErrBadCategoryName
	}
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if err := categoryNameFree(tx, name, id); err != nil {
		return err
	}
	res, err := tx.Exec(`UPDATE categories SET name = ? WHERE id = ?`, name, id)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return tx.Commit()
}

// categoryNameFree refuses two folders with the same heading. Two "JOGOS" in a
// column of twenty rows is a puzzle, not a feature.
func categoryNameFree(tx *sql.Tx, name, except string) error {
	var taken int
	if err := tx.QueryRow(`SELECT COUNT(*) FROM categories WHERE name = ? COLLATE NOCASE AND id <> ?`,
		name, except).Scan(&taken); err != nil {
		return err
	}
	if taken > 0 {
		return ErrNameTaken
	}
	return nil
}

// DeleteCategory removes a folder and **never a room**: what was inside it
// moves to the end of the top of the column, in the order it had, and every
// message in it stays where it was. A folder is a heading, and deleting a
// heading that took six channels and their history with it would be the most
// expensive misclick in the app.
func (s *Store) DeleteCategory(id string) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var exists int
	if err := tx.QueryRow(`SELECT COUNT(*) FROM categories WHERE id = ?`, id).Scan(&exists); err != nil {
		return err
	}
	if exists == 0 {
		return ErrNotFound
	}
	// Each kind goes after the last of its kind already at the top, keeping
	// the order it had inside the folder.
	for _, kind := range []string{ChannelText, ChannelVoice} {
		var base int
		if err := tx.QueryRow(`SELECT COALESCE(MAX(position), -1) + 1 FROM channels
			WHERE kind = ? AND category_id IS NULL AND deleted_at IS NULL`, kind).Scan(&base); err != nil {
			return err
		}
		rows, err := tx.Query(`SELECT id FROM channels WHERE kind = ? AND category_id = ?
			ORDER BY position, name, id`, kind, id)
		if err != nil {
			return err
		}
		var ids []string
		for rows.Next() {
			var cid string
			if err := rows.Scan(&cid); err != nil {
				rows.Close()
				return err
			}
			ids = append(ids, cid)
		}
		rows.Close()
		if err := rows.Err(); err != nil {
			return err
		}
		for i, cid := range ids {
			if _, err := tx.Exec(`UPDATE channels SET category_id = NULL, position = ? WHERE id = ?`, base+i, cid); err != nil {
				return err
			}
		}
	}
	if _, err := tx.Exec(`DELETE FROM categories WHERE id = ?`, id); err != nil {
		return err
	}
	return tx.Commit()
}

// ---------------------------------------------------------------- layout

// Placements is where every live room sits. It is the half of the layout event
// that is not the folders themselves.
func (s *Store) Placements() ([]Placement, error) {
	return placements(s.db)
}

func placements(q layoutReader) ([]Placement, error) {
	rows, err := q.Query(`SELECT id, COALESCE(category_id, ''), position FROM channels
		WHERE kind IN ('text','voice') AND deleted_at IS NULL ORDER BY kind, position, name, id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Placement{}
	for rows.Next() {
		var p Placement
		if err := rows.Scan(&p.ID, &p.CategoryID, &p.Position); err != nil {
			return nil, err
		}
		out = append(out, p)
	}
	return out, rows.Err()
}

// Layout reads folders and room placements from the same committed order.
// Separate reads could combine two concurrent saves into a layout that was
// never written, even though each save itself is atomic.
func (s *Store) Layout() ([]Category, []Placement, error) {
	tx, err := s.db.Begin()
	if err != nil {
		return nil, nil, err
	}
	defer tx.Rollback()
	cats, err := categories(tx)
	if err != nil {
		return nil, nil, err
	}
	rooms, err := placements(tx)
	if err != nil {
		return nil, nil, err
	}
	if err := tx.Commit(); err != nil {
		return nil, nil, err
	}
	return cats, rooms, nil
}

// SaveLayout writes the whole order of the column at once: the folders in
// order, and every room in order with the folder it is in. It is all or
// nothing, in one transaction, and it has to name **exactly** the folders and
// live rooms there are — not one more, not one less, none twice. A drag sends
// the column as the page sees it; if the page is behind (a room was created or
// deleted meanwhile) the answer is ErrStaleLayout and nothing is written.
//
// Positions are counted per folder and per kind: text channels sit above voice
// rooms inside each folder, as on Discord, so a room's place is "third voice
// room in Jogos" and not a number shared with the text channels.
func (s *Store) SaveLayout(categories []string, rooms []Placement) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()

	// The folders that exist, and the rooms that exist with their kind.
	known := map[string]bool{}
	rows, err := tx.Query(`SELECT id FROM categories`)
	if err != nil {
		return err
	}
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return err
		}
		known[id] = false
	}
	rows.Close()
	if len(categories) != len(known) {
		return ErrStaleLayout
	}
	for _, id := range categories {
		seen, ok := known[id]
		if !ok || seen {
			return ErrStaleLayout
		}
		known[id] = true
	}

	kinds := map[string]string{}
	rows, err = tx.Query(`SELECT id, kind FROM channels WHERE kind IN ('text','voice') AND deleted_at IS NULL`)
	if err != nil {
		return err
	}
	for rows.Next() {
		var id, kind string
		if err := rows.Scan(&id, &kind); err != nil {
			rows.Close()
			return err
		}
		kinds[id] = kind
	}
	rows.Close()
	if len(rooms) != len(kinds) {
		return ErrStaleLayout
	}

	for i, id := range categories {
		if _, err := tx.Exec(`UPDATE categories SET position = ? WHERE id = ?`, i, id); err != nil {
			return err
		}
	}
	next := map[string]int{} // "folder/kind" -> next position
	placed := map[string]bool{}
	for _, r := range rooms {
		kind, ok := kinds[r.ID]
		if !ok || placed[r.ID] {
			return ErrStaleLayout
		}
		if _, ok := known[r.CategoryID]; r.CategoryID != "" && !ok {
			return ErrStaleLayout
		}
		placed[r.ID] = true
		key := r.CategoryID + "/" + kind
		var folder any
		if r.CategoryID != "" {
			folder = r.CategoryID
		}
		if _, err := tx.Exec(`UPDATE channels SET category_id = ?, position = ? WHERE id = ?`,
			folder, next[key], r.ID); err != nil {
			return err
		}
		next[key]++
	}
	return tx.Commit()
}

// ---------------------------------------------------------------- pins

// Pin fixes a message to its channel, or unfixes it. Who may:
//
//   - in a conversation of two, either of the two;
//   - in a text channel, only the owner (until roles, D12c–e);
//   - in a voice room, nobody — a room has no messages to pin.
//
// A message from a channel the asker is not in is ErrNotFound, the same answer
// an invented id gets. A removed message cannot be pinned, and removing one
// takes its pin with it (DeleteMessage).
//
// Pinning twice, or unpinning something that is not pinned, is not an error:
// the page says which way it should end up, like a reaction.
//
// A pin that is new also writes a line in the channel, kind 'pin', from the
// person who pinned and answering the pinned message, in the same transaction
// (the owner's choice, 30/10/2026, as Discord does). It comes back as note;
// note.ID is 0 when nothing new was pinned. Unpinning writes nothing.
func (s *Store) Pin(userID, messageID int64, on, isOwner bool) (msg Message, note Message, err error) {
	msg, err = s.MessageFor(userID, messageID)
	if err != nil {
		return Message{}, Message{}, err
	}
	var kind string
	if err := s.db.QueryRow(`SELECT kind FROM channels WHERE id = ?`, msg.ChannelID).Scan(&kind); err != nil {
		return Message{}, Message{}, err
	}
	switch {
	case kind == ChannelDM:
	case kind == ChannelText && isOwner:
	case kind == ChannelText:
		return Message{}, Message{}, ErrNotAllowed
	default:
		return Message{}, Message{}, ErrNotFound
	}
	if on && msg.Deleted() {
		return Message{}, Message{}, ErrNotFound
	}
	// A line the server wrote (a missed call, a pin notice) is not something
	// to find again, and pinning a pin notice would write another one.
	if on && msg.Kind != "" {
		return Message{}, Message{}, ErrNotAllowed
	}

	tx, err := s.db.Begin()
	if err != nil {
		return Message{}, Message{}, err
	}
	defer tx.Rollback()
	// MessageFor ran before this transaction. Deletion may have removed the
	// message's pin or the asker's membership since then; neither may be
	// undone by a pin based on that earlier read.
	var deleted sql.NullInt64
	err = tx.QueryRow(`SELECT m.deleted_at FROM messages m
		JOIN channel_members cm ON cm.channel_id = m.channel_id AND cm.user_id = ?
		WHERE m.id = ?`, userID, messageID).Scan(&deleted)
	if errors.Is(err, sql.ErrNoRows) || (err == nil && on && deleted.Valid) {
		return Message{}, Message{}, ErrNotFound
	}
	if err != nil {
		return Message{}, Message{}, err
	}
	if on {
		var already, count int
		if err := tx.QueryRow(`SELECT COUNT(*) FROM pins WHERE message_id = ?`, messageID).Scan(&already); err != nil {
			return Message{}, Message{}, err
		}
		if already == 0 {
			if err := tx.QueryRow(`SELECT COUNT(*) FROM pins WHERE channel_id = ?`, msg.ChannelID).Scan(&count); err != nil {
				return Message{}, Message{}, err
			}
			if count >= MaxPinsPerChannel {
				return Message{}, Message{}, ErrPinLimit
			}
			now := s.now().UTC().Unix()
			if _, err := tx.Exec(`INSERT INTO pins (message_id, channel_id, pinned_by, pinned_at) VALUES (?, ?, ?, ?)`,
				messageID, msg.ChannelID, userID, now); err != nil {
				return Message{}, Message{}, err
			}
			res, err := tx.Exec(`INSERT INTO messages (channel_id, author_id, body, kind, reply_to, created_at) VALUES (?, ?, ?, 'pin', ?, ?)`,
				msg.ChannelID, userID, PinNoteBody, messageID, now)
			if err != nil {
				return Message{}, Message{}, err
			}
			if note.ID, err = res.LastInsertId(); err != nil {
				return Message{}, Message{}, err
			}
		}
	} else if _, err := tx.Exec(`DELETE FROM pins WHERE message_id = ?`, messageID); err != nil {
		return Message{}, Message{}, err
	}
	if err := tx.Commit(); err != nil {
		return Message{}, Message{}, err
	}
	if msg, err = s.MessageFor(userID, messageID); err != nil {
		return Message{}, Message{}, err
	}
	if note.ID != 0 {
		if note, err = s.MessageFor(userID, note.ID); err != nil {
			return Message{}, Message{}, err
		}
	}
	return msg, note, nil
}

// Pins is the pinned messages of one channel, the most recently pinned first,
// and only for somebody who is in it.
func (s *Store) Pins(userID int64, channelID string) ([]Message, error) {
	if _, err := s.ChannelFor(userID, channelID); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(`
SELECT m.id, m.channel_id, m.author_id, u.username, u.display_name, m.body,
       m.created_at, m.edited_at, m.deleted_at, d.username, m.reply_to, m.kind
  FROM pins p
  JOIN messages m ON m.id = p.message_id AND m.channel_id = p.channel_id
  JOIN users u ON u.id = m.author_id
  LEFT JOIN users d ON d.id = m.deleted_by
 WHERE p.channel_id = ? AND m.deleted_at IS NULL
 ORDER BY p.pinned_at DESC, p.message_id DESC`, channelID)
	if err != nil {
		return nil, err
	}
	list, err := scanMessages(rows)
	if err != nil {
		return nil, err
	}
	if list == nil {
		list = []Message{}
	}
	return s.fill(list)
}

// pinned is when each of these messages was pinned, for fill.
func (s *Store) pinned(in string, ids []any) (map[int64]time.Time, error) {
	rows, err := s.db.Query(`SELECT message_id, pinned_at FROM pins WHERE message_id IN `+in, ids...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[int64]time.Time{}
	for rows.Next() {
		var id, at int64
		if err := rows.Scan(&id, &at); err != nil {
			return nil, err
		}
		out[id] = time.Unix(at, 0).UTC()
	}
	return out, rows.Err()
}
