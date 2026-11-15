package api

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"image"
	"image/png"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"net/url"
	"reflect"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
	"unicode/utf8"

	"call/internal/harmony/store"
)

func TestPreviewExtraction(t *testing.T) {
	for _, tc := range []struct {
		name, body string
		want       []string
	}{
		{"suppressed", "<https://hidden.test> https://shown.test", []string{"https://shown.test"}},
		{"code", "`https://a.test` ``https://b.test`` ```go\nhttps://c.test\n``` https://d.test", []string{"https://d.test"}},
		{"unclosed code", "`https://a.test", nil},
		{"punctuation", "https://a.test.,;:!? (https://b.test/a_(b)). https://c.test/x))", []string{"https://a.test", "https://b.test/a_(b)", "https://c.test/x"}},
		{"dedup cap", "https://a.test https://a.test https://b.test https://c.test https://d.test", []string{"https://a.test", "https://b.test", "https://c.test"}},
		{"invalid", "ftp://a.test https://u:p@a.test https://a.test:8080 https://good.test:443/x", []string{"https://good.test:443/x"}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if got := previewURLs(tc.body); !reflect.DeepEqual(got, tc.want) {
				t.Fatalf("got %q want %q", got, tc.want)
			}
		})
	}
}

func TestPreviewSSRF(t *testing.T) {
	for _, raw := range []string{"127.0.0.1", "::1", "10.0.0.1", "172.16.0.1", "192.168.1.1", "169.254.169.254", "100.64.1.2", "::ffff:127.0.0.1", "fc00::1", "fe80::1", "0.0.0.0", "0.1.2.3", "224.0.0.1", "255.255.255.255", "::", "ff02::1", "64:ff9b::7f00:1"} {
		t.Run(raw, func(t *testing.T) {
			if publicPreviewIP(netip.MustParseAddr(raw)) {
				t.Fatal("non-public IP allowed")
			}
		})
	}
	if !publicPreviewIP(netip.MustParseAddr("8.8.8.8")) || !publicPreviewIP(netip.MustParseAddr("2606:4700:4700::1111")) {
		t.Fatal("public blocked")
	}
	for _, raw := range []string{"http://example.com:8080", "https://u:p@example.com", "file:///tmp/a", "https://example.com:0"} {
		if _, err := previewURL(raw); err == nil {
			t.Fatalf("allowed %s", raw)
		}
	}
	var reached atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { reached.Add(1) }))
	defer srv.Close()
	c := previewClient(nil)
	defer c.CloseIdleConnections()
	if res, err := c.Get(srv.URL); err == nil {
		res.Body.Close()
		t.Fatal("loopback httptest accepted")
	}
	if reached.Load() != 0 {
		t.Fatal("guard ran after connect")
	}
}

// Keep public URL validation intact. Only the test host is routed to httptest;
// its actual socket destination is explicitly allowed by the injected guard.
func localPreviewClient(srv *httptest.Server) *http.Client {
	addr := srv.Listener.Addr().String()
	c := previewClient(func(actual string) error {
		if actual == addr {
			return nil
		}
		h, p, e := net.SplitHostPort(actual)
		ip, _ := netip.ParseAddr(h)
		if e != nil || (p != "80" && p != "443") || !publicPreviewIP(ip) {
			return errPreviewBlocked
		}
		return nil
	})
	tr := c.Transport.(*http.Transport)
	dial := tr.DialContext
	tr.DialContext = func(ctx context.Context, network, address string) (net.Conn, error) {
		if address == "preview.test:80" {
			address = addr
		}
		return dial(ctx, network, address)
	}
	return c
}

func TestPreviewRedirectsAndProxy(t *testing.T) {
	var hits atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hits.Add(1)
		if r.URL.Path == "/blocked" {
			http.Redirect(w, r, "http://127.0.0.1:80/private", 302)
		} else if r.URL.Path == "/port" {
			http.Redirect(w, r, "http://example.com:8080/", 302)
		} else {
			http.Redirect(w, r, "http://preview.test/loop", 302)
		}
	}))
	defer srv.Close()
	t.Setenv("HTTP_PROXY", srv.URL)
	t.Setenv("HTTPS_PROXY", srv.URL)
	c := localPreviewClient(srv)
	defer c.CloseIdleConnections()
	if c.Transport.(*http.Transport).Proxy != nil {
		t.Fatal("environment proxy enabled")
	}
	for _, path := range []string{"/blocked", "/port", "/loop"} {
		before := hits.Load()
		if res, err := previewGet(context.Background(), c, "http://preview.test"+path); err == nil {
			res.Body.Close()
			t.Fatal("redirect accepted")
		}
		want := int32(1)
		if path == "/loop" {
			want = 4
		}
		if hits.Load()-before != want {
			t.Fatal("wrong redirect count")
		}
	}
}

func TestPreviewMetadata(t *testing.T) {
	base, _ := url.Parse("https://news.test/path/page")
	for _, tc := range []struct{ name, html, title, description, site, img, kind string }{
		{"og", `<head><title>Fallback</title><meta property="og:title" content="A &amp; &lt;b&gt;B&lt;/b&gt;"><meta name="twitter:title" content="Twitter"><meta property="og:description" content="One&#10; two"><meta property="og:site_name" content="News"><meta property="og:image" content="../photo.png"><meta property="og:type" content="video.movie"></head>`, "A & B", "One two", "News", "https://news.test/photo.png", "video"},
		{"twitter", `<head><title>Fallback</title><meta name="twitter:title" content="Twitter"><meta name="twitter:description" content="Description"><meta name="twitter:image" content="/img.jpg"></head>`, "Twitter", "Description", "", "https://news.test/img.jpg", "link"},
		{"fallback", `<head><title>Title &amp; news</title><meta name="description" content="Plain &lt;i&gt;story&lt;/i&gt;"></head><meta property="og:title" content="Wrong">`, "Title & news", "Plain story", "", "", "link"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			e, img, err := parsePreview(strings.NewReader(tc.html), base, "https://news.test/path/page")
			if err != nil || e.Title != tc.title || e.Description != tc.description || e.Site != tc.site || img != tc.img || e.Kind != tc.kind {
				t.Fatalf("%+v image %s err %v", e, img, err)
			}
		})
	}
	long := strings.Repeat("ç", 400)
	e, _, _ := parsePreview(strings.NewReader(`<head><meta property="og:title" content="`+long+`"><meta property="og:description" content="`+long+`"><meta property="og:site_name" content="`+long+`"></head>`), base, base.String())
	if utf8.RuneCountInString(e.Title) != 256 || utf8.RuneCountInString(e.Description) != 350 || utf8.RuneCountInString(e.Site) != 64 || !strings.HasSuffix(e.Title, "…") {
		t.Fatalf("truncation: %+v", e)
	}
}

func TestPreviewYouTube(t *testing.T) {
	for _, raw := range []string{"https://youtube.com/watch?v=jNQXAC9IVRw", "https://www.youtube.com/watch?v=jNQXAC9IVRw&x=1", "https://m.youtube.com/shorts/jNQXAC9IVRw", "https://music.youtube.com/watch?v=jNQXAC9IVRw", "https://youtu.be/jNQXAC9IVRw?t=1", "https://youtube.com/live/jNQXAC9IVRw"} {
		u, _ := url.Parse(raw)
		e, img, _ := parsePreview(strings.NewReader("<head><title>Me at the zoo</title></head>"), u, raw)
		if e.Kind != "video" || e.Site != "YouTube" || img != "https://i.ytimg.com/vi/jNQXAC9IVRw/hqdefault.jpg" {
			t.Fatalf("%s: %+v %s", raw, e, img)
		}
	}
	u, _ := url.Parse("https://youtube.com.evil.test/watch?v=jNQXAC9IVRw")
	if _, ok := youtubeID(u); ok {
		t.Fatal("suffix spoof")
	}
	u, _ = url.Parse("https://youtube.com/watch?v=invalid")
	if id, _ := youtubeID(u); id != "" {
		t.Fatal("invalid id")
	}
	consent, _ := url.Parse("https://consent.youtube.com/")
	e, img, _ := parsePreview(strings.NewReader("<head><title>Video</title></head>"), consent, "https://youtu.be/jNQXAC9IVRw")
	if e.Kind != "video" || e.Site != "YouTube" || img != "https://i.ytimg.com/vi/jNQXAC9IVRw/hqdefault.jpg" {
		t.Fatal("redirect lost YouTube identity", e, img)
	}
}

func previewPNG() []byte {
	var b bytes.Buffer
	_ = png.Encode(&b, image.NewRGBA(image.Rect(0, 0, 2, 1)))
	return b.Bytes()
}

func installPreviewTestClient(t *testing.T, s *Server, handler http.HandlerFunc) *httptest.Server {
	t.Helper()
	srv := httptest.NewServer(handler)
	s.previews.client.CloseIdleConnections()
	s.previews.client = localPreviewClient(srv)
	t.Cleanup(func() { s.Close(); srv.Close() })
	return srv
}

func TestPreviewFetchLimits(t *testing.T) {
	h := newHarness(t)
	installPreviewTestClient(t, h.api, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/huge-image":
			w.Write(append(previewPNG(), make([]byte, previewImageLimit)...))
		case "/not-image":
			w.Write([]byte("not an image"))
		case "/image":
			w.Header().Set("Content-Type", "text/plain")
			w.Write(previewPNG())
		case "/not-html":
			w.Header().Set("Content-Type", "text/plain")
			io.WriteString(w, "<title>Bad</title>")
		case "/large":
			w.Header().Set("Content-Type", "text/html")
			io.WriteString(w, strings.Repeat(" ", previewHTMLLimit)+"<title>Too late</title>")
		default:
			w.Header().Set("Content-Type", "text/html")
			io.WriteString(w, `<head><title>News</title><meta property="og:image" content="/image"></head>`)
		}
	})
	for _, path := range []string{"/huge-image", "/not-image"} {
		if _, _, _, err := h.api.fetchPreviewImage(context.Background(), "http://preview.test"+path); err == nil {
			t.Fatal("image accepted", path)
		}
	}
	for _, path := range []string{"/large", "/not-html"} {
		e, out := h.api.fetchPreview(context.Background(), "http://preview.test"+path)
		if out == "ok" || e.Title != "" {
			t.Fatal("body limit/type failed", e, out)
		}
	}
	e, out := h.api.fetchPreview(context.Background(), "http://preview.test/news")
	if out != "ok" || e.Width != 2 || e.Height != 1 || e.Image == "" {
		t.Fatalf("image sniff/decode: %+v %s", e, out)
	}
	h.api.cfg.FilesDir = ""
	e, out = h.api.fetchPreview(context.Background(), "http://preview.test/news")
	if out != "ok" || e.Title != "News" || e.Image != "" || e.Width != 0 {
		t.Fatalf("empty FilesDir: %+v", e)
	}
	// Parsing must stop on the closing head even when the body never arrives.
	reader := &previewCountReader{r: strings.NewReader("<head><title>News</title></head>" + strings.Repeat("x", 2*previewHTMLLimit))}
	u, _ := url.Parse("http://preview.test")
	parsePreview(reader, u, u.String())
	if reader.n >= previewHTMLLimit {
		t.Fatal("read past head")
	}
}

type previewCountReader struct {
	r io.Reader
	n int
}

func (r *previewCountReader) Read(b []byte) (int, error) { n, e := r.r.Read(b); r.n += n; return n, e }

func TestPreviewRateCacheAndSingleflight(t *testing.T) {
	h := newHarness(t)
	var hits atomic.Int32
	installPreviewTestClient(t, h.api, func(w http.ResponseWriter, r *http.Request) {
		hits.Add(1)
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, "<head><title>News</title></head>")
	})
	var wg sync.WaitGroup
	for i := 0; i < 10; i++ {
		wg.Go(func() { h.api.ensurePreview(1, "http://preview.test/shared") })
	}
	wg.Wait()
	if hits.Load() != 1 {
		t.Fatalf("duplicate fetches: %d", hits.Load())
	}
	for i := 0; i < 22; i++ {
		h.api.ensurePreview(1, fmt.Sprintf("http://preview.test/%d", i))
	}
	if hits.Load() != 20 {
		t.Fatalf("rate count: %d", hits.Load())
	}
	h.api.ensurePreview(1, "http://preview.test/shared")
	if hits.Load() != 20 {
		t.Fatal("cache spent rate")
	}
	h.api.ensurePreview(2, "http://preview.test/other")
	if hits.Load() != 21 {
		t.Fatal("rate not per user")
	}
	p := h.api.previews
	p.mu.Lock()
	if !p.allow(1, time.Now().Add(time.Minute)) {
		t.Fatal("window did not reset")
	}
	p.mu.Unlock()
}

func TestPreviewFailureCacheAndRefresh(t *testing.T) {
	h := newHarness(t)
	var hits atomic.Int32
	installPreviewTestClient(t, h.api, func(w http.ResponseWriter, r *http.Request) {
		hits.Add(1)
		w.Header().Set("Content-Type", "text/plain")
		io.WriteString(w, "not HTML")
	})
	raw := "http://preview.test/failure"
	h.api.ensurePreview(1, raw)
	h.api.ensurePreview(2, raw)
	if hits.Load() != 1 {
		t.Fatal("failure was not cached")
	}
	cache, err := h.st.Previews([]string{raw})
	if err != nil || cache[raw].Success {
		t.Fatal("failure cache", err)
	}
	p := cache[raw]
	p.FetchedAt = time.Now().Add(-time.Hour)
	if err := h.st.SavePreview(p); err != nil {
		t.Fatal(err)
	}
	h.api.ensurePreview(1, raw)
	if hits.Load() != 2 {
		t.Fatal("expired failure was not retried")
	}
	p.Success = true
	p.Embed.Title = "Cached"
	p.FetchedAt = time.Now().Add(-7 * 24 * time.Hour)
	if err := h.st.SavePreview(p); err != nil {
		t.Fatal(err)
	}
	h.api.ensurePreview(1, raw)
	if hits.Load() != 3 {
		t.Fatal("expired success was not retried")
	}
}

func TestPreviewSharedPageAndImageDeadline(t *testing.T) {
	h := newHarness(t)
	installPreviewTestClient(t, h.api, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/image" {
			<-r.Context().Done()
			return
		}
		if r.Header.Get("User-Agent") != previewUA || r.Header.Get("Accept-Language") != "pt-BR,pt;q=0.9,en;q=0.8" {
			t.Error("missing honest request headers")
		}
		w.Header().Set("Content-Type", "application/xhtml+xml")
		io.WriteString(w, `<head><title>News</title><meta property="og:image" content="/image"></head>`)
	})
	if h.api.previews.client.Timeout != 8*time.Second {
		t.Fatal("wrong timeout")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()
	start := time.Now()
	e, _ := h.api.fetchPreview(ctx, "http://preview.test/page")
	if ctx.Err() == nil || e.Image != "" || time.Since(start) > time.Second {
		t.Fatal("image did not share page deadline")
	}
}

func TestPreviewConcurrencyAndClose(t *testing.T) {
	h := newHarness(t)
	var active, max atomic.Int32
	entered := make(chan struct{}, 8)
	installPreviewTestClient(t, h.api, func(w http.ResponseWriter, r *http.Request) {
		n := active.Add(1)
		defer active.Add(-1)
		for {
			old := max.Load()
			if n <= old || max.CompareAndSwap(old, n) {
				break
			}
		}
		entered <- struct{}{}
		<-r.Context().Done()
	})
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		i := i
		wg.Go(func() { h.api.ensurePreview(1, fmt.Sprintf("http://preview.test/slow%d", i)) })
	}
	for i := 0; i < 4; i++ {
		select {
		case <-entered:
		case <-time.After(3 * time.Second):
			t.Fatal("workers stalled")
		}
	}
	select {
	case <-entered:
		t.Fatal("more than four fetches")
	case <-time.After(50 * time.Millisecond):
	}
	start := time.Now()
	h.api.Close()
	wg.Wait()
	if time.Since(start) > time.Second || max.Load() != 4 {
		t.Fatal("cancellation/concurrency failed")
	}
}

func TestPreviewEndToEnd(t *testing.T) {
	c := newCast(t)
	h := c.h
	gate := make(chan struct{})
	var once sync.Once
	t.Cleanup(func() { once.Do(func() { close(gate) }) })
	installPreviewTestClient(t, h.api, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/image" {
			w.Write(previewPNG())
			return
		}
		select {
		case <-gate:
		case <-r.Context().Done():
			return
		}
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, `<head><meta property="og:site_name" content="News"><meta property="og:title" content="Title &amp; news"><meta property="og:description" content="Story"><meta property="og:image" content="/image"></head>`)
	})
	dm := c.owner.openDM("alice")
	c.owner.mustSay(dm, "Earlier message")
	wa := c.alice.dial(t)
	defer wa.Close()
	waitEvent(t, wa, "presence")
	wb := c.bo.dial(t)
	defer wb.Close()
	waitEvent(t, wb, "presence")
	m := c.owner.mustSay(dm, "http://preview.test/news")
	id := int64(m["id"].(float64))
	if _, ok := m["embeds"]; ok {
		t.Fatal("post waited for preview")
	}
	once.Do(func() { close(gate) })
	ev := waitEvent(t, wa, "message_changed")
	data := eventData(t, ev)
	embeds, ok := data["embeds"].([]any)
	if !ok || len(embeds) != 1 {
		t.Fatalf("event: %+v", data)
	}
	e := embeds[0].(map[string]any)
	if e["url"] != "http://preview.test/news" || e["title"] != "Title & news" {
		t.Fatal(e)
	}
	t.Logf("news embed JSON: %s", mustPreviewJSON(e))
	path := e["image"].(string)
	res := c.alice.get(path)
	if res.status != 200 || res.header.Get("Content-Type") != "image/png" || res.header.Get("X-Content-Type-Options") != "nosniff" || res.header.Get("Cache-Control") != "private, max-age=86400" || res.header.Get("Content-Disposition") != "inline" || res.raw != string(previewPNG()) {
		t.Fatalf("image: %+v", res)
	}
	if r := c.bo.get(path); r.status != 404 {
		t.Fatal("outsider read image", r.status)
	}
	if r := h.device("anonymous").get(path); r.status != 401 {
		t.Fatal("anonymous read image", r.status)
	}
	if r := c.alice.get("/api/previews/" + strings.Repeat("a", 64)); r.status != 404 {
		t.Fatal("unknown image", r.status)
	}
	if r := c.bo.get("/api/channels/" + dm + "/messages"); r.status != 404 {
		t.Fatal("outsider read messages", r.status)
	}
	for _, query := range []string{"", "?around=" + fmt.Sprint(id), "?after=" + fmt.Sprint(id-1)} {
		if id == 1 && strings.Contains(query, "after") {
			continue
		}
		r := c.alice.get("/api/channels/" + dm + "/messages" + query)
		if r.status != 200 || !strings.Contains(r.raw, `"embeds"`) {
			t.Fatalf("history %s: %s", query, r.raw)
		}
	}
	if r := c.alice.get("/api/search?q=preview.test"); r.status != 200 || !strings.Contains(r.raw, `"embeds"`) {
		t.Fatal("search omitted cached embeds", r.raw)
	}
	// A socket outsider receives presence noise, but never the private message.
	wb.SetReadDeadline(time.Now().Add(150 * time.Millisecond))
	for {
		var event gatewayEvent
		if err := wb.ReadJSON(&event); err != nil {
			break
		}
		if event.Type == "message" || event.Type == "message_changed" {
			t.Fatal("outsider received private event")
		}
	}
	r := c.owner.do(http.MethodPatch, fmt.Sprintf("/api/messages/%d", id), map[string]string{"body": "http://preview.test/edited"})
	if r.status != 200 {
		t.Fatal(r.raw)
	}
	for {
		ev = waitEvent(t, wa, "message_changed")
		data = eventData(t, ev)
		if es, ok := data["embeds"].([]any); ok && es[0].(map[string]any)["url"] == "http://preview.test/edited" {
			break
		}
	}
	r = c.owner.do(http.MethodDelete, fmt.Sprintf("/api/messages/%d", id), nil)
	if r.status != 200 || r.body["embeds"] != nil {
		t.Fatal("deleted embeds", r.raw)
	}
	if strings.Contains(c.alice.get("/api/channels/"+dm+"/messages").raw, `"embeds"`) {
		t.Fatal("deleted history embeds")
	}
	if c.alice.get(path).status != 404 {
		t.Fatal("deleted message still authorizes image")
	}
	if strings.Contains(h.logs.String(), "preview.test") || strings.Contains(h.logs.String(), "Title & news") {
		t.Fatal("preview content leaked to logs")
	}
}

func mustPreviewJSON(v any) string { b, _ := json.Marshal(v); return string(b) }

func TestPreviewWireExamplesAndOmission(t *testing.T) {
	h := newHarness(t)
	installPreviewTestClient(t, h.api, func(w http.ResponseWriter, r *http.Request) { w.Write(previewPNG()) })
	raw := "https://www.youtube.com/watch?v=jNQXAC9IVRw"
	base, _ := url.Parse(raw)
	e, img, _ := parsePreview(strings.NewReader(`<head><title>Me at the zoo</title><meta name="description" content="The first video on YouTube"></head>`), base, raw)
	if img != "https://i.ytimg.com/vi/jNQXAC9IVRw/hqdefault.jpg" {
		t.Fatal(img)
	}
	id, w, hh, err := h.api.fetchPreviewImage(context.Background(), "http://preview.test/image")
	if err != nil {
		t.Fatal(err)
	}
	e.Image = "/api/previews/" + id
	e.Width = w
	e.Height = hh
	t.Logf("YouTube embed JSON: %s", mustPreviewJSON(e))
	cache := map[string]store.Preview{raw: {Embed: e, Success: true, FetchedAt: time.Now()}}
	m := store.Message{Body: raw}
	if len(h.api.messageJSON(m, cache)["embeds"].([]store.Embed)) != 1 {
		t.Fatal("missing embed")
	}
	for _, m := range []store.Message{{Body: "plain"}, {Body: raw, Kind: "call"}, {Body: raw, DeletedAt: time.Now()}} {
		if _, ok := h.api.messageJSON(m, cache)["embeds"]; ok {
			t.Fatal("unexpected embeds")
		}
	}
	bare := store.Embed{URL: raw, Kind: "video"}
	if mustPreviewJSON(bare) != `{"url":"https://www.youtube.com/watch?v=jNQXAC9IVRw","kind":"video"}` {
		t.Fatal("unknown fields not omitted")
	}
}

func TestPreviewLateFetchCannotRestoreOldContent(t *testing.T) {
	for _, action := range []string{"edit", "delete"} {
		t.Run(action, func(t *testing.T) {
			c := newCast(t)
			h := c.h
			entered, release := make(chan struct{}), make(chan struct{})
			installPreviewTestClient(t, h.api, func(w http.ResponseWriter, r *http.Request) {
				close(entered)
				select {
				case <-release:
				case <-r.Context().Done():
					return
				}
				w.Header().Set("Content-Type", "text/html")
				io.WriteString(w, "<head><title>Old preview</title></head>")
			})
			dm := c.owner.openDM("alice")
			ws := c.alice.dial(t)
			defer ws.Close()
			waitEvent(t, ws, "presence")
			posted := c.owner.mustSay(dm, "http://preview.test/late")
			id := int64(posted["id"].(float64))
			select {
			case <-entered:
			case <-time.After(3 * time.Second):
				t.Fatal("fetch did not start")
			}
			var res reply
			if action == "edit" {
				res = c.owner.do(http.MethodPatch, fmt.Sprintf("/api/messages/%d", id), map[string]string{"body": "replacement"})
			} else {
				res = c.owner.do(http.MethodDelete, fmt.Sprintf("/api/messages/%d", id), nil)
			}
			if res.status != 200 {
				t.Fatal(res.raw)
			}
			close(release)
			deadline := time.Now().Add(3 * time.Second)
			for {
				cache, err := h.st.Previews([]string{"http://preview.test/late"})
				if err != nil {
					t.Fatal(err)
				}
				if len(cache) > 0 {
					break
				}
				if time.Now().After(deadline) {
					t.Fatal("fetch did not finish")
				}
				time.Sleep(time.Millisecond)
			}
			ws.SetReadDeadline(time.Now().Add(150 * time.Millisecond))
			for {
				var ev gatewayEvent
				if err := ws.ReadJSON(&ev); err != nil {
					break
				}
				if ev.Type == "message_changed" {
					d := eventData(t, ev)
					if d["embeds"] != nil {
						t.Fatal("late preview restored old message", d)
					}
				}
			}
			if strings.Contains(c.alice.get("/api/channels/"+dm+"/messages").raw, `"embeds"`) {
				t.Fatal("late preview in history")
			}
		})
	}
}
