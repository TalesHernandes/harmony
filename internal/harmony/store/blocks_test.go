package store

import (
	"errors"
	"path/filepath"
	"testing"
)

func TestPersonalPreferencesPersistWithoutSharingOwnership(t *testing.T) {
	path := filepath.Join(t.TempDir(), "preferences.db")
	s, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	a := signUp(t, s, "alice")
	b := signUp(t, s, "bruno")
	if err := s.SetPersonPreference(a.ID, b.Username, false, true); err != nil {
		t.Fatal(err)
	}
	if err := s.SetPersonPreference(a.ID, b.Username, true, true); err != nil {
		t.Fatal(err)
	}
	if err := s.SetPersonPreference(a.ID, a.Username, true, true); !errors.Is(err, ErrNotFound) {
		t.Fatal(err)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	s, err = Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	list, err := s.PersonPreferences(a.ID)
	if err != nil || len(list) != 1 || !list[0].Muted || !list[0].Blocked {
		t.Fatal(list, err)
	}
	other, err := s.PersonPreferences(b.ID)
	if err != nil || len(other) != 0 {
		t.Fatal(other, err)
	}
	if err := s.SetPersonPreference(a.ID, b.Username, true, false); err != nil {
		t.Fatal(err)
	}
	list, err = s.PersonPreferences(a.ID)
	if err != nil || !list[0].Muted || list[0].Blocked {
		t.Fatal(list, err)
	}
}
