// Sprint D4 turned into tests: who is online, who gets woken up, and what they
// are told.
//
// The rule that governs the whole file is the one from §5 of the plan, applied
// to two new kinds of data. A push subscription is data belonging to the person
// who registered it — so Bruno naming Alice's endpoint has to be exactly as
// useless as naming an invented one. A pending notification is data about who
// talked to whom — so Bruno asking for Alice's gets Bruno's, which is nothing.
//
// The other half is the doorbell itself, and it is tested against a fake push
// service: an httptest server standing in for Google's. That is the only way to
// prove the things that go wrong silently in production — that the push carries
// **no body**, that it is signed, that a call is urgent and a message is not,
// and that what somebody wrote never leaves this machine.
package api

import (
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"call/internal/harmony/push"
	"call/internal/harmony/store"
)

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// pushCast is the ordinary cast on a server that has VAPID keys, plus a fake
// push service to receive what it sends.
type pushCast struct {
	*cast
	service *fakePushService
}

// fakePushService is Google's push service, for the length of one test. It
// records what arrived, which is the only way to assert on a request that goes
// out to a third party and whose reply nobody waits for.
type fakePushService struct {
	srv *httptest.Server

	mu   sync.Mutex
	got  []pushRequest
	seen chan struct{}
	// status is what it answers; 410 is how a push service says "this
	// subscription is dead, forget it".
	status int
}

type pushRequest struct {
	auth    string
	ttl     string
	urgency string
	topic   string
	body    string
	path    string
}

func newFakePushService(t *testing.T) *fakePushService {
	t.Helper()
	f := &fakePushService{seen: make(chan struct{}, 16), status: http.StatusCreated}
	f.srv = httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// A push service that accepts the connection and never answers — an
		// endpoint whose name says so hangs until the sender gives up.
		if strings.Contains(r.URL.Path, "/push/mudo") {
			<-r.Context().Done()
			return
		}
		body := make([]byte, 1024)
		n, _ := r.Body.Read(body)
		f.mu.Lock()
		f.got = append(f.got, pushRequest{
			auth:    r.Header.Get("Authorization"),
			ttl:     r.Header.Get("TTL"),
			urgency: r.Header.Get("Urgency"),
			topic:   r.Header.Get("Topic"),
			body:    string(body[:n]),
			path:    r.URL.Path,
		})
		status := f.status
		f.mu.Unlock()
		w.WriteHeader(status)
		select {
		case f.seen <- struct{}{}:
		default:
		}
	}))
	t.Cleanup(f.srv.Close)
	return f
}

// endpoint is the capability URL a browser would have been given.
func (f *fakePushService) endpoint(who string) string {
	return f.srv.URL + "/push/" + who
}

// wait blocks until one more push arrives, or gives up. Delivery happens on a
// goroutine of its own on purpose (notify.go), so there is nothing synchronous
// to assert on.
func (f *fakePushService) wait(t *testing.T) pushRequest {
	t.Helper()
	select {
	case <-f.seen:
	case <-time.After(5 * time.Second):
		t.Fatal("no push arrived at the push service")
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.got[len(f.got)-1]
}

// quiet asserts that nothing arrives. The window is short because the only way
// this fails is a push that was already decided on and sent.
func (f *fakePushService) quiet(t *testing.T) {
	t.Helper()
	select {
	case <-f.seen:
		t.Fatal("a push went out when nobody should have been woken")
	case <-time.After(600 * time.Millisecond):
	}
}

func (f *fakePushService) count() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.got)
}

// newPushCast builds a server whose doorbell is configured, with the fake push
// service's TLS client wired into the sender — a self-signed httptest server is
// not something Go's default client will talk to.
func newPushCast(t *testing.T) *pushCast {
	t.Helper()
	service := newFakePushService(t)
	c := newCastWithVAPID(t)
	c.h.api.notify.sender.SetClientForTest(service.srv.Client())
	return &pushCast{cast: c, service: service}
}

func newCastWithVAPID(t *testing.T) *cast {
	t.Helper()
	pub, priv, err := push.Generate()
	if err != nil {
		t.Fatal(err)
	}
	keys, err := push.NewKeys(pub, priv, "mailto:dono@example.com")
	if err != nil {
		t.Fatal(err)
	}
	h := newHarnessWith(t, keys)
	owner := h.join(h.bootstrapInvite(), "tales", goodPass)
	inviteA, _ := owner.inviteFrom("alice")
	alice := h.join(inviteA, "alice", goodPass2)
	inviteB, _ := owner.inviteFrom("bruno")
	bo := h.join(inviteB, "bruno", goodPass2)
	return &cast{h: h, owner: owner, alice: alice, bo: bo}
}

// newHarnessWith is newHarness with a doorbell. The two are kept apart so that
// every other test in this package goes on proving that harmony without VAPID
// keys is a harmony that works — which is what the machine looked like for the
// first minutes of this sprint's deploy.
func newHarnessWith(t *testing.T, keys *push.Keys) *harness {
	t.Helper()
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "harmony.db")
	filesDir := filepath.Join(dir, "files")
	if err := os.MkdirAll(filesDir, 0o700); err != nil {
		t.Fatal(err)
	}
	st, err := store.Open(dbPath)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { st.Close() })
	logs := &safeBuffer{}
	handler := New(Config{
		Mailer: &capturingMailer{},
		Store:  st, FilesDir: filesDir, BaseURL: baseURL, Secure: true, TrustProxy: true,
		Logger: slog.New(slog.NewTextHandler(logs, &slog.HandlerOptions{Level: slog.LevelDebug})),
		Params: &testParams, VAPID: keys,
	})
	srv := httptest.NewServer(handler)
	t.Cleanup(srv.Close)
	return &harness{t: t, st: st, api: handler, srv: srv, logs: logs, dbPath: dbPath, filesDir: filesDir}
}

func (d *device) presence() map[string]string {
	d.h.t.Helper()
	res := d.get("/api/presence")
	if res.status != http.StatusOK {
		d.h.t.Fatalf("GET /api/presence: %d %s", res.status, res.raw)
	}
	raw, _ := res.body["presence"].(map[string]any)
	out := map[string]string{}
	for k, v := range raw {
		out[k], _ = v.(string)
	}
	return out
}

func (d *device) setPresence(state string) reply {
	return d.do(http.MethodPost, "/api/presence", map[string]string{"state": state})
}

func (d *device) subscribePush(endpoint string) reply {
	return d.do(http.MethodPost, "/api/push/subscribe", map[string]any{
		"endpoint": endpoint,
		"keys":     map[string]string{"p256dh": "cGsyNTZkaA", "auth": "YXV0aA"},
	})
}

func (d *device) pushStatus(endpoint string) reply {
	return d.get("/api/push/status?endpoint=" + endpoint)
}

func (d *device) pending() []map[string]any {
	d.h.t.Helper()
	res := d.get("/api/notifications/pending")
	if res.status != http.StatusOK {
		d.h.t.Fatalf("GET /api/notifications/pending: %d %s", res.status, res.raw)
	}
	raw, _ := res.body["items"].([]any)
	out := make([]map[string]any, 0, len(raw))
	for _, item := range raw {
		out = append(out, item.(map[string]any))
	}
	return out
}

// waitFor polls until want is true or the time runs out. Presence moves when a
// socket closes, and a socket closing is something the server notices rather
// than something the test does.
func waitFor(t *testing.T, what string, want func() bool) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if want() {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

// ---------------------------------------------------------------------------
// presença
// ---------------------------------------------------------------------------

func TestPresenceNeedsASession(t *testing.T) {
	h := newHarness(t)
	stranger := h.device("browser")
	if res := stranger.get("/api/presence"); res.status != http.StatusUnauthorized {
		t.Errorf("GET /api/presence signed out: %d, want 401", res.status)
	}
	if res := stranger.setPresence("away"); res.status != http.StatusUnauthorized {
		t.Errorf("POST /api/presence signed out: %d, want 401", res.status)
	}
}

// The headline of the presence half: online is a socket, away is the browser
// saying so, and offline is the socket being gone. Nothing is stored, so all
// three are observed from the outside exactly as a friend's screen would.
func TestPresenceFollowsTheSocketAndTheAwayFlag(t *testing.T) {
	c := newCast(t)

	if state := c.owner.presence()["alice"]; state != "" {
		t.Fatalf("alice is %q before opening anything, want absent (offline)", state)
	}

	sock := c.alice.dial(t)
	waitFor(t, "alice to show up as online", func() bool {
		return c.owner.presence()["alice"] == PresenceOnline
	})

	// The screen went off, or she switched to another app.
	if res := c.alice.setPresence("away"); res.status != http.StatusNoContent {
		t.Fatalf("POST /api/presence: %d %s", res.status, res.raw)
	}
	if state := c.owner.presence()["alice"]; state != PresenceAway {
		t.Fatalf("alice is %q after saying away, want away", state)
	}

	// And back.
	c.alice.setPresence("active")
	if state := c.owner.presence()["alice"]; state != PresenceOnline {
		t.Fatalf("alice is %q after coming back, want online", state)
	}

	sock.Close()
	waitFor(t, "alice to go offline when her socket closes", func() bool {
		_, there := c.owner.presence()["alice"]
		return !there
	})
}

// Away is per device; the person is the aggregate. Somebody's computer going to
// sleep does not make their phone away, which is the whole reason the flag
// lives on the socket and not on the account.
func TestAwayIsPerDeviceAndThePersonIsTheAggregate(t *testing.T) {
	c := newCast(t)
	phone := c.h.device("phone")
	res := phone.do(http.MethodPost, "/api/login", map[string]string{
		"username": "alice", "password": goodPass2,
	})
	if res.status != http.StatusOK {
		t.Fatalf("second login for alice: %d %s", res.status, res.raw)
	}

	deskSock := c.alice.dial(t)
	defer deskSock.Close()
	phoneSock := phone.dial(t)
	defer phoneSock.Close()
	waitFor(t, "alice online", func() bool { return c.owner.presence()["alice"] == PresenceOnline })

	// One device away is not a person away.
	c.alice.setPresence("away")
	if state := c.owner.presence()["alice"]; state != PresenceOnline {
		t.Fatalf("alice is %q with one device still awake, want online", state)
	}
	// Both are.
	phone.setPresence("away")
	if state := c.owner.presence()["alice"]; state != PresenceAway {
		t.Fatalf("alice is %q with every device asleep, want away", state)
	}
	// And nobody else moved: the endpoint takes no name, so it can only ever
	// describe the person holding the session.
	if state := c.owner.presence()["bruno"]; state != "" {
		t.Fatalf("bruno became %q because alice said something about herself", state)
	}
}

func TestPresenceRefusesAStateItDoesNotKnow(t *testing.T) {
	c := newCast(t)
	if res := c.alice.setPresence("invisivel"); res.status != http.StatusBadRequest {
		t.Fatalf("POST /api/presence with a made-up state: %d %s, want 400", res.status, res.raw)
	}
}

// ---------------------------------------------------------------------------
// a assinatura de push é de quem assinou
// ---------------------------------------------------------------------------

func TestPushEndpointsNeedASession(t *testing.T) {
	h := newHarness(t)
	stranger := h.device("browser")
	for _, path := range []string{"/api/push/key", "/api/push/status", "/api/notifications/pending"} {
		if res := stranger.get(path); res.status != http.StatusUnauthorized {
			t.Errorf("GET %s signed out: %d, want 401", path, res.status)
		}
	}
	if res := stranger.subscribePush("https://push.example/abc"); res.status != http.StatusUnauthorized {
		t.Errorf("POST /api/push/subscribe signed out: %d, want 401", res.status)
	}
}

// The §5 test for the new table. Bruno naming Alice's endpoint is exactly as
// useless as naming one that never existed: he cannot delete it, he cannot see
// it, and nothing in either answer tells him it is there.
func TestBrunoCannotTouchAlicesPushSubscription(t *testing.T) {
	c := newPushCast(t)
	endpoint := c.service.endpoint("alice-phone")
	if res := c.alice.subscribePush(endpoint); res.status != http.StatusOK {
		t.Fatalf("alice subscribing: %d %s", res.status, res.raw)
	}

	// He asks about it…
	mine := c.bo.pushStatus(endpoint)
	invented := c.bo.pushStatus(c.service.endpoint("nao-existe"))
	if mine.raw != invented.raw {
		t.Fatalf("bruno gets a different answer for alice's endpoint (%s) than for an invented one (%s)",
			mine.raw, invented.raw)
	}
	if sub, _ := mine.body["subscribed"].(bool); sub {
		t.Error("bruno's status says he is subscribed to alice's endpoint")
	}

	// …and he tries to remove it.
	if res := c.bo.do(http.MethodPost, "/api/push/unsubscribe",
		map[string]string{"endpoint": endpoint}); res.status != http.StatusOK {
		t.Fatalf("unsubscribe: %d %s", res.status, res.raw)
	}
	if sub, _ := c.alice.pushStatus(endpoint).body["subscribed"].(bool); !sub {
		t.Fatal("bruno removed alice's registration")
	}

	// And it still rings.
	c.owner.mustSay(c.owner.generalID(), "oi @alice")
	got := c.service.wait(t)
	if !strings.Contains(got.path, "alice-phone") {
		t.Fatalf("the push went to %q", got.path)
	}
}

func TestPushEndpointMustBeAnHTTPSURL(t *testing.T) {
	c := newPushCast(t)
	for _, bad := range []string{"", "http://push.example/abc", "file:///etc/passwd", "https://"} {
		res := c.alice.subscribePush(bad)
		if res.status != http.StatusBadRequest {
			t.Errorf("subscribe with %q: %d %s, want 400", bad, res.status, res.raw)
		}
	}
}

// Without VAPID keys the doorbell is off and everything else works. This is the
// state every other test in this package runs in, and it is what the machine
// looks like before the operator pastes the keys into .env.harmony.
func TestWithoutKeysThereIsNoDoorbellAndNothingElseBreaks(t *testing.T) {
	c := newCast(t)
	res := c.alice.get("/api/push/key")
	if enabled, _ := res.body["enabled"].(bool); enabled {
		t.Error("a server with no keys says push is enabled")
	}
	if res := c.alice.subscribePush("https://push.example/abc"); res.status != http.StatusServiceUnavailable {
		t.Errorf("subscribing on a server with no keys: %d, want 503", res.status)
	}
	// And a mention still works, it just does not wake anybody.
	c.owner.mustSay(c.owner.generalID(), "oi @alice")
	if ch, _ := c.alice.channelByID(c.owner.generalID()); ch["mentions"].(float64) != 1 {
		t.Error("the mention itself stopped working without push keys")
	}
}

// ---------------------------------------------------------------------------
// o que a tela bloqueada mostra
// ---------------------------------------------------------------------------

// The whole path, and the sentence it produces. What is asserted here is as
// much what is *not* in it: the message text never leaves this machine, and the
// push that goes out to a third party has no body at all.
func TestAMentionWakesThePhoneAndNeverSaysWhatWasSaid(t *testing.T) {
	c := newPushCast(t)
	secret := "a senha do wifi e pastel-de-nata-42"
	if res := c.alice.subscribePush(c.service.endpoint("alice-phone")); res.status != http.StatusOK {
		t.Fatalf("subscribe: %d %s", res.status, res.raw)
	}

	c.owner.mustSay(c.owner.generalID(), "@alice "+secret)

	got := c.service.wait(t)
	if got.body != "" {
		t.Errorf("the push carried a body: %q", got.body)
	}
	if !strings.HasPrefix(got.auth, "vapid t=") {
		t.Errorf("the push was not signed: Authorization = %q", got.auth)
	}
	if got.topic != NotifyMention {
		t.Errorf("Topic = %q, want %q — without it a second mention stacks instead of replacing", got.topic, NotifyMention)
	}
	if got.urgency != "normal" {
		t.Errorf("Urgency = %q, want normal: a message is not worth waking a sleeping radio for", got.urgency)
	}

	// And what the woken service worker is told.
	items := c.alice.pending()
	if len(items) != 1 {
		t.Fatalf("alice has %d pending notifications, want 1", len(items))
	}
	item := items[0]
	if item["kind"] != NotifyMention {
		t.Errorf("kind = %v", item["kind"])
	}
	if title, _ := item["title"].(string); !strings.Contains(title, "tales") {
		t.Errorf("the notification does not say who: %v", item["title"])
	}
	if body, _ := item["body"].(string); !strings.Contains(body, "#geral") {
		t.Errorf("the notification does not say where: %v", item["body"])
	}
	whole := item["title"].(string) + item["body"].(string) + item["url"].(string)
	if strings.Contains(whole, "pastel-de-nata") {
		t.Errorf("the message text reached the lock screen: %q", whole)
	}
	// Nor the logs, which is the same rule one layer down.
	if strings.Contains(c.h.logs.String(), "pastel-de-nata") {
		t.Error("the message text reached the logs")
	}

	// Bruno was not mentioned, so nothing is waiting for him — and asking for
	// alice's is not something he can spell.
	if items := c.bo.pending(); len(items) != 0 {
		t.Errorf("bruno has %d pending notifications, want 0", len(items))
	}
}

func TestADirectMessageWakesTheOtherPersonAndNobodyElse(t *testing.T) {
	c := newPushCast(t)
	if res := c.alice.subscribePush(c.service.endpoint("alice-phone")); res.status != http.StatusOK {
		t.Fatalf("subscribe: %d %s", res.status, res.raw)
	}
	if res := c.bo.subscribePush(c.service.endpoint("bruno-phone")); res.status != http.StatusOK {
		t.Fatalf("subscribe: %d %s", res.status, res.raw)
	}

	dm := c.owner.openDM("alice")
	c.owner.mustSay(dm, "so entre nos dois")

	got := c.service.wait(t)
	if !strings.Contains(got.path, "alice-phone") {
		t.Fatalf("the push for a private message went to %q", got.path)
	}
	if n := c.service.count(); n != 1 {
		t.Fatalf("%d pushes went out for a two-person conversation, want 1", n)
	}
	if items := c.bo.pending(); len(items) != 0 {
		t.Errorf("bruno is being told about a conversation he is not in: %v", items)
	}
	items := c.alice.pending()
	if len(items) != 1 || items[0]["kind"] != NotifyDM {
		t.Fatalf("alice's pending = %v", items)
	}
}

// Nobody is woken while they are looking at the app. The phone buzzing about a
// message that is on the screen in front of you is the single most annoying
// thing this feature can do.
func TestSomebodyLookingAtTheAppIsNotWokenUp(t *testing.T) {
	c := newPushCast(t)
	if res := c.alice.subscribePush(c.service.endpoint("alice-phone")); res.status != http.StatusOK {
		t.Fatalf("subscribe: %d %s", res.status, res.raw)
	}
	sock := c.alice.dial(t)
	defer sock.Close()
	waitFor(t, "alice online", func() bool { return c.owner.presence()["alice"] == PresenceOnline })

	c.owner.mustSay(c.owner.generalID(), "@alice olha isso")
	c.service.quiet(t)

	// The screen goes off, and the next one does wake her.
	c.alice.setPresence("away")
	c.owner.mustSay(c.owner.generalID(), "@alice e isso tambem")
	c.service.wait(t)
}

// Reading it on the computer takes it off the phone. This is what the empty
// push buys that a payload never could: the notification is decided when it is
// read, not when it was sent.
func TestReadingSomewhereElseClearsWhatThePhoneWouldShow(t *testing.T) {
	c := newPushCast(t)
	msg := c.owner.mustSay(c.owner.generalID(), "@alice vem ca")
	if items := c.alice.pending(); len(items) != 1 {
		t.Fatalf("alice has %d pending, want 1", len(items))
	}
	id := int64(msg["id"].(float64))
	res := c.alice.do(http.MethodPost, "/api/channels/"+c.owner.generalID()+"/read",
		map[string]any{"last_read_id": id})
	if res.status != http.StatusOK {
		t.Fatalf("mark read: %d %s", res.status, res.raw)
	}
	if items := c.alice.pending(); len(items) != 0 {
		t.Fatalf("alice's phone would still show %v after she read it", items)
	}
}

// ---------------------------------------------------------------------------
// a campainha
// ---------------------------------------------------------------------------

func newRingCast(t *testing.T) *pushCast {
	t.Helper()
	c := newPushCast(t)
	if err := c.h.api.StartVoice(VoiceOptions{UDPPort: 0, PublicIP: "127.0.0.1", MaxPerRoom: 8}); err != nil {
		t.Fatalf("start voice: %v", err)
	}
	t.Cleanup(func() { c.h.api.StopVoice() })
	return c
}

func (d *device) ring(channelID, username string) reply {
	return d.do(http.MethodPost, "/api/voice/"+channelID+"/ring", map[string]string{"username": username})
}

// You call people to where you are. Ringing somebody into a room you are not in
// is a way to make another person's phone buzz from anywhere, so it is refused
// — and the refusal is the server's, not the button's.
func TestRingingOnlyWorksFromInsideTheRoom(t *testing.T) {
	c := newRingCast(t)
	room := c.owner.voiceID()

	if res := c.owner.ring(room, "alice"); res.status != http.StatusConflict || res.errorCode() != "not_in_room" {
		t.Fatalf("ringing from outside the room: %d %s, want 409 not_in_room", res.status, res.raw)
	}

	c.owner.mustJoinVoice(room)
	if res := c.owner.ring(room, "alice"); res.status != http.StatusOK {
		t.Fatalf("ringing from inside the room: %d %s", res.status, res.raw)
	}
}

func TestRingingSomebodyWhoIsNotThereAnswersLikeAnInventedName(t *testing.T) {
	c := newRingCast(t)
	room := c.owner.voiceID()
	c.owner.mustJoinVoice(room)

	invented := c.owner.ring(room, "ninguem")
	if invented.status != http.StatusNotFound {
		t.Fatalf("ringing an invented name: %d %s, want 404", invented.status, invented.raw)
	}
	// A direct message is a channel too, and it is not a room. Same answer.
	dm := c.owner.openDM("alice")
	if res := c.owner.ring(dm, "alice"); res.status != http.StatusNotFound {
		t.Fatalf("ringing into a conversation: %d %s, want 404", res.status, res.raw)
	}
	// And Bruno cannot ring into a room he is looking at from outside.
	if res := c.bo.ring(room, "alice"); res.status != http.StatusConflict {
		t.Fatalf("bruno ringing without being in the room: %d %s", res.status, res.raw)
	}
}

func TestRingingTwiceInARowIsRefused(t *testing.T) {
	c := newRingCast(t)
	room := c.owner.voiceID()
	c.owner.mustJoinVoice(room)
	if res := c.owner.ring(room, "alice"); res.status != http.StatusOK {
		t.Fatalf("first ring: %d %s", res.status, res.raw)
	}
	if res := c.owner.ring(room, "alice"); res.status != http.StatusTooManyRequests {
		t.Fatalf("second ring straight away: %d %s, want 429", res.status, res.raw)
	}
}

// The doorbell, end to end: it reaches the person called, it is urgent, it
// expires with the call, and nobody else hears it.
func TestTheDoorbellRingsTheRightPhoneAndIsUrgent(t *testing.T) {
	c := newRingCast(t)
	room := c.owner.voiceID()
	if res := c.alice.subscribePush(c.service.endpoint("alice-phone")); res.status != http.StatusOK {
		t.Fatalf("subscribe: %d %s", res.status, res.raw)
	}
	if res := c.bo.subscribePush(c.service.endpoint("bruno-phone")); res.status != http.StatusOK {
		t.Fatalf("subscribe: %d %s", res.status, res.raw)
	}
	c.owner.mustJoinVoice(room)

	if res := c.owner.ring(room, "alice"); res.status != http.StatusOK {
		t.Fatalf("ring: %d %s", res.status, res.raw)
	}
	got := c.service.wait(t)
	if !strings.Contains(got.path, "alice-phone") {
		t.Fatalf("the doorbell rang %q", got.path)
	}
	if got.urgency != "high" {
		t.Errorf("Urgency = %q, want high: a call has to wake a sleeping phone", got.urgency)
	}
	if got.ttl != "45" {
		t.Errorf("TTL = %q, want 45: nobody wants to be told at 23:40 about a call at 23:05", got.ttl)
	}
	if n := c.service.count(); n != 1 {
		t.Fatalf("%d phones rang, want 1", n)
	}

	items := c.alice.pending()
	if len(items) != 1 || items[0]["kind"] != NotifyRing {
		t.Fatalf("alice's pending = %v", items)
	}
	if url, _ := items[0]["url"].(string); !strings.Contains(url, room) {
		t.Errorf("tapping the notification would not take her to the room: %v", items[0]["url"])
	}
	if items := c.bo.pending(); len(items) != 0 {
		t.Errorf("bruno was told about a call that was not for him: %v", items)
	}
}

// A push service saying "this subscription is dead" is authoritative: the row
// goes, so the count stops lying about whether the doorbell can ring at all.
func TestADeadSubscriptionIsForgotten(t *testing.T) {
	c := newPushCast(t)
	endpoint := c.service.endpoint("alice-phone")
	if res := c.alice.subscribePush(endpoint); res.status != http.StatusOK {
		t.Fatalf("subscribe: %d %s", res.status, res.raw)
	}
	c.service.mu.Lock()
	c.service.status = http.StatusGone
	c.service.mu.Unlock()

	c.owner.mustSay(c.owner.generalID(), "@alice oi")
	c.service.wait(t)

	waitFor(t, "the dead subscription to be dropped", func() bool {
		sub, _ := c.alice.pushStatus(endpoint).body["subscribed"].(bool)
		return !sub
	})
}

// setPresenceBusy is the same report with the second half of the sentence: this
// device is in a call.
func (d *device) setPresenceBusy(state string, busy bool) reply {
	return d.do(http.MethodPost, "/api/presence", map[string]any{"state": state, "busy": busy})
}

// The owner's words, 13/09/2026: "ausente no pc tem que ser 5 minutos fora da
// aba". A tab that went behind another window is not somebody who left, and
// going grey the instant they alt-tab is what made him ask.
func TestHiddenTabStaysOnlineForFiveMinutes(t *testing.T) {
	c := newCast(t)

	clock := time.Now()
	c.h.api.hub.now = func() time.Time { return clock }

	sock := c.alice.dial(t)
	defer sock.Close()
	waitFor(t, "alice to show up as online", func() bool {
		return c.owner.presence()["alice"] == PresenceOnline
	})

	// She switched to another window. That is not leaving.
	if res := c.alice.setPresence(PresenceHidden); res.status != http.StatusNoContent {
		t.Fatalf("POST /api/presence hidden: %d %s", res.status, res.raw)
	}
	if state := c.owner.presence()["alice"]; state != PresenceOnline {
		t.Fatalf("alice is %q the moment her tab went behind something, want online", state)
	}

	// Four minutes later she is still just somebody in another window.
	clock = clock.Add(4 * time.Minute)
	if state := c.owner.presence()["alice"]; state != PresenceOnline {
		t.Fatalf("alice is %q after four minutes hidden, want online", state)
	}

	// Five, and she is away — counted by this server, because a telephone that
	// froze its page cannot count them.
	clock = clock.Add(2 * time.Minute)
	if state := c.owner.presence()["alice"]; state != PresenceAway {
		t.Fatalf("alice is %q after six minutes hidden, want away", state)
	}

	// Coming back is immediate, and starts the clock over.
	c.alice.setPresence("active")
	if state := c.owner.presence()["alice"]; state != PresenceOnline {
		t.Fatalf("alice is %q after coming back, want online", state)
	}
}

// The second sentence, the same evening: "se a pessoa esta em chamada, as fora
// da aba, eh pra ficar online ainda".
func TestInACallStaysOnlineHoweverLongTheTabIsHidden(t *testing.T) {
	c := newCast(t)

	clock := time.Now()
	c.h.api.hub.now = func() time.Time { return clock }

	sock := c.alice.dial(t)
	defer sock.Close()
	waitFor(t, "alice to show up as online", func() bool {
		return c.owner.presence()["alice"] == PresenceOnline
	})

	// In a call, and the window behind something else.
	if res := c.alice.setPresenceBusy(PresenceHidden, true); res.status != http.StatusNoContent {
		t.Fatalf("POST /api/presence hidden+busy: %d %s", res.status, res.raw)
	}
	clock = clock.Add(2 * time.Hour)
	if state := c.owner.presence()["alice"]; state != PresenceOnline {
		t.Fatalf("alice is %q two hours into a call with the tab hidden, want online", state)
	}

	// The call ends and the window has to answer for itself again — and it
	// does not get a fresh five minutes for having been in a call.
	if res := c.alice.setPresenceBusy(PresenceHidden, false); res.status != http.StatusNoContent {
		t.Fatalf("POST /api/presence hidden: %d %s", res.status, res.raw)
	}
	if state := c.owner.presence()["alice"]; state != PresenceAway {
		t.Fatalf("alice is %q when the call ends after two hours hidden, want away", state)
	}
}

// A call keeps somebody online; it does not make them reachable. A telephone in
// a pocket with a call running is still a telephone that should buzz when
// somebody writes — the two questions are answered by different fields on
// purpose.
func TestACallDoesNotSilenceNotifications(t *testing.T) {
	c := newCast(t)

	sock := c.alice.dial(t)
	defer sock.Close()
	waitFor(t, "alice to show up as online", func() bool {
		return c.owner.presence()["alice"] == PresenceOnline
	})

	alice, _, err := c.h.st.UserByUsername("alice")
	if err != nil {
		t.Fatal(err)
	}

	// Looking at the app: nothing to push.
	c.alice.setPresenceBusy("active", true)
	if !c.h.api.hub.visible(alice.ID) {
		t.Fatal("alice's tab is on the screen and the server thinks it is not")
	}

	// Screen off, call still running. Online to her friends, and still worth
	// telling that a message arrived.
	c.alice.setPresenceBusy(PresenceHidden, true)
	if c.h.api.hub.visible(alice.ID) {
		t.Fatal("alice's screen is off and the server still thinks she is looking at it")
	}
	if state := c.owner.presence()["alice"]; state != PresenceOnline {
		t.Fatalf("alice is %q on a call with her screen off, want online", state)
	}
}

// Two old devices whose push services never answer used to spend the one
// deadline the whole delivery shared, and the third — the phone in the pocket
// — got a context that was already over. Every device gets its own.
func TestTwoSilentDevicesDoNotSilenceTheThird(t *testing.T) {
	c := newPushCast(t)
	c.h.api.notify.sendTimeout = 600 * time.Millisecond
	// Oldest first is the order they are sent in, so the silent ones go first.
	for _, who := range []string{"mudo-1", "mudo-2", "alice-phone"} {
		if res := c.alice.subscribePush(c.service.endpoint(who)); res.status != http.StatusOK {
			t.Fatalf("subscribe %s: %d %s", who, res.status, res.raw)
		}
		time.Sleep(1100 * time.Millisecond) // created_at is in seconds
	}

	dm := c.owner.openDM("alice")
	c.owner.mustSay(dm, "acorda")

	got := c.service.wait(t)
	if !strings.Contains(got.path, "alice-phone") {
		t.Fatalf("the push went to %q", got.path)
	}
}

// A presence announcement that had read "online" could be overtaken by the last
// socket closing, and publish online after offline had gone out: the last
// word everybody heard was wrong, and the sweep never revisits somebody with no
// socket left, so it stayed wrong. The seam pauses one announcement in exactly
// that window.
func TestALateAnnouncementCannotBringSomebodyBack(t *testing.T) {
	c := newCast(t)
	alice, _, err := c.h.st.UserByUsername("alice")
	if err != nil {
		t.Fatal(err)
	}
	watcher := c.owner.dial(t)
	defer watcher.Close()
	sock := c.alice.dial(t)
	waitFor(t, "alice online", func() bool { return c.owner.presence()["alice"] == PresenceOnline })

	paused, release := make(chan struct{}), make(chan struct{})
	// Not a sync.Once: a second caller of Once.Do waits for the first to
	// finish, which would serialise the two announcements exactly as the fix
	// does, and the test would pass with the race still there.
	var armed atomic.Bool
	armed.Store(true)
	p := c.h.api.presence
	p.order.Lock()
	p.afterRead = func(uid int64) {
		if uid == alice.ID && armed.CompareAndSwap(true, false) {
			close(paused)
			<-release
		}
	}
	p.order.Unlock()

	late := make(chan struct{})
	go func() { c.h.api.presenceChanged(alice.ID, "alice"); close(late) }()
	<-paused

	// One reader for the whole test. A read that times out leaves this
	// websocket library's connection broken for every read after it, so the
	// waiting is done on the channel, never on the socket.
	heard := make(chan string, 64)
	go func() {
		defer close(heard)
		for {
			var ev gatewayEvent
			if err := watcher.ReadJSON(&ev); err != nil {
				return
			}
			if data, _ := ev.Data.(map[string]any); ev.Type == "presence" && data["username"] == "alice" {
				state, _ := data["state"].(string)
				heard <- state
			}
		}
	}()
	last := ""
	// listen takes what arrives until `until` says stop or nothing has come
	// for `quiet`.
	listen := func(quiet time.Duration, until func(string) bool) {
		for {
			select {
			case state, ok := <-heard:
				if !ok {
					return
				}
				last = state
				if until(state) {
					return
				}
			case <-time.After(quiet):
				return
			}
		}
	}

	// It read "online". Now her only socket closes. Without the fix, the
	// "offline" goes out now, and the paused announcement is only released
	// once it has — a fixed sleep here used to guess at how long the server
	// takes to notice a closed socket, and a guess too short let the test pass
	// with the race still in place. With the fix the "offline" waits behind
	// the paused one, so nothing arrives and the wait below is only a ceiling.
	sock.Close()
	listen(3*time.Second, func(state string) bool { return state == PresenceOffline })
	close(release)
	<-late
	listen(1500*time.Millisecond, func(string) bool { return false })
	if last != PresenceOffline {
		t.Errorf("the last thing the watcher heard about alice is %q, after her last socket closed", last)
	}
}
