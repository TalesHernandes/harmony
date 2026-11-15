package api

import (
	"net/http"
	"reflect"
	"strings"
	"testing"
	"time"

	"call/internal/harmony/passwd"
	"call/internal/harmony/store"
	"github.com/gorilla/websocket"
)

func adminPost(d *device, username, action string) reply {
	return d.do(http.MethodPost, "/api/admin/accounts/"+username+"/"+action, nil)
}

func adminLogin(d *device, password string) reply {
	return d.do(http.MethodPost, "/api/login", map[string]string{"username": "alice", "password": password})
}

func requireReply(t *testing.T, r reply, status int, code string) {
	t.Helper()
	if r.status != status || (code != "" && r.errorCode() != code) {
		t.Fatalf("got %d %s; want %d %s", r.status, r.raw, status, code)
	}
}

func TestAdminEndpointsRejectNonOwnerWithoutChanges(t *testing.T) {
	for _, action := range []string{"list", "disable", "enable", "reset-password"} {
		t.Run(action, func(t *testing.T) {
			c := newCast(t)
			if action == "enable" {
				requireReply(t, adminPost(c.owner, "alice", "disable"), 200, "")
			}
			before, err := c.h.st.ListAccounts()
			if err != nil {
				t.Fatal(err)
			}
			_, hash, err := c.h.st.UserByUsername("alice")
			if err != nil {
				t.Fatal(err)
			}
			var expected string
			for _, name := range []string{"alice", "tales", "missing"} {
				var r reply
				if action == "list" {
					r = c.bo.get("/api/admin/accounts")
				} else {
					r = adminPost(c.bo, name, action)
				}
				requireReply(t, r, 403, "not_allowed")
				if expected != "" && r.raw != expected {
					t.Fatal("account existence leaked")
				}
				expected = r.raw
			}
			after, err := c.h.st.ListAccounts()
			if err != nil {
				t.Fatal(err)
			}
			_, afterHash, err := c.h.st.UserByUsername("alice")
			if err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(before, after) || hash != afterHash {
				t.Fatal("non-owner changed accounts")
			}
			if action != "enable" {
				requireReply(t, c.alice.get("/api/me"), 200, "")
			}
		})
	}
}

func TestAdminAccountsListIncludesAuditMetadata(t *testing.T) {
	c := newCast(t)
	// Revoked and expired sessions must still contribute their last activity.
	u, _, _ := c.h.st.UserByUsername("alice")
	if _, err := c.h.st.DB().Exec(`UPDATE sessions SET revoked_at = 1, expires_at = 1, last_seen_at = 2000000000 WHERE user_id = ?`, u.ID); err != nil {
		t.Fatal(err)
	}
	code, _, err := c.h.st.CreateInvite(0, "cli", store.InviteTTL)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := c.h.st.SignUp(code, "never", "Never", "unused"); err != nil {
		t.Fatal(err)
	}
	r := c.owner.get("/api/admin/accounts")
	requireReply(t, r, 200, "")
	accounts := r.body["accounts"].([]any)
	if len(accounts) != 4 {
		t.Fatalf("accounts: %s", r.raw)
	}
	for i, name := range []string{"tales", "alice", "bruno", "never"} {
		a := accounts[i].(map[string]any)
		if a["username"] != name {
			t.Fatalf("wrong order: %s", r.raw)
		}
		for _, field := range []string{"display_name", "avatar", "is_admin", "created_at", "disabled", "disabled_at", "invited_by"} {
			if _, ok := a[field]; !ok {
				t.Fatalf("missing %s", field)
			}
		}
	}
	if accounts[0].(map[string]any)["invited_by"] != "" || accounts[1].(map[string]any)["invited_by"] != "tales" {
		t.Fatal("wrong invite creator")
	}
	if accounts[1].(map[string]any)["last_seen_at"] != time.Unix(2000000000, 0).UTC().Format(time.RFC3339) {
		t.Fatal("lost revoked session activity")
	}
	if _, ok := accounts[3].(map[string]any)["last_seen_at"]; ok {
		t.Fatal("invented activity for unused account")
	}
	if strings.Contains(r.raw, "password") || strings.Contains(r.raw, "token") {
		t.Fatal("list leaked secrets")
	}
}

func TestAdminDisableAndResetEndAllAccess(t *testing.T) {
	for _, action := range []string{"disable", "reset-password"} {
		t.Run(action, func(t *testing.T) {
			c := newVoiceCast(t)
			other := c.h.device("second login")
			requireReply(t, adminLogin(other, goodPass2), 200, "")
			ws := c.alice.dial(t)
			defer ws.Close()
			peer := c.owner.dial(t)
			defer peer.Close()
			// Seeing presence ensures registration has finished before revocation.
			waitEvent(t, ws, "presence")
			room := c.alice.voiceID()
			c.alice.mustJoinVoice(room)
			r := c.owner.call("alice")
			requireReply(t, r, 201, "")
			callID := r.body["id"].(string)
			waitEvent(t, ws, "call")
			u, _, _ := c.h.st.UserByUsername("alice")
			sess, _, err := c.h.st.Authenticate(c.alice.sessionCookie())
			if err != nil {
				t.Fatal(err)
			}
			if err := c.h.st.AddPushSubscription(u.ID, sess.ID, "https://push.example.test/device", "key", "auth", "test"); err != nil {
				t.Fatal(err)
			}
			if err := c.h.st.AddFCMToken(u.ID, sess.ID, "test-token"); err != nil {
				t.Fatal(err)
			}
			r = adminPost(c.owner, "alice", action)
			requireReply(t, r, 200, "")
			requireReply(t, c.alice.get("/api/me"), 401, "unauthenticated")
			requireReply(t, other.get("/api/me"), 401, "unauthenticated")
			_ = ws.SetReadDeadline(time.Now().Add(3 * time.Second))
			for {
				_, _, err := ws.ReadMessage()
				if err == nil {
					continue
				}
				if !websocket.IsCloseError(err, websocket.CloseNormalClosure, websocket.CloseNoStatusReceived) {
					t.Fatalf("socket not closed normally: %v", err)
				}
				break
			}
			ended := eventData(t, waitEvent(t, peer, "call_ended"))
			if ended["id"] != callID {
				t.Fatal("wrong call ended")
			}
			requireReply(t, c.owner.get("/api/calls/"+callID), 404, "not_found")
			if strings.Contains(c.owner.get("/api/voice/rooms").raw, "alice") {
				t.Fatal("voice survived revocation")
			}
			if strings.Contains(c.owner.get("/api/presence").raw, "alice") {
				t.Fatal("presence survived revocation")
			}
			if action == "disable" {
				requireReply(t, adminLogin(c.h.device("disabled"), goodPass2), 401, "bad_credentials")
				if strings.Contains(c.owner.get("/api/members").raw, "alice") {
					t.Fatal("disabled account in members")
				}
				for _, table := range []string{"push_subscriptions", "fcm_tokens"} {
					var n int
					if err := c.h.st.DB().QueryRow("SELECT count(*) FROM "+table+" WHERE user_id = ?", u.ID).Scan(&n); err != nil {
						t.Fatal(err)
					}
					if n != 0 {
						t.Fatal("push registration survived disable")
					}
				}
				before, _ := c.h.st.ListAccounts()
				requireReply(t, adminPost(c.owner, "alice", "disable"), 200, "")
				after, _ := c.h.st.ListAccounts()
				if !reflect.DeepEqual(before, after) {
					t.Fatal("disable not idempotent")
				}
				requireReply(t, adminPost(c.owner, "alice", "enable"), 200, "")
				requireReply(t, adminPost(c.owner, "alice", "enable"), 200, "")
				if alive, err := c.h.st.SessionAlive(sess.ID); err != nil || alive {
					t.Fatal("enable revived session")
				}
				requireReply(t, adminLogin(c.h.device("enabled"), goodPass2), 200, "")
			} else {
				password, ok := r.body["password"].(string)
				if !ok || password == "" || len(r.body) != 1 {
					t.Fatal("missing one-time password")
				}
				requireReply(t, adminLogin(c.h.device("old"), goodPass2), 401, "bad_credentials")
				requireReply(t, adminLogin(c.h.device("new"), password), 200, "")
				if strings.Contains(c.h.logs.String(), password) {
					t.Fatal("password leaked into logs")
				}
				if strings.Contains(c.owner.get("/api/admin/accounts").raw, password) {
					t.Fatal("password returned again")
				}
			}
		})
	}
}

func TestAdminCannotDisableAdminsOrResetSelf(t *testing.T) {
	c := newCast(t)
	if _, err := c.h.st.DB().Exec(`UPDATE users SET is_admin = 1 WHERE username = 'alice'`); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"tales", "alice"} {
		requireReply(t, adminPost(c.owner, name, "disable"), 400, "cannot_disable_owner")
	}
	requireReply(t, adminPost(c.owner, "tales", "reset-password"), 400, "use_own_settings")
	requireReply(t, c.owner.get("/api/me"), 200, "")
	requireReply(t, c.alice.get("/api/me"), 200, "")
}

func TestAdminUnknownAccount(t *testing.T) {
	c := newCast(t)
	for _, action := range []string{"disable", "enable", "reset-password"} {
		t.Run(action, func(t *testing.T) { requireReply(t, adminPost(c.owner, "missing", action), 404, "not_found") })
	}
}

func TestAdminResetDisabledAccountDoesNotEnableIt(t *testing.T) {
	c := newCast(t)
	requireReply(t, adminPost(c.owner, "alice", "disable"), 200, "")
	r := adminPost(c.owner, "alice", "reset-password")
	requireReply(t, r, 200, "")
	password := r.body["password"].(string)
	requireReply(t, adminLogin(c.h.device("disabled"), password), 401, "bad_credentials")
	requireReply(t, adminPost(c.owner, "alice", "enable"), 200, "")
	requireReply(t, adminLogin(c.h.device("enabled"), password), 200, "")
	if strings.Contains(c.h.logs.String(), password) {
		t.Fatal("password in logs")
	}
}

func TestAdminDisableRacingLoginCannotCreateSession(t *testing.T) {
	c := newCast(t)
	u, _, err := c.h.st.UserByUsername("alice")
	if err != nil {
		t.Fatal(err)
	}
	c.h.api.verified = func() {
		if err := c.h.st.DisableAccount(u.ID); err != nil {
			t.Error(err)
		}
	}
	requireReply(t, adminLogin(c.h.device("racing login"), goodPass2), 401, "bad_credentials")
}

func TestAdminResetRacingLoginCannotCreateSession(t *testing.T) {
	c := newCast(t)
	hash, err := passwd.Hash("replacement password", testParams)
	if err != nil {
		t.Fatal(err)
	}
	u, _, err := c.h.st.UserByUsername("alice")
	if err != nil {
		t.Fatal(err)
	}
	c.h.api.verified = func() {
		if err := c.h.st.SetPasswordHash(u.ID, hash); err != nil {
			t.Error(err)
		}
	}
	requireReply(t, adminLogin(c.h.device("racing login"), goodPass2), 401, "bad_credentials")
}

func TestAdminDisableKeepsOldMessageAuthor(t *testing.T) {
	c := newCast(t)
	channel := c.alice.generalID()
	c.alice.mustSay(channel, "before disable")
	requireReply(t, adminPost(c.owner, "alice", "disable"), 200, "")
	r := c.owner.get("/api/channels/" + channel + "/messages")
	requireReply(t, r, 200, "")
	if !strings.Contains(r.raw, `"username":"alice"`) || !strings.Contains(r.raw, `"display_name":"alice"`) {
		t.Fatal("old author disappeared")
	}
}
