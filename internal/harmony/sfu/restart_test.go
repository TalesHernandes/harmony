package sfu

import (
	"errors"
	"strings"
	"testing"

	"github.com/pion/webrtc/v4"
)

func iceCredential(sdp string) string {
	for _, line := range strings.Split(sdp, "\n") {
		if strings.HasPrefix(line, "a=ice-ufrag:") {
			return strings.TrimSpace(line)
		}
	}
	return ""
}

func TestRestartPreservesSessionTransportAndMedia(t *testing.T) {
	p := newParty(t, 8)
	alice := p.join(t, "room", 1, "alice")
	bruno := p.join(t, "room", 2, "bruno")
	stop := make(chan struct{})
	defer close(stop)
	feed(alice.share(SourceCamera, webrtc.MimeTypeVP8, "cam"), stop)
	waitFor(t, "camera packets", func() bool { return bruno.receivedKind("video") > 3 })
	sess := sessionOf(p.s, bruno.id)
	pc := sess.pc
	old := iceCredential(pc.LocalDescription().SDP)
	offer, err := p.s.RestartICE(bruno.id, bruno.user)
	if err != nil {
		t.Fatal(err)
	}
	if !offer.ICERestart || offer.SessionID != bruno.id || iceCredential(offer.SDP) == old {
		t.Fatal("restart did not preserve session and rotate ICE credentials")
	}
	// A response lost in flight can be fetched again without another restart.
	replay, err := p.s.RestartICE(bruno.id, bruno.user)
	if err != nil || replay.SDP != offer.SDP {
		t.Fatalf("restart replay: %v", err)
	}
	if err := bruno.answer(offer); err != nil {
		t.Fatal(err)
	}
	sess.negMu.Lock()
	answer := sess.answerSDP
	sess.negMu.Unlock()
	if err := p.s.Answer(bruno.id, bruno.user, answer); err != nil {
		t.Fatalf("replaying an accepted answer failed: %v", err)
	}
	before := bruno.receivedKind("video")
	waitFor(t, "video after restart", func() bool { return bruno.receivedKind("video") > before+3 })
	if sessionOf(p.s, bruno.id) != sess || sess.pc != pc {
		t.Fatal("restart replaced the session or PeerConnection")
	}
}

func TestRestartReplaysLostOrdinaryOfferBeforeRestarting(t *testing.T) {
	s := testServer(t, 8)
	offer, err := s.Join("room", Joiner{UserID: 1, Username: "alice"})
	if err != nil {
		t.Fatal(err)
	}
	replay, err := s.RestartICE(offer.SessionID, 1)
	if err != nil || replay.SDP != offer.SDP || replay.ICERestart {
		t.Fatalf("ordinary offer was not replayed: %v", err)
	}
	if _, err := s.RestartICE(offer.SessionID, 2); !errors.Is(err, ErrNoSession) {
		t.Fatalf("another user can restart this session: %v", err)
	}
}

func TestRestartGraceIsBoundedAndOldTimersCannotDropRecoveredSessions(t *testing.T) {
	s := testServer(t, 8)
	offer, err := s.Join("room", Joiner{UserID: 1, Username: "alice"})
	if err != nil {
		t.Fatal(err)
	}
	sess := sessionOf(s, offer.SessionID)
	s.holdForRestart(sess)
	at := sess.lostAt.Load()
	if at == 0 || sessionOf(s, sess.id) != sess {
		t.Fatal("outage immediately dropped session")
	}
	s.holdForRestart(sess)
	if sess.lostAt.Load() != at {
		t.Fatal("duplicate outage extended grace")
	}
	sess.lostAt.Store(0) // connected callback
	s.expireOutage(sess, at)
	if sessionOf(s, sess.id) != sess {
		t.Fatal("old timer dropped recovered session")
	}
	s.holdForRestart(sess)
	newAt := sess.lostAt.Load()
	s.expireOutage(sess, at)
	if sessionOf(s, sess.id) != sess {
		t.Fatal("old timer dropped next outage")
	}
	s.expireOutage(sess, newAt)
	if sessionOf(s, sess.id) != nil {
		t.Fatal("missing user retained indefinitely")
	}
}
