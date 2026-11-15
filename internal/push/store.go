package push

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"sync"
	"time"
)

// MaxSubscriptions bounds the store. This is a two-person app; the only
// reason there is more than one entry is a phone that was reinstalled, or a
// second device someone tried once.
const MaxSubscriptions = 8

// Subscription is what pushManager.subscribe() hands the page, as posted to
// /push/subscribe.
//
// P256dh and Auth are stored but unused: nothing here encrypts a payload
// (see the package comment). They are kept so that adding RFC 8291 later is a
// server change alone, instead of something that needs every device to
// subscribe again.
type Subscription struct {
	Endpoint string `json:"endpoint"`
	P256dh   string `json:"p256dh,omitempty"`
	Auth     string `json:"auth,omitempty"`
	Added    int64  `json:"added"`
}

// Store holds the push subscriptions: a map in memory, mirrored to one JSON
// file so a deploy does not silently switch the doorbell off.
//
// On the file. CLAUDE.md keeps signaling state in memory and forbids a
// database, and this is neither: it is a device registration, closer to
// configuration than to call state, and the file holds at most a handful of
// objects. The alternative — memory only — loses the subscription on every
// `docker compose up`, and the failure is invisible: the doorbell simply
// stops working until the phone happens to open the app in the foreground
// again. That is precisely the case this sprint exists to make work.
//
// Path may be empty, which makes the store memory-only. `make dev` and the
// e2e suite run that way.
type Store struct {
	mu   sync.Mutex
	path string
	subs map[string]Subscription
	now  func() time.Time
}

// NewStore loads the file at path if it exists. A missing file is a normal
// first run; a corrupt one is reported, because starting with a silently
// empty store is the failure mode this whole file exists to prevent.
func NewStore(path string) (*Store, error) {
	s := &Store{path: path, subs: map[string]Subscription{}, now: time.Now}
	if path == "" {
		return s, nil
	}
	raw, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return s, nil
	}
	if err != nil {
		return nil, fmt.Errorf("push store %s: %w", path, err)
	}
	var list []Subscription
	if err := json.Unmarshal(raw, &list); err != nil {
		return nil, fmt.Errorf("push store %s: %w", path, err)
	}
	for _, sub := range list {
		if sub.Endpoint != "" {
			s.subs[sub.Endpoint] = sub
		}
	}
	return s, nil
}

// Add stores sub, replacing any entry with the same endpoint.
func (s *Store) Add(sub Subscription) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, exists := s.subs[sub.Endpoint]; !exists && len(s.subs) >= MaxSubscriptions {
		// Drop the oldest rather than refuse: a phone that reinstalled is
		// the realistic cause of a full store, and the newest registration
		// is the one that can actually ring.
		s.evictOldestLocked()
	}
	sub.Added = s.now().Unix()
	s.subs[sub.Endpoint] = sub
	return s.saveLocked()
}

// Remove deletes an endpoint. Removing one that is not there is not an error:
// the page unsubscribing twice, or after a store reset, is normal.
func (s *Store) Remove(endpoint string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.subs[endpoint]; !ok {
		return nil
	}
	delete(s.subs, endpoint)
	return s.saveLocked()
}

// Has reports whether endpoint is registered.
func (s *Store) Has(endpoint string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, ok := s.subs[endpoint]
	return ok
}

// Len is the number of registered devices. This is the synchronous fact that
// decides whether a ring with nobody in the room can be answered by a push.
func (s *Store) Len() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.subs)
}

// List returns a copy, oldest first.
func (s *Store) List() []Subscription {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.listLocked()
}

func (s *Store) listLocked() []Subscription {
	out := make([]Subscription, 0, len(s.subs))
	for _, sub := range s.subs {
		out = append(out, sub)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Added != out[j].Added {
			return out[i].Added < out[j].Added
		}
		return out[i].Endpoint < out[j].Endpoint
	})
	return out
}

func (s *Store) evictOldestLocked() {
	list := s.listLocked()
	if len(list) > 0 {
		delete(s.subs, list[0].Endpoint)
	}
}

// saveLocked writes the whole file. Caller holds s.mu.
//
// Write-then-rename, not write-in-place: a truncated file after a crash or a
// full disk would read back as a parse error at the next boot, which — see
// NewStore — refuses to start rather than pretending the doorbell is fine.
func (s *Store) saveLocked() error {
	if s.path == "" {
		return nil
	}
	raw, err := json.Marshal(s.listLocked())
	if err != nil {
		return err
	}
	dir := filepath.Dir(s.path)
	tmp, err := os.CreateTemp(dir, ".push-*.json")
	if err != nil {
		return fmt.Errorf("push store: %w", err)
	}
	name := tmp.Name()
	defer os.Remove(name) // no-op once the rename succeeded
	if _, err := tmp.Write(raw); err != nil {
		tmp.Close()
		return fmt.Errorf("push store: %w", err)
	}
	if err := tmp.Close(); err != nil {
		return fmt.Errorf("push store: %w", err)
	}
	if err := os.Rename(name, s.path); err != nil {
		return fmt.Errorf("push store: %w", err)
	}
	return nil
}
