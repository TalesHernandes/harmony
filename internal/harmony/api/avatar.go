package api

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"image"
	"image/color"
	_ "image/gif"
	"image/jpeg"
	_ "image/png"
	"io"
	"math"
	"net/http"
	"time"

	"call/internal/harmony/sfu"
	"call/internal/harmony/store"
)

const maxAvatarBytes = 5 << 20

// avatarReadTime is how long a photo may take to arrive: five megabytes over a
// slow phone connection, with room to spare.
const avatarReadTime = 60 * time.Second

func (s *Server) handlePutAvatar(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	// The body is read before the lock and against a deadline: holding the
	// lock while a client trickled its bytes in let one connection stall every
	// avatar change and removal for as long as it cared to keep the body open.
	_ = http.NewResponseController(w).SetReadDeadline(time.Now().Add(avatarReadTime))
	r.Body = http.MaxBytesReader(w, r.Body, maxAvatarBytes)
	data, err := io.ReadAll(r.Body)
	// Serialize decoding to bound memory, and changes through their broadcasts
	// so that a late upload cannot announce a version superseded by a delete.
	s.avatarMu.Lock()
	defer s.avatarMu.Unlock()
	if err != nil {
		var large *http.MaxBytesError
		if errors.As(err, &large) {
			fail(w, 413, "avatar_too_large")
		} else {
			fail(w, 400, "bad_image")
		}
		return
	}
	switch http.DetectContentType(data) {
	case "image/jpeg", "image/png", "image/gif":
	default:
		fail(w, 415, "avatar_type")
		return
	}
	cfg, _, err := image.DecodeConfig(bytes.NewReader(data))
	if err != nil {
		fail(w, 400, "bad_image")
		return
	}
	if cfg.Width <= 0 || cfg.Height <= 0 || cfg.Width > 8000 || cfg.Height > 8000 || int64(cfg.Width)*int64(cfg.Height) > 40000000 {
		fail(w, 413, "avatar_dimensions")
		return
	}
	src, _, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		fail(w, 400, "bad_image")
		return
	}
	var out bytes.Buffer
	if err = jpeg.Encode(&out, squareAvatar(src), &jpeg.Options{Quality: 85}); err != nil {
		s.chatError(w, err, "avatar encode")
		return
	}
	// Random rather than a content hash: uploading identical bytes is still a
	// new version, including after deleting the previous photo.
	var version [6]byte
	if _, err = rand.Read(version[:]); err != nil {
		s.chatError(w, err, "avatar version")
		return
	}
	v := hex.EncodeToString(version[:])
	if !s.saveAvatar(w, cu, out.Bytes(), v) {
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"avatar": v})
}

// squareAvatar samples the centered square with bilinear interpolation. RGBA
// components are premultiplied, so adding the missing alpha flattens on white
// before interpolation and avoids dark fringes around transparent pixels.
func squareAvatar(src image.Image) *image.RGBA {
	b := src.Bounds()
	side := min(b.Dx(), b.Dy())
	left, top := b.Min.X+(b.Dx()-side)/2, b.Min.Y+(b.Dy()-side)/2
	dst := image.NewRGBA(image.Rect(0, 0, 256, 256))
	white := func(x, y int) [3]float64 {
		r, g, bl, a := src.At(x, y).RGBA()
		return [3]float64{float64(r+65535-a) / 257, float64(g+65535-a) / 257, float64(bl+65535-a) / 257}
	}
	for y := 0; y < 256; y++ {
		fy := max(0, (float64(y)+0.5)*float64(side)/256-0.5)
		y0 := int(fy)
		y1 := min(y0+1, side-1)
		dy := fy - float64(y0)
		for x := 0; x < 256; x++ {
			fx := max(0, (float64(x)+0.5)*float64(side)/256-0.5)
			x0 := int(fx)
			x1 := min(x0+1, side-1)
			dx := fx - float64(x0)
			a, b, c, d := white(left+x0, top+y0), white(left+x1, top+y0), white(left+x0, top+y1), white(left+x1, top+y1)
			var rgb [3]uint8
			for i := range rgb {
				rgb[i] = uint8(math.Round((a[i]*(1-dx)+b[i]*dx)*(1-dy) + (c[i]*(1-dx)+d[i]*dx)*dy))
			}
			dst.SetRGBA(x, y, color.RGBA{rgb[0], rgb[1], rgb[2], 255})
		}
	}
	return dst
}

func (s *Server) saveAvatar(w http.ResponseWriter, cu ctxUser, data []byte, version string) bool {
	if err := s.st.SetAvatar(cu.user.ID, data, version); err != nil {
		if errors.Is(err, store.ErrRateLimited) {
			fail(w, 429, "avatar_limit")
		} else {
			s.chatError(w, err, "avatar save")
		}
		return false
	}
	s.hub.publish(s.hub.watchers(), "profile", map[string]string{"username": cu.user.Username, "avatar": version})
	return true
}

func (s *Server) handleDeleteAvatar(w http.ResponseWriter, _ *http.Request, cu ctxUser) {
	s.avatarMu.Lock()
	defer s.avatarMu.Unlock()
	if s.saveAvatar(w, cu, nil, "") {
		w.WriteHeader(http.StatusNoContent)
	}
}

func (s *Server) handleGetAvatar(w http.ResponseWriter, r *http.Request, _ ctxUser) {
	data, err := s.st.Avatar(r.PathValue("username"))
	if err != nil {
		s.chatError(w, err, "avatar read")
		return
	}
	w.Header().Set("Content-Type", "image/jpeg")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Cache-Control", "private, max-age=31536000, immutable")
	w.Write(data)
}

func (s *Server) avatarVersion(username string) string {
	v, err := s.st.AvatarVersion(username)
	if err != nil && !errors.Is(err, store.ErrNotFound) {
		s.log.Error("avatar version", "err", err)
	}
	return v
}

func (s *Server) personJSON(username, display string) map[string]any {
	return map[string]any{"username": username, "display_name": display, "avatar": s.avatarVersion(username)}
}

func (s *Server) avatarPeers(peers []sfu.Peer) []sfu.Peer {
	for i := range peers {
		peers[i].Avatar = s.avatarVersion(peers[i].Username)
	}
	return peers
}

func (s *Server) avatarOffer(offer sfu.Offer) sfu.Offer {
	for i := range offer.Tracks {
		offer.Tracks[i].Avatar = s.avatarVersion(offer.Tracks[i].Username)
	}
	return offer
}
