// The eight the Codex audit of the voice area found on 23/09/2026, one test
// each. Every one of them was inverted — the fix taken out, the test watched
// going red — before it was kept.
//
// Several are races. Where the window is too narrow to hit by running things
// side by side, a seam holds one party inside it: a test that only sometimes
// catches a race is a test that passes with the race still there.
package sfu

import (
	"errors"
	"io"
	"log/slog"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/pion/webrtc/v4"
)

// screenOf finds the publication of one person's screen in a room.
func screenOf(s *SFU, roomID, sessionID string) *publication {
	s.mu.RLock()
	r := s.rooms[roomID]
	s.mu.RUnlock()
	if r == nil {
		return nil
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	for _, p := range r.pubs {
		if p.owner.id == sessionID && p.source == SourceScreen {
			return p
		}
	}
	return nil
}

func sessionOf(s *SFU, id string) *session {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.sessions[id]
}

// 1. Two "who is watching" answers interleaved could reach the browser in the
// wrong order: "switch on" and then a stale "switch off". The first answer here
// is held inside the send, the way a goroutine suspended between working it out
// and sending it would be, while somebody starts watching.
func TestAStaleWatchersAnswerCannotArriveLast(t *testing.T) {
	quiet := slog.New(slog.NewTextHandler(io.Discard, nil))
	s := &SFU{log: quiet}
	owner := &session{id: "dono"}
	r := &room{id: "sala", sfu: s, peers: map[string]*session{"dono": owner}, pubs: map[string]*publication{}}
	owner.room = r
	pub := &publication{id: "tela", owner: owner, source: SourceScreen}
	r.pubs[pub.id] = pub

	hold, held := make(chan struct{}), make(chan struct{})
	var mu sync.Mutex
	var sent []bool
	first := true
	s.cfg.Signal = func(_ int64, _, kind string, data any) {
		mu.Lock()
		wait := first
		first = false
		mu.Unlock()
		if wait {
			close(held)
			<-hold
		}
		mu.Lock()
		sent = append(sent, data.(map[string]any)["screen"].(bool))
		mu.Unlock()
	}

	// Nobody watching, and it is on its way out.
	stale := make(chan struct{})
	go func() { s.watchersChanged(owner, true); close(stale) }()
	<-held

	// Somebody starts watching, and that answer goes out too.
	pub.subs.Add(1)
	fresh := make(chan struct{})
	go func() { s.watchersChanged(owner, false); close(fresh) }()
	// Without the fix the fresh answer goes out now, and the stale one is
	// released only once it has; with the fix the fresh one waits behind the
	// stale one, so nothing arrives and this is only a ceiling. A fixed sleep
	// here passed with the race in place whenever the fresh answer was slower
	// than the sleep.
	for deadline := time.Now().Add(2 * time.Second); time.Now().Before(deadline); {
		mu.Lock()
		n := len(sent)
		mu.Unlock()
		if n > 0 {
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	close(hold)
	<-stale
	<-fresh

	mu.Lock()
	defer mu.Unlock()
	if len(sent) == 0 || !sent[len(sent)-1] {
		t.Fatalf("the last thing the person showing was told is %v, with somebody watching", sent)
	}
}

// 2. The last person leaving decided the room was empty, let go of it, and only
// then took it out of the index. Somebody joining in between moved into a room
// nobody would ever find again. The seam makes the last person leave exactly
// there.
func TestJoiningAsTheLastPersonLeavesKeepsTheRoomFindable(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")

	var once sync.Once
	p.s.joinedRoom = func(string) {
		once.Do(func() {
			if err := p.s.Leave(alice.id, 1); err != nil {
				t.Error(err)
			}
		})
	}
	bruno := p.join(t, "sala", 2, "bruno")
	p.s.joinedRoom = nil

	peers := p.s.Rooms([]string{"sala"})["sala"]
	if len(peers) != 1 || peers[0].SessionID != bruno.id {
		t.Fatalf("bruno joined, and the room says %+v", peers)
	}
	// And the next person to arrive is in the same room as him.
	carla := p.join(t, "sala", 3, "carla")
	if n := len(p.s.Rooms([]string{"sala"})["sala"]); n != 2 {
		t.Fatalf("carla arrived and the room has %d people; she is in a room of her own (%s)", n, carla.id)
	}
}

// 3. Leaving closed the connection but left every subscription counted: the
// person showing a screen went on being told somebody was watching it.
func TestLeavingStopsCountingAsAWatcher(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")
	bruno := p.join(t, "sala", 2, "bruno")
	stop := make(chan struct{})
	defer close(stop)
	feed(alice.share(SourceScreen, webrtc.MimeTypeVP9, "tela"), stop)
	waitFor(t, "alice's screen", func() bool { return screenOf(p.s, "sala", alice.id) != nil })

	bruno.watch(t, "alice")
	pub := screenOf(p.s, "sala", alice.id)
	waitFor(t, "bruno to be counted", func() bool { return pub.subs.Load() == 1 })

	if err := p.s.Leave(bruno.id, 2); err != nil {
		t.Fatal(err)
	}
	if n := pub.subs.Load(); n != 0 {
		t.Fatalf("bruno left and alice's screen still has %d watchers", n)
	}
	pub.outMu.Lock()
	n := len(pub.outs)
	pub.outMu.Unlock()
	if n != 0 {
		t.Fatalf("bruno left and %d forwarders are still carrying her screen to him", n)
	}
}

// 4. Two subscriptions to the same thing at once — the person showing starts
// sharing while this device presses "assistir" — both found nothing there and
// both built a sender. Only one could ever be removed.
func TestTwoSubscriptionsAtOnceAreOne(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")
	bruno := p.join(t, "sala", 2, "bruno")
	stop := make(chan struct{})
	defer close(stop)
	feed(alice.share(SourceScreen, webrtc.MimeTypeVP9, "tela"), stop)
	waitFor(t, "alice's screen", func() bool { return screenOf(p.s, "sala", alice.id) != nil })
	bruno.watch(t, "alice")
	pub := screenOf(p.s, "sala", alice.id)
	sess := sessionOf(p.s, bruno.id)

	for round := 0; round < 20; round++ {
		sess.unsubscribe(pub)
		var wg sync.WaitGroup
		for i := 0; i < 4; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				_ = sess.subscribe(pub)
			}()
		}
		wg.Wait()
		if n := pub.subs.Load(); n != 1 {
			t.Fatalf("round %d: four subscriptions at once counted %d", round, n)
		}
	}
	sess.unsubscribe(pub)
	if n := pub.subs.Load(); n != 0 {
		t.Fatalf("after the last unsubscribe the screen still counts %d", n)
	}
}

// 5. A layer arriving late was handed to everybody who had not moved, whatever
// they had asked for: a telephone that set the small picture as its ceiling got
// the big one the moment it turned up.
func TestALateLayerDoesNotBreakTheCeiling(t *testing.T) {
	f := layered(t, "")
	pub := f.pub
	pub.outs["s1"] = f
	pub.addLayer("baixa", 1)
	f.setCeiling("baixa")

	pub.addLayer("alta", 2)
	if got := f.watching(); got != "baixa" {
		t.Fatalf("the big layer arrived and a subscriber capped at baixa is now on %q", got)
	}
}

// And the other end of the same shortcut: a layer going away sent everybody on
// it to the biggest one left, ceiling or not.
func TestALayerGoingAwayDoesNotBreakTheCeiling(t *testing.T) {
	f := layered(t, "media")
	pub := f.pub
	pub.addLayer("alta", 1)
	pub.addLayer("media", 2)
	pub.addLayer("baixa", 3)
	pub.outs["s1"] = f
	f.setCeiling("media")

	pub.removeLayer("media")
	if got := f.watching(); got != "baixa" {
		t.Fatalf("media went away and a subscriber capped at media was moved to %q", got)
	}
}

// 6. The ceiling changed the list of layers the sender must keep on, but the
// sender was only told when the subscriber's own layer moved. Somebody already
// held down on the small layer who then shrank the window changed nothing that
// was sent — and the person showing went on encoding two big layers for nobody.
func TestANewCeilingReachesTheSender(t *testing.T) {
	f := layered(t, "baixa")
	pub := f.pub
	pub.addLayer("alta", 1)
	pub.addLayer("media", 2)
	pub.addLayer("baixa", 3)
	pub.outs["s1"] = f
	pub.simul, pub.source = true, SourceScreen
	pub.id = "tela"
	sess := pub.owner
	sess.room.pubs = map[string]*publication{pub.id: pub}
	pub.subs.Add(1)

	var mu sync.Mutex
	var last []string
	sess.room.sfu.cfg.Signal = func(_ int64, _, _ string, data any) {
		mu.Lock()
		last = data.(map[string]any)["layers"].([]string)
		mu.Unlock()
	}
	sess.room.sfu.watchersChanged(sess, true)
	mu.Lock()
	before := strings.Join(last, ",")
	mu.Unlock()
	if before != "alta,media,baixa" {
		t.Fatalf("with no ceiling the sender should keep every layer on: %q", before)
	}

	f.setCeiling("baixa")
	mu.Lock()
	after := strings.Join(last, ",")
	mu.Unlock()
	if after != "baixa" {
		t.Fatalf("the only watcher asked for baixa and the sender was last told %q", after)
	}
}

// 7. An offer from the browser that was installed and then refused left the
// connection waiting for an answer that never came. Every later offer, from
// either side, was turned away as "negotiation in flight".
func TestARefusedOfferDoesNotLeaveTheConnectionStuck(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "sala", 1, "alice")

	// An offer that says "screen" and opens no picture slot: the browser's
	// connection as it is, offered again.
	offer, err := alice.pc.CreateOffer(nil)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := p.s.ClientOffer(alice.id, 1, []string{SourceScreen}, offer.SDP); !errors.Is(err, ErrNoNewSlot) {
		t.Fatalf("an offer with no picture in it: %v, want ErrNoNewSlot", err)
	}
	sess := sessionOf(p.s, alice.id)
	if st := sess.pc.SignalingState(); st != webrtc.SignalingStateStable {
		t.Fatalf("after refusing the offer the connection is %s", st)
	}
	// And the server can still reach this device: a camera it asks for gets
	// a slot and an offer, instead of waiting behind the refused one.
	before := len(p.allSDP())
	if err := p.s.Publish(alice.id, 1, []string{SourceMic, SourceCamera}); err != nil {
		t.Fatal(err)
	}
	waitFor(t, "a new offer to reach alice", func() bool { return len(p.allSDP()) > before })
}

// 8. Every SFU started a heartbeat that nothing ever stopped.
func TestClosingTheServerStopsItsHeartbeat(t *testing.T) {
	runtime.GC()
	before := runtime.NumGoroutine()
	for i := 0; i < 5; i++ {
		s, err := New(Config{UDPPort: 0, PublicIP: "127.0.0.1", IncludeLoopback: true, MaxPerRoom: 8, Logger: testLogger()})
		if err != nil {
			t.Fatal(err)
		}
		s.Close()
	}
	var after int
	deadline := time.Now().Add(3 * time.Second)
	for {
		after = runtime.NumGoroutine()
		if after <= before+1 || time.Now().After(deadline) {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if after > before+1 {
		t.Fatalf("five servers opened and closed left %d goroutines behind", after-before)
	}
}
