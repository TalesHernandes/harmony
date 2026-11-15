package store

import (
	"fmt"
	"reflect"
	"testing"
	"time"
)

func TestFCMTokenOwnershipAndSessions(t *testing.T) {
	st := memStore(t)
	a := signUp(t, st, "alice")
	b := signUp(t, st, "bruno")
	_, sa, err := st.CreateSession(a.ID, "android", time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	_, sb, err := st.CreateSession(b.ID, "android", time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	check := func(uid int64, want []string) {
		t.Helper()
		got, e := st.LiveFCMTokens(uid)
		if e != nil || !reflect.DeepEqual(got, want) {
			t.Fatalf("tokens=%v want=%v err=%v", got, want, e)
		}
		count, e := st.CountFCMTokens(uid)
		if e != nil || count != len(want) {
			t.Fatalf("count=%d err=%v", count, e)
		}
	}
	must := func(e error) {
		t.Helper()
		if e != nil {
			t.Fatal(e)
		}
	}
	must(st.AddFCMToken(a.ID, sa.ID, "phone"))
	check(a.ID, []string{"phone"})
	must(st.RemoveFCMToken(b.ID, "phone"))
	check(a.ID, []string{"phone"})
	must(st.AddFCMToken(b.ID, sb.ID, "phone"))
	check(a.ID, nil)
	check(b.ID, []string{"phone"})
	must(st.RevokeSession(b.ID, sb.ID))
	check(b.ID, nil)
	must(st.AddFCMToken(a.ID, sa.ID, "phone"))
	must(st.DropFCMToken("phone"))
	check(a.ID, nil)
	must(st.AddFCMToken(a.ID, sa.ID, "phone"))
	must(st.RemoveFCMToken(a.ID, "phone"))
	check(a.ID, nil)
	must(st.RemoveFCMToken(a.ID, "missing"))
	must(st.DropFCMToken("missing"))
	must(st.AddFCMToken(a.ID, sa.ID, "phone"))
	st.now = func() time.Time { return time.Now().Add(2 * time.Hour) }
	check(a.ID, nil)
}

func TestFCMTokenLimitAndCascade(t *testing.T) {
	st := memStore(t)
	a := signUp(t, st, "alice")
	_, sess, err := st.CreateSession(a.ID, "android", time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	st.now = func() time.Time { return now }
	for i := 0; i < MaxSubscriptionsPerUser+3; i++ {
		now = now.Add(time.Second)
		if err := st.AddFCMToken(a.ID, sess.ID, fmt.Sprintf("phone-%02d", i)); err != nil {
			t.Fatal(err)
		}
	}
	got, err := st.LiveFCMTokens(a.ID)
	if err != nil || len(got) != MaxSubscriptionsPerUser || got[0] != "phone-03" {
		t.Fatalf("tokens=%v err=%v", got, err)
	}
	if _, err := st.db.Exec(`DELETE FROM sessions WHERE id=?`, sess.ID); err != nil {
		t.Fatal(err)
	}
	var n int
	if err := st.db.QueryRow(`SELECT count(*) FROM fcm_tokens`).Scan(&n); err != nil || n != 0 {
		t.Fatalf("cascade count=%d err=%v", n, err)
	}
}
