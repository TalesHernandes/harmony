package store

import (
	"errors"
	"sync"
	"testing"
	"time"
)

func TestDeleteRoomRevokesAccessAndPreservesHistory(t *testing.T) {
	for _, kind := range []string{ChannelText, ChannelVoice} {
		t.Run(kind, func(t *testing.T) {
			st := memStore(t)
			owner := signUp(t, st, "owner")
			friend := signUp(t, st, "friend")
			ch, err := st.createRoom(kind, owner.ID, "test", "")
			if err != nil {
				t.Fatal(err)
			}
			a, err := st.CreateAttachment(owner.ID, ch.ID, "test.png", "image/png", 100, 1, 1)
			if err != nil {
				t.Fatal(err)
			}
			msg, err := st.PostMessage(owner.ID, ch.ID, "retained @friend", []string{a.ID}, 0)
			if err != nil {
				t.Fatal(err)
			}
			now := time.Unix(1800000000, 0)
			st.SetClock(func() time.Time { return now })
			if err := st.DeleteRoom(owner.ID, ch.ID); err != nil {
				t.Fatal(err)
			}
			late := signUp(t, st, "newcomer")
			for _, u := range []User{owner, friend, late} {
				channels, err := st.ListChannels(u.ID)
				if err != nil {
					t.Fatal(err)
				}
				for _, listed := range channels {
					if listed.ID == ch.ID {
						t.Fatalf("deleted room listed for %s", u.Username)
					}
				}
				if _, err := st.ChannelFor(u.ID, ch.ID); !errors.Is(err, ErrNotFound) {
					t.Fatalf("ChannelFor: %v", err)
				}
				if _, err := st.Messages(u.ID, ch.ID, 0, 50); !errors.Is(err, ErrNotFound) {
					t.Fatalf("history: %v", err)
				}
				if _, err := st.PostMessage(u.ID, ch.ID, "denied", nil, 0); !errors.Is(err, ErrNotFound) {
					t.Fatalf("send: %v", err)
				}
				if _, err := st.MessageFor(u.ID, msg.ID); !errors.Is(err, ErrNotFound) {
					t.Fatalf("message: %v", err)
				}
				if _, err := st.AttachmentFor(u.ID, a.ID); !errors.Is(err, ErrNotFound) {
					t.Fatalf("attachment: %v", err)
				}
				found, err := st.SearchMessages(u.ID, "retained", 50)
				if err != nil || len(found) != 0 {
					t.Fatalf("search = %v, %v", found, err)
				}
				voices, err := st.VoiceChannelIDs(u.ID)
				if err != nil {
					t.Fatal(err)
				}
				for _, id := range voices {
					if id == ch.ID {
						t.Fatal("deleted voice room listed")
					}
				}
			}
			ids, err := st.ChannelMemberIDs(ch.ID)
			if err != nil || len(ids) != 0 {
				t.Fatalf("remaining members = %v, %v", ids, err)
			}
			var deletedAt, deletedBy int64
			if err := st.db.QueryRow(`SELECT deleted_at, deleted_by FROM channels WHERE id = ?`, ch.ID).Scan(&deletedAt, &deletedBy); err != nil {
				t.Fatal(err)
			}
			if deletedAt != now.Unix() || deletedBy != owner.ID {
				t.Fatalf("deletion audit = %d, %d", deletedAt, deletedBy)
			}
			var body string
			if err := st.db.QueryRow(`SELECT body FROM messages WHERE id = ?`, msg.ID).Scan(&body); err != nil || body != "retained @friend" {
				t.Fatalf("retained message = %q, %v", body, err)
			}
			var messageID int64
			if err := st.db.QueryRow(`SELECT message_id FROM attachments WHERE id = ?`, a.ID).Scan(&messageID); err != nil || messageID != msg.ID {
				t.Fatalf("retained attachment = %d, %v", messageID, err)
			}
			if err := st.DeleteRoom(owner.ID, ch.ID); !errors.Is(err, ErrNotFound) {
				t.Fatalf("second delete: %v", err)
			}
			recreated, err := st.createRoom(kind, owner.ID, "test", "")
			if err != nil {
				t.Fatal(err)
			}
			if recreated.ID == ch.ID || recreated.Position != ch.Position {
				t.Fatalf("replacement = %+v, original = %+v", recreated, ch)
			}
			if _, err := st.ChannelFor(late.ID, recreated.ID); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestDeleteRoomRejectsLastTextDMAndMissing(t *testing.T) {
	st := memStore(t)
	owner := signUp(t, st, "owner")
	friend := signUp(t, st, "friend")
	dm, err := st.EnsureDM(owner.ID, friend.ID)
	if err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{dm.ID, "invented"} {
		if err := st.DeleteRoom(owner.ID, id); !errors.Is(err, ErrNotFound) {
			t.Fatalf("delete %s: %v", id, err)
		}
	}
	ch, err := st.CreateTextChannel(owner.ID, "extra", "")
	if err != nil {
		t.Fatal(err)
	}
	if err := st.DeleteRoom(owner.ID, ch.ID); err != nil {
		t.Fatal(err)
	}
	if err := st.DeleteRoom(owner.ID, "geral"); !errors.Is(err, ErrLastChannel) {
		t.Fatalf("last text: %v", err)
	}
	for _, id := range []string{"geral", dm.ID} {
		if _, err := st.ChannelFor(friend.ID, id); err != nil {
			t.Fatalf("rejected deletion revoked access: %v", err)
		}
	}
}

func TestDeleteRoomRollsBackWhenMembershipRemovalFails(t *testing.T) {
	st := memStore(t)
	owner := signUp(t, st, "owner")
	if _, err := st.db.Exec(`CREATE TRIGGER refuse_removal BEFORE DELETE ON channel_members BEGIN SELECT RAISE(ABORT, 'test failure'); END`); err != nil {
		t.Fatal(err)
	}
	if err := st.DeleteRoom(owner.ID, "voz-geral"); err == nil {
		t.Fatal("delete unexpectedly succeeded")
	}
	var live int
	if err := st.db.QueryRow(`SELECT COUNT(*) FROM channels WHERE id = 'voz-geral' AND deleted_at IS NULL AND deleted_by IS NULL`).Scan(&live); err != nil || live != 1 {
		t.Fatalf("rollback = %d, %v", live, err)
	}
	if _, err := st.ChannelFor(owner.ID, "voz-geral"); err != nil {
		t.Fatal(err)
	}
}

func TestConcurrentDeletesPreserveLastTextChannel(t *testing.T) {
	st := memStore(t)
	owner := signUp(t, st, "owner")
	extra, err := st.CreateTextChannel(owner.ID, "extra", "")
	if err != nil {
		t.Fatal(err)
	}
	results := make(chan error, 2)
	var wg sync.WaitGroup
	for _, id := range []string{"geral", extra.ID} {
		wg.Add(1)
		go func(id string) { defer wg.Done(); results <- st.DeleteRoom(owner.ID, id) }(id)
	}
	wg.Wait()
	close(results)
	var deleted, protected int
	for err := range results {
		switch {
		case err == nil:
			deleted++
		case errors.Is(err, ErrLastChannel):
			protected++
		default:
			t.Fatal(err)
		}
	}
	if deleted != 1 || protected != 1 {
		t.Fatalf("deleted = %d, protected = %d", deleted, protected)
	}
}
