package store

import (
	"database/sql"
	"errors"
	"fmt"
	"regexp"
	"sort"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"
)

// Limits the layers above enforce. They live here because the store is what
// counts, and a limit whose number sits somewhere else drifts away from it.
const (
	// MaxMessageRunes is generous for chat and small enough that a bug in a
	// client cannot post a book.
	MaxMessageRunes = 4000
	// MaxChannelNameRunes and MaxTopicRunes keep the channel column readable.
	MaxChannelNameRunes = 32
	MaxTopicRunes       = 140
	// MaxAttachmentsPerMessage: four images is a screenshot dump, not a photo
	// album.
	MaxAttachmentsPerMessage = 4
	// MaxReactionRunes is how long one reaction may be. It is not one
	// character: a single emoji as it comes off a telephone keyboard is
	// routinely several code points — a face with a skin tone is two, a flag
	// is two, a family joined with zero-width joiners is seven — and cutting
	// those off would be refusing the keyboard people actually use.
	MaxReactionRunes = 12
	// MaxReactionsPerPerson is how many different reactions one person may put
	// on one message. It is the only limit on this table, and it is enough:
	// the row of buttons under a message is shared, so the way to spoil it is
	// to fill it, and nobody needs seven opinions about one line.
	MaxReactionsPerPerson = 6
)

// Usage kinds counted in the usage table, for the per-person limits
// PLANO-DISCORD.md §5 asks for.
const (
	UsageMessage = "message"
	UsageUpload  = "upload"
)

// UsageRetention is how long a usage row is worth keeping: the longest window
// any limit uses, plus a margin. PurgeExpired deletes past it.
const UsageRetention = 48 * time.Hour

// Channel kinds. A voice channel is a channel like any other — the same row in
// the same table, the same membership rows — so "may this person be in it?" is
// the question the store already answers and not a second rule written later.
const (
	ChannelText  = "text"
	ChannelDM    = "dm"
	ChannelVoice = "voice"
)

// Errors the API turns into status codes.
var (
	// ErrLastChannel protects the last live text channel.
	ErrLastChannel = errors.New("store: last text channel")
	// ErrBadChannelName is a name the channel column cannot show.
	ErrBadChannelName = errors.New("store: bad channel name")
	// ErrBadMessage is an empty or over-long body.
	ErrBadMessage = errors.New("store: bad message")
	// ErrBadAttachments is more attachments than a message may carry, or an
	// attachment that is not this person's, or not from this channel, or
	// already spent on another message.
	ErrBadAttachments = errors.New("store: bad attachments")
	// ErrBadReaction is something that is not an emoji, or one person's
	// seventh reaction to the same message.
	ErrBadReaction = errors.New("store: bad reaction")
)

// Channel is a text channel or a direct message.
type Channel struct {
	ID        string
	Kind      string
	Name      string
	Topic     string
	Position  int
	CreatedAt time.Time
	// CategoryID is the folder a room sits in, or "" for the top of the
	// column (D12a). Always "" for a conversation.
	CategoryID string
	// Filled by ListChannels and ChannelFor only.
	PeerName        string // the other person, for a DM
	PeerDisplayName string
	Unread          int
	Mentions        int
	LastMessageID   int64
	LastReadID      int64
	// Muted is this person's own silence on this channel: it never notifies
	// them, and ListChannels reports no unread for it at all.
	Muted bool
}

// Attachment is one uploaded image. The file on disk is named after ID; Name is
// only what to show the reader and what to call it on the way back out.
type Attachment struct {
	ID        string
	MessageID int64
	ChannelID string
	OwnerID   int64
	Name      string
	MIME      string
	Size      int64
	Width     int
	Height    int
	CreatedAt time.Time
}

// Reaction is one emoji on one message, and everybody who pressed it. The
// names are what makes it worth having: "quem foi que riu disso" is most of the
// point, and with twenty people the list is short enough to simply carry.
//
// There is no "mine" field, deliberately. The same message is sent down the
// socket to everybody in the channel at once, so a field meaning "yours" would
// have to be a different answer per reader — the page works it out by looking
// for its own name in Users, which is the same fact without a second shape.
type Reaction struct {
	Emoji string
	Users []string
}

// Message is one line of a conversation. A removed message keeps its row and
// its id: DeletedAt says it is gone, and the ids around it keep their meaning.
type Message struct {
	ID          int64
	ChannelID   string
	AuthorID    int64
	AuthorName  string
	AuthorLabel string
	Body        string
	// Kind is '' for everything a person wrote, and 'call' for the notice this
	// server leaves when a call went unanswered (migration 9).
	Kind        string
	CreatedAt   time.Time
	EditedAt    time.Time
	DeletedAt   time.Time
	DeletedBy   string
	Attachments []Attachment
	Mentions    []string

	// ReplyTo is the message this one answers, or 0. The three fields under it
	// are the line quoted above the reply, and they are read from the original
	// at the moment a page asks rather than copied in when the reply is written
	// — which is the whole reason editing the original changes every quotation
	// of it, and removing it turns every quotation into a tombstone, without a
	// single stored row being rewritten.
	ReplyTo        int64
	ReplyToAuthor  string
	ReplyToLabel   string
	ReplyToBody    string
	ReplyToDeleted bool

	// Reactions in the order the first person put each one on, which is the
	// order the buttons are drawn in and the reason it never reshuffles under
	// somebody's finger.
	Reactions []Reaction

	// Pinned is whether the message is fixed to its channel (D12a).
	Pinned bool
}

// MaxQuoteRunes is how much of the answered message is carried along. It is a
// glance back, not a second copy of the conversation.
const MaxQuoteRunes = 140

// Deleted reports whether the message is a tombstone.
func (m Message) Deleted() bool { return !m.DeletedAt.IsZero() }

// mentionRE finds @username in a message body. The shape is the username shape
// from ValidUsername, so "@tales." mentions tales and leaves the full stop
// where the writer put it.
var mentionRE = regexp.MustCompile(`(?i)@([a-z0-9][a-z0-9._-]{1,30}[a-z0-9])`)

var channelNameRE = regexp.MustCompile(`^[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?$`)

// NormalizeChannelName is the Discord habit: lower case, spaces become dashes.
func NormalizeChannelName(s string) string {
	s = strings.ToLower(strings.TrimSpace(s))
	s = strings.TrimPrefix(s, "#")
	return strings.Join(strings.Fields(s), "-")
}

// ValidChannelName reports whether the name is one to thirty-two characters of
// lower-case letters, digits and dashes.
func ValidChannelName(s string) bool {
	return len(s) >= 1 && len(s) <= MaxChannelNameRunes && channelNameRE.MatchString(s)
}

// dmKey is the unique name of a conversation between two people, and it does
// not depend on who opened it first.
func dmKey(a, b int64) string {
	if a > b {
		a, b = b, a
	}
	return fmt.Sprintf("%d:%d", a, b)
}

// ---------------------------------------------------------------- channels

// CreateTextChannel makes a channel everybody can see, and gives everybody the
// membership row that lets them see it. Both halves happen in one transaction:
// a channel nobody is a member of would be invisible and unfixable from the
// outside.
func (s *Store) CreateTextChannel(createdBy int64, name, topic string) (Channel, error) {
	return s.createRoom(ChannelText, createdBy, name, topic)
}

// CreateVoiceChannel makes a voice room. The only thing that differs from a
// text channel is the kind: who may be inside is still the membership table,
// and the media server never asks that question itself — it is told.
func (s *Store) CreateVoiceChannel(createdBy int64, name, topic string) (Channel, error) {
	return s.createRoom(ChannelVoice, createdBy, name, topic)
}

func (s *Store) createRoom(kind string, createdBy int64, name, topic string) (Channel, error) {
	name = NormalizeChannelName(name)
	topic = strings.TrimSpace(topic)
	if !ValidChannelName(name) || len([]rune(topic)) > MaxTopicRunes {
		return Channel{}, ErrBadChannelName
	}
	id, err := newID()
	if err != nil {
		return Channel{}, err
	}
	now := s.now().UTC()

	tx, err := s.db.Begin()
	if err != nil {
		return Channel{}, err
	}
	defer tx.Rollback()

	var taken int
	if err := tx.QueryRow(`SELECT COUNT(*) FROM channels WHERE kind = ? AND name = ? COLLATE NOCASE AND deleted_at IS NULL`, kind, name).Scan(&taken); err != nil {
		return Channel{}, err
	}
	if taken > 0 {
		return Channel{}, ErrNameTaken
	}
	var position int
	if err := tx.QueryRow(`SELECT COALESCE(MAX(position), 0) + 1 FROM channels WHERE kind = ? AND deleted_at IS NULL`, kind).Scan(&position); err != nil {
		return Channel{}, err
	}
	if _, err := tx.Exec(
		`INSERT INTO channels (id, kind, name, topic, position, created_by, created_at)
		 VALUES (?, ?, ?, ?, ?, ?, ?)`,
		id, kind, name, topic, position, createdBy, now.Unix()); err != nil {
		return Channel{}, err
	}
	if _, err := tx.Exec(
		`INSERT INTO channel_members (channel_id, user_id, added_at, last_read_message_id)
		 SELECT ?, id, ?, 0 FROM users`, id, now.Unix()); err != nil {
		return Channel{}, err
	}
	if err := tx.Commit(); err != nil {
		return Channel{}, err
	}
	return Channel{ID: id, Kind: kind, Name: name, Topic: topic, Position: position, CreatedAt: now}, nil
}

// DeleteRoom hides a shared room without destroying its messages or attachments.
// The caller enforces admin access; userID records who deleted it. Revoking
// membership and marking the room happen together, including the last-room check.
func (s *Store) DeleteRoom(userID int64, channelID string) error {
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
	if kind == ChannelText {
		var live int
		if err := tx.QueryRow(`SELECT COUNT(*) FROM channels WHERE kind = 'text' AND deleted_at IS NULL`).Scan(&live); err != nil {
			return err
		}
		if live <= 1 {
			return ErrLastChannel
		}
	}
	if _, err := tx.Exec(`UPDATE channels SET deleted_at = ?, deleted_by = ? WHERE id = ?`, s.now().UTC().Unix(), userID, channelID); err != nil {
		return err
	}
	if _, err := tx.Exec(`DELETE FROM channel_members WHERE channel_id = ?`, channelID); err != nil {
		return err
	}
	return tx.Commit()
}

// EnsureDM returns the conversation between two people, creating it the first
// time. The unique dm_key is what makes two simultaneous "open a DM" presses
// end up in the same conversation instead of two.
func (s *Store) EnsureDM(a, b int64) (Channel, error) {
	if a == b || a <= 0 || b <= 0 {
		return Channel{}, ErrNotFound
	}
	key := dmKey(a, b)
	if ch, err := s.dmByKey(key); err == nil {
		// Asking for a conversation is the way back into one that was closed.
		// Both sides are cleared, not just the asker's: the person being
		// written to closed it once and is about to be written to again, and a
		// conversation that arrives invisible is a message that was swallowed.
		if _, err := s.db.Exec(
			`UPDATE channel_members SET hidden_at = NULL WHERE channel_id = ?`, ch.ID); err != nil {
			return Channel{}, err
		}
		return ch, nil
	} else if !errors.Is(err, ErrNotFound) {
		return Channel{}, err
	}

	id, err := newID()
	if err != nil {
		return Channel{}, err
	}
	now := s.now().UTC()
	tx, err := s.db.Begin()
	if err != nil {
		return Channel{}, err
	}
	defer tx.Rollback()
	if _, err := tx.Exec(
		`INSERT INTO channels (id, kind, name, position, created_by, created_at, dm_key)
		 VALUES (?, 'dm', '', 0, ?, ?, ?)`, id, a, now.Unix(), key); err != nil {
		// Somebody else created it between the lookup and here; dm_key is
		// UNIQUE precisely so that this is a lost race and not a second
		// conversation.
		//
		// End the transaction before looking again, and do not merge these two
		// lines. The pool is one connection wide (db.go), this transaction is
		// holding it, and dmByKey asks the pool for a connection of its own: it
		// would wait for the transaction that only returns after it, with no
		// context and no deadline. That is not a slow query, it is the whole
		// service stopping — every other request queues behind the same
		// connection, and /healthz keeps answering ok because it never asks the
		// database anything.
		_ = tx.Rollback()
		if ch, e := s.dmByKey(key); e == nil {
			return ch, nil
		}
		return Channel{}, err
	}
	for _, uid := range []int64{a, b} {
		if _, err := tx.Exec(
			`INSERT INTO channel_members (channel_id, user_id, added_at, last_read_message_id)
			 VALUES (?, ?, ?, 0)`, id, uid, now.Unix()); err != nil {
			return Channel{}, err
		}
	}
	if err := tx.Commit(); err != nil {
		return Channel{}, err
	}
	return s.ChannelFor(a, id)
}

func (s *Store) dmByKey(key string) (Channel, error) {
	var ch Channel
	var created int64
	err := s.db.QueryRow(
		`SELECT id, kind, name, topic, position, created_at FROM channels WHERE dm_key = ?`, key).
		Scan(&ch.ID, &ch.Kind, &ch.Name, &ch.Topic, &ch.Position, &created)
	if errors.Is(err, sql.ErrNoRows) {
		return Channel{}, ErrNotFound
	}
	if err != nil {
		return Channel{}, err
	}
	ch.CreatedAt = time.Unix(created, 0).UTC()
	return ch, nil
}

// ChannelFor is the one gate. It answers with the channel only when the asker
// has a membership row, so an id belonging to two other people answers exactly
// what an invented id answers.
func (s *Store) ChannelFor(userID int64, channelID string) (Channel, error) {
	var ch Channel
	var created int64
	err := s.db.QueryRow(
		`SELECT c.id, c.kind, c.name, c.topic, c.position, c.created_at, COALESCE(c.category_id, '')
		   FROM channels c
		   JOIN channel_members m ON m.channel_id = c.id AND m.user_id = ?
		  WHERE c.id = ?`, userID, channelID).
		Scan(&ch.ID, &ch.Kind, &ch.Name, &ch.Topic, &ch.Position, &created, &ch.CategoryID)
	if errors.Is(err, sql.ErrNoRows) {
		return Channel{}, ErrNotFound
	}
	if err != nil {
		return Channel{}, err
	}
	ch.CreatedAt = time.Unix(created, 0).UTC()
	if ch.Kind == ChannelDM {
		var peer, label sql.NullString
		_ = s.db.QueryRow(
			`SELECT u.username, u.display_name FROM channel_members m
			   JOIN users u ON u.id = m.user_id
			  WHERE m.channel_id = ? AND m.user_id <> ?`, channelID, userID).Scan(&peer, &label)
		ch.PeerName, ch.PeerDisplayName = peer.String, label.String
	}
	return ch, nil
}

// VoiceChannelIDs is the list of voice rooms one person may be shown. The media
// server keeps no idea of who may be where; it is handed this list and reports
// only on what is in it, which is how "A cannot see B's room" stays a single
// rule in a single place.
func (s *Store) VoiceChannelIDs(userID int64) ([]string, error) {
	rows, err := s.db.Query(
		`SELECT c.id FROM channels c
		   JOIN channel_members m ON m.channel_id = c.id AND m.user_id = ?
		   LEFT JOIN categories g ON g.id = c.category_id
		  WHERE c.kind = 'voice' ORDER BY COALESCE(g.position, -1), c.position, c.name`, userID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []string{}
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		out = append(out, id)
	}
	return out, rows.Err()
}

// ChannelMemberIDs is who a live update goes to. Nothing is ever broadcast to
// everybody: the fan-out list is the membership table, same as the reads.
func (s *Store) ChannelMemberIDs(channelID string) ([]int64, error) {
	rows, err := s.db.Query(`SELECT user_id FROM channel_members WHERE channel_id = ?`, channelID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []int64
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		out = append(out, id)
	}
	return out, rows.Err()
}

// ListChannels is the channel column: the text channels, the conversations this
// person is part of, and for each one how much is unread and how much of that
// unread names them.
//
// A conversation this person has closed is not in it — that is what closing
// one means — unless something has been said in it since, in which case it
// comes back. Closing is "not now", never "never again": a closed conversation
// that quietly ate messages would be a blocklist that nobody was told about.
func (s *Store) ListChannels(userID int64) ([]Channel, error) {
	rows, err := s.db.Query(`
SELECT c.id, c.kind, c.name, c.topic, c.position, c.created_at, COALESCE(c.category_id, ''),
       m.last_read_message_id,
       (SELECT COUNT(*) FROM messages x
         WHERE x.channel_id = c.id AND x.id > m.last_read_message_id
           AND x.author_id <> ? AND x.deleted_at IS NULL
           AND NOT EXISTS(SELECT 1 FROM person_preferences p WHERE p.owner_id=m.user_id AND p.about_id=x.author_id AND p.blocked=1)),
       (SELECT COUNT(*) FROM mentions n JOIN messages y ON y.id = n.message_id
         WHERE n.user_id = ? AND n.channel_id = c.id
           AND n.message_id > m.last_read_message_id AND y.deleted_at IS NULL
           AND NOT EXISTS(SELECT 1 FROM person_preferences p WHERE p.owner_id=m.user_id AND p.about_id=y.author_id AND p.blocked=1)),
       (SELECT COALESCE(MAX(z.id), 0) FROM messages z WHERE z.channel_id = c.id),
       CASE WHEN c.kind = 'dm' THEN (
            SELECT u.username FROM channel_members mm JOIN users u ON u.id = mm.user_id
             WHERE mm.channel_id = c.id AND mm.user_id <> ? LIMIT 1) END,
       CASE WHEN c.kind = 'dm' THEN (
            SELECT u.display_name FROM channel_members mm JOIN users u ON u.id = mm.user_id
             WHERE mm.channel_id = c.id AND mm.user_id <> ? LIMIT 1) END,
       m.muted_at
  FROM channels c
  JOIN channel_members m ON m.channel_id = c.id AND m.user_id = ?
  LEFT JOIN categories g ON g.id = c.category_id
 WHERE m.hidden_at IS NULL
    OR EXISTS (SELECT 1 FROM messages w
                WHERE w.channel_id = c.id AND w.id > m.last_read_message_id
                  AND w.author_id <> ? AND w.deleted_at IS NULL)
 ORDER BY c.kind,
          -- The rooms at the top of the column first, then folder by folder
          -- (D12a). An app that draws no folders still gets the rooms in the
          -- order the owner sees them, only without the headings.
          COALESCE(g.position, -1), c.position,
          -- Conversations come newest-first, channels stay where the owner
          -- put them. They are ordered by different things because they *are*
          -- different things: a channel is a place, and a place that moved
          -- every time somebody spoke in it would be a place nobody could
          -- find. A conversation is a person, and the person you spoke to last
          -- is the one you are most likely to want.
          --
          -- A conversation with nothing in it yet sorts by when it was made,
          -- which puts a brand new one at the top — where somebody who has
          -- just opened it is looking.
          CASE WHEN c.kind = 'dm'
               THEN -COALESCE((SELECT MAX(t.created_at) FROM messages t
                                WHERE t.channel_id = c.id), c.created_at)
               ELSE 0 END,
          c.name`,
		userID, userID, userID, userID, userID, userID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Channel{}
	for rows.Next() {
		var ch Channel
		var created int64
		var peer, peerLabel sql.NullString
		var muted sql.NullInt64
		if err := rows.Scan(&ch.ID, &ch.Kind, &ch.Name, &ch.Topic, &ch.Position, &created,
			&ch.CategoryID, &ch.LastReadID, &ch.Unread, &ch.Mentions, &ch.LastMessageID, &peer, &peerLabel,
			&muted); err != nil {
			return nil, err
		}
		ch.CreatedAt = time.Unix(created, 0).UTC()
		ch.PeerName, ch.PeerDisplayName = peer.String, peerLabel.String
		ch.Muted = muted.Valid
		if ch.Muted {
			// Silenced means it never asks for anything. The count is not
			// hidden from the page and then shown anyway: it is not counted.
			ch.Unread, ch.Mentions = 0, 0
		}
		out = append(out, ch)
	}
	return out, rows.Err()
}

// SetMuted silences a channel for one person, or stops silencing it. Silenced
// means it never asks for anything: no notification on a telephone, no count in
// the column, no bold name. What is in it is still all there when it is opened.
//
// It is per person and per channel, so silencing #geral silences it for you and
// for nobody else — which is the only way a shared channel can work.
func (s *Store) SetMuted(userID int64, channelID string, muted bool) error {
	if _, err := s.ChannelFor(userID, channelID); err != nil {
		return err
	}
	var at any
	if muted {
		at = s.now().UTC().Unix()
	}
	_, err := s.db.Exec(
		`UPDATE channel_members SET muted_at = ? WHERE channel_id = ? AND user_id = ?`,
		at, channelID, userID)
	return err
}

// Muted reports whether this person has silenced this channel. It is asked once
// per notification, which is the only place it changes a decision.
func (s *Store) Muted(userID int64, channelID string) (bool, error) {
	var at sql.NullInt64
	err := s.db.QueryRow(
		`SELECT muted_at FROM channel_members WHERE channel_id = ? AND user_id = ?`,
		channelID, userID).Scan(&at)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	return at.Valid, err
}

// HideChannel closes a conversation for one person: it leaves the column and
// nothing else happens to it. The messages stay, the other person's copy is
// untouched, and it comes back the moment either of them writes in it again
// (ListChannels) or this person opens it again (EnsureDM).
//
// Only a conversation, never a channel of the server. A shared channel that one
// person could make disappear is a channel they would then have no way back
// into — and a room everybody is in is not a tab.
// It also marks the conversation read, and that is not a side effect — it is
// how "comes back when there is something new" is decided. Coming back is
// asked as a question about message ids and never about the clock: the row is
// written and the message that reopens it can easily land in the same second,
// and a comparison of two timestamps a second apart would have answered "no"
// and swallowed it. Closing something you are looking at means you have read
// it, so the mark is honest as well as useful.
func (s *Store) HideChannel(userID int64, channelID string) error {
	ch, err := s.ChannelFor(userID, channelID)
	if err != nil {
		return err
	}
	if ch.Kind != ChannelDM {
		return ErrNotFound
	}
	_, err = s.db.Exec(`
UPDATE channel_members
   SET hidden_at = ?,
       last_read_message_id = MAX(last_read_message_id,
           COALESCE((SELECT MAX(id) FROM messages WHERE channel_id = ?), 0))
 WHERE channel_id = ? AND user_id = ?`,
		s.now().UTC().Unix(), channelID, channelID, userID)
	return err
}

// MarkRead moves this person's read mark in one channel. Backwards is allowed
// on purpose — "mark as unread" is the same call with a smaller id — but only
// within a channel they belong to.
func (s *Store) MarkRead(userID int64, channelID string, upTo int64) error {
	res, err := s.db.Exec(
		`UPDATE channel_members SET last_read_message_id = ? WHERE channel_id = ? AND user_id = ?`,
		upTo, channelID, userID)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

// ---------------------------------------------------------------- messages

// Messages returns a page of history, oldest first. before is exclusive and 0
// means "the newest page"; the caller scrolls back by passing the id of the
// oldest line it already has.
func (s *Store) Messages(userID int64, channelID string, before int64, limit int) ([]Message, error) {
	if _, err := s.ChannelFor(userID, channelID); err != nil {
		return nil, err
	}
	if limit <= 0 || limit > 200 {
		limit = 50
	}
	if before <= 0 {
		before = 1 << 62
	}
	rows, err := s.db.Query(`
SELECT m.id, m.channel_id, m.author_id, u.username, u.display_name, m.body,
       m.created_at, m.edited_at, m.deleted_at, d.username, m.reply_to, m.kind
  FROM messages m
  JOIN users u ON u.id = m.author_id
  LEFT JOIN users d ON d.id = m.deleted_by
 WHERE m.channel_id = ? AND m.id < ?
 ORDER BY m.id DESC LIMIT ?`, channelID, before, limit)
	if err != nil {
		return nil, err
	}
	list, err := scanMessages(rows)
	if err != nil {
		return nil, err
	}
	// Query descending (that is the index), hand back ascending (that is
	// reading order).
	sort.Slice(list, func(i, j int) bool { return list[i].ID < list[j].ID })
	return s.fill(list)
}

// MessagesWindow reads around a live anchor, or forward from it. An anchor
// from another channel and a missing id answer alike; so does a deleted one
// to jump to, but reading on past one is fine: the last message a reader has
// may be removed while they read, and scrolling down must not stop there.
// Each side fetches one extra row so its continuation flag is exact.
func (s *Store) MessagesWindow(userID int64, channelID string, anchorID int64, limit int, around bool) ([]Message, bool, bool, error) {
	if _, err := s.ChannelFor(userID, channelID); err != nil {
		return nil, false, false, err
	}
	anchor, err := s.MessageFor(userID, anchorID)
	if err != nil {
		return nil, false, false, err
	}
	if anchor.ChannelID != channelID || (around && anchor.Deleted()) {
		return nil, false, false, ErrNotFound
	}
	if limit <= 0 || limit > 200 {
		limit = 50
	}
	count := limit
	if around {
		count = limit / 2
	}
	rows, err := s.db.Query(`
SELECT m.id, m.channel_id, m.author_id, u.username, u.display_name, m.body,
       m.created_at, m.edited_at, m.deleted_at, d.username, m.reply_to, m.kind
  FROM messages m
  JOIN users u ON u.id = m.author_id
  LEFT JOIN users d ON d.id = m.deleted_by
 WHERE m.channel_id = ? AND m.id > ?
 ORDER BY m.id ASC LIMIT ?`, channelID, anchorID, count+1)
	if err != nil {
		return nil, false, false, err
	}
	newer, err := scanMessages(rows)
	if err != nil {
		return nil, false, false, err
	}
	hasNewer := len(newer) > count
	if hasNewer {
		newer = newer[:count]
	}
	newer, err = s.fill(newer)
	if err != nil {
		return nil, false, false, err
	}
	list := newer
	hasMore := false
	if around {
		older, err := s.Messages(userID, channelID, anchorID, count+1)
		if err != nil {
			return nil, false, false, err
		}
		hasMore = len(older) > count
		if hasMore {
			older = older[len(older)-count:]
		}
		list = append(append(older, anchor), newer...)
	}
	return list, hasMore, hasNewer, nil
}

func scanMessages(rows *sql.Rows) ([]Message, error) {
	defer rows.Close()
	out := []Message{}
	for rows.Next() {
		var m Message
		var created int64
		var edited, deleted, replyTo sql.NullInt64
		var deletedBy sql.NullString
		if err := rows.Scan(&m.ID, &m.ChannelID, &m.AuthorID, &m.AuthorName, &m.AuthorLabel,
			&m.Body, &created, &edited, &deleted, &deletedBy, &replyTo, &m.Kind); err != nil {
			return nil, err
		}
		m.ReplyTo = replyTo.Int64
		m.CreatedAt = time.Unix(created, 0).UTC()
		m.EditedAt, m.DeletedAt, m.DeletedBy = fromUnix(edited), fromUnix(deleted), deletedBy.String
		if m.Deleted() {
			// A tombstone carries no text out of the database, ever.
			m.Body = ""
		}
		out = append(out, m)
	}
	return out, rows.Err()
}

// fill attaches the images and the mention list to a page of messages.
func (s *Store) fill(list []Message) ([]Message, error) {
	if len(list) == 0 {
		return list, nil
	}
	byID := make(map[int64]int, len(list))
	ids := make([]any, 0, len(list))
	marks := make([]string, 0, len(list))
	for i, m := range list {
		byID[m.ID] = i
		ids = append(ids, m.ID)
		marks = append(marks, "?")
	}
	in := "(" + strings.Join(marks, ",") + ")"

	rows, err := s.db.Query(`
SELECT id, message_id, channel_id, owner_id, name, mime, size, width, height, created_at
  FROM attachments WHERE message_id IN `+in+` ORDER BY created_at, id`, ids...)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var a Attachment
		var created int64
		var msg sql.NullInt64
		if err := rows.Scan(&a.ID, &msg, &a.ChannelID, &a.OwnerID, &a.Name, &a.MIME,
			&a.Size, &a.Width, &a.Height, &created); err != nil {
			rows.Close()
			return nil, err
		}
		a.MessageID, a.CreatedAt = msg.Int64, time.Unix(created, 0).UTC()
		if i, ok := byID[a.MessageID]; ok && !list[i].Deleted() {
			list[i].Attachments = append(list[i].Attachments, a)
		}
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}

	rows, err = s.db.Query(`SELECT n.message_id, u.username FROM mentions n
		JOIN users u ON u.id = n.user_id WHERE n.message_id IN `+in, ids...)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var id int64
		var name string
		if err := rows.Scan(&id, &name); err != nil {
			rows.Close()
			return nil, err
		}
		if i, ok := byID[id]; ok {
			list[i].Mentions = append(list[i].Mentions, name)
		}
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}

	// And the line each reply is answering. The join walks from the reply to
	// the original **and requires both to be in the same channel** — belt and
	// braces over the check PostMessage already makes, so that a row saying
	// otherwise could still never quote a message out of a conversation the
	// reader is not in.
	rows, err = s.db.Query(`
SELECT a.id, u.username, u.display_name, r.body, r.deleted_at
  FROM messages a
  JOIN messages r ON r.id = a.reply_to AND r.channel_id = a.channel_id
  JOIN users u ON u.id = r.author_id
 WHERE a.id IN `+in, ids...)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var id int64
		var name, label, body string
		var deleted sql.NullInt64
		if err := rows.Scan(&id, &name, &label, &body, &deleted); err != nil {
			rows.Close()
			return nil, err
		}
		i, ok := byID[id]
		if !ok {
			continue
		}
		list[i].ReplyToAuthor, list[i].ReplyToLabel = name, label
		list[i].ReplyToDeleted = deleted.Valid
		if deleted.Valid {
			// A tombstone carries no text out of the database here either.
			body = ""
		}
		if r := []rune(body); len(r) > MaxQuoteRunes {
			body = string(r[:MaxQuoteRunes]) + "…"
		}
		list[i].ReplyToBody = body
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}

	// Which of them are pinned. A tombstone never is (DeleteMessage takes the
	// pin away), but the check is here too, like the reactions below.
	pins, err := s.pinned(in, ids)
	if err != nil {
		return nil, err
	}
	for id := range pins {
		if i, ok := byID[id]; ok && !list[i].Deleted() {
			list[i].Pinned = true
		}
	}

	// And the reactions, grouped by emoji, each in the order the first person
	// put it there. One query for the whole page, like everything else here: a
	// request per message is what makes a scroll-back slow, and it would be
	// fifty of them.
	rows, err = s.db.Query(`
SELECT r.message_id, r.emoji, u.username
  FROM reactions r
  JOIN users u ON u.id = r.user_id
 WHERE r.message_id IN `+in+`
 ORDER BY (SELECT MIN(f.created_at) FROM reactions f
            WHERE f.message_id = r.message_id AND f.emoji = r.emoji),
          r.emoji, r.created_at, u.username`, ids...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var id int64
		var emoji, who string
		if err := rows.Scan(&id, &emoji, &who); err != nil {
			return nil, err
		}
		i, ok := byID[id]
		// A tombstone carries its reactions away with it. What they were about
		// is gone, and a row of laughing faces under "mensagem removida" is
		// somebody's reaction to a line nobody can read.
		if !ok || list[i].Deleted() {
			continue
		}
		at := len(list[i].Reactions) - 1
		if at < 0 || list[i].Reactions[at].Emoji != emoji {
			list[i].Reactions = append(list[i].Reactions, Reaction{Emoji: emoji})
			at++
		}
		list[i].Reactions[at].Users = append(list[i].Reactions[at].Users, who)
	}
	return list, rows.Err()
}

// MessageFor reads one message, and only from a channel the asker belongs to.
func (s *Store) MessageFor(userID int64, id int64) (Message, error) {
	rows, err := s.db.Query(`
SELECT m.id, m.channel_id, m.author_id, u.username, u.display_name, m.body,
       m.created_at, m.edited_at, m.deleted_at, d.username, m.reply_to, m.kind
  FROM messages m
  JOIN users u ON u.id = m.author_id
  LEFT JOIN users d ON d.id = m.deleted_by
  JOIN channel_members cm ON cm.channel_id = m.channel_id AND cm.user_id = ?
 WHERE m.id = ?`, userID, id)
	if err != nil {
		return Message{}, err
	}
	list, err := scanMessages(rows)
	if err != nil {
		return Message{}, err
	}
	if len(list) == 0 {
		return Message{}, ErrNotFound
	}
	list, err = s.fill(list)
	if err != nil {
		return Message{}, err
	}
	return list[0], nil
}

// ---------------------------------------------------------------- reactions

// ValidReaction says whether this is something somebody may put on a message.
//
// The check is on the *shape*, not against a list of allowed emoji, and the
// difference matters: a list is a list this server would have to ship, and the
// keyboard most of these are typed on belongs to a telephone, not to us. What
// the shape refuses is the thing worth refusing — letters, digits, spaces,
// punctuation — because a free-text column hanging off a message is a second
// message body, and this one would have no length limit worth the name, no
// rate limit and no way to remove it.
//
// What is allowed is a picture - one base with what belongs to it (a skin
// tone, the variation selector that says "draw this one in colour", the
// zero-width joiner that glues a family together, the tag run of a regional
// flag) - and nothing that is only the belongings. The bases are the symbols
// (an emoji is one), plus the handful of emoji Unicode files under other
// headings: math arrows and squares (↔️, ◼️), punctuation that is not ASCII
// (‼️, 〰️) and ℹ️. And the keycap, 1️⃣, which is a digit with decorations and
// gets a rule of its own so that a bare digit still does not pass.
//
// Until 24/09/2026 only the symbols counted, and 24 of the panel's own emoji
// were refused as reactions, the keycaps among them ("não estou conseguindo
// reagir com alguns emojis"). reaction_test.go now reads the panel's list and
// holds the two in step.
func ValidReaction(emoji string) bool {
	emoji = strings.TrimSpace(emoji)
	if emoji == "" || utf8.RuneCountInString(emoji) > MaxReactionRunes {
		return false
	}
	if keycap(emoji) {
		return true
	}
	base := false
	for _, r := range emoji {
		switch {
		case r < 0x80: // letters, digits, spaces, ASCII punctuation
			return false
		case unicode.In(r, unicode.So, unicode.Sm, unicode.Po, unicode.Pd) || r == 'ℹ':
			base = true
		case unicode.Is(unicode.Sk, r): // skin tones
		case unicode.Is(unicode.Mn, r): // the variation selectors
		case r == '\u200d': // the joiner, for the ones made of several
		case r >= 0xe0020 && r <= 0xe007f: // the tags of 🏴 flags
		default:
			return false
		}
	}
	return base
}

// keycap is #️⃣, *️⃣ or 0️⃣ to 9️⃣: the key, an optional variation selector,
// and the enclosing keycap mark.
func keycap(s string) bool {
	r := []rune(s)
	if len(r) < 2 || len(r) > 3 || r[len(r)-1] != '\u20e3' {
		return false
	}
	if !(r[0] == '#' || r[0] == '*' || (r[0] >= '0' && r[0] <= '9')) {
		return false
	}
	return len(r) == 2 || r[1] == '\ufe0f'
}

// React turns one person's reaction on or off and hands back the message as it
// now stands, for the caller to send to the room.
//
// It is one call and not two, because from where somebody is sitting it is one
// gesture: the button under a message is a switch, and pressing it again takes
// it back. `on` says which way, and the page says it rather than working it out
// from what it is holding, so that two devices of the same person pressing at
// the same moment end up agreeing instead of alternating.
//
// Authorisation is the read at the top, and it is the same read the page itself
// would do: MessageFor joins channel_members, so a message in a conversation
// this person is not part of comes back not-found — the same answer an invented
// id gets.
func (s *Store) React(userID, messageID int64, emoji string, on bool) (Message, error) {
	emoji = strings.TrimSpace(emoji)
	if !ValidReaction(emoji) {
		return Message{}, ErrBadReaction
	}
	msg, err := s.MessageFor(userID, messageID)
	if err != nil {
		return Message{}, err
	}
	// A tombstone takes no reactions, for the same reason it hands none out:
	// there is nothing there to have an opinion about.
	if msg.Deleted() {
		return Message{}, ErrBadReaction
	}
	if on {
		var others int
		// Counting the *other* ones, so that pressing something that is already
		// there keeps working when somebody is at the limit: it changes
		// nothing, and refusing it would be refusing a button that is lit.
		if err := s.db.QueryRow(
			`SELECT COUNT(*) FROM reactions WHERE message_id = ? AND user_id = ? AND emoji <> ?`,
			messageID, userID, emoji).Scan(&others); err != nil {
			return Message{}, err
		}
		if others >= MaxReactionsPerPerson {
			return Message{}, ErrBadReaction
		}
		// OR IGNORE: pressing it twice from two devices is not an error, it is
		// the same fact arriving twice.
		if _, err := s.db.Exec(
			`INSERT OR IGNORE INTO reactions (message_id, user_id, emoji, created_at) VALUES (?, ?, ?, ?)`,
			messageID, userID, emoji, s.now().Unix()); err != nil {
			return Message{}, err
		}
	} else if _, err := s.db.Exec(
		`DELETE FROM reactions WHERE message_id = ? AND user_id = ? AND emoji = ?`,
		messageID, userID, emoji); err != nil {
		return Message{}, err
	}
	return s.MessageFor(userID, messageID)
}

// PostMessage writes one line. The whole thing is one transaction: a message
// whose mention rows failed halfway would show a badge that never clears.
//
// replyTo is 0 for an ordinary message. When it is not, it has to name a
// message in this same channel: answering something from another conversation
// would drag a line of it into a room where it does not belong, and the
// quotation is drawn from the original every time it is read.
func (s *Store) PostMessage(userID int64, channelID, body string, attachmentIDs []string, replyTo int64) (Message, error) {
	return s.PostMessageWithin(userID, channelID, body, attachmentIDs, replyTo, nil)
}

// MessageQuota is how many messages a person may post over Window.
type MessageQuota struct {
	Window time.Duration
	Max    int
}

// PostMessageWithin is PostMessage with the ceiling counted inside the
// transaction that writes the message — the same mistake CreateAttachmentWithin
// fixed for uploads. Counting first and posting afterwards let a burst of
// requests read the same count before any of them wrote, so a loop sending
// in parallel was never limited at all. A nil quota means no ceiling.
func (s *Store) PostMessageWithin(userID int64, channelID, body string, attachmentIDs []string, replyTo int64, q *MessageQuota) (Message, error) {
	if _, err := s.ChannelFor(userID, channelID); err != nil {
		return Message{}, err
	}
	body = strings.TrimSpace(body)
	if len([]rune(body)) > MaxMessageRunes {
		return Message{}, ErrBadMessage
	}
	if body == "" && len(attachmentIDs) == 0 {
		return Message{}, ErrBadMessage
	}
	if len(attachmentIDs) > MaxAttachmentsPerMessage {
		return Message{}, ErrBadAttachments
	}
	now := s.now().UTC()

	tx, err := s.db.Begin()
	if err != nil {
		return Message{}, err
	}
	defer tx.Rollback()

	// Check inside the write transaction so a new DM can never bypass the block.
	var denied bool
	if err := tx.QueryRow(`SELECT EXISTS(SELECT 1 FROM channels c JOIN channel_members cm ON cm.channel_id=c.id JOIN person_preferences p ON ((p.owner_id=? AND p.about_id=cm.user_id) OR (p.about_id=? AND p.owner_id=cm.user_id)) WHERE c.id=? AND c.kind='dm' AND cm.user_id<>? AND p.blocked=1)`, userID, userID, channelID, userID).Scan(&denied); err != nil {
		return Message{}, err
	}
	if denied {
		return Message{}, ErrDelivery
	}

	if q != nil {
		var n int
		if err := tx.QueryRow(
			`SELECT COUNT(*) FROM usage WHERE user_id = ? AND kind = ? AND at >= ?`,
			userID, UsageMessage, now.Add(-q.Window).Unix()).Scan(&n); err != nil {
			return Message{}, err
		}
		if n >= q.Max {
			return Message{}, ErrMessageLimit
		}
	}

	var answered any
	if replyTo > 0 {
		var found int64
		err := tx.QueryRow(`SELECT id FROM messages WHERE id = ? AND channel_id = ?`,
			replyTo, channelID).Scan(&found)
		if errors.Is(err, sql.ErrNoRows) {
			// Deliberately the same answer as "no such message": whether id
			// 4021 exists somewhere else on this server is not something an
			// outsider to that conversation gets to find out.
			return Message{}, ErrNotFound
		}
		if err != nil {
			return Message{}, err
		}
		answered = found
	}

	res, err := tx.Exec(
		`INSERT INTO messages (channel_id, author_id, body, created_at, reply_to) VALUES (?, ?, ?, ?, ?)`,
		channelID, userID, body, now.Unix(), answered)
	if err != nil {
		return Message{}, err
	}
	id, err := res.LastInsertId()
	if err != nil {
		return Message{}, err
	}

	for _, aid := range attachmentIDs {
		// Every condition matters: the file has to be this person's, from this
		// channel, and not already on another message. Without the last one a
		// member could re-post an image id they once saw into another channel.
		r, err := tx.Exec(
			`UPDATE attachments SET message_id = ?
			  WHERE id = ? AND owner_id = ? AND channel_id = ? AND message_id IS NULL`,
			id, aid, userID, channelID)
		if err != nil {
			return Message{}, err
		}
		if n, _ := r.RowsAffected(); n == 0 {
			return Message{}, ErrBadAttachments
		}
	}

	if err := insertMentions(tx, id, channelID, body); err != nil {
		return Message{}, err
	}
	if _, err := tx.Exec(`INSERT INTO usage (user_id, kind, at) VALUES (?, ?, ?)`,
		userID, UsageMessage, now.Unix()); err != nil {
		return Message{}, err
	}
	if err := tx.Commit(); err != nil {
		return Message{}, err
	}
	return s.MessageFor(userID, id)
}

// PostNote writes a line that the **server** is responsible for, not a person.
//
// There is exactly one today: a call that nobody answered (kind 'call', see
// migration 9). It is deliberately not the same door as PostMessage — it does
// not count against anybody's messages-per-minute, because nobody typed it; it
// carries no attachments and answers nothing; and the author is only there so
// that the conversation knows which way the call was going.
//
// The body is a word this server chose, not a sentence: 'missed' or 'rejected'.
// The wording the reader sees is the client's business, and it differs at the
// two ends of the same call — "you did not answer" and "nobody answered" are
// the same row.
func (s *Store) PostNote(authorID int64, channelID, kind, body string) (Message, error) {
	if kind == "" || body == "" || len(body) > 64 {
		return Message{}, ErrBadMessage
	}
	if _, err := s.ChannelFor(authorID, channelID); err != nil {
		return Message{}, err
	}
	res, err := s.db.Exec(
		`INSERT INTO messages (channel_id, author_id, body, kind, created_at) VALUES (?, ?, ?, ?, ?)`,
		channelID, authorID, body, kind, s.now().UTC().Unix())
	if err != nil {
		return Message{}, err
	}
	id, err := res.LastInsertId()
	if err != nil {
		return Message{}, err
	}
	return s.MessageFor(authorID, id)
}

// MaxSearchResults is one screenful and a bit. Somebody who does not find what
// they wanted in fifty lines wants a different word, not a longer list.
const MaxSearchResults = 50

// SearchMessages is "where did I see that?" — every message this person can
// read whose text contains what they typed, newest first.
//
// **The membership join is the whole of the authorisation**, exactly as in
// every other read here: the search runs over messages of channels this person
// has a row in, so a word that appears in somebody else's conversation is not
// findable, not countable, and not distinguishable from a word nobody ever
// wrote. There is no second index to keep in step with who may read what,
// because there is no second index at all — for twenty people and a few tens of
// thousands of lines, SQLite reading the rows is quicker than a round trip to
// the person's own screen.
//
// Deleted messages and the server's own notices are left out: a tombstone has
// no text to find, and "missed" is not a word anybody is looking for.
func (s *Store) SearchMessages(userID int64, query string, limit int) ([]Message, error) {
	query = strings.TrimSpace(query)
	if len([]rune(query)) < 2 {
		return []Message{}, nil
	}
	if limit <= 0 || limit > MaxSearchResults {
		limit = MaxSearchResults
	}
	// The wildcards are LIKE's, not the searcher's: somebody looking for "100%"
	// is looking for a per cent sign.
	esc := strings.NewReplacer(`\`, `\\`, `%`, `\%`, `_`, `\_`).Replace(query)
	rows, err := s.db.Query(`
SELECT m.id, m.channel_id, m.author_id, u.username, u.display_name, m.body,
       m.created_at, m.edited_at, m.deleted_at, d.username, m.reply_to, m.kind
  FROM messages m
  JOIN users u ON u.id = m.author_id
  LEFT JOIN users d ON d.id = m.deleted_by
  JOIN channel_members cm ON cm.channel_id = m.channel_id AND cm.user_id = ?
 WHERE m.deleted_at IS NULL AND m.kind = ''
   AND m.body LIKE ? ESCAPE '\' COLLATE NOCASE
 ORDER BY m.id DESC LIMIT ?`, userID, "%"+esc+"%", limit)
	if err != nil {
		return nil, err
	}
	list, err := scanMessages(rows)
	if err != nil {
		return nil, err
	}
	return s.fill(list)
}

// UploadedBytes is how much this person has put on the disk inside a window.
//
// It is read from the attachments themselves rather than from a counter kept
// beside them, because the attachments *are* the count: a row per file, with
// its size and the moment it arrived. A separate number would be one more thing
// that can disagree with the disk.
func (s *Store) UploadedBytes(userID int64, window time.Duration) (int64, error) {
	since := s.now().Add(-window).Unix()
	var total sql.NullInt64
	err := s.db.QueryRow(
		`SELECT SUM(size) FROM attachments WHERE owner_id = ? AND created_at > ?`,
		userID, since).Scan(&total)
	if err != nil {
		return 0, err
	}
	return total.Int64, nil
}

// Everyone is the word that names the whole channel at once. It is checked
// before the usernames are, so a person who somehow registered as "todos" does
// not quietly take the word away from everybody else.
const Everyone = "todos"

// insertMentions records who a message names. Only members of that channel
// count: writing "@alguem" in a conversation they are not in must not create a
// notification pointing at a conversation they cannot open.
func insertMentions(tx *sql.Tx, messageID int64, channelID, body string) error {
	seen := map[string]bool{}
	for _, m := range mentionRE.FindAllStringSubmatch(body, -1) {
		name := NormalizeUsername(m[1])
		if seen[name] {
			continue
		}
		seen[name] = true
		if name == Everyone {
			// Everybody in this channel, and nobody outside it — the same rule as
			// a name, applied to all of them. The writer is left out: a person
			// does not need to be told they wrote something.
			if _, err := tx.Exec(`
INSERT OR IGNORE INTO mentions (message_id, user_id, channel_id)
SELECT ?, cm.user_id, ?
  FROM channel_members cm
  JOIN messages msg ON msg.id = ?
 WHERE cm.channel_id = ? AND cm.user_id <> msg.author_id`,
				messageID, channelID, messageID, channelID); err != nil {
				return err
			}
			continue
		}
		if _, err := tx.Exec(`
INSERT OR IGNORE INTO mentions (message_id, user_id, channel_id)
SELECT ?, u.id, ?
  FROM users u
  JOIN channel_members cm ON cm.user_id = u.id AND cm.channel_id = ?
 WHERE u.username = ? COLLATE NOCASE`, messageID, channelID, channelID, name); err != nil {
			return err
		}
	}
	return nil
}

// EditMessage rewrites your own line, and records that it was rewritten. Only
// the author, never the owner: editing somebody else's words is not moderation.
func (s *Store) EditMessage(userID, id int64, body string) (Message, error) {
	body = strings.TrimSpace(body)
	if body == "" || len([]rune(body)) > MaxMessageRunes {
		return Message{}, ErrBadMessage
	}
	now := s.now().UTC()
	tx, err := s.db.Begin()
	if err != nil {
		return Message{}, err
	}
	defer tx.Rollback()

	// Membership is checked here, inside the transaction, and not only by the
	// MessageFor at the end: a channel the owner deleted keeps its history but
	// drops its members, and an author who lost access must not rewrite it.
	var channelID string
	err = tx.QueryRow(`SELECT m.channel_id FROM messages m
		 JOIN channel_members cm ON cm.channel_id = m.channel_id AND cm.user_id = m.author_id
		 WHERE m.id = ? AND m.author_id = ? AND m.deleted_at IS NULL`, id, userID).Scan(&channelID)
	if errors.Is(err, sql.ErrNoRows) {
		return Message{}, ErrNotFound
	}
	if err != nil {
		return Message{}, err
	}
	if _, err := tx.Exec(`UPDATE messages SET body = ?, edited_at = ? WHERE id = ?`,
		body, now.Unix(), id); err != nil {
		return Message{}, err
	}
	if _, err := tx.Exec(`DELETE FROM mentions WHERE message_id = ?`, id); err != nil {
		return Message{}, err
	}
	if err := insertMentions(tx, id, channelID, body); err != nil {
		return Message{}, err
	}
	if err := tx.Commit(); err != nil {
		return Message{}, err
	}
	return s.MessageFor(userID, id)
}

// Moderation says whose messages somebody may remove besides their own.
type Moderation int

const (
	// OwnOnly is everybody: their own lines and nobody else's.
	OwnOnly Moderation = iota
	// OthersInText is Manage messages (D12c): anybody's line in a text
	// channel. Not in a conversation of two — a role is about the server's
	// rooms, not about somebody's private talk.
	OthersInText
	// OthersAnywhere is the owner, as before roles: anybody's line in any
	// channel the owner is in.
	OthersAnywhere
)

// DeleteMessage leaves a tombstone: the row, the id and who removed it stay.
// The author may remove their own; a moderator (see Moderation) may remove
// other people's, but only in a channel they are already in, so this is
// moderation and not a back door into a conversation nobody invited them to.
func (s *Store) DeleteMessage(userID int64, id int64, mod Moderation) (Message, error) {
	now := s.now().UTC()
	q := `UPDATE messages SET deleted_at = ?, deleted_by = ?, body = ''
	       WHERE id = ? AND deleted_at IS NULL AND author_id = ?
	         AND channel_id IN (SELECT channel_id FROM channel_members WHERE user_id = ?)`
	args := []any{now.Unix(), userID, id, userID, userID}
	switch mod {
	case OthersAnywhere:
		q = `UPDATE messages SET deleted_at = ?, deleted_by = ?, body = ''
		      WHERE id = ? AND deleted_at IS NULL
		        AND channel_id IN (SELECT channel_id FROM channel_members WHERE user_id = ?)`
		args = []any{now.Unix(), userID, id, userID}
	case OthersInText:
		q = `UPDATE messages SET deleted_at = ?, deleted_by = ?, body = ''
		      WHERE id = ? AND deleted_at IS NULL
		        AND channel_id IN (SELECT channel_id FROM channel_members WHERE user_id = ?)
		        AND (author_id = ? OR channel_id IN (SELECT id FROM channels WHERE kind = 'text'))`
		args = []any{now.Unix(), userID, id, userID, userID}
	}
	res, err := s.db.Exec(q, args...)
	if err != nil {
		return Message{}, err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return Message{}, ErrNotFound
	}
	if _, err := s.db.Exec(`DELETE FROM mentions WHERE message_id = ?`, id); err != nil {
		return Message{}, err
	}
	// A pin of a line nobody can read any more is a hole in the pin list, and
	// it would count towards the fifty.
	if _, err := s.db.Exec(`DELETE FROM pins WHERE message_id = ?`, id); err != nil {
		return Message{}, err
	}
	return s.MessageFor(userID, id)
}

// ---------------------------------------------------------------- attachments

// CreateAttachment records an upload that is not on a message yet. The caller
// has already checked the bytes; this only writes the row, and the id it
// returns is the name the file gets on disk.
func (s *Store) CreateAttachment(userID int64, channelID, name, mime string, size int64, w, h int) (Attachment, error) {
	return s.CreateAttachmentWithin(userID, channelID, name, mime, size, w, h, nil)
}

// UploadQuota is the ceiling a new attachment has to fit inside, counted over
// Window: how many files, and how many bytes.
type UploadQuota struct {
	Window   time.Duration
	MaxFiles int
	MaxBytes int64
}

// CreateAttachmentWithin is CreateAttachment with the daily ceiling applied in
// the same transaction that writes the row.
//
// Reading the totals first and writing afterwards was two mistakes at once.
// The size of the file arriving was not in the sum, so the last upload of the
// day could be a whole file past the ceiling; and several uploads read the
// same totals before any of them wrote, so the ceiling held only for people
// who waited their turn. Both stop being possible when the count, the sum and
// the insert are one statement sequence inside one transaction.
//
// A nil quota means no ceiling — the maintenance paths and the tests that are
// about something else.
func (s *Store) CreateAttachmentWithin(userID int64, channelID, name, mime string, size int64, w, h int, q *UploadQuota) (Attachment, error) {
	if _, err := s.ChannelFor(userID, channelID); err != nil {
		return Attachment{}, err
	}
	id, err := newID()
	if err != nil {
		return Attachment{}, err
	}
	now := s.now().UTC()
	tx, err := s.db.Begin()
	if err != nil {
		return Attachment{}, err
	}
	defer tx.Rollback()
	if q != nil {
		since := now.Add(-q.Window).Unix()
		var files int
		if err := tx.QueryRow(
			`SELECT COUNT(*) FROM usage WHERE user_id = ? AND kind = ? AND at >= ?`,
			userID, UsageUpload, since).Scan(&files); err != nil {
			return Attachment{}, err
		}
		if files >= q.MaxFiles {
			return Attachment{}, ErrUploadFiles
		}
		var used sql.NullInt64
		if err := tx.QueryRow(
			`SELECT SUM(size) FROM attachments WHERE owner_id = ? AND created_at > ?`,
			userID, since).Scan(&used); err != nil {
			return Attachment{}, err
		}
		if used.Int64+size > q.MaxBytes {
			return Attachment{}, ErrUploadBytes
		}
	}
	if _, err := tx.Exec(
		`INSERT INTO attachments (id, message_id, channel_id, owner_id, name, mime, size, width, height, created_at)
		 VALUES (?, NULL, ?, ?, ?, ?, ?, ?, ?, ?)`,
		id, channelID, userID, name, mime, size, w, h, now.Unix()); err != nil {
		return Attachment{}, err
	}
	if _, err := tx.Exec(`INSERT INTO usage (user_id, kind, at) VALUES (?, ?, ?)`,
		userID, UsageUpload, now.Unix()); err != nil {
		return Attachment{}, err
	}
	if err := tx.Commit(); err != nil {
		return Attachment{}, err
	}
	return Attachment{ID: id, ChannelID: channelID, OwnerID: userID, Name: name,
		MIME: mime, Size: size, Width: w, Height: h, CreatedAt: now}, nil
}

// ReturnUpload undoes CreateAttachmentWithin for a file that never reached the
// disk: the row goes, and so does the one use it was counted as. Without it a
// full disk or a permissions mistake charged every failed attempt against the
// day's allowance, and once the disk was fixed the person was told they had
// uploaded too much — of files nobody ever received.
//
// Only a row that is still this person's and still on no message: this is the
// failure path of the request that just created it, and nothing else.
func (s *Store) ReturnUpload(userID int64, a Attachment) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	res, err := tx.Exec(`DELETE FROM attachments WHERE id = ? AND owner_id = ? AND message_id IS NULL`,
		a.ID, userID)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	if _, err := tx.Exec(
		`DELETE FROM usage WHERE rowid = (
		   SELECT rowid FROM usage WHERE user_id = ? AND kind = ? AND at = ? LIMIT 1)`,
		userID, UsageUpload, a.CreatedAt.Unix()); err != nil {
		return err
	}
	return tx.Commit()
}

// AttachmentFor is the gate on the images, and it is the same gate as the
// messages: membership of the channel the image was posted in. Guessing an id
// from somebody else's conversation answers what a made-up id answers. An
// upload not yet posted is still its owner's draft, so until it has a message
// only its owner may read it, member or not.
func (s *Store) AttachmentFor(userID int64, id string) (Attachment, error) {
	var a Attachment
	var created int64
	var msg sql.NullInt64
	err := s.db.QueryRow(`
SELECT a.id, a.message_id, a.channel_id, a.owner_id, a.name, a.mime, a.size, a.width, a.height, a.created_at
  FROM attachments a
  JOIN channel_members cm ON cm.channel_id = a.channel_id AND cm.user_id = ?
  LEFT JOIN messages m ON m.id = a.message_id
 WHERE a.id = ? AND ((a.message_id IS NULL AND a.owner_id = ?) OR (a.message_id IS NOT NULL AND m.deleted_at IS NULL))`, userID, id, userID).
		Scan(&a.ID, &msg, &a.ChannelID, &a.OwnerID, &a.Name, &a.MIME, &a.Size, &a.Width, &a.Height, &created)
	if errors.Is(err, sql.ErrNoRows) {
		return Attachment{}, ErrNotFound
	}
	if err != nil {
		return Attachment{}, err
	}
	a.MessageID, a.CreatedAt = msg.Int64, time.Unix(created, 0).UTC()
	return a, nil
}

// AttachmentLive reports whether an id is still a row that anybody may read.
// The file sweep uses it the other way round from everything else here — it
// starts from the files on disk — because a file nobody may read any more is
// exactly what has to go.
//
// A deleted message's attachment counts as gone even though the row stays: the
// row is what the daily upload quota is measured from, and deleting a message
// must not hand back the allowance it spent.
func (s *Store) AttachmentLive(id string) (bool, error) {
	var one int
	err := s.db.QueryRow(`
SELECT 1 FROM attachments a
  LEFT JOIN messages m ON m.id = a.message_id
 WHERE a.id = ? AND (a.message_id IS NULL OR m.deleted_at IS NULL)`, id).Scan(&one)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	return err == nil, err
}

// ---------------------------------------------------------------- limits

// RecordedUsage counts how many times this person did something inside window.
// The rows are written by PostMessage and CreateAttachment, in the same
// transaction as the thing they count, so the count cannot drift from reality
// and a restart does not reset somebody's quota.
func (s *Store) RecordedUsage(userID int64, kind string, window time.Duration) (int, error) {
	cutoff := s.now().Add(-window).Unix()
	var n int
	err := s.db.QueryRow(`SELECT COUNT(*) FROM usage WHERE user_id = ? AND kind = ? AND at >= ?`,
		userID, kind, cutoff).Scan(&n)
	return n, err
}
