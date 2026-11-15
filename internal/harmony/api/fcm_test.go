package api

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"io"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"call/internal/harmony/push"
)

type fcmCast struct {
	*cast
	received chan []byte
	status   atomic.Int32
}

func newFCMCast(t *testing.T) *fcmCast {
	t.Helper()
	f := &fcmCast{received: make(chan []byte, 32)}
	f.status.Store(200)
	service := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/token" {
			_, _ = io.WriteString(w, `{"access_token":"test-access","expires_in":3600}`)
			return
		}
		b, _ := io.ReadAll(r.Body)
		f.received <- b
		status := int(f.status.Load())
		w.WriteHeader(status)
		if status == 200 {
			_, _ = io.WriteString(w, `{}`)
		} else {
			_, _ = io.WriteString(w, `{"error":{"details":[{"errorCode":"UNREGISTERED"}]}}`)
		}
	}))
	t.Cleanup(service.Close)
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	der, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	credentials, err := json.Marshal(map[string]string{"type": "service_account", "project_id": "test-project", "client_email": "test@example.com", "token_uri": service.URL + "/token", "private_key": string(pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: der}))})
	if err != nil {
		t.Fatal(err)
	}
	sender, err := push.NewFCM(credentials)
	if err != nil {
		t.Fatal(err)
	}
	sender.SetTransportForTest(service.Client(), service.URL+"/send")
	h := newHarnessTweaked(t, func(cfg *Config) { cfg.FCM = sender })
	owner := h.join(h.bootstrapInvite(), "tales", goodPass)
	invite, _ := owner.inviteFrom("alice")
	alice := h.join(invite, "alice", goodPass2)
	invite, _ = owner.inviteFrom("bruno")
	bo := h.join(invite, "bruno", goodPass2)
	f.cast = &cast{h: h, owner: owner, alice: alice, bo: bo}
	return f
}

func (f *fcmCast) register(t *testing.T) {
	t.Helper()
	res := f.alice.do("POST", "/api/push/fcm", map[string]string{"token": "alice-device"})
	if res.status != 200 || res.body["enabled"] != true || res.body["registered"] != true || res.body["devices"] != float64(1) {
		t.Fatalf("register %d %s", res.status, res.raw)
	}
}
func (f *fcmCast) quiet(t *testing.T) {
	t.Helper()
	select {
	case <-f.received:
		t.Fatal("unexpected FCM send")
	case <-time.After(600 * time.Millisecond):
	}
}
func (f *fcmCast) wait(t *testing.T, kind, channel, message, call string) {
	t.Helper()
	var raw []byte
	select {
	case raw = <-f.received:
	case <-time.After(5 * time.Second):
		t.Fatal("no FCM send")
	}
	for _, secret := range []string{"private-message-text", "tales", "notification", "title", "body"} {
		if strings.Contains(string(raw), secret) {
			t.Fatalf("private content in FCM: %s", raw)
		}
	}
	var body struct {
		Message struct {
			Token   string            `json:"token"`
			Data    map[string]string `json:"data"`
			Android map[string]string `json:"android"`
		} `json:"message"`
	}
	if err := json.Unmarshal(raw, &body); err != nil {
		t.Fatal(err)
	}
	m := body.Message
	want := map[string]string{"k": kind, "at": m.Data["at"]}
	if channel != "" {
		want["c"] = channel
	}
	if message != "" {
		want["m"] = message
	}
	if call != "" {
		want["call"] = call
	}
	if len(m.Data) != len(want) {
		t.Fatalf("unexpected data %v", m.Data)
	}
	for k, v := range want {
		if m.Data[k] != v {
			t.Fatalf("data %v want %v", m.Data, want)
		}
	}
	at, err := strconv.ParseInt(m.Data["at"], 10, 64)
	if err != nil || time.Since(time.UnixMilli(at)) > time.Minute || at > time.Now().UnixMilli() {
		t.Fatalf("invalid at %q", m.Data["at"])
	}
	ttl := "43200s"
	if kind == "ring" || kind == "call" {
		ttl = "60s"
	}
	if m.Token != "alice-device" || m.Android["ttl"] != ttl || m.Android["collapse_key"] != kind || m.Android["priority"] != "HIGH" {
		t.Fatalf("message %s", raw)
	}
}

func TestFCMRoutesAndIsolation(t *testing.T) {
	f := newFCMCast(t)
	for _, path := range []string{"/api/push/fcm", "/api/push/fcm/remove"} {
		if res := f.h.device("guest").do("POST", path, map[string]string{"token": "alice-device"}); res.status != 401 {
			t.Fatalf("unauthenticated %s: %d", path, res.status)
		}
	}
	for _, token := range []string{"", strings.Repeat("a", 4097), "bad token", "bad/token", "?", "bad\n"} {
		res := f.alice.do("POST", "/api/push/fcm", map[string]string{"token": token})
		if res.status != 400 || res.errorCode() != "bad_token" {
			t.Fatalf("bad token %d %s", res.status, res.raw)
		}
	}
	f.register(t)
	alice, _, err := f.h.st.UserByUsername("alice")
	if err != nil {
		t.Fatal(err)
	}
	for _, token := range []string{"alice-device", "invented"} {
		res := f.owner.do("POST", "/api/push/fcm/remove", map[string]string{"token": token})
		if res.status != 200 || res.body["registered"] != false || res.body["devices"] != float64(0) {
			t.Fatalf("remove %d %s", res.status, res.raw)
		}
	}
	tokens, err := f.h.st.LiveFCMTokens(alice.ID)
	if err != nil || len(tokens) != 1 {
		t.Fatalf("another account removed token: %v %v", tokens, err)
	}
	res := f.alice.do("POST", "/api/push/fcm/remove", map[string]string{"token": "alice-device"})
	if res.status != 200 || res.body["devices"] != float64(0) {
		t.Fatalf("own remove: %s", res.raw)
	}
	if strings.Contains(f.h.logs.String(), "alice-device") {
		t.Fatal("token in logs")
	}
}

func TestFCMDisabled(t *testing.T) {
	c := newCast(t)
	res := c.alice.do("POST", "/api/push/fcm", map[string]string{"token": "phone"})
	if res.status != 503 || res.errorCode() != "push_disabled" {
		t.Fatalf("disabled %d %s", res.status, res.raw)
	}
	res = c.alice.do("POST", "/api/push/fcm/remove", map[string]string{"token": "phone"})
	if res.status != 200 || res.body["enabled"] != false || res.body["registered"] != false {
		t.Fatalf("disabled remove %s", res.raw)
	}
}

func TestFCMMessagesContainOnlyIdentifiers(t *testing.T) {
	for _, kind := range []string{"dm", "mention"} {
		t.Run(kind, func(t *testing.T) {
			f := newFCMCast(t)
			f.register(t)
			channel := f.owner.generalID()
			text := "@alice private-message-text"
			if kind == "dm" {
				channel = f.owner.openDM("alice")
				text = "private-message-text"
			}
			msg := f.owner.mustSay(channel, text)
			id := strconv.FormatInt(int64(msg["id"].(float64)), 10)
			f.wait(t, kind, channel, id, "")
			pending := f.alice.pending()
			if len(pending) != 1 || pending[0]["message_id"] != msg["id"] {
				t.Fatalf("pending %v", pending)
			}
			f.owner.mustSay(channel, text)
			f.quiet(t)
		})
	}
}

func TestFCMRoomAndDirectCalls(t *testing.T) {
	t.Run("room", func(t *testing.T) {
		f := newFCMCast(t)
		f.register(t)
		if err := f.h.api.StartVoice(VoiceOptions{UDPPort: 0, PublicIP: "127.0.0.1", MaxPerRoom: 8}); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = f.h.api.StopVoice() })
		room := f.owner.voiceID()
		f.owner.mustJoinVoice(room)
		if res := f.owner.ring(room, "alice"); res.status != 200 {
			t.Fatalf("ring %s", res.raw)
		}
		f.wait(t, "ring", room, "", "")
	})
	t.Run("direct", func(t *testing.T) {
		f := newFCMCast(t)
		f.register(t)
		res := f.owner.call("alice")
		if res.status != 201 {
			t.Fatalf("call %s", res.raw)
		}
		id := res.body["id"].(string)
		f.wait(t, "call", "", "", id)
		p := f.alice.pending()
		if len(p) != 1 || p[0]["kind"] != "ring" || p[0]["call"] != nil || p[0]["call_id"] != nil {
			t.Fatalf("pending %v", p)
		}
	})
}

func TestFCMVisibleAndLogoutSuppressDelivery(t *testing.T) {
	for _, mode := range []string{"visible", "logout"} {
		t.Run(mode, func(t *testing.T) {
			f := newFCMCast(t)
			f.register(t)
			if mode == "visible" {
				socket := f.alice.dial(t)
				defer socket.Close()
				alice, _, err := f.h.st.UserByUsername("alice")
				if err != nil {
					t.Fatal(err)
				}
				waitFor(t, "visible", func() bool { return f.h.api.hub.visible(alice.ID) })
			} else {
				if res := f.alice.do("POST", "/api/logout", nil); res.status != 200 {
					t.Fatalf("logout %s", res.raw)
				}
			}
			f.owner.mustSay(f.owner.openDM("alice"), "private-message-text")
			f.quiet(t)
		})
	}
}

func TestFCMUnregisteredIsDropped(t *testing.T) {
	f := newFCMCast(t)
	f.register(t)
	f.status.Store(404)
	channel := f.owner.openDM("alice")
	msg := f.owner.mustSay(channel, "private-message-text")
	f.wait(t, "dm", channel, strconv.FormatInt(int64(msg["id"].(float64)), 10), "")
	alice, _, err := f.h.st.UserByUsername("alice")
	if err != nil {
		t.Fatal(err)
	}
	waitFor(t, "token dropped", func() bool { tokens, err := f.h.st.LiveFCMTokens(alice.ID); return err == nil && len(tokens) == 0 })
}

func TestFCMAndWebPushShareCooldown(t *testing.T) {
	f := newFCMCast(t)
	// A notification with no registered devices must not spend the cooldown.
	channel := f.owner.openDM("alice")
	f.owner.mustSay(channel, "private-message-text")
	f.quiet(t)
	f.register(t)
	web := newFakePushService(t)
	pub, priv, err := push.Generate()
	if err != nil {
		t.Fatal(err)
	}
	keys, err := push.NewKeys(pub, priv, "mailto:test@example.com")
	if err != nil {
		t.Fatal(err)
	}
	// Configure the test seam before starting the request that uses it.
	f.h.api.vapid = keys
	f.h.api.notify.sender = push.NewSender(keys)
	f.h.api.notify.sender.SetClientForTest(web.srv.Client())
	if res := f.alice.subscribePush(web.endpoint("alice-web")); res.status != 200 {
		t.Fatalf("web subscribe %s", res.raw)
	}
	msg := f.owner.mustSay(channel, "private-message-text")
	f.wait(t, "dm", channel, strconv.FormatInt(int64(msg["id"].(float64)), 10), "")
	got := web.wait(t)
	if got.body != "" {
		t.Fatal("Web Push gained a payload")
	}
	f.owner.mustSay(channel, "private-message-text")
	f.quiet(t)
	web.quiet(t)
}
