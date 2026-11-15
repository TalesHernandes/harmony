package store

import (
	"errors"
	"fmt"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

func memStore(t *testing.T) *Store {
	t.Helper()
	st, err := OpenMemory()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	return st
}

func fileStore(t *testing.T) (*Store, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "harmony.db")
	st, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	return st, path
}

// signUp is the two-step every test needs: an invite, then an account made with
// it. The hash is a stand-in; the argon2id rules live in the passwd package.
func signUp(t *testing.T, st *Store, username string) User {
	t.Helper()
	code, _, err := st.CreateInvite(0, "test", InviteTTL)
	if err != nil {
		t.Fatal(err)
	}
	u, err := st.SignUp(code, username, username, "$argon2id$fake")
	if err != nil {
		t.Fatalf("sign up %s: %v", username, err)
	}
	return u
}

// PLANO-DISCORD.md §5, integrity: WAL is not optional, and the daily backup
// assumes it.
func TestFileDatabaseUsesWAL(t *testing.T) {
	st, _ := fileStore(t)
	mode, err := st.JournalMode()
	if err != nil {
		t.Fatal(err)
	}
	if mode != "wal" {
		t.Fatalf("journal mode is %q, want wal", mode)
	}
	if err := st.IntegrityCheck(); err != nil {
		t.Fatal(err)
	}
}

func TestFirstAccountIsAdminAndTheRestAreNot(t *testing.T) {
	st := memStore(t)
	owner := signUp(t, st, "tales")
	if !owner.IsAdmin {
		t.Fatal("the first account is not admin")
	}
	second := signUp(t, st, "namorada")
	if second.IsAdmin {
		t.Fatal("the second account came out admin")
	}
}

// The rule from §5: an invite is single use. Two people racing on the same code
// must produce exactly one account, so the check cannot be a read followed by a
// write with a gap in between.
func TestInviteIsSpentExactlyOnce(t *testing.T) {
	st := memStore(t)
	code, _, err := st.CreateInvite(0, "", InviteTTL)
	if err != nil {
		t.Fatal(err)
	}

	const racers = 8
	var wg sync.WaitGroup
	results := make([]error, racers)
	wg.Add(racers)
	for i := 0; i < racers; i++ {
		go func(i int) {
			defer wg.Done()
			_, results[i] = st.SignUp(code, "racer"+string(rune('a'+i)), "racer", "$argon2id$fake")
		}(i)
	}
	wg.Wait()

	won := 0
	for _, err := range results {
		if err == nil {
			won++
		} else if err != ErrInviteBad {
			t.Fatalf("loser got the wrong error: %v", err)
		}
	}
	if won != 1 {
		t.Fatalf("%d accounts created from one invite, want 1", won)
	}
	if n, _ := st.CountUsers(); n != 1 {
		t.Fatalf("%d users in the database, want 1", n)
	}
}

func TestInviteExpires(t *testing.T) {
	st := memStore(t)
	code, _, err := st.CreateInvite(0, "", InviteTTL)
	if err != nil {
		t.Fatal(err)
	}
	if !st.InviteUsable(code) {
		t.Fatal("a fresh invite is not usable")
	}
	st.SetClock(func() time.Time { return time.Now().Add(InviteTTL + time.Minute) })
	if st.InviteUsable(code) {
		t.Fatal("an expired invite is still usable")
	}
	if _, err := st.SignUp(code, "late", "late", "$argon2id$fake"); err != ErrInviteBad {
		t.Fatalf("signing up with an expired invite: %v, want ErrInviteBad", err)
	}
}

func TestInviteCodeIsNotStored(t *testing.T) {
	st := memStore(t)
	code, inv, err := st.CreateInvite(0, "", InviteTTL)
	if err != nil {
		t.Fatal(err)
	}
	if inv.ID == code {
		t.Fatal("the public invite id is the code itself")
	}
	var stored string
	if err := st.db.QueryRow(`SELECT code_hash FROM invites WHERE id = ?`, inv.ID).Scan(&stored); err != nil {
		t.Fatal(err)
	}
	if stored == code {
		t.Fatal("the invite code is stored in the clear")
	}
	if len(stored) != 64 {
		t.Fatalf("stored invite secret is %d chars, want a 64-char sha256", len(stored))
	}
}

func TestUsernameRules(t *testing.T) {
	valid := []string{"tales", "a.b", "user_1", "abc", "namorada-2"}
	invalid := []string{"", "ab", "-tales", "tales-", "Tales", "with space", "emoji😀name",
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
	for _, s := range valid {
		if !ValidUsername(s) {
			t.Errorf("%q should be a valid username", s)
		}
	}
	for _, s := range invalid {
		if ValidUsername(s) {
			t.Errorf("%q should not be a valid username", s)
		}
	}
	// Validation runs on the normalized form, so surrounding space and a
	// capital letter are tidied rather than refused.
	if !ValidUsername(NormalizeUsername("  Tales  ")) {
		t.Error("a name that only needed normalizing was refused")
	}
}

func TestUsernamesDoNotCollideOnlyByCase(t *testing.T) {
	st := memStore(t)
	signUp(t, st, "tales")
	code, _, _ := st.CreateInvite(0, "", InviteTTL)
	if _, err := st.SignUp(code, "TALES", "Tales", "$argon2id$fake"); err != ErrNameTaken {
		t.Fatalf("second Tales: %v, want ErrNameTaken", err)
	}
}

func TestSessionTokenIs256Bits(t *testing.T) {
	st := memStore(t)
	u := signUp(t, st, "tales")
	token, sess, err := st.CreateSession(u.ID, "test-agent", SessionTTL)
	if err != nil {
		t.Fatal(err)
	}
	// base64url without padding: 32 raw bytes become 43 characters.
	if len(token) != 43 {
		t.Fatalf("token is %d chars, want the 43 of 32 random bytes", len(token))
	}
	var stored string
	if err := st.db.QueryRow(`SELECT token_hash FROM sessions WHERE id = ?`, sess.ID).Scan(&stored); err != nil {
		t.Fatal(err)
	}
	if stored == token {
		t.Fatal("the session token is stored in the clear")
	}
	if _, _, err := st.Authenticate(token); err != nil {
		t.Fatalf("fresh token rejected: %v", err)
	}
}

func TestSessionExpiryAndRevocation(t *testing.T) {
	st := memStore(t)
	u := signUp(t, st, "tales")

	token, sess, _ := st.CreateSession(u.ID, "phone", SessionTTL)
	if err := st.RevokeSession(u.ID, sess.ID); err != nil {
		t.Fatal(err)
	}
	if _, _, err := st.Authenticate(token); err != ErrSessionStale {
		t.Fatalf("revoked token: %v, want ErrSessionStale", err)
	}

	old, _, _ := st.CreateSession(u.ID, "pc", SessionTTL)
	st.SetClock(func() time.Time { return time.Now().Add(SessionTTL + time.Hour) })
	if _, _, err := st.Authenticate(old); err != ErrSessionStale {
		t.Fatalf("expired token: %v, want ErrSessionStale", err)
	}
}

// "Log out of every device" has to leave exactly the device that asked.
func TestRevokeAllSessionsKeepsTheCurrentOne(t *testing.T) {
	st := memStore(t)
	u := signUp(t, st, "tales")
	here, current, _ := st.CreateSession(u.ID, "pc", SessionTTL)
	gone, _, _ := st.CreateSession(u.ID, "phone", SessionTTL)
	other, _, _ := st.CreateSession(u.ID, "tablet", SessionTTL)

	n, err := st.RevokeAllSessions(u.ID, current.ID)
	if err != nil {
		t.Fatal(err)
	}
	if n != 2 {
		t.Fatalf("revoked %d sessions, want 2", n)
	}
	if _, _, err := st.Authenticate(here); err != nil {
		t.Fatal("the session that asked was logged out too")
	}
	for _, token := range []string{gone, other} {
		if _, _, err := st.Authenticate(token); err != ErrSessionStale {
			t.Fatal("another device stayed logged in")
		}
	}
}

func TestChangingThePasswordEndsEverySession(t *testing.T) {
	st := memStore(t)
	u := signUp(t, st, "tales")
	token, _, _ := st.CreateSession(u.ID, "pc", SessionTTL)
	if err := st.SetPasswordHash(u.ID, "$argon2id$new"); err != nil {
		t.Fatal(err)
	}
	if _, _, err := st.Authenticate(token); err != ErrSessionStale {
		t.Fatal("a session survived the password change")
	}
}

// §5: one person does not reach another person's row by putting its id in the
// URL. Ownership is part of the WHERE clause, so the answer to somebody else's
// id is the same as the answer to a made-up one.
func TestOneUserCannotTouchAnothersRows(t *testing.T) {
	st := memStore(t)
	a := signUp(t, st, "tales")
	b := signUp(t, st, "estranho")

	_, aSession, _ := st.CreateSession(a.ID, "pc", SessionTTL)
	_, aInvite, _ := st.CreateInvite(a.ID, "for a friend", InviteTTL)

	if err := st.RevokeSession(b.ID, aSession.ID); err != ErrNotFound {
		t.Fatalf("B revoked A's session: %v", err)
	}
	if err := st.RevokeInvite(b.ID, aInvite.ID); err != ErrNotFound {
		t.Fatalf("B revoked A's invite: %v", err)
	}
	if n, _ := st.RevokeAllSessions(b.ID, ""); n != 0 {
		t.Fatalf("B revoked %d of A's sessions with revoke-all", n)
	}

	list, err := st.ListSessions(b.ID)
	if err != nil {
		t.Fatal(err)
	}
	for _, s := range list {
		if s.ID == aSession.ID {
			t.Fatal("A's session is in B's device list")
		}
	}
	invites, err := st.ListInvites(b.ID)
	if err != nil {
		t.Fatal(err)
	}
	if len(invites) != 0 {
		t.Fatalf("B sees %d of A's invites", len(invites))
	}

	// And A still has everything.
	if list, _ := st.ListSessions(a.ID); len(list) != 1 {
		t.Fatal("A's own session went missing")
	}
}

// The owner's view of the whole server: every account, and every invite with
// the names on both ends. It has to include the bootstrap invite, which has no
// creator and is therefore exactly the row a per-creator query drops.
func TestOwnerViewShowsWhoLetWhomIn(t *testing.T) {
	st := memStore(t)
	owner := signUp(t, st, "tales") // through a command-line invite, created_by 0
	code, _, err := st.CreateInvite(owner.ID, "namorada", InviteTTL)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := st.SignUp(code, "namorada", "Namorada", "$argon2id$fake"); err != nil {
		t.Fatal(err)
	}
	_, open, _ := st.CreateInvite(owner.ID, "um amigo", InviteTTL)

	users, err := st.ListUsers()
	if err != nil {
		t.Fatal(err)
	}
	if len(users) != 2 || !users[0].IsAdmin || users[1].IsAdmin {
		t.Fatalf("accounts came back wrong: %+v", users)
	}

	all, err := st.ListAllInvites()
	if err != nil {
		t.Fatal(err)
	}
	if len(all) != 3 {
		t.Fatalf("%d invites, want 3 (bootstrap, namorada, open)", len(all))
	}
	var sawBootstrap, sawSpent bool
	for _, inv := range all {
		switch {
		case inv.CreatedByName == "" && inv.UsedByName == "tales":
			sawBootstrap = true
		case inv.CreatedByName == "tales" && inv.UsedByName == "namorada":
			sawSpent = true
		}
	}
	if !sawBootstrap {
		t.Error("the command-line invite is missing, or claims a creator")
	}
	if !sawSpent {
		t.Error("the invite the second account used does not name both ends")
	}

	// The per-creator list still hides the bootstrap one, which is why the
	// owner's view is a separate query rather than the same one.
	mine, err := st.ListInvites(owner.ID)
	if err != nil {
		t.Fatal(err)
	}
	if len(mine) != 2 {
		t.Fatalf("the owner created %d invites by this count, want 2", len(mine))
	}

	// And an admin can cancel an unused invite whoever opened it, while a spent
	// one is a record and stays.
	if err := st.RevokeAnyInvite(open.ID); err != nil {
		t.Fatalf("admin could not cancel an open invite: %v", err)
	}
	spent := all[0]
	for _, inv := range all {
		if inv.Used() {
			spent = inv
		}
	}
	if err := st.RevokeAnyInvite(spent.ID); err != ErrNotFound {
		t.Fatalf("a spent invite was deleted: %v", err)
	}
	if left, _ := st.ListAllInvites(); len(left) != 2 {
		t.Fatalf("%d invites left, want 2", len(left))
	}
}

func TestDisabledAccountCannotAuthenticate(t *testing.T) {
	st := memStore(t)
	u := signUp(t, st, "banido")
	token, _, _ := st.CreateSession(u.ID, "pc", SessionTTL)
	if _, err := st.db.Exec(`UPDATE users SET disabled_at = ? WHERE id = ?`, time.Now().Unix(), u.ID); err != nil {
		t.Fatal(err)
	}
	if _, _, err := st.Authenticate(token); err != ErrSessionStale {
		t.Fatal("a disabled account kept its session")
	}
}

func TestFailureCountingIsPerKeyAndWindowed(t *testing.T) {
	st := memStore(t)
	for i := 0; i < 3; i++ {
		if err := st.RecordFailure(FailAccount, "tales"); err != nil {
			t.Fatal(err)
		}
	}
	_ = st.RecordFailure(FailAccount, "outro")
	_ = st.RecordFailure(FailIP, "tales")

	if n, _ := st.Failures(FailAccount, "tales", time.Minute); n != 3 {
		t.Fatalf("account failures = %d, want 3", n)
	}
	if n, _ := st.Failures(FailAccount, "outro", time.Minute); n != 1 {
		t.Fatalf("failures leaked between accounts: %d", n)
	}
	if n, _ := st.Failures(FailIP, "tales", time.Minute); n != 1 {
		t.Fatalf("failures leaked between kinds: %d", n)
	}

	st.SetClock(func() time.Time { return time.Now().Add(2 * time.Minute) })
	if n, _ := st.Failures(FailAccount, "tales", time.Minute); n != 0 {
		t.Fatalf("failures outside the window still count: %d", n)
	}
	st.SetClock(time.Now)
	if err := st.ClearFailures(FailAccount, "tales"); err != nil {
		t.Fatal(err)
	}
	if n, _ := st.Failures(FailAccount, "tales", time.Minute); n != 0 {
		t.Fatalf("ClearFailures left %d rows", n)
	}
}

// §5, integrity: a backup that has never been restored is not a backup. This is
// the machine half of that promise — the other half is the monthly restore of
// the real encrypted file, in docs/DEPLOY.md.
func TestBackupIsAConsistentRestorableCopy(t *testing.T) {
	st, _ := fileStore(t)
	owner := signUp(t, st, "tales")
	_, _, _ = st.CreateInvite(owner.ID, "namorada", InviteTTL)

	copyPath := filepath.Join(t.TempDir(), "backup.db")
	if err := st.BackupTo(copyPath); err != nil {
		t.Fatal(err)
	}

	restored, err := Open(copyPath)
	if err != nil {
		t.Fatalf("the backup does not open: %v", err)
	}
	defer restored.Close()
	if err := restored.IntegrityCheck(); err != nil {
		t.Fatalf("restored copy fails integrity check: %v", err)
	}
	n, err := restored.CountUsers()
	if err != nil {
		t.Fatal(err)
	}
	if n != 1 {
		t.Fatalf("restored copy holds %d accounts, want 1", n)
	}
	if invites, _ := restored.ListInvites(owner.ID); len(invites) != 1 {
		t.Fatal("the invite did not survive the backup")
	}
	// The schema version travels inside the file, so a restored backup does not
	// re-run migrations that already ran.
	var version int
	if err := restored.db.QueryRow(`PRAGMA user_version`).Scan(&version); err != nil {
		t.Fatal(err)
	}
	if version != len(migrations) {
		t.Fatalf("restored user_version = %d, want %d", version, len(migrations))
	}
}

func TestPurgeExpiredKeepsWhatIsStillValid(t *testing.T) {
	st := memStore(t)
	u := signUp(t, st, "tales")
	live, _, _ := st.CreateSession(u.ID, "pc", SessionTTL)
	short, _, _ := st.CreateSession(u.ID, "phone", time.Second)
	_, _, _ = st.CreateInvite(u.ID, "still good", InviteTTL)
	_ = st.RecordFailure(FailIP, "1.2.3.4")

	st.SetClock(func() time.Time { return time.Now().Add(time.Hour) })
	if err := st.PurgeExpired(15 * time.Minute); err != nil {
		t.Fatal(err)
	}
	if _, _, err := st.Authenticate(live); err != nil {
		t.Fatalf("a live session was purged: %v", err)
	}
	if _, _, err := st.Authenticate(short); err != ErrSessionStale {
		t.Fatal("the expired session is still usable")
	}
	if invites, _ := st.ListInvites(u.ID); len(invites) != 1 {
		t.Fatal("a valid invite was purged")
	}
	if n, _ := st.Failures(FailIP, "1.2.3.4", 24*time.Hour); n != 0 {
		t.Fatal("old failure rows were not purged")
	}
}

// Two people clicking each other's name at the same moment, which is how a
// pair's first conversation usually opens. One of the two loses the race on
// dm_key, and the losing path used to look the conversation up again without
// ending its own transaction first: with a one-connection pool that is the
// whole service stopping, not a slow query. Audit of 2026-09-20, F2.
//
// Forty rounds because the interleaving has to land, and a test that only
// sometimes exercises the path is a test that sometimes proves nothing.
func TestOpeningTheSameDMFromBothSidesAtOnce(t *testing.T) {
	st := memStore(t)

	for round := 0; round < 40; round++ {
		a := signUp(t, st, fmt.Sprintf("alfa%d", round))
		b := signUp(t, st, fmt.Sprintf("bravo%d", round))

		done := make(chan struct {
			ch  Channel
			err error
		}, 2)
		var ready sync.WaitGroup
		ready.Add(2)
		start := make(chan struct{})
		for i := 0; i < 2; i++ {
			go func() {
				ready.Done()
				<-start
				ch, err := st.EnsureDM(a.ID, b.ID)
				done <- struct {
					ch  Channel
					err error
				}{ch, err}
			}()
		}
		ready.Wait()
		close(start)

		var ids []string
		for i := 0; i < 2; i++ {
			select {
			case got := <-done:
				if got.err != nil {
					t.Fatalf("round %d: EnsureDM: %v", round, got.err)
				}
				ids = append(ids, got.ch.ID)
			case <-time.After(5 * time.Second):
				// Not "slow": the connection is held by a transaction that is
				// waiting for that same connection, so this never returns.
				t.Fatalf("round %d: EnsureDM deadlocked; %d of 2 came back", round, i)
			}
		}
		if ids[0] != ids[1] {
			t.Fatalf("round %d: two conversations for one pair: %s and %s", round, ids[0], ids[1])
		}
	}
}

// The daily upload ceiling has to count the file that is arriving, and it has
// to count it in the same breath as it writes the row. Neither was true: the
// check asked whether the *previous* total had reached the ceiling, so the
// last upload of the day could be a whole file past it, and several uploads
// read the same total before any of them wrote. Audit of 2026-09-20, F8.
func TestTheUploadCeilingCountsTheFileArriving(t *testing.T) {
	st := memStore(t)
	u := signUp(t, st, "tales")
	ch, err := st.EnsureDM(u.ID, signUp(t, st, "alice").ID)
	if err != nil {
		t.Fatal(err)
	}
	const mib = 1 << 20
	quota := &UploadQuota{Window: 24 * time.Hour, MaxFiles: 60, MaxBytes: 512 * mib}

	// 511 MiB in, all of it allowed.
	for i := 0; i < 15; i++ {
		if _, err := st.CreateAttachmentWithin(u.ID, ch.ID, "f.png", "image/png", 32*mib, 1, 1, quota); err != nil {
			t.Fatalf("file %d of 32 MiB: %v", i+1, err)
		}
	}
	if _, err := st.CreateAttachmentWithin(u.ID, ch.ID, "f.png", "image/png", 31*mib, 1, 1, quota); err != nil {
		t.Fatalf("the 31 MiB that fills it to 511: %v", err)
	}

	// The next 32 MiB would make 543. One byte over is over.
	if _, err := st.CreateAttachmentWithin(u.ID, ch.ID, "f.png", "image/png", 32*mib, 1, 1, quota); !errors.Is(err, ErrUploadBytes) {
		t.Errorf("a file that goes past the ceiling was accepted: %v", err)
	}
	if _, err := st.CreateAttachmentWithin(u.ID, ch.ID, "f.png", "image/png", 1*mib, 1, 1, quota); err != nil {
		t.Errorf("a file that still fits was refused: %v", err)
	}

	// And the count, which was already right, stays right.
	tight := &UploadQuota{Window: 24 * time.Hour, MaxFiles: 17, MaxBytes: 512 * mib}
	if _, err := st.CreateAttachmentWithin(u.ID, ch.ID, "f.png", "image/png", 1, 1, 1, tight); !errors.Is(err, ErrUploadFiles) {
		t.Errorf("the seventeenth file was accepted against a limit of seventeen: %v", err)
	}
}

// The same ceiling under a burst. Eight uploads that arrive together must not
// all read the same total and all be let through.
func TestTheUploadCeilingHoldsUnderABurst(t *testing.T) {
	st := memStore(t)
	u := signUp(t, st, "tales")
	ch, err := st.EnsureDM(u.ID, signUp(t, st, "alice").ID)
	if err != nil {
		t.Fatal(err)
	}
	const mib = 1 << 20
	quota := &UploadQuota{Window: 24 * time.Hour, MaxFiles: 60, MaxBytes: 4 * mib}

	var wg sync.WaitGroup
	accepted := make([]bool, 8)
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			_, err := st.CreateAttachmentWithin(u.ID, ch.ID, "f.png", "image/png", mib, 1, 1, quota)
			accepted[i] = err == nil
		}(i)
	}
	wg.Wait()

	n := 0
	for _, ok := range accepted {
		if ok {
			n++
		}
	}
	if n > 4 {
		t.Errorf("%d of 8 one-megabyte uploads were accepted against a four megabyte ceiling", n)
	}
	if n == 0 {
		t.Error("none were accepted; the ceiling is not the thing being measured")
	}
}

// The message ceiling had the upload ceiling's shape: count first, post
// afterwards. Thirty messages sent at once all read the same count before any
// of them wrote, and all thirty went in against a limit of twenty.
func TestTheMessageCeilingHoldsUnderABurst(t *testing.T) {
	st := memStore(t)
	u := signUp(t, st, "tales")
	ch, err := st.EnsureDM(u.ID, signUp(t, st, "alice").ID)
	if err != nil {
		t.Fatal(err)
	}
	quota := &MessageQuota{Window: time.Minute, Max: 20}

	var wg sync.WaitGroup
	accepted := make([]bool, 30)
	for i := range accepted {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			_, err := st.PostMessageWithin(u.ID, ch.ID, "oi", nil, 0, quota)
			accepted[i] = err == nil
		}(i)
	}
	wg.Wait()

	n := 0
	for _, ok := range accepted {
		if ok {
			n++
		}
	}
	if n != 20 {
		t.Errorf("%d of 30 messages sent at once were accepted against a ceiling of 20", n)
	}
	if _, err := st.PostMessageWithin(u.ID, ch.ID, "mais uma", nil, 0, quota); !errors.Is(err, ErrMessageLimit) {
		t.Errorf("the twenty-first message: %v, want ErrMessageLimit", err)
	}
}
