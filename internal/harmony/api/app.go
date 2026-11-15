package api

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"os"
	"regexp"
	"strconv"
	"sync"
	"time"
)

// Published builds share a directory, but each platform accepts only its own
// names and owns a separate release cache. Uploads under .tmp names are ignored.
// Both metadata and downloads require an active session.
type appPlatform struct {
	name        string
	pattern     *regexp.Regexp
	prefix      string
	contentType string
	extension   string
}

var androidApp = appPlatform{"Android", regexp.MustCompile(`^harmony-([1-9][0-9]{0,8})-([0-9A-Za-z._-]{1,32})\.apk$`), "/api/app/android/", "application/vnd.android.package-archive", ".apk"}
var windowsApp = appPlatform{"Windows", regexp.MustCompile(`^harmony-([1-9][0-9]{0,8})-([0-9A-Za-z._-]{1,32})\.exe$`), "/api/app/windows/", "application/vnd.microsoft.portable-executable", ".exe"}

type appRelease struct {
	VersionCode int    `json:"version_code"`
	VersionName string `json:"version_name"`
	Size        int64  `json:"size"`
	SHA256      string `json:"sha256"`
	URL         string `json:"url"`
}

// The handler owns one bounded cache per server. Holding the lock through the
// hash also coalesces concurrent requests without another dependency.
func (s *Server) appReleaseHandler(platform appPlatform) func(http.ResponseWriter, *http.Request, ctxUser) {
	var mu sync.Mutex
	var cached appRelease
	var cachedName string
	var cachedTime time.Time
	return func(w http.ResponseWriter, r *http.Request, _ ctxUser) {
		w.Header().Set("Cache-Control", "no-store")
		mu.Lock()
		defer mu.Unlock()
		if s.cfg.AppDir == "" {
			fail(w, 404, "no_app")
			return
		}
		root, err := os.OpenRoot(s.cfg.AppDir)
		if err != nil {
			if os.IsNotExist(err) {
				fail(w, 404, "no_app")
			} else {
				fail(w, 500, "internal")
			}
			return
		}
		defer root.Close()
		dir, err := root.Open(".")
		if err != nil {
			fail(w, 500, "internal")
			return
		}
		entries, err := dir.ReadDir(-1)
		dir.Close()
		if err != nil {
			fail(w, 500, "internal")
			return
		}
		name, version := "", ""
		code := 0
		seen := make(map[int]string)
		for _, entry := range entries {
			parts := platform.pattern.FindStringSubmatch(entry.Name())
			if parts == nil || !entry.Type().IsRegular() {
				continue
			}
			n, _ := strconv.Atoi(parts[1])
			if previous, ok := seen[n]; ok {
				s.log.Warn("duplicate "+platform.name+" version", "version", n, "first", previous, "second", entry.Name())
			}
			seen[n] = entry.Name()
			if n > code || (n == code && entry.Name() > name) {
				name, version, code = entry.Name(), parts[2], n
			}
		}
		if name == "" {
			fail(w, 404, "no_app")
			return
		}
		file, err := root.Open(name)
		if err != nil {
			fail(w, 500, "internal")
			return
		}
		defer file.Close()
		info, err := file.Stat()
		if err != nil || !info.Mode().IsRegular() {
			fail(w, 500, "internal")
			return
		}
		if name != cachedName || info.Size() != cached.Size || !info.ModTime().Equal(cachedTime) {
			hash := sha256.New()
			size, err := io.Copy(hash, file)
			if err != nil || size != info.Size() {
				fail(w, 500, "internal")
				return
			}
			after, err := file.Stat()
			if err != nil || after.Size() != info.Size() || !after.ModTime().Equal(info.ModTime()) {
				fail(w, 500, "internal")
				return
			}
			cached = appRelease{code, version, size, hex.EncodeToString(hash.Sum(nil)), platform.prefix + name}
			cachedName, cachedTime = name, info.ModTime()
		}
		writeJSON(w, http.StatusOK, cached)
	}
}

// appDownloadDeadline lets a 115 MB package finish at ~0.5 Mbit/s.
const appDownloadDeadline = 30 * time.Minute

func (s *Server) appDownloadHandler(platform appPlatform) func(http.ResponseWriter, *http.Request, ctxUser) {
	return func(w http.ResponseWriter, r *http.Request, cu ctxUser) {
		name := r.PathValue("name")
		parts := platform.pattern.FindStringSubmatch(name)
		if parts == nil || s.cfg.AppDir == "" {
			fail(w, 404, "not_found")
			return
		}
		// Root also prevents a symlink in the publishing directory from escaping it.
		root, err := os.OpenRoot(s.cfg.AppDir)
		if err != nil {
			fail(w, 404, "not_found")
			return
		}
		defer root.Close()
		file, err := root.Open(name)
		if err != nil {
			fail(w, 404, "not_found")
			return
		}
		defer file.Close()
		info, err := file.Stat()
		if err != nil || !info.Mode().IsRegular() {
			fail(w, 404, "not_found")
			return
		}
		w.Header().Set("Content-Type", platform.contentType)
		w.Header().Set("Content-Disposition", fmt.Sprintf(`attachment; filename="harmony-%s%s"`, parts[2], platform.extension))
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Cache-Control", "private, no-cache")
		// The server-wide 30 s write timeout would cut the ~115 MB native
		// Windows package on any link slower than ~30 Mbit/s, forever, since
		// the installed programs retry the same file. Give the body room.
		_ = http.NewResponseController(w).SetWriteDeadline(time.Now().Add(appDownloadDeadline))
		http.ServeContent(w, r, name, info.ModTime(), file)
		s.log.Info(platform.name+" download", "user", cu.user.Username, "version", parts[1])
	}
}

// newestAppCode reads only the names in the publish directory: the newest code
// for a platform, or 0. Hashing stays in the release handler, which the
// clients call after hearing about a new code.
func (s *Server) newestAppCode(platform appPlatform) int {
	if s.cfg.AppDir == "" {
		return 0
	}
	entries, err := os.ReadDir(s.cfg.AppDir)
	if err != nil {
		return 0
	}
	code := 0
	for _, entry := range entries {
		parts := platform.pattern.FindStringSubmatch(entry.Name())
		if parts == nil || !entry.Type().IsRegular() {
			continue
		}
		if n, _ := strconv.Atoi(parts[1]); n > code {
			code = n
		}
	}
	return code
}

// sweepAppReleaseOnce tells every open native Windows programme, on the
// presence tick, that a newer build was published, so it downloads now
// instead of at its next six-hourly check. The event carries only the code:
// the programme still asks /api/app/windows with its own session and checks
// size and hash as before. The first sweep after a start only records what is
// there, so a restart does not announce an old build.
func (s *Server) sweepAppReleaseOnce() {
	code := int64(s.newestAppCode(windowsApp))
	previous := s.appSeen.Swap(code)
	if previous == 0 || code <= previous {
		return
	}
	s.log.Info("windows app published", "version_code", code)
	s.hub.publishAll("app_release", map[string]any{"platform": "windows", "version_code": code})
}
