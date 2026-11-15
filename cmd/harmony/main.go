// Command harmony is the server for the private Discord-like service: the HTTP
// API, the invite-only accounts, and the SQLite file they live in.
//
// It is also the maintenance tool for that file. The backup and restore-check
// paths are subcommands of the same binary on purpose: they then use the same
// schema and the same integrity rules as the running service, and a backup
// script cannot drift away from the thing it is backing up.
//
//	harmony                     serve
//	harmony -invite             mint a bootstrap invite and print the link
//	harmony -invite -n 20       mint twenty of them in one go
//	harmony -audit              print the accounts and who invited whom
//	harmony -reset-password U   set a new random password for @U and print it
//	harmony -backup FILE        write a consistent copy of the database
//	harmony -verify FILE        check a copy and print what is inside it
//	harmony -vapid-keygen       print a fresh Web Push key pair for .env.harmony
//	harmony -healthcheck        probe the running server (container health)
package main

import (
	"context"
	"encoding/base64"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"call/internal/harmony/api"
	"call/internal/harmony/mail"
	"call/internal/harmony/passwd"
	"call/internal/harmony/push"
	"call/internal/harmony/store"
)

func main() {
	addr := flag.String("addr", envOr("HARMONY_ADDR", ":8080"), "listen address")
	dbPath := flag.String("db", envOr("HARMONY_DB", "/data/harmony.db"), "path to the SQLite file")
	filesDir := flag.String("files", envOr("HARMONY_FILES", "/data/files"), "directory the uploaded images are written to")
	appDir := flag.String("app", envOr("HARMONY_APP_DIR", "/data/app"), "directory containing published Android APKs")
	diagDir := flag.String("diagnostics", envOr("HARMONY_DIAGNOSTICS_DIR", "/data/diagnostics"), "directory the programs' diagnostic reports are written to")
	invite := flag.Bool("invite", false, "mint a bootstrap invite, print the link, and exit")
	note := flag.String("note", "bootstrap", "note recorded on the invite made by -invite")
	// How many -invite mints. One is what a person wants; the browser tests want
	// a hundred, and asking for them one process at a time cost eighteen seconds
	// of every single run.
	inviteCount := flag.Int("n", 1, "how many invites -invite mints")
	audit := flag.Bool("audit", false, "print the accounts and every invite, with who made it and who used it")
	reset := flag.String("reset-password", "", "give this account a new random password, print it, and end its sessions")
	reset2FA := flag.String("reset-2fa", "", "disable this account's second factor and end its sessions")
	backup := flag.String("backup", "", "write a consistent copy of the database to this path and exit")
	verify := flag.String("verify", "", "check the database copy at this path and exit")
	healthcheck := flag.Bool("healthcheck", false, "probe the running server and exit")
	vapidKeygen := flag.Bool("vapid-keygen", false, "print a fresh Web Push (VAPID) key pair and exit")
	voicePort := flag.Int("voice-port", envInt("HARMONY_VOICE_PORT", 40000), "UDP port the media server listens on (0 disables voice)")
	voiceIP := flag.String("voice-ip", os.Getenv("HARMONY_VOICE_PUBLIC_IP"), "public address to advertise to browsers for voice")
	voiceMax := flag.Int("voice-max", envInt("HARMONY_VOICE_MAX", 8), "how many people fit in one voice channel")
	// The watchman, which runs on the *other* machine (watch.go).
	watch := flag.String("watch", "", "watch this health URL from outside and wake a phone when it stops answering")
	exportWatchFor := flag.String("export-watch", "", "print this account's push endpoints, for the watchman on the other machine")
	flag.Parse()

	log := slog.New(slog.NewTextHandler(os.Stderr, nil))

	if *healthcheck {
		os.Exit(probe(*addr))
	}
	// Before the database is opened: generating keys is something the operator
	// does once, on a machine that may not have a database at all.
	if *vapidKeygen {
		os.Exit(run(log, printVAPIDKeys))
	}
	if *verify != "" {
		os.Exit(run(log, func() error { return verifyDB(*verify) }))
	}
	// Also before the database: the machine that watches this one does not have
	// a copy of it and must never need one.
	if *watch != "" {
		os.Exit(run(log, func() error { return watchServer(log, *watch) }))
	}

	st, err := store.Open(*dbPath)
	if err != nil {
		log.Error("open database", "err", err, "path", *dbPath)
		os.Exit(2)
	}
	defer st.Close()

	switch {
	case *reset2FA != "":
		os.Exit(run(log, func() error {
			u, _, err := st.UserByUsername(strings.TrimPrefix(store.NormalizeUsername(*reset2FA), "@"))
			if err != nil {
				return errors.New("account not found")
			}
			if err := st.ResetTwoFA(u.ID); err != nil {
				return err
			}
			mailer, err := mail.FromEnv()
			if err != nil || mailer == nil {
				log.Warn("mail unavailable", "user_id", u.ID, "purpose", "twofa_reset")
				return nil
			}
			if u.EmailVerified {
				ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
				defer cancel()
				if mailer.Send(ctx, u.Email, "Segurança da sua conta harmony", "O dono desligou a verificação em duas etapas da sua conta. Todos os aparelhos foram desconectados.\nSe não foi você, procure o dono do servidor e troque a senha.") != nil {
					log.Warn("mail failed", "user_id", u.ID, "purpose", "twofa_reset")
				}
			}
			return nil
		}))
	case *invite:
		os.Exit(run(log, func() error { return mintInvite(st, *note, *inviteCount) }))
	case *audit:
		os.Exit(run(log, func() error { return printAudit(st) }))
	case *reset != "":
		os.Exit(run(log, func() error { return resetPassword(st, *reset) }))
	case *backup != "":
		os.Exit(run(log, func() error { return backupTo(st, *backup) }))
	case *exportWatchFor != "":
		os.Exit(run(log, func() error { return exportWatch(st, *exportWatchFor) }))
	}

	baseURL := os.Getenv("HARMONY_BASE_URL")
	if baseURL == "" {
		log.Warn("HARMONY_BASE_URL is empty: invite links will be relative and cross-site writes are not checked")
	}
	mode, err := st.JournalMode()
	if err != nil {
		log.Error("journal mode", "err", err)
		os.Exit(2)
	}
	if err := os.MkdirAll(*filesDir, 0o700); err != nil {
		log.Error("uploads directory", "err", err, "path", *filesDir)
		os.Exit(2)
	}
	users, _ := st.CountUsers()
	log.Info("harmony starting", "db", *dbPath, "journal", mode, "users", users, "base_url", baseURL)

	// The doorbell's identity. Missing keys are not an error: harmony runs
	// exactly as it did in D3, and says so once in the log — a server that
	// refused to start over a notification key would be a server the owner
	// cannot boot after losing an .env line.
	var vapid *push.Keys
	if pub, priv := os.Getenv("HARMONY_VAPID_PUBLIC"), os.Getenv("HARMONY_VAPID_PRIVATE"); pub != "" && priv != "" {
		subject := os.Getenv("HARMONY_VAPID_SUBJECT")
		if subject == "" {
			subject = baseURL
		}
		vapid, err = push.NewKeys(pub, priv, subject)
		if err != nil {
			// This one *is* fatal: keys that are present and wrong mean a
			// doorbell that looks configured and silently never rings, which is
			// the exact failure this feature is prone to.
			log.Error("vapid keys", "err", err)
			os.Exit(2)
		}
		log.Info("web push enabled")
		if where, ok := certificateAuthorities(); !ok {
			// Not fatal: everything else on this server works without it, and a
			// server that refuses to boot over a notification detail is worse.
			// Loud, though — this failed silently for a whole evening, one push
			// at a time, and the only trace was a TLS error per notification.
			log.Error("no certificate authorities in this image: every push will fail with " +
				"\"certificate signed by unknown authority\". The image must carry " +
				where + " (see harmony.Dockerfile)")
		}
	} else {
		log.Warn("HARMONY_VAPID_PUBLIC/PRIVATE are empty: web push notifications are off (harmony -vapid-keygen)")
	}

	var fcm *push.FCM
	if encoded := os.Getenv("HARMONY_FCM_CREDENTIALS"); encoded != "" {
		credentials, decodeErr := base64.StdEncoding.DecodeString(encoded)
		if decodeErr != nil {
			credentials, decodeErr = base64.RawStdEncoding.DecodeString(encoded)
		}
		if decodeErr != nil {
			log.Error("fcm credentials: invalid base64")
			os.Exit(2)
		}
		fcm, err = push.NewFCM(credentials)
		if err != nil {
			// Present but invalid credentials must not silently disable delivery.
			log.Error("fcm credentials", "err", err)
			os.Exit(2)
		}
		log.Info("fcm push enabled", "project_id", fcm.ProjectID())
	} else {
		log.Info("HARMONY_FCM_CREDENTIALS is empty: fcm push disabled")
	}

	// How two browsers on a direct call find a path to each other. STUN is
	// enough for most pairs of people and is what an empty configuration falls
	// back to; TURN is the relay for the pairs it is not enough for — two
	// telephones on mobile networks, typically — and it is off unless both the
	// addresses and the secret are set, because half of it configured is a
	// relay nobody can authenticate to.
	// The default is Google's public STUN because it is the one that works
	// with nothing configured. Set HARMONY_STUN_URLS to your own — coturn
	// answers STUN on the same port it relays on — and nobody's address goes
	// to a third party to learn what it already is.
	ice := api.ICEConfig{STUNURLs: splitList(envOr("HARMONY_STUN_URLS", "stun:stun.l.google.com:19302"))}
	if urls := splitList(os.Getenv("HARMONY_TURN_URLS")); len(urls) > 0 {
		secret := os.Getenv("HARMONY_TURN_SECRET")
		if len(secret) < 16 {
			// Fatal for the same reason a wrong VAPID key is: a TURN that looks
			// configured and cannot authenticate is a call that fails only for
			// the people who most needed it, on somebody else's network, where
			// nobody can see why.
			log.Error("HARMONY_TURN_SECRET (>=16 chars) is required when HARMONY_TURN_URLS is set")
			os.Exit(2)
		}
		ice.TURNURLs, ice.TURNSecret = urls, []byte(secret)
		// "for direct calls" is what this said until 18/09/2026, and the
		// sentence was true when it was written and quietly stopped being. The
		// voice channel was built with no ICE servers at all, and the one line
		// that could have said so said it in a way that read as a description
		// of the relay rather than as a limit of it. It cost a sprint: the
		// owner's Firefox could not join a voice channel, and the log was
		// announcing the fix every time the server started.
		log.Info("turn enabled for direct calls and voice channels", "urls", len(urls))
	} else {
		log.Info("turn off: a direct call between two hard NATs may not connect (HARMONY_TURN_URLS)")
	}

	mailer, err := mail.FromEnv()
	if err != nil {
		log.Error("invalid mail configuration")
		os.Exit(2)
	}
	srv := api.New(api.Config{
		Mailer:         mailer,
		EmailRequired:  os.Getenv("HARMONY_EMAIL_REQUIRED") == "1",
		Store:          st,
		FilesDir:       *filesDir,
		AppDir:         *appDir,
		DiagnosticsDir: *diagDir,
		BaseURL:        baseURL,
		Secure:         os.Getenv("HARMONY_INSECURE_COOKIE") != "1",
		TrustProxy:     os.Getenv("TRUST_PROXY") == "1",
		Logger:         log,
		VAPID:          vapid,
		FCM:            fcm,
		ICE:            ice,
	})

	// The media server. Voice is the one part of harmony that needs a port of
	// its own open on the machine, so it is also the one part that can be
	// switched off without touching anything else: -voice-port 0 and the app
	// runs exactly as it did in D1.
	if *voicePort > 0 {
		if *voiceIP == "" {
			log.Warn("HARMONY_VOICE_PUBLIC_IP is empty: browsers will be offered an address from inside the container and voice will not connect")
		}
		if err := srv.StartVoice(api.VoiceOptions{
			UDPPort: *voicePort, PublicIP: *voiceIP, MaxPerRoom: *voiceMax,
		}); err != nil {
			log.Error("media server", "err", err, "udp", *voicePort)
			os.Exit(2)
		}
		defer srv.StopVoice()
	} else {
		log.Info("voice disabled", "reason", "voice-port is 0")
	}

	if err := startProfiler(log); err != nil {
		log.Error("profiler", "err", err)
		os.Exit(1)
	}

	stopHousekeeping := make(chan struct{})
	go srv.Housekeeping(stopHousekeeping)
	// A tab that has been hidden long enough becomes "ausente" on its own, and
	// this is what tells everybody else's screen. Without it a dot would keep
	// saying "online" on a quiet evening until something unrelated shook it.
	go srv.PresenceSweep(stopHousekeeping)
	go srv.CallSweep(stopHousekeeping)

	httpSrv := &http.Server{
		Addr:              *addr,
		Handler:           srv,
		ReadHeaderTimeout: 10 * time.Second,
		// A login costs argon2 time on purpose; the write timeout has to leave
		// room for it plus a slow phone on mobile data.
		WriteTimeout: 30 * time.Second,
		IdleTimeout:  120 * time.Second,
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	errc := make(chan error, 1)
	go func() {
		if err := httpSrv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			errc <- err
		}
	}()

	select {
	case err := <-errc:
		log.Error("listen", "err", err)
		os.Exit(1)
	case <-ctx.Done():
	}

	close(stopHousekeeping)
	if err := srv.StopVoice(); err != nil {
		log.Error("media server stop", "err", err)
	}
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := httpSrv.Shutdown(shutdownCtx); err != nil {
		log.Error("shutdown", "err", err)
	}
	log.Info("harmony stopped")
}

func run(log *slog.Logger, fn func() error) int {
	if err := fn(); err != nil {
		log.Error("command failed", "err", err)
		return 1
	}
	return 0
}

// certificateAuthorities reports whether this machine has a list of certificate
// authorities, which is what a TLS handshake we *start* needs in order to check
// the other side. Reported rather than dialled: a check that reaches out to a
// third party on every boot would be a worse cure than the disease.
//
// The paths are Go's own list for Linux (crypto/x509/root_linux.go), shortened
// to the two that matter here — Alpine's, which is what the build stage has, and
// Debian's.
func certificateAuthorities() (where string, ok bool) {
	const alpine = "/etc/ssl/certs/ca-certificates.crt"
	for _, path := range []string{alpine, "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem"} {
		if info, err := os.Stat(path); err == nil && info.Size() > 0 {
			return path, true
		}
	}
	if dir, err := os.ReadDir("/etc/ssl/certs"); err == nil && len(dir) > 0 {
		return "/etc/ssl/certs", true
	}
	return alpine, false
}

// printVAPIDKeys mints the Web Push identity of this server. It is run once, by
// hand, and the two lines are pasted into /srv/harmony/.env.harmony — never into
// the repository, which is what the secret scanner checks.
//
// Changing the pair later is not free: every browser subscribed with the old
// public key stops being reachable and has to turn notifications on again. That
// is the whole reason this prints instead of generating on boot.
func printVAPIDKeys() error {
	pub, priv, err := push.Generate()
	if err != nil {
		return err
	}
	fmt.Println("HARMONY_VAPID_PUBLIC=" + pub)
	fmt.Println("HARMONY_VAPID_PRIVATE=" + priv)
	// The subject identifies the operator to the push service (RFC 8292 §2),
	// and it may be an https URL instead of a mailto:. The address of the
	// service itself is the honest answer and puts nobody's e-mail on Google's
	// wire; a mailto: is only worth it if you want them to be able to write to
	// you when something is wrong with the pushes.
	fmt.Println("HARMONY_VAPID_SUBJECT=https://harmony.taleshernandes.com")
	fmt.Println()
	fmt.Println("cole as tres linhas no /srv/harmony/.env.harmony e suba de novo.")
	fmt.Println("a privada e um segredo: nao vai para o git, nao vai para o log.")
	return nil
}

// mintInvite is how the very first account is created: there is no signed-in
// person to ask for one, and there is deliberately no open signup page.
//
// It mints howMany of them, because one caller wants one and the other wants a
// hundred: a person opening the server to a friend, and the browser tests,
// which need one per account they create and were paying a whole process per
// invite to get them.
func mintInvite(st *store.Store, note string, howMany int) error {
	if howMany < 1 {
		return fmt.Errorf("-n has to be at least 1, not %d", howMany)
	}
	base := os.Getenv("HARMONY_BASE_URL")
	for i := 0; i < howMany; i++ {
		code, inv, err := st.CreateInvite(0, note, store.InviteTTL)
		if err != nil {
			return err
		}
		fmt.Printf("invite  %s\n", code)
		fmt.Printf("link    %s/signup?code=%s\n", base, code)
		fmt.Printf("valid   until %s (single use)\n", inv.ExpiresAt.Format(time.RFC1123))
	}
	return nil
}

// printAudit answers the two questions the owner of a private server actually
// asks: who is in it, and who let each of them in. Both come out of the
// database rather than out of a log, because a spent invite is never purged
// while a container log rotates away.
func printAudit(st *store.Store) error {
	users, err := st.ListUsers()
	if err != nil {
		return err
	}
	fmt.Printf("contas (%d)\n", len(users))
	for _, u := range users {
		role := "membro"
		if u.IsAdmin {
			role = "dono"
		}
		if u.Disabled {
			role += ", desativada"
		}
		fmt.Printf("  #%d  @%-18s %-24s %-18s criada %s\n",
			u.ID, u.Username, u.DisplayName, role, stamp(u.CreatedAt))
	}

	invites, err := st.ListAllInvites()
	if err != nil {
		return err
	}
	fmt.Printf("\nconvites (%d)\n", len(invites))
	for _, inv := range invites {
		by := "linha de comando"
		if inv.CreatedByName != "" {
			by = "@" + inv.CreatedByName
		}
		state := "aberto, vale ate " + stamp(inv.ExpiresAt)
		if inv.Used() {
			state = "usado por @" + inv.UsedByName + " em " + stamp(inv.UsedAt)
		} else if inv.ExpiresAt.Before(time.Now()) {
			state = "vencido em " + stamp(inv.ExpiresAt)
		}
		note := inv.Note
		if note == "" {
			note = "(sem anotacao)"
		}
		fmt.Printf("  %s\n", inv.ID)
		fmt.Printf("      criado por %s em %s — %s\n", by, stamp(inv.CreatedAt), note)
		fmt.Printf("      %s\n", state)
	}
	return nil
}

func stamp(t time.Time) string { return t.Format("02/01 15:04 UTC") }

// resetPassword is the owner's maintenance fallback when account recovery
// cannot be completed through the verified email address.
//
// The new password is generated here rather than accepted as an argument: a
// password typed on a command line lands in the shell history, and one chosen
// in a hurry for somebody else is always a bad one. Every session the account
// had ends, because a forgotten password and a stolen one look the same from
// here.
func resetPassword(st *store.Store, username string) error {
	username = store.NormalizeUsername(strings.TrimPrefix(strings.TrimSpace(username), "@"))
	user, _, err := st.UserByUsername(username)
	if err != nil {
		return fmt.Errorf("no account called @%s", username)
	}
	password, err := store.RandomPassword()
	if err != nil {
		return err
	}
	hash, err := passwd.Hash(password, passwd.Default)
	if err != nil {
		return err
	}
	if err := st.SetPasswordHash(user.ID, hash); err != nil {
		return err
	}
	// SetPasswordHash already ends the sessions; say so, so that whoever runs
	// this knows the person has to sign in again everywhere.
	fmt.Printf("conta    @%s (%s)\n", user.Username, user.DisplayName)
	fmt.Printf("senha    %s\n", password)
	fmt.Printf("aviso    todas as sessoes dessa conta foram encerradas; peca para trocar a senha depois de entrar\n")
	return nil
}

// backupTo checks the live database and then copies it. The check comes first
// so a corrupt file is never quietly propagated over a good backup.
func backupTo(st *store.Store, path string) error {
	if err := st.IntegrityCheck(); err != nil {
		return err
	}
	if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
		return err
	}
	if err := st.BackupTo(path); err != nil {
		return err
	}
	info, err := os.Stat(path)
	if err != nil {
		return err
	}
	fmt.Printf("backup  %s (%d bytes)\n", path, info.Size())
	return nil
}

// verifyDB is the half of "the backup was tested" that a machine can do: open
// the restored copy, run SQLite's integrity check, and show that the accounts
// are actually in there. A file that opens but holds nothing is the failure
// this catches.
func verifyDB(path string) error {
	if _, err := os.Stat(path); err != nil {
		return err
	}
	st, err := store.Open(path)
	if err != nil {
		return err
	}
	defer st.Close()
	if err := st.IntegrityCheck(); err != nil {
		return err
	}
	users, err := st.CountUsers()
	if err != nil {
		return err
	}
	fmt.Printf("verify  %s: integrity ok, %d account(s)\n", path, users)
	if users == 0 {
		return errors.New("restored database has no accounts")
	}
	return nil
}

// probe is the container health check: the scratch image has no shell and no
// curl, so the binary asks itself.
func probe(addr string) int {
	host, port, err := net.SplitHostPort(addr)
	if err != nil {
		fmt.Fprintln(os.Stderr, "healthcheck: bad addr:", err)
		return 1
	}
	if host == "" || host == "0.0.0.0" || host == "::" {
		host = "127.0.0.1"
	}
	client := &http.Client{Timeout: 3 * time.Second}
	resp, err := client.Get("http://" + net.JoinHostPort(host, port) + "/healthz")
	if err != nil {
		fmt.Fprintln(os.Stderr, "healthcheck:", err)
		return 1
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		fmt.Fprintln(os.Stderr, "healthcheck: status", resp.StatusCode)
		return 1
	}
	return 0
}

// splitList reads a comma-separated environment value, the way the 1:1 app's
// signalling server reads the same two settings.
func splitList(v string) []string {
	out := []string{}
	for _, part := range strings.Split(v, ",") {
		if part = strings.TrimSpace(part); part != "" {
			out = append(out, part)
		}
	}
	return out
}

func envInt(key string, fallback int) int {
	if v := os.Getenv(key); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return fallback
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
