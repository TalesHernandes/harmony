// The watchman: the one part of harmony that does not run on the harmony
// machine.
//
// Everything else in this repository answers the question "is the service
// working?" from inside the service, which is the one place that cannot answer
// it. A server that has fallen over does not notice, and does not tell anybody;
// the owner finds out when somebody says "tá fora do ar?" in the morning. What
// this adds is a second machine, already paid for and already trusted with the
// nightly backup — the Oracle box — asking the same question from outside, once
// a minute, and waking the owner's telephone when the answer stops being yes.
//
// **Why the alert is a push with nothing in it.** The rule from the D4 has not
// moved: a push carries no payload, and the telephone asks the server what to
// show. Here that turns out to be exactly right instead of merely safe — when
// the alert arrives, the telephone tries to ask harmony what happened, harmony
// does not answer, and the service worker says so in words ("não consegui falar
// com o harmony"). The failure to reach the server *is* the message, and it was
// proved by the telephone itself rather than claimed by a machine in another
// country.
//
// **Two ways to a telephone (D9, 03/10/2026).** It began with Web Push only,
// the browser of 13/09/2026. The owner now lives in the Android app, and in the
// week of 18–25/09 the watchman believed harmony down while its one subscription
// pointed at a browser nobody was looking at. So it also wakes the app through
// Firebase, with the same credentials the server uses: a data message carrying
// only "down" or "up", and the app checks the server itself before saying so.
//
// **What it needs, and what it does not.** It needs the VAPID keys, because a
// push service only accepts a push signed by the same key the subscription was
// made with, and the endpoints to push to. It does **not** need the database,
// the harmony binary's own configuration, a copy of any message, or any way in
// to anything: it can wake a telephone and that is the whole of its power.
package main

import (
	"context"
	"encoding/base64"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	hpush "call/internal/harmony/push"
	"call/internal/harmony/store"
	"call/internal/push"
)

// watchDefaults are the numbers with nobody's opinion in them: often enough to
// be told inside a few minutes, patient enough that one dropped packet on the
// way is not an alarm at three in the morning.
const (
	watchEvery    = time.Minute
	watchFailures = 3
	watchTimeout  = 10 * time.Second
)

// watchServer is the loop. It reports a fall once and a recovery once, and says
// nothing at all in between, because an alert that repeats is an alert people
// switch off.
func watchServer(log *slog.Logger, target string) error {
	endpoints := splitList(os.Getenv("HARMONY_WATCH_ENDPOINTS"))
	tokens := splitList(os.Getenv("HARMONY_WATCH_FCM_TOKENS"))
	if len(endpoints) == 0 && len(tokens) == 0 {
		return errors.New("HARMONY_WATCH_ENDPOINTS and HARMONY_WATCH_FCM_TOKENS are empty: there would be nobody to tell")
	}
	var sender *push.Sender
	if len(endpoints) > 0 {
		keys, err := watchKeys()
		if err != nil {
			return err
		}
		sender = push.NewSender(keys)
	}
	var fcm *hpush.FCM
	if len(tokens) > 0 {
		var err error
		if fcm, err = watchFCM(); err != nil {
			return err
		}
	}
	every := watchEvery
	if s := os.Getenv("HARMONY_WATCH_EVERY"); s != "" {
		d, err := time.ParseDuration(s)
		if err != nil {
			return fmt.Errorf("HARMONY_WATCH_EVERY: %w", err)
		}
		every = d
	}
	client := &http.Client{Timeout: watchTimeout}

	log.Info("watching", "target", target, "every", every, "browsers", len(endpoints), "apps", len(tokens))

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	t := time.NewTicker(every)
	defer t.Stop()

	// down is what has already been reported, so that a service that is still
	// down at the twentieth check does not ring the telephone twenty times.
	down := false
	misses := 0
	for {
		if err := probeOnce(client, target); err != nil {
			misses++
			log.Warn("no answer", "attempt", misses, "err", err)
			if !down && misses >= watchFailures {
				down = true
				log.Error("harmony is down", "target", target, "after", misses)
				endpoints = wakeEverybody(log, sender, endpoints)
				tokens = wakeApps(log, fcm, tokens, "down")
			}
		} else {
			if down {
				log.Info("harmony is back", "target", target)
				endpoints = wakeEverybody(log, sender, endpoints)
				tokens = wakeApps(log, fcm, tokens, "up")
			}
			down, misses = false, 0
		}
		select {
		case <-stop:
			log.Info("watchman stopping")
			return nil
		case <-t.C:
		}
	}
}

// probeOnce is one question. Anything but a 200 is a no, including a 500 from a
// server that is running but has lost its database: "the process exists" is not
// what anybody wanted to know.
func probeOnce(client *http.Client, target string) error {
	req, err := http.NewRequest(http.MethodGet, target, http.NoBody)
	if err != nil {
		return err
	}
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("status %d", resp.StatusCode)
	}
	return nil
}

// wakeEverybody pushes to each telephone and hands back the ones still worth
// keeping. A subscription the push service says is dead is dropped: it belongs
// to a browser that was uninstalled or logged out, and going on trying is how a
// log fills up with the same error for months.
func wakeEverybody(log *slog.Logger, sender *push.Sender, endpoints []string) []string {
	if len(endpoints) == 0 {
		return endpoints
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	alive := endpoints[:0:0]
	for _, endpoint := range endpoints {
		err := sender.Send(ctx, endpoint)
		switch {
		case err == nil:
			alive = append(alive, endpoint)
		case errors.Is(err, push.ErrGone):
			// Never the endpoint itself in a log line: it is the address of
			// somebody's telephone.
			log.Warn("a phone is no longer subscribed; dropping it")
		default:
			log.Error("could not wake a phone", "err", err)
			alive = append(alive, endpoint)
		}
	}
	if len(alive) == 0 {
		log.Error("no browser left to warn: export the subscriptions again (harmony -export-watch)")
	}
	return alive
}

// wakeApps is the same for the Android app. The message says only which way
// the service went; what the app shows is decided after it asks the server
// itself (Core.watchAlert), so a watchman that is wrong about the server — the
// week of 502s in September — is told apart from a server that is down.
func wakeApps(log *slog.Logger, fcm *hpush.FCM, tokens []string, state string) []string {
	if len(tokens) == 0 {
		return tokens
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	at := strconv.FormatInt(time.Now().UnixMilli(), 10)
	alive := tokens[:0:0]
	for _, token := range tokens {
		err := fcm.Send(ctx, hpush.FCMMessage{
			Token: token,
			Data:  map[string]string{"k": "watch", "s": state, "at": at},
			// Hours, not minutes: a phone asleep in a drawer should still hear
			// that the server fell; and "back" replaces "down" (same key).
			TTL:         6 * time.Hour,
			CollapseKey: "watch",
		})
		switch {
		case err == nil:
			alive = append(alive, token)
		case errors.Is(err, hpush.ErrGone):
			log.Warn("an app is no longer registered; dropping it")
		default:
			log.Error("could not wake an app", "err", err)
			alive = append(alive, token)
		}
	}
	if len(alive) == 0 {
		log.Error("no app left to warn: export the tokens again (harmony -export-watch)")
	}
	return alive
}

// watchFCM reads the server's own Firebase credentials, under the same name.
func watchFCM() (*hpush.FCM, error) {
	encoded := os.Getenv("HARMONY_FCM_CREDENTIALS")
	if encoded == "" {
		return nil, errors.New("HARMONY_FCM_CREDENTIALS is needed to wake the Android app")
	}
	credentials, err := base64.StdEncoding.DecodeString(encoded)
	if err != nil {
		credentials, err = base64.RawStdEncoding.DecodeString(encoded)
	}
	if err != nil {
		return nil, errors.New("HARMONY_FCM_CREDENTIALS: invalid base64")
	}
	return hpush.NewFCM(credentials)
}

// watchKeys reads the same VAPID variables the server reads, under the same
// names, so that the file the operator copies across is the file they already
// have. A watchman with the wrong keys is worse than none: it would run for
// months and be refused by the push service every time.
func watchKeys() (*push.Keys, error) {
	pub, priv := os.Getenv("HARMONY_VAPID_PUBLIC"), os.Getenv("HARMONY_VAPID_PRIVATE")
	if pub == "" || priv == "" {
		return nil, errors.New("HARMONY_VAPID_PUBLIC and HARMONY_VAPID_PRIVATE are needed to wake a telephone")
	}
	subject := os.Getenv("HARMONY_VAPID_SUBJECT")
	if subject == "" {
		subject = os.Getenv("HARMONY_BASE_URL")
	}
	return push.NewKeys(pub, priv, subject)
}

// exportWatch prints the push endpoints of one account, for the watchman on the
// other machine to be given.
//
// It prints a secret and says so. An endpoint is not a password — a push
// service will not accept a push to it without a signature from this server's
// VAPID key — but it is the address of somebody's telephone, and it is treated
// the way the coturn secret was treated when it crossed machines in the D4h:
// printed once, moved by hand into an environment variable, and never written
// down anywhere else.
func exportWatch(st *store.Store, username string) error {
	user, _, err := st.UserByUsername(username)
	if err != nil {
		return fmt.Errorf("no such account: %s", username)
	}
	subs, err := st.LivePushSubscriptions(user.ID)
	if err != nil {
		return err
	}
	tokens, err := st.LiveFCMTokens(user.ID)
	if err != nil {
		return err
	}
	if len(subs) == 0 && len(tokens) == 0 {
		return fmt.Errorf("%s has no telephone subscribed: open harmony on the phone and switch notifications on", username)
	}
	list := make([]string, 0, len(subs))
	for _, s := range subs {
		list = append(list, s.Endpoint)
	}
	fmt.Printf("# %d browser(s) and %d app(s) of @%s. Put this in the watchman's environment, nowhere else:\n", len(list), len(tokens), username)
	fmt.Printf("HARMONY_WATCH_ENDPOINTS=%s\n", strings.Join(list, ","))
	fmt.Printf("HARMONY_WATCH_FCM_TOKENS=%s\n", strings.Join(tokens, ","))
	return nil
}
