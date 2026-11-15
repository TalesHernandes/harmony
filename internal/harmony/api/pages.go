package api

import (
	"bytes"
	"embed"
	"io/fs"
	"net/http"
	"strings"
	"time"

	"call/internal/harmony/store"
)

// assets holds the web client. It is compiled into the binary, so a deploy is
// one file and there is no directory on the machine a request could escape
// into. assets/app is the built Preact bundle (web/harmony, see
// docs/DEPLOY.md); assets/404.html is the one page written by hand, because it
// has to answer before any of the rest exists.
//
//go:embed assets
var assets embed.FS

// appDir is the built client. Everything under it is served at /static/, and
// only that subtree: nothing here is reached by a path the visitor chooses.
const appDir = "assets/app"

var static, _ = fs.Sub(assets, appDir)

// stylesheetPlaceholder is the name the hand-written pages link to. They cannot
// link to the real one: Vite puts a content hash in the file name and that hash
// changes on every build, so a page written by hand would name last build's
// file and get a 404 — which is exactly what /static/harmony.css did, silently,
// from D0 until now.
const stylesheetPlaceholder = "/static/harmony.css"

// stylesheetHref is the real path, found once at startup by asking the embedded
// filesystem what the build actually emitted.
//
// If there is no stylesheet in there the placeholder stays, and the page is as
// unstyled as it was before; a build that emitted no CSS has broken the app
// itself long before it gets to this page, and the test that walks this path
// (TestTheErrorPageStylesheetIsActuallyServed) is what says so out loud.
var stylesheetHref = builtStylesheet()

func builtStylesheet() string {
	// Sorted, so a build that somehow emits two picks the same one every time.
	built, err := fs.Glob(assets, appDir+"/assets/*.css")
	if err != nil || len(built) == 0 {
		return stylesheetPlaceholder
	}
	return "/static/" + strings.TrimPrefix(built[0], appDir+"/")
}

// rootFiles are the built client's files that must answer from the root rather
// than from /static/. The list is closed on purpose: it is not a directory
// handler, so nothing new appears here without somebody writing it down.
var rootFiles = []string{
	"sw.js",
	"manifest.webmanifest",
	"icon-192.png",
	"icon-512.png",
	"icon-maskable-512.png",
}

func (s *Server) registerPages() {
	s.mux.HandleFunc("GET /{$}", s.pageHome)
	s.mux.HandleFunc("GET /login", s.pageLogin)
	s.mux.HandleFunc("GET /signup", s.pageSignup)
	s.mux.Handle("GET /static/", http.StripPrefix("/static/", s.staticHandler()))
	// The PWA's four files have to answer from the root, and only these four.
	//
	// The service worker is the one that is not negotiable: a worker's scope is
	// the directory it was served from, so one served at /static/ could never
	// wake the app for a page at /. The icons and the manifest are here because
	// the manifest names them with absolute paths, and because a phone that adds
	// harmony to its home screen asks for them before it has any JavaScript.
	for _, name := range rootFiles {
		s.mux.HandleFunc("GET /"+name, s.appFile(name))
	}
	// Anything not matched above. Without this, unknown paths fall through to
	// the root pattern's mux entry and answer 200 with the app shell.
	s.mux.HandleFunc("GET /", s.notFound)
}

// shell is the one HTML document the client ships. Which of the three screens
// it shows — sign in, sign up, the app — it decides from the address bar, but
// what a signed-out visitor is *allowed* to reach is decided here, on the
// server, in the three handlers below.
func (s *Server) shell(w http.ResponseWriter, status int) {
	s.page(w, status, "app/index.html")
}

func (s *Server) staticHandler() http.Handler {
	fileServer := http.FileServer(http.FS(static))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// The bundle's file names carry a content hash, so those may be kept
		// for good; anything else compiled in changes with a deploy that does
		// not change its name, so it is revalidated.
		if strings.HasPrefix(r.URL.Path, "assets/") || strings.HasPrefix(r.URL.Path, "/assets/") {
			w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
		} else {
			w.Header().Set("Cache-Control", "no-cache")
		}
		fileServer.ServeHTTP(w, r)
	})
}

// appFile serves one named file of the built client. The name comes from
// rootFiles above and never from the request, so there is no path here for
// anything to escape through.
func (s *Server) appFile(name string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		body, err := assets.ReadFile(appDir + "/" + name)
		if err != nil {
			s.notFound(w, r)
			return
		}
		switch {
		case strings.HasSuffix(name, ".js"):
			w.Header().Set("Content-Type", "text/javascript; charset=utf-8")
		case strings.HasSuffix(name, ".webmanifest"):
			w.Header().Set("Content-Type", "application/manifest+json")
		case strings.HasSuffix(name, ".png"):
			w.Header().Set("Content-Type", "image/png")
		}
		// Revalidated, not cached: these names do not change between deploys,
		// and a service worker frozen at last week's copy is the one bug in this
		// area nobody can debug over the telephone.
		w.Header().Set("Cache-Control", "no-cache")
		_, _ = w.Write(body)
	}
}

func (s *Server) page(w http.ResponseWriter, status int, name string) {
	body, err := assets.ReadFile("assets/" + name)
	if err != nil {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}
	// The built client's own index.html already names the hashed file, so this
	// finds nothing there; it is the hand-written pages that need it.
	body = bytes.ReplaceAll(body, []byte(stylesheetPlaceholder), []byte(stylesheetHref))
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-cache")
	w.WriteHeader(status)
	_, _ = w.Write(body)
}

func (s *Server) notFound(w http.ResponseWriter, _ *http.Request) {
	s.page(w, http.StatusNotFound, "404.html")
}

func (s *Server) pageHome(w http.ResponseWriter, r *http.Request) {
	if _, ok := s.currentUser(r); !ok {
		http.Redirect(w, r, "/login", http.StatusSeeOther)
		return
	}
	s.shell(w, http.StatusOK)
}

func (s *Server) pageLogin(w http.ResponseWriter, r *http.Request) {
	if _, ok := s.currentUser(r); ok {
		http.Redirect(w, r, "/", http.StatusSeeOther)
		return
	}
	s.shell(w, http.StatusOK)
}

// pageSignup is the rule from PLANO-DISCORD.md §5 made literal: without a
// usable invite in the query string there is no signup page to look at, and the
// answer is the same 404 a made-up URL gets. Nothing tells a stranger that
// signup exists at all, or whether a code they guessed once existed.
func (s *Server) pageSignup(w http.ResponseWriter, r *http.Request) {
	code := r.URL.Query().Get("code")
	if code == "" || !s.st.InviteUsable(code) {
		// Cost a guesser the same rate limit budget a POST would.
		_ = s.st.RecordFailure(store.FailSignup, s.clientIP(r))
		s.notFound(w, r)
		return
	}
	s.shell(w, http.StatusOK)
}

// AbsoluteURL builds a link on the public origin.
func (s *Server) AbsoluteURL(path string) string {
	return strings.TrimSuffix(s.cfg.BaseURL, "/") + path
}

// Housekeeping runs the daily purge. main starts it; it is here because it is
// about the same rows the handlers above create.
func (s *Server) Housekeeping(stop <-chan struct{}) {
	t := time.NewTicker(6 * time.Hour)
	defer t.Stop()
	for {
		if err := s.st.PurgeExpired(FailureWindow); err != nil {
			s.log.Error("purge", "err", err)
		}
		// The rows go above, the bytes go here: an image whose message never
		// happened is a file nobody can reach and nobody asked to keep.
		s.sweepFiles()
		select {
		case <-stop:
			return
		case <-t.C:
		}
	}
}
