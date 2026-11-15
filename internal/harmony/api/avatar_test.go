package api

import (
	"bytes"
	"encoding/binary"
	"hash/crc32"
	"image"
	"image/color"
	"image/gif"
	"image/jpeg"
	"image/png"
	"io"
	"net/http"
	"testing"
	"time"
)

func avatarUpload(d *device, data []byte, path string) reply {
	return d.do(http.MethodPut, path, nil, func(r *http.Request) {
		r.Body = io.NopCloser(bytes.NewReader(data))
		r.ContentLength = int64(len(data))
		// Deliberately wrong: the bytes, never this header, choose the type.
		r.Header.Set("Content-Type", "text/plain")
	})
}

func avatarPNG(t *testing.T) []byte {
	t.Helper()
	src := image.NewNRGBA(image.Rect(0, 0, 6, 2))
	for y := 0; y < 2; y++ {
		for x := 0; x < 6; x++ {
			if x < 2 || x > 3 {
				src.SetNRGBA(x, y, color.NRGBA{R: 255, A: 255})
			}
		}
	}
	var b bytes.Buffer
	if err := png.Encode(&b, src); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

func TestAvatarLifecycleAndAuthorization(t *testing.T) {
	h := newHarness(t)
	a := h.join(h.bootstrapInvite(), "alice", goodPass)
	b := h.join(h.bootstrapInvite(), "bruno", goodPass)
	sock := a.dial(t)
	defer sock.Close()
	data := avatarPNG(t)
	upload := avatarUpload(b, data, "/api/me/avatar")
	if upload.status != 200 {
		t.Fatalf("upload: %d %s", upload.status, upload.raw)
	}
	version := upload.body["avatar"].(string)
	var event gatewayEvent
	for {
		_ = sock.SetReadDeadline(time.Now().Add(5 * time.Second))
		if err := sock.ReadJSON(&event); err != nil {
			t.Fatal(err)
		}
		if event.Type == "profile" {
			break
		}
	}
	changed := event.Data.(map[string]any)
	if changed["username"] != "bruno" || changed["avatar"] != version {
		t.Fatal(changed)
	}
	if len(version) != 12 {
		t.Fatalf("version: %q", version)
	}
	photo := a.get("/api/people/bruno/avatar?v=" + version)
	if photo.status != 200 || photo.header.Get("Content-Type") != "image/jpeg" || photo.header.Get("X-Content-Type-Options") != "nosniff" || photo.header.Get("Cache-Control") != "private, max-age=31536000, immutable" {
		t.Fatalf("photo: %d %v", photo.status, photo.header)
	}
	img, err := jpeg.Decode(bytes.NewReader([]byte(photo.raw)))
	if err != nil {
		t.Fatal(err)
	}
	if img.Bounds().Dx() != 256 || img.Bounds().Dy() != 256 {
		t.Fatal(img.Bounds())
	}
	r, g, bl, _ := img.At(128, 128).RGBA()
	if r < 64000 || g < 64000 || bl < 64000 {
		t.Fatal("crop must discard red edges and flatten transparency on white")
	}
	stored, err := h.st.Avatar("bruno")
	if err != nil || !bytes.Equal(stored, []byte(photo.raw)) || bytes.Equal(stored, data) {
		t.Fatal("only processed JPEG must be stored")
	}
	if got := b.get("/api/me").body["user"].(map[string]any)["avatar"]; got != version {
		t.Fatal(got)
	}
	for _, member := range a.get("/api/members").body["members"].([]any) {
		m := member.(map[string]any)
		if m["username"] == "bruno" && m["avatar"] != version {
			t.Fatal(m)
		}
	}
	// Inventing a target, in the URL or the query, cannot mutate that account.
	if got := avatarUpload(a, data, "/api/people/bruno/avatar"); got.status == 200 {
		t.Fatal("A changed B")
	}
	if got := a.do("DELETE", "/api/people/bruno/avatar", nil); got.status == 204 {
		t.Fatal("A deleted B")
	}
	if got := avatarUpload(a, data, "/api/me/avatar?username=bruno"); got.status != 200 {
		t.Fatal(got)
	}
	if got := a.do("DELETE", "/api/me/avatar?username=bruno", nil); got.status != 204 {
		t.Fatal(got)
	}
	if got := a.get("/api/people/bruno/avatar"); got.raw != photo.raw {
		t.Fatal("B changed through A's endpoint")
	}
	stranger := h.device("stranger")
	for _, method := range []string{"GET", "PUT", "DELETE"} {
		path := "/api/me/avatar"
		if method == "GET" {
			path = "/api/people/bruno/avatar"
		}
		if got := stranger.do(method, path, nil); got.status != 401 {
			t.Fatal(got)
		}
	}
	if got := a.get("/api/people/nobody/avatar"); got.status != 404 {
		t.Fatal(got)
	}
	if got := a.get("/api/people/alice/avatar"); got.status != 404 {
		t.Fatal(got)
	}
	if got := avatarUpload(b, data, "/api/me/avatar"); got.body["avatar"] == version {
		t.Fatal("identical upload reused version")
	}
	if got := b.do("DELETE", "/api/me/avatar", nil); got.status != 204 {
		t.Fatal(got)
	}
	if got := a.get("/api/people/bruno/avatar"); got.status != 404 {
		t.Fatal(got)
	}
}

func TestAvatarFormatsAndLimits(t *testing.T) {
	h := newHarness(t)
	d := h.join(h.bootstrapInvite(), "alice", goodPass)
	data := avatarPNG(t)
	for _, format := range []string{"jpeg", "gif"} {
		var b bytes.Buffer
		src := image.NewRGBA(image.Rect(0, 0, 3, 5))
		var err error
		if format == "jpeg" {
			err = jpeg.Encode(&b, src, nil)
		} else {
			err = gif.Encode(&b, src, nil)
		}
		if err != nil {
			t.Fatal(err)
		}
		if got := avatarUpload(d, b.Bytes(), "/api/me/avatar"); got.status != 200 {
			t.Fatalf("%s: %v", format, got)
		}
	}
	for _, bad := range [][]byte{[]byte("not an image"), []byte("RIFF\x00\x00\x00\x00WEBPVP8 "), []byte("<svg></svg>")} {
		if got := avatarUpload(d, bad, "/api/me/avatar"); got.status != 415 || got.errorCode() != "avatar_type" {
			t.Fatal(got)
		}
	}
	if got := avatarUpload(d, make([]byte, maxAvatarBytes+1), "/api/me/avatar"); got.status != 413 {
		t.Fatal(got)
	}
	if got := avatarUpload(d, data[:33], "/api/me/avatar"); got.status != 400 {
		t.Fatal(got)
	}
	// Valid IHDR, no image data: 413 proves config was checked before Decode.
	for _, dims := range [][2]uint32{{8001, 1}, {1, 8001}, {8000, 8000}, {7000, 6000}} {
		bomb := append([]byte(nil), data[:33]...)
		binary.BigEndian.PutUint32(bomb[16:20], dims[0])
		binary.BigEndian.PutUint32(bomb[20:24], dims[1])
		binary.BigEndian.PutUint32(bomb[29:33], crc32.ChecksumIEEE(bomb[12:29]))
		if got := avatarUpload(d, bomb, "/api/me/avatar"); got.status != 413 || got.errorCode() != "avatar_dimensions" {
			t.Fatal(got)
		}
	}
	// Both uploads and removals count; malformed uploads above do not.
	for i := 2; i < 10; i++ {
		if got := avatarUpload(d, data, "/api/me/avatar"); got.status != 200 {
			t.Fatal(got)
		}
	}
	if got := avatarUpload(d, data, "/api/me/avatar"); got.status != 429 || got.errorCode() != "avatar_limit" {
		t.Fatal(got)
	}
	if got := d.do("DELETE", "/api/me/avatar", nil); got.status != 429 {
		t.Fatal(got)
	}
	h.st.SetClock(func() time.Time { return time.Now().Add(time.Hour + time.Second) })
	if got := d.do("DELETE", "/api/me/avatar", nil); got.status != 204 {
		t.Fatal(got)
	}
}
