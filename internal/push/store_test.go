package push

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestStoreSurvivesARestart(t *testing.T) {
	path := filepath.Join(t.TempDir(), "push.json")

	first, err := NewStore(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := first.Add(Subscription{Endpoint: "https://push.example.org/a", P256dh: "pk", Auth: "au"}); err != nil {
		t.Fatal(err)
	}

	// The point of the file: this is a new process, exactly as it is after
	// `docker compose up -d --build`.
	second, err := NewStore(path)
	if err != nil {
		t.Fatal(err)
	}
	if second.Len() != 1 {
		t.Fatalf("a deploy lost the subscription: Len = %d", second.Len())
	}
	if !second.Has("https://push.example.org/a") {
		t.Error("endpoint not restored")
	}
	got := second.List()[0]
	if got.P256dh != "pk" || got.Auth != "au" {
		t.Errorf("keys not restored: %+v", got)
	}
	if got.Added == 0 {
		t.Error("Added timestamp not restored")
	}
}

func TestStoreRemove(t *testing.T) {
	path := filepath.Join(t.TempDir(), "push.json")
	s, err := NewStore(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Add(Subscription{Endpoint: "https://push.example.org/a"}); err != nil {
		t.Fatal(err)
	}
	if err := s.Remove("https://push.example.org/a"); err != nil {
		t.Fatal(err)
	}
	if s.Len() != 0 {
		t.Fatalf("Len = %d after remove", s.Len())
	}
	// Removing something that is not there is normal, not an error: the page
	// unsubscribing twice, or after the store was reset, hits this.
	if err := s.Remove("https://push.example.org/gone"); err != nil {
		t.Fatalf("removing an absent endpoint: %v", err)
	}
	reloaded, err := NewStore(path)
	if err != nil {
		t.Fatal(err)
	}
	if reloaded.Len() != 0 {
		t.Error("removal did not reach the file")
	}
}

func TestStoreReplacesSameEndpoint(t *testing.T) {
	s, err := NewStore("")
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 3; i++ {
		if err := s.Add(Subscription{Endpoint: "https://push.example.org/a"}); err != nil {
			t.Fatal(err)
		}
	}
	if s.Len() != 1 {
		t.Fatalf("re-subscribing the same endpoint should not add rows: Len = %d", s.Len())
	}
}

// The page re-posts its subscription on every login (the self-healing path),
// so a store that grew a row per login would fill up on its own.
func TestStoreEvictsOldestWhenFull(t *testing.T) {
	s, err := NewStore("")
	if err != nil {
		t.Fatal(err)
	}
	base := time.Unix(1_000_000, 0)
	tick := 0
	s.now = func() time.Time { tick++; return base.Add(time.Duration(tick) * time.Second) }

	for i := 0; i < MaxSubscriptions+2; i++ {
		if err := s.Add(Subscription{Endpoint: "https://push.example.org/" + string(rune('a'+i))}); err != nil {
			t.Fatal(err)
		}
	}
	if s.Len() != MaxSubscriptions {
		t.Fatalf("Len = %d, want the cap %d", s.Len(), MaxSubscriptions)
	}
	if s.Has("https://push.example.org/a") {
		t.Error("the oldest entry should have been evicted")
	}
	if !s.Has("https://push.example.org/" + string(rune('a'+MaxSubscriptions+1))) {
		t.Error("the newest entry — the one that can actually ring — was dropped")
	}
}

func TestStoreWithNoPathIsMemoryOnly(t *testing.T) {
	s, err := NewStore("")
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Add(Subscription{Endpoint: "https://push.example.org/a"}); err != nil {
		t.Fatalf("memory-only store should accept writes: %v", err)
	}
	if s.Len() != 1 {
		t.Fatal("not stored")
	}
}

// Starting with a silently empty store is the exact failure this file exists
// to prevent, so a corrupt one has to be loud.
func TestCorruptStoreIsAnError(t *testing.T) {
	path := filepath.Join(t.TempDir(), "push.json")
	if err := os.WriteFile(path, []byte("{not json"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := NewStore(path); err == nil {
		t.Fatal("want an error for a corrupt store file")
	}
}

func TestMissingStoreFileIsAFreshStart(t *testing.T) {
	s, err := NewStore(filepath.Join(t.TempDir(), "never-written.json"))
	if err != nil {
		t.Fatalf("a missing file is a normal first run: %v", err)
	}
	if s.Len() != 0 {
		t.Fatal("want an empty store")
	}
}
