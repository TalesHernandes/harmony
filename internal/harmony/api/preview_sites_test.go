package api

import (
	"context"
	"io"
	"net/http"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"call/internal/harmony/store"
)

// Each site's oEmbed answer, shaped as the real one measured from the Oracle
// machine on 30/10/2026, and the card made from it.
func TestPreviewSitesUseTheirOembed(t *testing.T) {
	h := newHarness(t)
	var page atomic.Int32
	asked := map[string]string{}
	installPreviewTestClient(t, h.api, func(w http.ResponseWriter, r *http.Request) {
		asked[r.URL.Path] = r.URL.Query().Get("url")
		w.Header().Set("Content-Type", "application/json")
		switch r.URL.Path {
		case "/yt":
			io.WriteString(w, `{"title":"New Augments go kinda CRAZY!","author_name":"Vandiril","thumbnail_url":"https://i.ytimg.com/vi/HPia_aT2e0I/hqdefault.jpg","type":"video"}`)
		case "/tt":
			io.WriteString(w, `{"title":"Scramble up ur name & I’ll try to guess it","author_name":"Scout & Suki","thumbnail_url":"http://preview.test/thumb.png","thumbnail_width":"720","thumbnail_height":"1280"}`)
		case "/x":
			io.WriteString(w, `{"author_name":"US Department of the Interior","html":"<blockquote class=\"twitter-tweet\"><p lang=\"en\" dir=\"ltr\">Sunsets don&#39;t get much better than this one <a href=\"https://t.co/x\">pic.twitter.com/x</a></p>&mdash; US Interior (@Interior) <a href=\"https://twitter.com/Interior/status/463440424141459456\">May 5, 2014</a></blockquote>","provider_name":"X"}`)
		case "/vimeo":
			io.WriteString(w, `{"title":"The New Vimeo Player","author_name":"Vimeo","thumbnail_url":"http://preview.test/thumb.png","thumbnail_width":1280,"thumbnail_height":720}`)
		case "/spotify":
			io.WriteString(w, `{"title":"Never Gonna Give You Up","thumbnail_url":"http://preview.test/thumb.png"}`)
		case "/reddit":
			io.WriteString(w, `{"author_name":"someone","html":"<blockquote class=\"reddit-embed-bq\"><a href=\"https://www.reddit.com/r/golang/comments/abc/x/\">Go 1.26 is out &amp; fast</a><br> by <a href=\"https://www.reddit.com/user/someone/\">u/someone</a> in <a href=\"https://www.reddit.com/r/golang/\">golang</a></blockquote>"}`)
		case "/sc":
			io.WriteString(w, `{"title":"Flickermood by Forss","author_name":"Forss","thumbnail_url":"http://preview.test/thumb.png"}`)
		case "/thumb.png":
			w.Header().Set("Content-Type", "image/png")
			w.Write(previewPNG())
		default:
			page.Add(1)
			http.NotFound(w, r)
		}
	})
	h.api.previews.endpoints = map[string]string{
		"YouTube": "http://preview.test/yt?format=json", "TikTok": "http://preview.test/tt", "X": "http://preview.test/x",
		"Vimeo": "http://preview.test/vimeo", "Spotify": "http://preview.test/spotify", "Reddit": "http://preview.test/reddit",
		"SoundCloud": "http://preview.test/sc",
	}
	ctx := context.Background()
	check := func(raw, path, wantAsk string, want store.Embed) {
		t.Helper()
		e, outcome := h.api.fetchPreview(ctx, raw)
		if outcome != "ok" {
			t.Fatalf("%s: %s", raw, outcome)
		}
		if asked[path] != wantAsk {
			t.Fatalf("%s asked %q, want %q", raw, asked[path], wantAsk)
		}
		got := e
		got.Image = ""
		want.URL = raw
		if got != want {
			t.Fatalf("%s:\n got %+v\nwant %+v", raw, got, want)
		}
		if want.Width > 0 && !strings.HasPrefix(e.Image, "/api/previews/") {
			t.Fatalf("%s: picture not kept: %+v", raw, e)
		}
	}
	// The YouTube picture is i.ytimg.com's; keep the test off the internet.
	files := h.api.cfg.FilesDir
	h.api.cfg.FilesDir = ""
	check("https://youtu.be/HPia_aT2e0I?si=abc&t=3", "/yt", "https://www.youtube.com/watch?v=HPia_aT2e0I",
		store.Embed{Kind: "video", Site: "YouTube", Title: "New Augments go kinda CRAZY!", Description: "Vandiril"})
	check("https://x.com/Interior/status/463440424141459456?s=20", "/x", "https://twitter.com/Interior/status/463440424141459456",
		store.Embed{Kind: "link", Site: "X", Title: "US Department of the Interior (@Interior)", Description: "Sunsets don't get much better than this one pic.twitter.com/x"})
	check("https://old.reddit.com/r/golang/comments/abc/go_126/", "/reddit", "https://www.reddit.com/r/golang/comments/abc/",
		store.Embed{Kind: "link", Site: "Reddit", Title: "Go 1.26 is out & fast", Description: "r/golang"})
	h.api.cfg.FilesDir = files
	check("https://www.tiktok.com/@scout2015/video/6718335390845095173?lang=pt", "/tt", "https://www.tiktok.com/@scout2015/video/6718335390845095173",
		store.Embed{Kind: "video", Site: "TikTok", Title: "Scramble up ur name & I’ll try to guess it", Description: "Scout & Suki", Width: 2, Height: 1})
	check("https://vimeo.com/76979871", "/vimeo", "https://vimeo.com/76979871",
		store.Embed{Kind: "video", Site: "Vimeo", Title: "The New Vimeo Player", Description: "Vimeo", Width: 2, Height: 1})
	check("https://open.spotify.com/intl-pt/track/4cOdK2wGLETKBW3PvgPWq", "/spotify", "https://open.spotify.com/track/4cOdK2wGLETKBW3PvgPWq",
		store.Embed{Kind: "link", Site: "Spotify", Title: "Never Gonna Give You Up", Width: 2, Height: 1})
	check("https://soundcloud.com/forss/flickermood?in=x", "/sc", "https://soundcloud.com/forss/flickermood",
		store.Embed{Kind: "link", Site: "SoundCloud", Title: "Flickermood by Forss", Description: "Forss", Width: 2, Height: 1})
	if page.Load() != 0 {
		t.Fatal("a known site's page was read instead of its oEmbed")
	}
}

// A failed oEmbed is a failure, never the site's page (YouTube's page from a
// datacenter is the generic one); Reddit alone falls back to its page.
func TestPreviewSiteFailures(t *testing.T) {
	h := newHarness(t)
	var pages atomic.Int32
	installPreviewTestClient(t, h.api, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/yt", "/reddit":
			http.Error(w, "nope", http.StatusBadRequest)
		case "/x":
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, `{"author_name":"Someone","html":""}`)
		default:
			pages.Add(1)
			w.Header().Set("Content-Type", "text/html")
			io.WriteString(w, `<head><meta property="og:title" content="A thread"></head>`)
		}
	})
	h.api.previews.endpoints = map[string]string{"YouTube": "http://preview.test/yt", "X": "http://preview.test/x", "Reddit": "http://preview.test/reddit"}
	ctx := context.Background()
	for _, raw := range []string{"https://www.youtube.com/watch?v=HPia_aT2e0I", "https://x.com/a/status/1"} {
		if _, outcome := h.api.fetchPreview(ctx, raw); outcome != "site_error" {
			t.Fatalf("%s: %s", raw, outcome)
		}
	}
	if pages.Load() != 0 {
		t.Fatal("YouTube or X fell back to the page")
	}
	// Reddit's page through the test host stands for the real one.
	h.api.previews.endpoints["Reddit"] = "http://preview.test/reddit"
	site, u, ask := siteFor("https://www.reddit.com/r/golang/comments/abc/x/")
	if site == nil || site.name != "Reddit" || u == nil || ask != "https://www.reddit.com/r/golang/comments/abc/" || !site.fallback {
		t.Fatal("reddit post not recognised", site, ask)
	}
	if _, _, _, handled := h.api.fetchSite(ctx, "https://www.reddit.com/r/golang/comments/abc/x/"); handled {
		t.Fatal("a failed Reddit oEmbed must fall back to the page")
	}
	// The guard still stands in front of a site's endpoint.
	plain := newHarness(t)
	plain.api.previews.endpoints = map[string]string{"Vimeo": "http://127.0.0.1/vimeo"}
	if _, outcome := plain.api.fetchPreview(ctx, "https://vimeo.com/1"); outcome != "blocked" {
		t.Fatalf("site endpoint on loopback: %s", outcome)
	}
}

func TestPreviewSiteRecognition(t *testing.T) {
	for raw, want := range map[string]string{
		"https://www.youtube.com/@channel":                  "",
		"https://youtube.com.evil.test/watch?v=HPia_aT2e0I": "",
		"https://x.com/home":                                "",
		"https://x.com/a/status/12x":                        "",
		"https://twitter.com/a/status/1/photo/1":            "X",
		"https://vm.tiktok.com/ZMabc/":                      "TikTok",
		"https://www.tiktok.com/@a":                         "",
		"https://vimeo.com/channels/staff":                  "",
		"https://open.spotify.com/user/abc":                 "",
		"https://www.reddit.com/r/golang/":                  "",
		"https://soundcloud.com/forss":                      "",
		"https://g1.globo.com/":                             "",
	} {
		site, _, _ := siteFor(raw)
		got := ""
		if site != nil {
			got = site.name
		}
		if got != want {
			t.Fatalf("%s: %q, want %q", raw, got, want)
		}
	}
}

// What YouTube shows the Oracle machine, kept as a success by the first
// version: "- YouTube", its slogan and its logo. Never a card.
func TestPreviewGenericPageRejected(t *testing.T) {
	h := newHarness(t)
	installPreviewTestClient(t, h.api, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		switch r.URL.Path {
		case "/generic":
			io.WriteString(w, `<head><title>- Preview</title><meta name="description" content="Aproveite vídeos e músicas que você ama"></head>`)
		case "/named":
			io.WriteString(w, `<head><meta property="og:site_name" content="Instagram"><meta property="og:title" content="Instagram"></head>`)
		default:
			io.WriteString(w, `<head><title>Preview of a real page</title></head>`)
		}
	})
	ctx := context.Background()
	for _, raw := range []string{"http://preview.test/generic", "http://preview.test/named"} {
		if _, outcome := h.api.fetchPreview(ctx, raw); outcome != "generic" {
			t.Fatalf("%s: %s", raw, outcome)
		}
	}
	if e, outcome := h.api.fetchPreview(ctx, "http://preview.test/real"); outcome != "ok" || e.Title != "Preview of a real page" {
		t.Fatal("a real page was rejected", outcome, e)
	}
}

// Rows written before PreviewVersion are not shown and are fetched again,
// so the "- YouTube" cards disappear after the deploy without anyone
// sending the link again.
func TestPreviewOldVersionIsFetchedAgain(t *testing.T) {
	h := newHarness(t)
	var hits atomic.Int32
	installPreviewTestClient(t, h.api, func(w http.ResponseWriter, r *http.Request) {
		hits.Add(1)
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, `<head><title>Fresh</title></head>`)
	})
	raw := "http://preview.test/old"
	if err := h.st.SavePreview(store.Preview{Embed: store.Embed{URL: raw, Kind: "link", Title: "- YouTube"}, Success: true, FetchedAt: time.Now()}); err != nil {
		t.Fatal(err)
	}
	if _, err := h.st.DB().Exec(`UPDATE previews SET version=1 WHERE url=?`, raw); err != nil {
		t.Fatal(err)
	}
	m := store.Message{Body: "olha " + raw}
	if embeds := previewEmbeds(m, h.api.cachedPreviews([]store.Message{m})); len(embeds) != 0 {
		t.Fatal("an old row was shown", embeds)
	}
	h.api.ensurePreview(1, raw)
	cache, _ := h.st.Previews([]string{raw})
	if hits.Load() != 1 || cache[raw].Stale || cache[raw].Embed.Title != "Fresh" {
		t.Fatal("old row not fetched again", hits.Load(), cache[raw])
	}
}
