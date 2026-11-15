package api

import (
	"crypto/sha256"
	"encoding/hex"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// appHarness is a server with a directory of published builds.
func appHarness(t *testing.T) (*harness, string) {
	t.Helper()
	dir := filepath.Join(t.TempDir(), "app")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	h := newHarnessTweaked(t, func(c *Config) { c.AppDir = dir })
	return h, dir
}

func writeAppFile(t *testing.T, dir, name, body string) string {
	t.Helper()
	if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256([]byte(body))
	return hex.EncodeToString(sum[:])
}

type testAppPlatform struct {
	name, prefix, extension, contentType string
}

var testAppPlatforms = []testAppPlatform{
	{"Android", "/api/app/android/", ".apk", "application/vnd.android.package-archive"},
	{"Windows", "/api/app/windows/", ".exe", "application/vnd.microsoft.portable-executable"},
}

func (platform testAppPlatform) path(value string) string {
	return strings.ReplaceAll(strings.ReplaceAll(value, "/api/app/android", strings.TrimSuffix(platform.prefix, "/")), ".apk", platform.extension)
}

func TestAppNeedsASession(t *testing.T) {
	for _, platform := range testAppPlatforms {
		t.Run(platform.name, func(t *testing.T) {
			appPath := platform.path
			h, dir := appHarness(t)
			writeAppFile(t, dir, appPath("harmony-11-0.9.apk"), "apk")
			anon := h.device("anon")
			for _, p := range []string{appPath("/api/app/android"), appPath("/api/app/android/harmony-11-0.9.apk")} {
				if res := anon.get(p); res.status != http.StatusUnauthorized {
					t.Fatalf("%s without a session: %d", p, res.status)
				}
			}
		})
	}
}

func TestAppNothingPublished(t *testing.T) {
	for _, platform := range testAppPlatforms {
		t.Run(platform.name, func(t *testing.T) {
			appPath := platform.path
			h, dir := appHarness(t)
			a := h.join(h.bootstrapInvite(), "ana", "senha-longa-da-ana-1")
			if res := a.get(appPath("/api/app/android")); res.status != http.StatusNotFound || res.errorCode() != "no_app" {
				t.Fatalf("empty directory: %d %s", res.status, res.raw)
			}
			if err := os.Remove(dir); err != nil {
				t.Fatal(err)
			}
			if res := a.get(appPath("/api/app/android")); res.status != http.StatusNotFound || res.errorCode() != "no_app" {
				t.Fatalf("missing directory: %d %s", res.status, res.raw)
			}
		})
	}
}

func TestAppPicksTheNewestAndIgnoresTheRest(t *testing.T) {
	for _, platform := range testAppPlatforms {
		t.Run(platform.name, func(t *testing.T) {
			appPath := platform.path
			h, dir := appHarness(t)
			writeAppFile(t, dir, appPath("harmony-10-0.8.apk"), "old")
			want := writeAppFile(t, dir, appPath("harmony-11-0.9.apk"), "newest one")
			writeAppFile(t, dir, appPath("harmony-9-0.7.apk"), "older")
			// An upload in progress, and names that are not published builds.
			for _, junk := range []string{appPath("harmony-12-1.0.apk.tmp"), appPath("outro.apk"), appPath("harmony-abc-1.apk"), appPath("harmony-0-1.apk"), appPath("harmony-13-a b.apk")} {
				writeAppFile(t, dir, junk, "junk")
			}
			a := h.join(h.bootstrapInvite(), "ana", "senha-longa-da-ana-1")
			res := a.get(appPath("/api/app/android"))
			if res.status != http.StatusOK {
				t.Fatalf("release: %d %s", res.status, res.raw)
			}
			if res.body["version_code"] != float64(11) || res.body["version_name"] != "0.9" ||
				res.body["size"] != float64(len("newest one")) || res.body["sha256"] != want ||
				res.body["url"] != appPath("/api/app/android/harmony-11-0.9.apk") {
				t.Fatalf("release: %s", res.raw)
			}
			if cc := res.header.Get("Cache-Control"); cc != "no-store" {
				t.Fatalf("cache-control %q", cc)
			}
		})
	}
}

func TestAppHashFollowsTheFile(t *testing.T) {
	for _, platform := range testAppPlatforms {
		t.Run(platform.name, func(t *testing.T) {
			appPath := platform.path
			h, dir := appHarness(t)
			writeAppFile(t, dir, appPath("harmony-11-0.9.apk"), "first")
			a := h.join(h.bootstrapInvite(), "ana", "senha-longa-da-ana-1")
			first := a.get(appPath("/api/app/android")).body["sha256"]
			// Same name, same size, new content and a new time: must be read again.
			want := writeAppFile(t, dir, appPath("harmony-11-0.9.apk"), "again")
			later := time.Now().Add(time.Hour)
			if err := os.Chtimes(filepath.Join(dir, appPath("harmony-11-0.9.apk")), later, later); err != nil {
				t.Fatal(err)
			}
			got := a.get(appPath("/api/app/android")).body["sha256"]
			if got == first || got != want {
				t.Fatalf("sha256 after replacing the file: %v (was %v, want %s)", got, first, want)
			}
		})
	}
}

func TestAppDownload(t *testing.T) {
	for _, platform := range testAppPlatforms {
		t.Run(platform.name, func(t *testing.T) {
			appPath := platform.path
			h, dir := appHarness(t)
			body := "0123456789abcdefghij"
			writeAppFile(t, dir, appPath("harmony-11-0.9.apk"), body)
			writeAppFile(t, dir, appPath("harmony-10-0.8.apk"), "older")
			// A file beside the directory, which no name may reach.
			if err := os.WriteFile(filepath.Join(filepath.Dir(dir), appPath("segredo.apk")), []byte("secret"), 0o644); err != nil {
				t.Fatal(err)
			}
			a := h.join(h.bootstrapInvite(), "ana", "senha-longa-da-ana-1")

			res := a.get(appPath("/api/app/android/harmony-11-0.9.apk"))
			if res.status != http.StatusOK || res.raw != body {
				t.Fatalf("download: %d %q", res.status, res.raw)
			}
			for k, v := range map[string]string{
				"Content-Type":           platform.contentType,
				"Content-Disposition":    appPath(`attachment; filename="harmony-0.9.apk"`),
				"X-Content-Type-Options": "nosniff",
				"Cache-Control":          "private, no-cache",
			} {
				if got := res.header.Get(k); got != v {
					t.Fatalf("%s = %q, want %q", k, got, v)
				}
			}
			// An older version is still there to be had.
			if res := a.get(appPath("/api/app/android/harmony-10-0.8.apk")); res.status != http.StatusOK || res.raw != "older" {
				t.Fatalf("older: %d %q", res.status, res.raw)
			}
			// Resuming: a range is answered with the range.
			part := a.get(appPath("/api/app/android/harmony-11-0.9.apk"), func(r *http.Request) { r.Header.Set("Range", "bytes=0-9") })
			if part.status != http.StatusPartialContent || part.raw != body[:10] {
				t.Fatalf("range: %d %q", part.status, part.raw)
			}
			for _, p := range []string{
				appPath("harmony-12-1.0.apk"),
				appPath("..%2Fsegredo.apk"),
				appPath("%2e%2e%2Fsegredo.apk"),
				appPath("..%5Csegredo.apk"),
				appPath("segredo.apk"),
				appPath("harmony-11-0.9.apk.tmp"),
			} {
				if res := a.get(platform.prefix + p); res.status != http.StatusNotFound || res.raw == "secret" {
					t.Fatalf("%s: %d %q", p, res.status, res.raw)
				}
			}
		})
	}
}

// Nothing here belongs to one person, so "A cannot read B's" becomes: a
// session that was taken away reads nothing.
func TestAppRevokedSessionReadsNothing(t *testing.T) {
	for _, platform := range testAppPlatforms {
		t.Run(platform.name, func(t *testing.T) {
			appPath := platform.path
			h, dir := appHarness(t)
			writeAppFile(t, dir, appPath("harmony-11-0.9.apk"), "apk")
			a := h.join(h.bootstrapInvite(), "ana", "senha-longa-da-ana-1")
			if res := a.get(appPath("/api/app/android")); res.status != http.StatusOK {
				t.Fatalf("before: %d", res.status)
			}
			if res := a.do(http.MethodPost, "/api/logout", map[string]string{}); res.status != http.StatusOK {
				t.Fatalf("logout: %d %s", res.status, res.raw)
			}
			for _, p := range []string{appPath("/api/app/android"), appPath("/api/app/android/harmony-11-0.9.apk")} {
				if res := a.get(p); res.status != http.StatusUnauthorized {
					t.Fatalf("%s after logout: %d", p, res.status)
				}
			}
		})
	}
}

func TestAppPlatformsStaySeparate(t *testing.T) {
	h, dir := appHarness(t)
	apkHash := writeAppFile(t, dir, "harmony-11-0.9.apk", "apk")
	exeHash := writeAppFile(t, dir, "harmony-12-1.0.exe", "exe")
	a := h.join(h.bootstrapInvite(), "ana", "senha-longa-da-ana-1")
	for _, check := range []struct{ path, hash string }{
		{"/api/app/android", apkHash}, {"/api/app/windows", exeHash},
		{"/api/app/android", apkHash}, {"/api/app/windows", exeHash},
	} {
		res := a.get(check.path)
		if res.status != http.StatusOK || res.body["sha256"] != check.hash {
			t.Fatalf("%s: %d %s", check.path, res.status, res.raw)
		}
	}
	for _, path := range []string{"/api/app/windows/harmony-11-0.9.apk", "/api/app/android/harmony-12-1.0.exe"} {
		if res := a.get(path); res.status != http.StatusNotFound {
			t.Fatalf("%s: %d", path, res.status)
		}
	}
	if err := os.Remove(filepath.Join(dir, "harmony-12-1.0.exe")); err != nil {
		t.Fatal(err)
	}
	if res := a.get("/api/app/windows"); res.status != http.StatusNotFound || res.errorCode() != "no_app" {
		t.Fatalf("APK is not a Windows release: %d %s", res.status, res.raw)
	}
	writeAppFile(t, dir, "harmony-12-1.0.exe", "exe")
	if err := os.Remove(filepath.Join(dir, "harmony-11-0.9.apk")); err != nil {
		t.Fatal(err)
	}
	if res := a.get("/api/app/android"); res.status != http.StatusNotFound || res.errorCode() != "no_app" {
		t.Fatalf("EXE is not an Android release: %d %s", res.status, res.raw)
	}
}

// The native Windows package is ~115 MB, far beyond what the server-wide
// write timeout lets a slow link carry; the download must outlive it.
func TestAppDownloadOutlivesWriteTimeout(t *testing.T) {
	h, dir := appHarness(t)
	body := strings.Repeat("x", 32<<20)
	writeAppFile(t, dir, "harmony-11-native.exe", body)
	a := h.join(h.bootstrapInvite(), "ana", "senha-longa-da-ana-1")

	slow := httptest.NewUnstartedServer(h.api)
	slow.Config.WriteTimeout = 300 * time.Millisecond
	slow.Start()
	t.Cleanup(slow.Close)

	base, _ := url.Parse(h.srv.URL)
	req, _ := http.NewRequest("GET", slow.URL+"/api/app/windows/harmony-11-native.exe", nil)
	for _, c := range a.c.Jar.Cookies(base) {
		req.AddCookie(c)
	}
	req.Header.Set("User-Agent", a.ua)
	res, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		t.Fatalf("status %d", res.StatusCode)
	}
	// Read slowly enough that the whole body takes several write timeouts.
	got, buf := 0, make([]byte, 1<<20)
	for {
		n, err := io.ReadFull(res.Body, buf)
		got += n
		if err != nil {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if got != len(body) {
		t.Fatalf("read %d of %d bytes", got, len(body))
	}
}

// A build published while programmes are open is announced to every socket
// once, with only its code; a restart does not announce what was already there.
func TestAppReleaseIsAnnouncedOnce(t *testing.T) {
	h, dir := appHarness(t)
	writeAppFile(t, dir, "harmony-9-native.exe", "nine")
	a := &gatewayConn{userID: 1, sessID: "a", send: make(chan []byte, 4)}
	b := &gatewayConn{userID: 2, sessID: "b", send: make(chan []byte, 4)}
	h.api.hub.add(a)
	h.api.hub.add(b)
	h.api.sweepAppReleaseOnce() // the start: records 9, says nothing
	writeAppFile(t, dir, "harmony-10-native.apk", "android is not windows")
	h.api.sweepAppReleaseOnce()
	if len(a.send) != 0 || len(b.send) != 0 {
		t.Fatal("announced a build that was already there, or an Android one")
	}
	writeAppFile(t, dir, "harmony-10-native.exe", "ten")
	h.api.sweepAppReleaseOnce()
	h.api.sweepAppReleaseOnce()
	for _, c := range []*gatewayConn{a, b} {
		if len(c.send) != 1 {
			t.Fatalf("socket %s got %d announcements, want 1", c.sessID, len(c.send))
		}
		got := string(<-c.send)
		if got != `{"type":"app_release","data":{"platform":"windows","version_code":10}}` {
			t.Fatalf("announcement %s", got)
		}
	}
}
