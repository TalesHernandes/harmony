package store

import (
	"encoding/base32"
	"errors"
	"strings"
	"testing"
	"time"
)

func TestRFC6238SHA1Vectors(t *testing.T) {
	secret := base32.StdEncoding.WithPadding(base32.NoPadding).EncodeToString([]byte("12345678901234567890"))
	for _, v := range []struct {
		at   int64
		want string
	}{{59, "94287082"}, {1111111109, "07081804"}, {1111111111, "14050471"}, {1234567890, "89005924"}, {2000000000, "69279037"}, {20000000000, "65353130"}} {
		if got := TOTPCode(secret, v.at/30, 8); got != v.want {
			t.Fatalf("%d: %s, want %s", v.at, got, v.want)
		}
	}
}
func securityUser(t *testing.T, s *Store, name string) User {
	t.Helper()
	invite, _, err := s.CreateInvite(0, "test", time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	u, err := s.SignUp(invite, name, name, "hash", name+"@example.org")
	if err != nil {
		t.Fatal(err)
	}
	return u
}
func TestEmailProofFiveAttemptsOwnershipAndPurge(t *testing.T) {
	s, err := OpenMemory()
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	a := securityUser(t, s, "alice")
	b := securityUser(t, s, "bruno")
	if err = s.IssueEmailProof(a.ID, a.Email, "123456"); err != nil {
		t.Fatal(err)
	}
	if err = s.UseProof(b.ID, "", "verify_email", "123456", ""); err == nil {
		t.Fatal("B consumed A proof")
	}
	for i := 0; i < 5; i++ {
		if err = s.UseProof(a.ID, "", "verify_email", "654321", ""); !errors.Is(err, ErrCredentials) {
			t.Fatal(err)
		}
	}
	if err = s.UseProof(a.ID, "", "verify_email", "123456", ""); !errors.Is(err, ErrRateLimited) {
		t.Fatal("sixth accepted", err)
	}
	now := time.Now()
	s.SetClock(func() time.Time { return now.Add(11 * time.Minute) })
	if err = s.PurgeExpired(time.Hour); err != nil {
		t.Fatal(err)
	}
	var n int
	s.db.QueryRow(`SELECT count(*) FROM security_tokens`).Scan(&n)
	if n != 0 {
		t.Fatal("expired proofs retained")
	}
}
func TestTOTPWindowReplayAndRecoveryRotation(t *testing.T) {
	s, err := OpenMemory()
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	u := securityUser(t, s, "alice")
	now := time.Unix(1234567890, 0)
	s.SetClock(func() time.Time { return now })
	s.db.Exec(`UPDATE users SET email_verified_at=? WHERE id=?`, now.Unix(), u.ID)
	secret, err := s.SetupTOTP(u.ID, "hash")
	if err != nil {
		t.Fatal(err)
	}
	codes, err := s.ChangeFactor(u.ID, "hash", "totp", TOTPCode(secret, now.Unix()/30, 6))
	if err != nil || len(codes) != 10 {
		t.Fatal(err)
	}
	token, err := s.IssueProof(u.ID, "login_challenge", "", "", 10*time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	if err = s.UseProof(u.ID, token, "login_challenge", TOTPCode(secret, now.Unix()/30, 6), ""); err == nil {
		t.Fatal("setup code replayed")
	}
	if err = s.UseProof(u.ID, token, "login_challenge", TOTPCode(secret, now.Unix()/30+1, 6), ""); err != nil {
		t.Fatal("+1 rejected", err)
	}
	token, err = s.IssueProof(u.ID, "login_challenge", "", "", 10*time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	if err = s.UseProof(u.ID, token, "login_challenge", codes[0], ""); err != nil {
		t.Fatal(err)
	}
	token, _ = s.IssueProof(u.ID, "login_challenge", "", "", 10*time.Minute)
	if err = s.UseProof(u.ID, token, "login_challenge", codes[0], ""); err == nil {
		t.Fatal("recovery replayed")
	}
	next, err := s.ChangeFactor(u.ID, "hash", "recovery", "")
	if err != nil || len(next) != 10 {
		t.Fatal(err)
	}
	token, _ = s.IssueProof(u.ID, "login_challenge", "", "", 10*time.Minute)
	if err = s.UseProof(u.ID, token, "login_challenge", codes[1], ""); err == nil {
		t.Fatal("old recovery survived rotation")
	}
}
func TestEmailUniqueNormalizedAndInviteRollback(t *testing.T) {
	s, err := OpenMemory()
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	securityUser(t, s, "alice")
	invite, _, _ := s.CreateInvite(0, "test", time.Hour)
	if _, err = s.SignUp(invite, "bruno", "bruno", "hash", " ALICE@EXAMPLE.ORG "); !errors.Is(err, ErrNameTaken) {
		t.Fatal(err)
	}
	if !s.InviteUsable(invite) {
		t.Fatal("duplicate email spent invite")
	}
}

func TestSecurityProofAndSessionCannotRaceAccountChanges(t *testing.T) {
	s, err := OpenMemory()
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	u := securityUser(t, s, "alice")
	old, err := s.IssueProof(u.ID, "reset_password", "", "", time.Hour, ProofBinding{PasswordHash: "hash", Version: u.SecurityVersion})
	if err != nil {
		t.Fatal(err)
	}
	if err = s.ChangePassword(u.ID, "hash", "new-hash", ""); err != nil {
		t.Fatal(err)
	}
	if _, err = s.Proof(old, "reset_password"); !errors.Is(err, ErrCredentials) {
		t.Fatal("old reset survived password change", err)
	}
	if _, err = s.IssueProof(u.ID, "login_challenge", "", "", time.Hour, ProofBinding{PasswordHash: "hash", Version: u.SecurityVersion}); !errors.Is(err, ErrCredentials) {
		t.Fatal("old password issued new challenge", err)
	}
	if _, _, err = s.CreateSessionForHash(u.ID, "new-hash", "test", time.Hour, u.SecurityVersion); !errors.Is(err, ErrCredentials) {
		t.Fatal("old security version issued session", err)
	}
	current, err := s.UserByID(u.ID)
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err = s.CreateSessionForHash(u.ID, "new-hash", "test", time.Hour, current.SecurityVersion); err != nil {
		t.Fatal(err)
	}
}

func TestEmailResendInvalidatesOldCodeAndExpiry(t *testing.T) {
	s, err := OpenMemory()
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	u := securityUser(t, s, "alice")
	if err = s.IssueEmailProof(u.ID, u.Email, "123456"); err != nil {
		t.Fatal(err)
	}
	if err = s.IssueEmailProof(u.ID, u.Email, "654321"); err != nil {
		t.Fatal(err)
	}
	if err = s.UseProof(u.ID, "", "verify_email", "123456", ""); !errors.Is(err, ErrCredentials) {
		t.Fatal("old code survived resend", err)
	}
	s.SetClock(func() time.Time { return time.Now().Add(11 * time.Minute) })
	if err = s.UseProof(u.ID, "", "verify_email", "654321", ""); !errors.Is(err, ErrCredentials) {
		t.Fatal("expired code accepted", err)
	}
}

func TestTOTPAcceptsPreviousStepAndCLIResetRevokesAccess(t *testing.T) {
	s, err := OpenMemory()
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	u := securityUser(t, s, "alice")
	now := time.Unix(1234567890, 0)
	s.SetClock(func() time.Time { return now })
	s.db.Exec(`UPDATE users SET email_verified_at=? WHERE id=?`, now.Unix(), u.ID)
	secret, err := s.SetupTOTP(u.ID, "hash")
	if err != nil {
		t.Fatal(err)
	}
	if _, err = s.ChangeFactor(u.ID, "hash", "totp", TOTPCode(secret, now.Unix()/30-1, 6)); err != nil {
		t.Fatal("-1 rejected", err)
	}
	challenge, err := s.IssueProof(u.ID, "login_challenge", "", "", time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	token, _, err := s.CreateSession(u.ID, "test", time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	if err = s.ResetTwoFA(u.ID); err != nil {
		t.Fatal(err)
	}
	current, hash, err := s.UserByUsername("alice")
	if err != nil || current.TwoFA != "" || hash != "hash" {
		t.Fatal("reset changed password or retained factor", err)
	}
	if _, _, err = s.Authenticate(token); !errors.Is(err, ErrSessionStale) {
		t.Fatal("reset retained session", err)
	}
	if _, err = s.Proof(challenge, "login_challenge"); !errors.Is(err, ErrCredentials) {
		t.Fatal("reset retained challenge", err)
	}
}

func TestPurgePreservesHourlySecurityLimitsAndRefundCannotDeleteReusedRow(t *testing.T) {
	s, err := OpenMemory()
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	now := time.Unix(1234567890, 0)
	s.SetClock(func() time.Time { return now })
	for i := 0; i < 3; i++ {
		if ok, err := s.ReserveAttempt("forgot_account", "1", time.Hour, 3); err != nil || !ok {
			t.Fatal(err)
		}
	}
	now = now.Add(20 * time.Minute)
	if err = s.PurgeExpired(15 * time.Minute); err != nil {
		t.Fatal(err)
	}
	if ok, err := s.ReserveAttempt("forgot_account", "1", time.Hour, 3); err != nil || ok {
		t.Fatal("purge reset hourly limit", err)
	}
	_, receipt, err := s.ReserveAttemptID(FailAccount, "alice", time.Hour, 5)
	if err != nil {
		t.Fatal(err)
	}
	if err = s.ClearFailures(FailAccount, "alice"); err != nil {
		t.Fatal(err)
	}
	if _, err = s.db.Exec(`INSERT INTO auth_failures(rowid,kind,key,at,reservation) VALUES (?,?,'alice',?,'different')`, receipt.id, FailAccount, now.Unix()); err != nil {
		t.Fatal(err)
	}
	if err = s.RefundAttempt(FailAccount, "alice", receipt); err != nil {
		t.Fatal(err)
	}
	if n, err := s.Failures(FailAccount, "alice", time.Hour); err != nil || n != 1 {
		t.Fatal("old receipt removed a different attempt", err)
	}
}

// Recovery codes are copied by hand from paper, so they never contain a pair
// people confuse, and they are accepted however they come back: lower case,
// with or without dashes, with spaces instead.
func TestRecoveryCodesSurviveBeingCopiedByHand(t *testing.T) {
	for i := 0; i < 200; i++ {
		raw, err := recoveryCode()
		if err != nil {
			t.Fatal(err)
		}
		if len(raw) != 16 || strings.ContainsAny(raw, "0O1IL5S8B") {
			t.Fatalf("recovery code %q is not 16 unambiguous characters", raw)
		}
	}
	want := "ACDEFGHJKMNPQRTU"
	for _, typed := range []string{"ACDE-FGHJ-KMNP-QRTU", "acde-fghj-kmnp-qrtu", " ACDE FGHJ KMNP QRTU ", "ACDEFGHJKMNPQRTU"} {
		if got := normalizeRecovery(typed); got != want {
			t.Errorf("normalizeRecovery(%q) = %q, want %q", typed, got, want)
		}
	}
}
