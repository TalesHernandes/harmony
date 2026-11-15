package store

import (
	"path/filepath"
	"testing"
	"time"
)

func TestPreviewCacheExpiryBatchAndPersistence(t *testing.T) {
	path := filepath.Join(t.TempDir(), "preview.db")
	s, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now().Truncate(time.Second)
	for _, p := range []Preview{{Embed: Embed{URL: "https://news.test", Kind: "link", Title: "News"}, Success: true, FetchedAt: now}, {Embed: Embed{URL: "https://failed.test", Kind: "link"}, FetchedAt: now}} {
		if err = s.SavePreview(p); err != nil {
			t.Fatal(err)
		}
	}
	s.Close()
	s, err = Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	cache, err := s.Previews([]string{"https://news.test", "https://failed.test", "https://missing.test"})
	if err != nil || len(cache) != 2 {
		t.Fatalf("batch %v %v", cache, err)
	}
	success, failure := cache["https://news.test"], cache["https://failed.test"]
	if !success.Fresh(now.Add(7*24*time.Hour-time.Second)) || success.Fresh(now.Add(7*24*time.Hour)) || !failure.Fresh(now.Add(time.Hour-time.Second)) || failure.Fresh(now.Add(time.Hour)) {
		t.Fatal("TTL")
	}
	success.Embed.Title = "Updated"
	if err = s.SavePreview(success); err != nil {
		t.Fatal(err)
	}
	cache, err = s.Previews([]string{success.Embed.URL})
	if err != nil || cache[success.Embed.URL].Embed.Title != "Updated" {
		t.Fatal("upsert")
	}
	if empty, err := s.Previews(nil); err != nil || len(empty) != 0 {
		t.Fatal("empty page")
	}
}

func TestPreviewImageMembershipAndStaleBindings(t *testing.T) {
	s := memStore(t)
	a := signUp(t, s, "alice")
	b := signUp(t, s, "bruno")
	outsider := signUp(t, s, "carol")
	ch, err := s.EnsureDM(a.ID, b.ID)
	if err != nil {
		t.Fatal(err)
	}
	m, err := s.PostMessage(a.ID, ch.ID, "https://news.test", nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	p := Preview{Embed: Embed{URL: m.Body, Kind: "link", Image: "/api/previews/image-id"}, Success: true, FetchedAt: time.Now()}
	if err = s.SavePreview(p); err != nil {
		t.Fatal(err)
	}
	if err = s.BindPreviewLinks(outsider.ID, m, []string{m.Body}); err == nil {
		t.Fatal("outsider bound links")
	}
	if err = s.BindPreviewLinks(a.ID, m, []string{m.Body}); err != nil {
		t.Fatal(err)
	}
	if s.PreviewImageFor(b.ID, "image-id") != nil || s.PreviewImageFor(outsider.ID, "image-id") == nil {
		t.Fatal("image membership")
	}
	if _, err = s.EditMessage(a.ID, m.ID, "removed"); err != nil {
		t.Fatal(err)
	}
	if s.PreviewImageFor(b.ID, "image-id") == nil || s.BindPreviewLinks(a.ID, m, []string{m.Body}) == nil {
		t.Fatal("stale edit authorized")
	}
	if _, err = s.DeleteMessage(a.ID, m.ID, OwnOnly); err != nil {
		t.Fatal(err)
	}
	if s.PreviewImageFor(a.ID, "image-id") == nil {
		t.Fatal("deleted message authorized image")
	}
}
