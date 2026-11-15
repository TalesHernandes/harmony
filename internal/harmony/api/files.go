package api

import (
	"bytes"
	"errors"
	"image"
	_ "image/gif"  // DecodeConfig for the formats we accept
	_ "image/jpeg" // "
	_ "image/png"  // "
	"io"
	"mime/multipart"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"call/internal/harmony/store"
)

// Upload rules, PLANO-DISCORD.md §5: "type and size validated on the server,
// served with headers that stop it being executed, never by a path the user
// chooses".
const (
	// MaxUploadBytes was a phone screenshot with room to spare while pictures
	// were the only thing this took. The D4i opened it to documents, sound and
	// video, because "I cannot send you that file" is the sentence that sends a
	// group of friends back to WhatsApp — and thirty-two megabytes is a video
	// off a telephone, a song, or a book, and is still not a file transfer
	// service.
	MaxUploadBytes = 32 << 20
	// UploadsPerDay is the per-person limit. It is a runaway-client brake, not
	// a rationing scheme: nobody sends sixty pictures an evening on purpose.
	UploadsPerDay = 60
	// BytesPerDay is the brake that matters now that a single file can be large.
	// Sixty files a day used to be at most half a gigabyte; at the new size the
	// same sixty would be two, every day, per person. This is the number that
	// keeps a disk from filling up while nobody is looking, and it is generous:
	// half a gigabyte a day is a long evening of holiday videos.
	BytesPerDay  = 512 << 20
	uploadWindow = 24 * time.Hour
)

// allowedUploadTypes is decided by the bytes, never by what the browser said
// and never by the file name. A .png that is not a PNG is refused.
//
// What is *not* here is as much of the decision as what is: nothing that a
// browser or an operating system would consider running. No scripts, no
// documents with macros, no installers, no archives that unpack themselves.
// Everything on this list is something that is looked at, listened to or read,
// and everything is served back with a content type this server decided, with
// nosniff and a CSP of 'none' (handleFile).
var allowedUploadTypes = map[string]bool{
	// Pictures, as since the D1.
	"image/png":  true,
	"image/jpeg": true,
	"image/gif":  true,
	"image/webp": true,
	// Reading.
	"application/pdf": true,
	"text/plain":      true,
	// Listening. A voice note off a telephone lands as one of these; an .m4a
	// arrives as video/mp4, because what the bytes say is "an MPEG container"
	// and the bytes are what is trusted here.
	"audio/mpeg":      true,
	"audio/wave":      true,
	"audio/aiff":      true,
	"application/ogg": true,
	// Watching.
	"video/mp4":  true,
	"video/webm": true,
	// And the one way to send anything else at all, unopened by this server and
	// never unpacked by it.
	"application/zip": true,
}

// isImage says whether this is one of the four the composer shows as a picture
// and the message draws inline.
func isImage(mime string) bool {
	return strings.HasPrefix(mime, "image/")
}

// fileIDRE is the shape of the ids newID mints. Every path built under the
// upload directory is checked against it first, so no id from anywhere can
// contain a dot-dot or a slash and become a path.
var fileIDRE = regexp.MustCompile(`^[A-Za-z0-9_-]{1,64}$`)

// filePath is the only place a name under the upload directory is built. The
// name is the row id and nothing else: not the uploader's file name, not an
// extension, not anything a person typed. A file on this disk therefore cannot
// be called ".htaccess" or "x.php" no matter what was uploaded.
func (s *Server) filePath(id string) (string, error) {
	if !fileIDRE.MatchString(id) {
		return "", errors.New("api: bad file id")
	}
	return filepath.Join(s.cfg.FilesDir, id), nil
}

// uploadReadTime is how long an upload's body may take to arrive. A variable
// only so a test can shorten it.
var uploadReadTime = 5 * time.Minute

// handleUpload takes one image for one channel and answers with the row that
// a later message can attach. Two separate steps, because the picture is
// chosen and shown in the composer before the message exists.
func (s *Server) handleUpload(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	channelID := r.PathValue("id")
	if _, err := s.st.ChannelFor(cu.user.ID, channelID); err != nil {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	if s.cfg.FilesDir == "" {
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	n, err := s.st.RecordedUsage(cu.user.ID, store.UsageUpload, uploadWindow)
	if err != nil {
		s.log.Error("usage", "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	if n >= UploadsPerDay {
		fail(w, http.StatusTooManyRequests, "upload_limit")
		return
	}
	used, err := s.st.UploadedBytes(cu.user.ID, uploadWindow)
	if err != nil {
		s.log.Error("usage", "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	if used >= BytesPerDay {
		fail(w, http.StatusTooManyRequests, "upload_bytes_limit")
		return
	}

	// Two guards, both needed: MaxBytesReader stops the socket before the disk
	// is touched, and the read below stops a multipart part that lies about
	// its length.
	// And a deadline, because a byte limit is not a time limit: a body that
	// stops arriving used to hold its handler for as long as the connection
	// stayed open. 32 MiB at 1 Mbit/s is about four and a half minutes.
	_ = http.NewResponseController(w).SetReadDeadline(time.Now().Add(uploadReadTime))
	r.Body = http.MaxBytesReader(w, r.Body, MaxUploadBytes+(1<<20))
	if err := r.ParseMultipartForm(1 << 20); err != nil {
		fail(w, http.StatusRequestEntityTooLarge, "too_large")
		return
	}
	defer r.MultipartForm.RemoveAll()

	part, header, err := r.FormFile("file")
	if err != nil {
		fail(w, http.StatusBadRequest, "bad_request")
		return
	}
	defer part.Close()

	data, err := io.ReadAll(io.LimitReader(part, MaxUploadBytes+1))
	if err != nil {
		fail(w, http.StatusBadRequest, "bad_request")
		return
	}
	if len(data) > MaxUploadBytes {
		fail(w, http.StatusRequestEntityTooLarge, "too_large")
		return
	}
	if len(data) == 0 {
		fail(w, http.StatusBadRequest, "bad_request")
		return
	}

	mime := http.DetectContentType(data)
	if i := strings.IndexByte(mime, ';'); i >= 0 {
		mime = strings.TrimSpace(mime[:i])
	}
	if !allowedUploadTypes[mime] {
		fail(w, http.StatusUnsupportedMediaType, "bad_file_type")
		return
	}
	width, height := 0, 0
	if isImage(mime) {
		if cfg, _, err := image.DecodeConfig(bytes.NewReader(data)); err == nil {
			width, height = cfg.Width, cfg.Height
		}
	}

	// The ceiling again, now that the size is known and inside the
	// transaction that writes the row. The two checks at the top of this
	// handler are the cheap no, so a person who is already over it is told
	// before uploading anything; this is the one that decides.
	att, err := s.st.CreateAttachmentWithin(cu.user.ID, channelID,
		safeFileName(header), mime, int64(len(data)), width, height,
		&store.UploadQuota{Window: uploadWindow, MaxFiles: UploadsPerDay, MaxBytes: BytesPerDay})
	switch {
	case errors.Is(err, store.ErrUploadFiles):
		fail(w, http.StatusTooManyRequests, "upload_limit")
		return
	case errors.Is(err, store.ErrUploadBytes):
		fail(w, http.StatusTooManyRequests, "upload_bytes_limit")
		return
	case err != nil:
		s.log.Error("attachment", "err", err)
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	path, err := s.filePath(att.ID)
	if err == nil {
		if err = os.WriteFile(path, data, 0o600); err != nil {
			_ = os.Remove(path) // a half-written file is not a file
		}
	}
	if err != nil {
		s.log.Error("write upload", "err", err)
		// The row and the use it was counted as were committed before the
		// bytes were written; a file that never arrived does not spend the
		// day's allowance.
		if err := s.st.ReturnUpload(cu.user.ID, att); err != nil {
			s.log.Error("return upload", "err", err)
		}
		fail(w, http.StatusInternalServerError, "server_error")
		return
	}
	// Size and type only: the file name a person picked is theirs, and a log
	// line is a place it does not need to be.
	s.log.Info("upload", "user_id", cu.user.ID, "bytes", len(data), "mime", mime)
	writeJSON(w, http.StatusCreated, s.attachmentJSON(att))
}

// safeFileName keeps something readable to show under the picture and to name
// it if somebody saves it, with every part that could mean "a place on a disk"
// removed. It is never used to build a path.
func safeFileName(h *multipart.FileHeader) string {
	name := "arquivo"
	if h != nil && h.Filename != "" {
		name = h.Filename
	}
	name = filepath.Base(strings.ReplaceAll(name, `\`, "/"))
	name = strings.Map(func(r rune) rune {
		switch {
		case r < 0x20, r == 0x7f, r == '/', r == '\\', r == '"':
			return -1
		}
		return r
	}, name)
	name = strings.TrimSpace(strings.TrimLeft(name, "."))
	if name == "" {
		name = "arquivo"
	}
	if len([]rune(name)) > 80 {
		name = string([]rune(name)[:80])
	}
	return name
}

// handleFile serves one uploaded image, to a member of the channel it was
// posted in and to nobody else. The id of a picture from a conversation
// somebody is not part of answers exactly what an invented id answers.
func (s *Server) handleFile(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	att, err := s.st.AttachmentFor(cu.user.ID, r.PathValue("id"))
	if err != nil {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	path, err := s.filePath(att.ID)
	if err != nil {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	f, err := os.Open(path)
	if err != nil {
		fail(w, http.StatusNotFound, "not_found")
		return
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		fail(w, http.StatusNotFound, "not_found")
		return
	}

	h := w.Header()
	// The stored type, decided from the bytes at upload time — never sniffed
	// again here, and never taken from the request.
	h.Set("Content-Type", att.MIME)
	h.Set("X-Content-Type-Options", "nosniff")
	// Nothing this file could contain may run or fetch anything, even if a
	// browser were talked into treating it as a document.
	h.Set("Content-Security-Policy", "default-src 'none'; sandbox")
	// A picture and a video are looked at where they are; everything else is
	// offered as a file to keep. "attachment" is also the safer of the two for
	// anything this server did not draw itself.
	how := "attachment"
	if isImage(att.MIME) || strings.HasPrefix(att.MIME, "video/") || strings.HasPrefix(att.MIME, "audio/") {
		how = "inline"
	}
	h.Set("Content-Disposition", how+"; filename*=UTF-8''"+urlEscapeName(att.Name))
	// The id is unique and its bytes never change, so this is safe to keep.
	h.Set("Cache-Control", "private, max-age=31536000, immutable")
	http.ServeContent(w, r, "", info.ModTime(), f)
}

// urlEscapeName percent-encodes for the filename* parameter of
// Content-Disposition, which is where a name with an accent in it belongs.
func urlEscapeName(name string) string {
	const safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"
	var b strings.Builder
	for _, c := range []byte(name) {
		if strings.IndexByte(safe, c) >= 0 {
			b.WriteByte(c)
			continue
		}
		const hex = "0123456789ABCDEF"
		b.WriteByte('%')
		b.WriteByte(hex[c>>4])
		b.WriteByte(hex[c&0x0f])
	}
	return b.String()
}

// sweepFiles deletes files whose row is gone: a picture chosen in the composer
// and then thought better of, or one whose message was removed. The store's
// purge drops the rows; this drops the bytes, and it starts from the disk so
// that a file with no row at all is caught too.
func (s *Server) sweepFiles() {
	if s.cfg.FilesDir == "" {
		return
	}
	entries, err := os.ReadDir(s.cfg.FilesDir)
	if err != nil {
		return
	}
	cutoff := time.Now().Add(-24 * time.Hour)
	for _, e := range entries {
		if e.IsDir() || !fileIDRE.MatchString(e.Name()) {
			continue
		}
		info, err := e.Info()
		if err != nil || info.ModTime().After(cutoff) {
			continue // young enough that its message may still be on its way
		}
		live, err := s.st.AttachmentLive(e.Name())
		if err != nil || live {
			continue
		}
		if err := os.Remove(filepath.Join(s.cfg.FilesDir, e.Name())); err != nil {
			s.log.Error("sweep upload", "err", err)
		}
	}
}
