package store

import (
	"bytes"
	"errors"
	"fmt"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestAvatarMigrationAndBackupPreserveAccountsAndPhotos(t *testing.T) {
	st, _ := fileStore(t)
	u := signUp(t, st, "alice")
	// Simulate a shipped database just before the avatar migration.
	if _, err := st.db.Exec(`DROP TABLE person_preferences; DROP TABLE user_roles; DROP TABLE roles; DROP TABLE pins; ALTER TABLE channels DROP COLUMN category_id; DROP TABLE categories;
DROP TABLE message_preview_links; DROP TABLE previews; DROP TABLE security_tokens; DROP TABLE recovery_codes;
ALTER TABLE auth_failures DROP COLUMN reservation;
DROP INDEX users_email_unique;
ALTER TABLE users DROP COLUMN security_version;
ALTER TABLE users DROP COLUMN email;
ALTER TABLE users DROP COLUMN email_verified_at;
ALTER TABLE users DROP COLUMN twofa;
ALTER TABLE users DROP COLUMN totp_secret;
ALTER TABLE users DROP COLUMN totp_last_step;
ALTER TABLE users DROP COLUMN totp_pending;
ALTER TABLE users DROP COLUMN totp_pending_until;
ALTER TABLE users DROP COLUMN avatar_blob; ALTER TABLE users DROP COLUMN avatar_version; PRAGMA user_version = 11`); err != nil {
		t.Fatal(err)
	}
	if err := st.migrate(); err != nil {
		t.Fatal(err)
	}
	if got, err := st.UserByID(u.ID); err != nil || got.Username != u.Username || got.Avatar != "" {
		t.Fatalf("account after migration: %+v %v", got, err)
	}
	data := []byte("processed JPEG stand-in")
	if err := st.SetAvatar(u.ID, data, "version"); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "backup.db")
	if err := st.BackupTo(path); err != nil {
		t.Fatal(err)
	}
	restored, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer restored.Close()
	if got, err := restored.Avatar(u.Username); err != nil || !bytes.Equal(got, data) {
		t.Fatalf("photo in backup: %q %v", got, err)
	}
	if v, err := restored.AvatarVersion(u.Username); err != nil || v != "version" {
		t.Fatalf("version in backup: %q %v", v, err)
	}
	// The quota travels in the same backup as the photo.
	for i := 1; i < 10; i++ {
		if err := restored.SetAvatar(u.ID, data, fmt.Sprint(i)); err != nil {
			t.Fatal(err)
		}
	}
	if err := restored.SetAvatar(u.ID, nil, ""); !errors.Is(err, ErrRateLimited) {
		t.Fatal(err)
	}
}

func TestAvatarQuotaIsAtomicAndPerUser(t *testing.T) {
	st := memStore(t)
	a, b := signUp(t, st, "alice"), signUp(t, st, "bruno")
	var accepted atomic.Int32
	var wg sync.WaitGroup
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			err := st.SetAvatar(a.ID, []byte("JPEG"), fmt.Sprint(i))
			if err == nil {
				accepted.Add(1)
			} else if !errors.Is(err, ErrRateLimited) {
				t.Error(err)
			}
		}(i)
	}
	wg.Wait()
	if accepted.Load() != 10 {
		t.Fatalf("accepted %d changes, want 10", accepted.Load())
	}
	if err := st.SetAvatar(b.ID, []byte("JPEG"), "bruno"); err != nil {
		t.Fatal(err)
	}
	st.SetClock(func() time.Time { return time.Now().Add(time.Hour + time.Second) })
	if err := st.SetAvatar(a.ID, nil, ""); err != nil {
		t.Fatal(err)
	}
}
