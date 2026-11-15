package api

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/url"
	"regexp"
	"strconv"
	"strings"

	"call/internal/harmony/store"
)

// The sites whose own page says nothing useful to a server in a datacenter,
// read through the preview service each of them publishes (oEmbed) instead.
// Measured from the Oracle machine on 30/10/2026: YouTube's watch page there
// is a generic one ("- YouTube", the site's own slogan, its logo), and the
// first version kept it as a success; the oEmbed answers with the video. The
// same request also answers for TikTok, X, Vimeo and Reddit posts. Only the
// sites' own endpoints, with the honest User-Agent: never another crawler's
// name, never a third-party mirror.

const previewJSONLimit = 256 << 10

type oembed struct {
	Title        string  `json:"title"`
	AuthorName   string  `json:"author_name"`
	ThumbnailURL string  `json:"thumbnail_url"`
	ThumbWidth   flexInt `json:"thumbnail_width"`
	ThumbHeight  flexInt `json:"thumbnail_height"`
	HTML         string  `json:"html"`
}

// TikTok sends sizes as strings, Vimeo as numbers.
type flexInt int

func (f *flexInt) UnmarshalJSON(b []byte) error {
	s := strings.Trim(string(b), `"`)
	n, err := strconv.Atoi(s)
	if err != nil {
		*f = 0
		return nil
	}
	*f = flexInt(n)
	return nil
}

type previewSite struct {
	name     string
	endpoint string // the site's oEmbed; "url=" is appended
	// ask returns the link to ask about, or false when the link is not this
	// site's (or not one its oEmbed knows, like a channel page).
	ask func(u *url.URL) (string, bool)
	// fill makes the card; false when the answer had nothing to show.
	fill func(u *url.URL, o oembed) (e store.Embed, image string, ok bool)
	// fallback: on a failed oEmbed, read the page as any other link.
	fallback bool
}

func hostIs(u *url.URL, hosts ...string) bool {
	h := strings.ToLower(u.Hostname())
	for _, want := range hosts {
		if h == want {
			return true
		}
	}
	return false
}

func pathParts(u *url.URL) []string {
	var out []string
	for _, p := range strings.Split(u.Path, "/") {
		if p != "" {
			out = append(out, p)
		}
	}
	return out
}

var digits = regexp.MustCompile(`^[0-9]+$`)
var tweetHandle = regexp.MustCompile(`^[A-Za-z0-9_]{1,15}$`)

var previewSites = []previewSite{
	{
		name: "YouTube", endpoint: "https://www.youtube.com/oembed?format=json",
		ask: func(u *url.URL) (string, bool) {
			id, yt := youtubeID(u)
			if !yt || id == "" {
				return "", false
			}
			return "https://www.youtube.com/watch?v=" + id, true
		},
		fill: func(u *url.URL, o oembed) (store.Embed, string, bool) {
			id, _ := youtubeID(u)
			return store.Embed{Kind: "video", Site: "YouTube", Title: o.Title, Description: o.AuthorName},
				"https://i.ytimg.com/vi/" + id + "/hqdefault.jpg", o.Title != ""
		},
	},
	{
		name: "TikTok", endpoint: "https://www.tiktok.com/oembed",
		ask: func(u *url.URL) (string, bool) {
			if hostIs(u, "vm.tiktok.com", "vt.tiktok.com") && len(pathParts(u)) == 1 {
				return "https://" + strings.ToLower(u.Hostname()) + u.Path, true
			}
			p := pathParts(u)
			if !hostIs(u, "tiktok.com", "www.tiktok.com", "m.tiktok.com") || len(p) < 3 || !strings.HasPrefix(p[0], "@") || p[1] != "video" || !digits.MatchString(p[2]) {
				return "", false
			}
			return "https://www.tiktok.com/" + p[0] + "/video/" + p[2], true
		},
		fill: func(u *url.URL, o oembed) (store.Embed, string, bool) {
			title := o.Title
			if title == "" {
				title = o.AuthorName
			}
			return store.Embed{Kind: "video", Site: "TikTok", Title: title, Description: o.AuthorName}, o.ThumbnailURL, title != ""
		},
	},
	{
		name: "X", endpoint: "https://publish.twitter.com/oembed?omit_script=1",
		ask: func(u *url.URL) (string, bool) {
			p := pathParts(u)
			if !hostIs(u, "x.com", "www.x.com", "mobile.x.com", "twitter.com", "www.twitter.com", "mobile.twitter.com") || len(p) < 3 || !tweetHandle.MatchString(p[0]) || p[1] != "status" || !digits.MatchString(p[2]) {
				return "", false
			}
			return "https://twitter.com/" + p[0] + "/status/" + p[2], true
		},
		fill: func(u *url.URL, o oembed) (store.Embed, string, bool) {
			text := previewText(between(between(o.HTML, "<p", "</p>"), ">", ""), 350)
			title := o.AuthorName
			if p := pathParts(u); len(p) > 0 && title != "" {
				title += " (@" + p[0] + ")"
			}
			return store.Embed{Kind: "link", Site: "X", Title: title, Description: text}, "", title != "" && text != ""
		},
	},
	{
		name: "Vimeo", endpoint: "https://vimeo.com/api/oembed.json",
		ask: func(u *url.URL) (string, bool) {
			p := pathParts(u)
			if !hostIs(u, "vimeo.com", "www.vimeo.com") || len(p) == 0 || !digits.MatchString(p[0]) {
				return "", false
			}
			return "https://vimeo.com/" + p[0], true
		},
		fill: func(u *url.URL, o oembed) (store.Embed, string, bool) {
			return store.Embed{Kind: "video", Site: "Vimeo", Title: o.Title, Description: o.AuthorName, Width: int(o.ThumbWidth), Height: int(o.ThumbHeight)}, o.ThumbnailURL, o.Title != ""
		},
	},
	{
		name: "Spotify", endpoint: "https://open.spotify.com/oembed",
		ask: func(u *url.URL) (string, bool) {
			p := pathParts(u)
			if len(p) > 0 && strings.HasPrefix(p[0], "intl-") {
				p = p[1:]
			}
			kinds := map[string]bool{"track": true, "album": true, "playlist": true, "episode": true, "show": true, "artist": true}
			if !hostIs(u, "open.spotify.com") || len(p) < 2 || !kinds[p[0]] {
				return "", false
			}
			return "https://open.spotify.com/" + p[0] + "/" + p[1], true
		},
		fill: func(u *url.URL, o oembed) (store.Embed, string, bool) {
			return store.Embed{Kind: "link", Site: "Spotify", Title: o.Title}, o.ThumbnailURL, o.Title != ""
		},
	},
	{
		name: "Reddit", endpoint: "https://www.reddit.com/oembed", fallback: true,
		ask: func(u *url.URL) (string, bool) {
			p := pathParts(u)
			if !hostIs(u, "reddit.com", "www.reddit.com", "old.reddit.com", "new.reddit.com") || len(p) < 4 || p[0] != "r" || p[2] != "comments" {
				return "", false
			}
			return "https://www.reddit.com/r/" + p[1] + "/comments/" + p[3] + "/", true
		},
		fill: func(u *url.URL, o oembed) (store.Embed, string, bool) {
			title := previewText(between(between(o.HTML, "<a", "</a>"), ">", ""), 256)
			return store.Embed{Kind: "link", Site: "Reddit", Title: title, Description: "r/" + pathParts(u)[1]}, "", title != ""
		},
	},
	{
		name: "SoundCloud", endpoint: "https://soundcloud.com/oembed?format=json",
		ask: func(u *url.URL) (string, bool) {
			p := pathParts(u)
			if !hostIs(u, "soundcloud.com", "www.soundcloud.com", "m.soundcloud.com") || len(p) < 2 {
				return "", false
			}
			return "https://soundcloud.com/" + p[0] + "/" + p[1], true
		},
		fill: func(u *url.URL, o oembed) (store.Embed, string, bool) {
			return store.Embed{Kind: "link", Site: "SoundCloud", Title: o.Title, Description: o.AuthorName}, o.ThumbnailURL, o.Title != ""
		},
	},
}

// between is the text after the first start and up to the next end (""
// meaning the rest), or "" when start is not there.
func between(s, start, end string) string {
	i := strings.Index(s, start)
	if i < 0 {
		return ""
	}
	s = s[i+len(start):]
	if end == "" {
		return s
	}
	if j := strings.Index(s, end); j >= 0 {
		return s[:j]
	}
	return s
}

func siteFor(raw string) (*previewSite, *url.URL, string) {
	u, err := url.Parse(raw)
	if err != nil {
		return nil, nil, ""
	}
	for i := range previewSites {
		if ask, ok := previewSites[i].ask(u); ok {
			return &previewSites[i], u, ask
		}
	}
	return nil, nil, ""
}

var errPreviewEmpty = errors.New("preview empty")

// fetchSite asks the site's oEmbed. handled is false when the link is no
// known site's, or the site falls back to its page after a failure.
func (s *Server) fetchSite(ctx context.Context, raw string) (e store.Embed, image, outcome string, handled bool) {
	site, u, ask := siteFor(raw)
	if site == nil {
		return e, "", "", false
	}
	endpoint := site.endpoint
	if override := s.previews.endpoints[site.name]; override != "" {
		endpoint = override
	}
	sep := "?"
	if strings.Contains(endpoint, "?") {
		sep = "&"
	}
	o, err := s.readOembed(ctx, endpoint+sep+"url="+url.QueryEscape(ask))
	if err == nil {
		var ok bool
		if e, image, ok = site.fill(u, o); !ok {
			err = errPreviewEmpty
		}
	}
	if err != nil {
		if site.fallback {
			return store.Embed{}, "", "", false
		}
		if errors.Is(err, errPreviewBlocked) {
			return store.Embed{}, "", "blocked", true
		}
		return store.Embed{}, "", "site_error", true
	}
	e.URL = raw
	e.Title = previewPlain(e.Title, 256)
	e.Description = previewPlain(e.Description, 350)
	return e, image, "ok", true
}

// previewPlain is previewText for text that is not HTML (an oEmbed title):
// "a <b> c" stays as written.
func previewPlain(s string, max int) string {
	r := []rune(strings.Join(strings.Fields(s), " "))
	if len(r) > max {
		r = append(r[:max-1], '…')
	}
	return string(r)
}

func (s *Server) readOembed(ctx context.Context, address string) (oembed, error) {
	var o oembed
	res, err := previewGet(ctx, s.previews.client, address)
	if err != nil {
		return o, err
	}
	defer res.Body.Close()
	data, err := io.ReadAll(io.LimitReader(res.Body, previewJSONLimit+1))
	if err != nil {
		return o, err
	}
	if len(data) > previewJSONLimit {
		return o, errors.New("too_big")
	}
	return o, json.Unmarshal(data, &o)
}

// genericPage is a page that names only its site ("- YouTube", "Instagram"):
// what a site shows a datacenter it will not serve, never a preview.
func genericPage(e store.Embed, final *url.URL) bool {
	title := strings.TrimSpace(strings.Trim(e.Title, " -|–—:·"))
	if title == "" {
		return true
	}
	names := []string{e.Site}
	if final != nil {
		host := strings.Split(strings.TrimPrefix(strings.ToLower(final.Hostname()), "www."), ".")
		if len(host) > 0 {
			names = append(names, host[0])
		}
	}
	for _, name := range names {
		if name != "" && strings.EqualFold(title, strings.TrimSpace(name)) {
			return true
		}
	}
	return false
}
