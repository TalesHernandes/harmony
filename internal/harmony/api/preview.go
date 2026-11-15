package api

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"image"
	_ "image/gif"
	_ "image/jpeg"
	_ "image/png"
	"io"
	"mime"
	"net"
	"net/http"
	"net/netip"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
	"unicode"

	"call/internal/harmony/store"
	"golang.org/x/net/html"
)

const previewUA = "Mozilla/5.0 (compatible; harmony-preview/1.0)"
const previewHTMLLimit = 1 << 20
const previewImageLimit = 5 << 20

var errPreviewBlocked = errors.New("preview blocked")
var linkPattern = regexp.MustCompile(`https?://[^\s<>` + "`" + `"\x00-\x1f]+`)

func previewURLs(body string) []string {
	// Blank code and angle-suppressed text before recognizing URLs.
	var clean strings.Builder
	for i := 0; i < len(body); {
		if body[i] == '`' {
			n := 1
			for i+n < len(body) && body[i+n] == '`' {
				n++
			}
			end := strings.Index(body[i+n:], strings.Repeat("`", n))
			if end < 0 {
				break
			}
			i += n + end + n
			clean.WriteByte(' ')
			continue
		}
		if body[i] == '<' {
			if end := strings.IndexByte(body[i:], '>'); end >= 0 {
				i += end + 1
				clean.WriteByte(' ')
				continue
			}
		}
		clean.WriteByte(body[i])
		i++
	}
	var out []string
	seen := map[string]bool{}
	for _, raw := range linkPattern.FindAllString(clean.String(), -1) {
		for {
			u := strings.TrimRight(raw, ".,;:!?")
			if strings.HasSuffix(u, ")") && strings.Count(u, ")") > strings.Count(u, "(") {
				u = strings.TrimSuffix(u, ")")
			}
			if u == raw {
				break
			}
			raw = u
		}
		if _, err := previewURL(raw); err != nil || seen[raw] {
			continue
		}
		seen[raw] = true
		out = append(out, raw)
		if len(out) == 3 {
			break
		}
	}
	return out
}

func previewURL(raw string) (*url.URL, error) {
	u, err := url.Parse(raw)
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Hostname() == "" || u.User != nil || (u.Port() != "" && u.Port() != "80" && u.Port() != "443") {
		return nil, errPreviewBlocked
	}
	return u, nil
}

func publicPreviewIP(ip netip.Addr) bool {
	ip = ip.Unmap()
	if !ip.IsValid() || !ip.IsGlobalUnicast() || ip.IsPrivate() || ip.IsLoopback() || ip.IsLinkLocalUnicast() {
		return false
	}
	for _, p := range []string{"0.0.0.0/8", "100.64.0.0/10", "192.0.0.0/24", "192.0.2.0/24", "198.18.0.0/15", "198.51.100.0/24", "203.0.113.0/24", "240.0.0.0/4", "2001:db8::/32", "64:ff9b::/96", "64:ff9b:1::/48", "2002::/16"} {
		if netip.MustParsePrefix(p).Contains(ip) {
			return false
		}
	}
	return true
}

// The guard sees the numeric address selected by Dialer immediately before
// connect, rather than a DNS answer checked separately from the connection.
func previewClient(guard func(string) error) *http.Client {
	if guard == nil {
		guard = func(addr string) error {
			host, port, err := net.SplitHostPort(addr)
			if err != nil || (port != "80" && port != "443") {
				return errPreviewBlocked
			}
			ip, err := netip.ParseAddr(host)
			if err != nil || !publicPreviewIP(ip) {
				return errPreviewBlocked
			}
			return nil
		}
	}
	d := &net.Dialer{Timeout: 8 * time.Second, Control: func(_, addr string, _ syscall.RawConn) error { return guard(addr) }}
	tr := &http.Transport{Proxy: nil, DialContext: d.DialContext, ForceAttemptHTTP2: true, MaxIdleConns: 4, MaxIdleConnsPerHost: 2, IdleConnTimeout: 30 * time.Second, TLSHandshakeTimeout: 8 * time.Second, MaxResponseHeaderBytes: 32 << 10}
	return &http.Client{Transport: tr, Timeout: 8 * time.Second, CheckRedirect: func(req *http.Request, via []*http.Request) error {
		if len(via) > 3 {
			return errPreviewBlocked
		}
		_, err := previewURL(req.URL.String())
		return err
	}}
}

func previewGet(ctx context.Context, c *http.Client, raw string) (*http.Response, error) {
	if _, err := previewURL(raw); err != nil {
		return nil, err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, raw, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", previewUA)
	req.Header.Set("Accept-Language", "pt-BR,pt;q=0.9,en;q=0.8")
	res, err := c.Do(req)
	if err != nil {
		return nil, err
	}
	if res.StatusCode != http.StatusOK {
		res.Body.Close()
		return nil, errors.New("preview status")
	}
	return res, nil
}

func previewText(s string, max int) string {
	z := html.NewTokenizer(strings.NewReader(s))
	var b strings.Builder
	for {
		switch z.Next() {
		case html.ErrorToken:
			goto done
		case html.TextToken:
			b.Write(z.Text())
		case html.StartTagToken, html.EndTagToken, html.SelfClosingTagToken:
			b.WriteByte(' ')
		}
	}
done:
	r := []rune(strings.Join(strings.FieldsFunc(b.String(), unicode.IsSpace), " "))
	if len(r) > max {
		r = append(r[:max-1], '…')
	}
	return string(r)
}

func youtubeID(u *url.URL) (string, bool) {
	h := strings.ToLower(u.Hostname())
	yt := h == "youtube.com" || h == "www.youtube.com" || h == "m.youtube.com" || h == "music.youtube.com" || h == "youtu.be"
	if !yt {
		return "", false
	}
	id := ""
	parts := strings.Split(strings.Trim(u.Path, "/"), "/")
	if h == "youtu.be" && len(parts) == 1 {
		id = parts[0]
	} else if u.Path == "/watch" {
		id = u.Query().Get("v")
	} else if len(parts) == 2 && (parts[0] == "shorts" || parts[0] == "live") {
		id = parts[1]
	}
	if len(id) != 11 {
		return "", true
	}
	for _, c := range id {
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '_' || c == '-') {
			return "", true
		}
	}
	return id, true
}

func parsePreview(r io.Reader, base *url.URL, raw string) (store.Embed, string, error) {
	z := html.NewTokenizer(io.LimitReader(r, previewHTMLLimit))
	z.SetMaxBuf(previewHTMLLimit)
	meta := map[string]string{}
	var title strings.Builder
	inTitle := false
parse:
	for {
		tt := z.Next()
		switch tt {
		case html.ErrorToken:
			break parse
		case html.EndTagToken:
			t := z.Token()
			if t.Data == "head" {
				break parse
			}
			if t.Data == "title" {
				inTitle = false
			}
		case html.TextToken:
			if inTitle {
				title.Write(z.Text())
			}
		case html.StartTagToken, html.SelfClosingTagToken:
			t := z.Token()
			if t.Data == "body" {
				break parse
			}
			if t.Data == "title" {
				inTitle = true
			}
			if t.Data != "meta" {
				continue
			}
			key, value := "", ""
			for _, a := range t.Attr {
				switch a.Key {
				case "property", "name":
					key = strings.ToLower(a.Val)
				case "content":
					value = a.Val
				}
			}
			if meta[key] == "" {
				meta[key] = value
			}
		}
	}
	first := func(keys ...string) string {
		for _, k := range keys {
			if v := meta[k]; v != "" {
				return v
			}
		}
		return ""
	}
	e := store.Embed{URL: raw, Kind: "link", Title: previewText(first("og:title", "twitter:title"), 256), Description: previewText(first("og:description", "twitter:description", "description"), 350), Site: previewText(meta["og:site_name"], 64)}
	if e.Title == "" {
		e.Title = previewText(title.String(), 256)
	}
	img := first("og:image:secure_url", "og:image", "twitter:image", "twitter:image:src")
	if strings.HasPrefix(strings.ToLower(meta["og:type"]), "video") {
		e.Kind = "video"
	}
	original, err := url.Parse(raw)
	if err != nil {
		original = base
	}
	id, yt := youtubeID(original)
	if !yt {
		id, yt = youtubeID(base)
	}
	if yt {
		e.Kind = "video"
		e.Site = "YouTube"
		if img == "" && id != "" {
			img = "https://i.ytimg.com/vi/" + id + "/hqdefault.jpg"
		}
	}
	if img != "" {
		if u, err := url.Parse(strings.TrimSpace(img)); err == nil {
			img = base.ResolveReference(u).String()
		} else {
			img = ""
		}
	}
	// Declared sizes are useful only when an image is actually retained.
	if img != "" {
		e.Width, _ = strconv.Atoi(meta["og:image:width"])
		e.Height, _ = strconv.Atoi(meta["og:image:height"])
		if e.Width < 0 {
			e.Width = 0
		}
		if e.Height < 0 {
			e.Height = 0
		}
	}
	return e, img, nil
}

type previewRate struct {
	start time.Time
	count int
}
type previewService struct {
	mu       sync.Mutex
	inflight map[string]chan struct{}
	rates    map[int64]previewRate
	slots    chan struct{}
	client   *http.Client
	// endpoints replaces a site's oEmbed address (preview_sites.go), for tests.
	endpoints map[string]string
	ctx       context.Context
	cancel    context.CancelFunc
}

func newPreviewService() *previewService {
	ctx, cancel := context.WithCancel(context.Background())
	return &previewService{inflight: map[string]chan struct{}{}, rates: map[int64]previewRate{}, slots: make(chan struct{}, 4), client: previewClient(nil), ctx: ctx, cancel: cancel}
}

// Close cancels outstanding previews without waiting for remote servers.
// There was no existing Server.Close hook; callers may use this on shutdown.
func (s *Server) Close() error {
	s.previews.cancel()
	s.previews.client.CloseIdleConnections()
	return nil
}

func (p *previewService) allow(userID int64, now time.Time) bool {
	for id, r := range p.rates {
		if now.Sub(r.start) >= time.Minute {
			delete(p.rates, id)
		}
	}
	r := p.rates[userID]
	if r.start.IsZero() {
		r.start = now
	}
	if r.count >= 20 {
		return false
	}
	r.count++
	p.rates[userID] = r
	return true
}

func (s *Server) cachedPreviews(messages []store.Message) map[string]store.Preview {
	seen := map[string]bool{}
	var urls []string
	for _, m := range messages {
		if m.Deleted() || m.Kind != "" {
			continue
		}
		for _, u := range previewURLs(m.Body) {
			if !seen[u] {
				seen[u] = true
				urls = append(urls, u)
			}
		}
	}
	cache, err := s.st.Previews(urls)
	if err != nil {
		s.log.Info("preview", "result", "cache_error")
	}
	return cache
}

func previewEmbeds(m store.Message, cache map[string]store.Preview) []store.Embed {
	if m.Deleted() || m.Kind != "" {
		return nil
	}
	var embeds []store.Embed
	for _, u := range previewURLs(m.Body) {
		if p, ok := cache[u]; ok && p.Success && p.Fresh(time.Now()) {
			embeds = append(embeds, p.Embed)
		}
	}
	return embeds
}

func (s *Server) startPreviews(userID int64, m store.Message) {
	if m.Deleted() || m.Kind != "" {
		return
	}
	urls := previewURLs(m.Body)
	if err := s.st.BindPreviewLinks(userID, m, urls); err != nil {
		return
	}
	if len(urls) == 0 {
		return
	}
	go func() {
		for _, u := range urls {
			if s.previews.ctx.Err() != nil {
				return
			}
			s.ensurePreview(userID, u)
			// Reload after fetching: an edit or deletion must never resurrect old content.
			current, err := s.st.MessageFor(userID, m.ID)
			if err != nil {
				return
			}
			if current.Deleted() || current.Body != m.Body || current.Kind != "" {
				return
			}
			if len(previewEmbeds(current, s.cachedPreviews([]store.Message{current}))) > 0 {
				s.notifyChannel(current.ChannelID, "message_changed", s.messageJSON(current))
			}
		}
	}()
}

func (s *Server) ensurePreview(userID int64, raw string) {
	p := s.previews
	p.mu.Lock()
	if done, ok := p.inflight[raw]; ok {
		p.mu.Unlock()
		select {
		case <-done:
		case <-p.ctx.Done():
		}
		return
	}
	cache, err := s.st.Previews([]string{raw})
	if err != nil {
		p.mu.Unlock()
		return
	}
	if item, ok := cache[raw]; ok && item.Fresh(time.Now()) {
		p.mu.Unlock()
		return
	}
	if !p.allow(userID, time.Now()) {
		p.mu.Unlock()
		return
	}
	done := make(chan struct{})
	p.inflight[raw] = done
	p.mu.Unlock()
	defer func() { p.mu.Lock(); delete(p.inflight, raw); close(done); p.mu.Unlock() }()
	select {
	case p.slots <- struct{}{}:
	case <-p.ctx.Done():
		return
	}
	defer func() { <-p.slots }()
	ctx, cancel := context.WithTimeout(p.ctx, 8*time.Second)
	defer cancel()
	e, outcome := s.fetchPreview(ctx, raw)
	if ctx.Err() != nil {
		outcome = "timeout"
	}
	if p.ctx.Err() != nil {
		return
	}
	if err := s.st.SavePreview(store.Preview{Embed: e, Success: outcome == "ok", FetchedAt: time.Now()}); err != nil {
		outcome = "cache_error"
	}
	s.log.Info("preview", "result", outcome)
}

func (s *Server) fetchPreview(ctx context.Context, raw string) (store.Embed, string) {
	empty := store.Embed{URL: raw, Kind: "link"}
	if e, img, outcome, handled := s.fetchSite(ctx, raw); handled {
		if outcome != "ok" {
			return empty, outcome
		}
		s.attachPreviewImage(ctx, &e, img)
		return e, "ok"
	}
	res, err := previewGet(ctx, s.previews.client, raw)
	if err != nil {
		if errors.Is(err, errPreviewBlocked) {
			return empty, "blocked"
		}
		return empty, "fetch_error"
	}
	defer res.Body.Close()
	typ, _, _ := mime.ParseMediaType(res.Header.Get("Content-Type"))
	if typ != "text/html" && typ != "application/xhtml+xml" {
		return empty, "not_html"
	}
	e, img, _ := parsePreview(res.Body, res.Request.URL, raw)
	if genericPage(e, res.Request.URL) {
		return empty, "generic"
	}
	s.attachPreviewImage(ctx, &e, img)
	if e.Title == "" && e.Description == "" && e.Image == "" {
		return empty, "empty"
	}
	return e, "ok"
}

// attachPreviewImage keeps a copy of the card's picture; without one the
// card has no size either.
func (s *Server) attachPreviewImage(ctx context.Context, e *store.Embed, img string) {
	if img != "" && s.cfg.FilesDir != "" {
		id, w, h, err := s.fetchPreviewImage(ctx, img)
		if err == nil {
			e.Image = "/api/previews/" + id
			if w > 0 && h > 0 {
				e.Width = w
				e.Height = h
			}
		}
	}
	if e.Image == "" {
		e.Width = 0
		e.Height = 0
	}
}

func (s *Server) fetchPreviewImage(ctx context.Context, raw string) (string, int, int, error) {
	res, err := previewGet(ctx, s.previews.client, raw)
	if err != nil {
		return "", 0, 0, err
	}
	defer res.Body.Close()
	data, err := io.ReadAll(io.LimitReader(res.Body, previewImageLimit+1))
	if err != nil {
		return "", 0, 0, err
	}
	if len(data) > previewImageLimit {
		return "", 0, 0, errors.New("too_big")
	}
	if !previewImageType(http.DetectContentType(data)) {
		return "", 0, 0, errors.New("not_image")
	}
	w, h := 0, 0
	if cfg, _, err := image.DecodeConfig(bytes.NewReader(data)); err == nil {
		w = cfg.Width
		h = cfg.Height
	}
	dir := filepath.Join(s.cfg.FilesDir, "previews")
	if err = os.MkdirAll(dir, 0700); err != nil {
		return "", 0, 0, err
	}
	// Content addressing makes concurrent URLs sharing an image safe to publish.
	sum := sha256.Sum256(data)
	id := hex.EncodeToString(sum[:])
	f, err := os.CreateTemp(dir, ".preview-*")
	if err != nil {
		return "", 0, 0, err
	}
	defer os.Remove(f.Name())
	_, err = f.Write(data)
	closeErr := f.Close()
	if err != nil {
		return "", 0, 0, err
	}
	if closeErr != nil {
		return "", 0, 0, closeErr
	}
	if err = os.Rename(f.Name(), filepath.Join(dir, id)); err != nil {
		return "", 0, 0, err
	}
	return id, w, h, nil
}

func previewImageType(t string) bool {
	return t == "image/jpeg" || t == "image/png" || t == "image/gif" || t == "image/webp"
}

func (s *Server) handlePreviewImage(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	id := r.PathValue("id")
	decoded, err := hex.DecodeString(id)
	if err != nil || len(decoded) != 32 || len(id) != 64 || s.cfg.FilesDir == "" || s.st.PreviewImageFor(cu.user.ID, id) != nil {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	f, err := os.Open(filepath.Join(s.cfg.FilesDir, "previews", id))
	if err != nil {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	defer f.Close()
	var head [512]byte
	n, _ := f.Read(head[:])
	typ := http.DetectContentType(head[:n])
	if !previewImageType(typ) {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	f.Seek(0, io.SeekStart)
	w.Header().Set("Content-Type", typ)
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Cache-Control", "private, max-age=86400")
	w.Header().Set("Content-Disposition", "inline")
	info, err := f.Stat()
	if err != nil {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	http.ServeContent(w, r, id, info.ModTime(), f)
}
