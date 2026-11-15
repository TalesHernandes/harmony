// Sprint D1 turned into tests, in the same spirit as security_test.go: the
// rules of PLANO-DISCORD.md §5 applied to the part of the service that now
// holds what people actually say to each other.
//
// The one that matters most is the first section. A direct message is a
// channel with two members, and every read in the store joins that membership
// table, so "B cannot read A's DM" is not a check somebody remembered to write
// — it is the only way the query can be spelled. These tests prove it from the
// outside, by asking as B.
package api

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"mime/multipart"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"call/internal/harmony/store"
)

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// cast is the owner and two friends, each on their own device. The owner mints
// the invites, because since D0 invites are the owner's alone.
type cast struct {
	h                *harness
	owner, alice, bo *device
}

func newCast(t *testing.T) *cast {
	t.Helper()
	h := newHarness(t)
	owner := h.join(h.bootstrapInvite(), "tales", goodPass)
	inviteA, _ := owner.inviteFrom("alice")
	alice := h.join(inviteA, "alice", goodPass2)
	inviteB, _ := owner.inviteFrom("bruno")
	bo := h.join(inviteB, "bruno", goodPass2)
	return &cast{h: h, owner: owner, alice: alice, bo: bo}
}

// generalID is the channel every account is a member of from the first
// migration onwards.
func (d *device) generalID() string {
	d.h.t.Helper()
	for _, c := range d.channels() {
		if c["kind"] == "text" && c["name"] == "geral" {
			return c["id"].(string)
		}
	}
	d.h.t.Fatal("no #geral in the channel list")
	return ""
}

func (d *device) channels() []map[string]any {
	d.h.t.Helper()
	res := d.get("/api/channels")
	if res.status != http.StatusOK {
		d.h.t.Fatalf("GET /api/channels: %d %s", res.status, res.raw)
	}
	raw, _ := res.body["channels"].([]any)
	out := make([]map[string]any, 0, len(raw))
	for _, c := range raw {
		out = append(out, c.(map[string]any))
	}
	return out
}

func (d *device) channelByID(id string) (map[string]any, bool) {
	for _, c := range d.channels() {
		if c["id"] == id {
			return c, true
		}
	}
	return nil, false
}

// openDM opens the conversation with username and returns its channel id.
func (d *device) openDM(username string) string {
	d.h.t.Helper()
	res := d.do(http.MethodPost, "/api/dms", map[string]string{"username": username})
	if res.status != http.StatusOK {
		d.h.t.Fatalf("POST /api/dms %s: %d %s", username, res.status, res.raw)
	}
	id, _ := res.body["id"].(string)
	if id == "" {
		d.h.t.Fatalf("dm response has no id: %s", res.raw)
	}
	return id
}

// say posts a message and returns the whole reply, so a test can assert on the
// status as easily as on the body.
func (d *device) say(channelID, body string, attachments ...string) reply {
	if attachments == nil {
		attachments = []string{}
	}
	return d.do(http.MethodPost, "/api/channels/"+channelID+"/messages",
		map[string]any{"body": body, "attachments": attachments})
}

func (d *device) mustSay(channelID, body string, attachments ...string) map[string]any {
	d.h.t.Helper()
	res := d.say(channelID, body, attachments...)
	if res.status != http.StatusCreated {
		d.h.t.Fatalf("post to %s: %d %s", channelID, res.status, res.raw)
	}
	return res.body
}

func (d *device) history(channelID string) []map[string]any {
	d.h.t.Helper()
	res := d.get("/api/channels/" + channelID + "/messages")
	if res.status != http.StatusOK {
		d.h.t.Fatalf("history of %s: %d %s", channelID, res.status, res.raw)
	}
	raw, _ := res.body["messages"].([]any)
	out := make([]map[string]any, 0, len(raw))
	for _, m := range raw {
		out = append(out, m.(map[string]any))
	}
	return out
}

// pngBytes builds a real, small PNG: the upload check reads the bytes, so a
// test that wants to be let in has to bring the real thing.
func pngBytes(t *testing.T, w, h int) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	img.Set(0, 0, color.RGBA{R: 200, G: 40, B: 90, A: 255})
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// upload posts one file to a channel the way the browser does.
func (d *device) upload(channelID, name string, data []byte) reply {
	d.h.t.Helper()
	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	part, err := mw.CreateFormFile("file", name)
	if err != nil {
		d.h.t.Fatal(err)
	}
	if _, err := part.Write(data); err != nil {
		d.h.t.Fatal(err)
	}
	mw.Close()

	req, err := http.NewRequest(http.MethodPost, d.h.srv.URL+"/api/channels/"+channelID+"/uploads", &body)
	if err != nil {
		d.h.t.Fatal(err)
	}
	req.Header.Set("Content-Type", mw.FormDataContentType())
	req.Header.Set("Origin", baseURL)
	req.Header.Set("X-Forwarded-For", d.ip)
	resp, err := d.c.Do(req)
	if err != nil {
		d.h.t.Fatal(err)
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)
	out := reply{status: resp.StatusCode, raw: string(raw), header: resp.Header}
	_ = json.Unmarshal(raw, &out.body)
	return out
}

// dial opens this device's live connection, carrying the same cookie jar the
// rest of its requests use.
func (d *device) dial(t *testing.T) *websocket.Conn {
	t.Helper()
	dialer := websocket.Dialer{Jar: d.c.Jar, HandshakeTimeout: 5 * time.Second}
	url := "ws" + strings.TrimPrefix(d.h.srv.URL, "http") + "/ws"
	ws, resp, err := dialer.Dial(url, http.Header{"Origin": {baseURL}})
	if err != nil {
		status := 0
		if resp != nil {
			status = resp.StatusCode
		}
		t.Fatalf("dial /ws: %v (status %d)", err, status)
	}
	return ws
}

// ---------------------------------------------------------------------------
// "uma pessoa não lê a DM de outra trocando um número na URL"
// ---------------------------------------------------------------------------

// The headline rule of the sprint. Bo asks for every single thing that names
// the conversation between the owner and Alice, and gets exactly what a
// made-up id gets — not a different code, not a different body: the same bytes,
// because a different answer would already say "that conversation exists".
func TestOneMemberCannotReachAnotherPairsDirectMessage(t *testing.T) {
	c := newCast(t)
	dm := c.owner.openDM("alice")
	c.owner.mustSay(dm, "segredo entre nos dois")
	msg := c.alice.mustSay(dm, "combinado")
	msgID := fmt.Sprintf("%.0f", msg["id"].(float64))

	invented := "AAAAAAAAAAAAAAAA"
	type probe struct {
		method, path string
		body         any
	}
	probes := []probe{
		{http.MethodGet, "/api/channels/%s", nil},
		{http.MethodGet, "/api/channels/%s/messages", nil},
		{http.MethodPost, "/api/channels/%s/messages", map[string]any{"body": "oi", "attachments": []string{}}},
		{http.MethodPost, "/api/channels/%s/read", map[string]any{"last_read_id": 1}},
	}
	for _, p := range probes {
		real := c.bo.do(p.method, fmt.Sprintf(p.path, dm), p.body)
		fake := c.bo.do(p.method, fmt.Sprintf(p.path, invented), p.body)
		if real.status != http.StatusNotFound {
			t.Errorf("%s %s on somebody else's DM = %d, want 404", p.method, p.path, real.status)
		}
		if real.status != fake.status || real.raw != fake.raw {
			t.Errorf("%s %s: a real id answers %d %q and an invented one %d %q; they must be identical",
				p.method, p.path, real.status, real.raw, fake.status, fake.raw)
		}
	}

	// The message id is a small integer, so it is the easiest thing in the
	// service to guess. Editing and removing it must fail the same way.
	for _, p := range []probe{
		{http.MethodPatch, "/api/messages/%s", map[string]any{"body": "mudei"}},
		{http.MethodDelete, "/api/messages/%s", nil},
	} {
		real := c.bo.do(p.method, fmt.Sprintf(p.path, msgID), p.body)
		fake := c.bo.do(p.method, fmt.Sprintf(p.path, "999999"), p.body)
		if real.status != http.StatusNotFound || real.raw != fake.raw {
			t.Errorf("%s on a message from somebody else's DM = %d %q, want the same as an invented id (%d %q)",
				p.method, real.status, real.raw, fake.status, fake.raw)
		}
	}

	// Nothing of it appears in Bo's own list either — not the channel, not a
	// count, not a name.
	if _, ok := c.bo.channelByID(dm); ok {
		t.Error("somebody else's DM appears in Bo's channel list")
	}
	if res := c.bo.get("/api/channels"); strings.Contains(res.raw, dm) {
		t.Error("Bo's channel list mentions the id of a DM he is not part of")
	}

	// And the two people who are in it still have everything.
	if got := len(c.alice.history(dm)); got != 2 {
		t.Errorf("Alice sees %d messages in her own DM, want 2", got)
	}
	if got := len(c.owner.history(dm)); got != 2 {
		t.Errorf("the owner sees %d messages in the DM, want 2", got)
	}
}

// The owner is the owner of the server, not of everybody's conversations. The
// one power moderation needs — removing a message — stops at the door of a
// conversation nobody invited them into.
func TestTheOwnerCannotReadADirectMessageBetweenTwoOthers(t *testing.T) {
	c := newCast(t)
	dm := c.alice.openDM("bruno")
	msg := c.alice.mustSay(dm, "coisa nossa")
	msgID := fmt.Sprintf("%.0f", msg["id"].(float64))

	if res := c.owner.get("/api/channels/" + dm + "/messages"); res.status != http.StatusNotFound {
		t.Errorf("the owner read a DM between two other people: %d %s", res.status, res.raw)
	}
	if res := c.owner.do(http.MethodDelete, "/api/messages/"+msgID, nil); res.status != http.StatusNotFound {
		t.Errorf("the owner removed a message from a DM they are not in: %d %s", res.status, res.raw)
	}
	// In a channel the owner is actually in, moderation does work.
	general := c.alice.generalID()
	public := c.alice.mustSay(general, "no canal publico")
	publicID := fmt.Sprintf("%.0f", public["id"].(float64))
	if res := c.owner.do(http.MethodDelete, "/api/messages/"+publicID, nil); res.status != http.StatusOK {
		t.Errorf("the owner could not remove a message from a shared channel: %d %s", res.status, res.raw)
	}
}

// An image is a read like any other, and it is the one that would be easiest to
// get wrong: a file served by id from a folder is the classic way a private
// picture becomes a public one.
func TestAnImageIsOnlyReadableByTheChannelItWasPostedIn(t *testing.T) {
	c := newCast(t)
	dm := c.owner.openDM("alice")
	up := c.owner.upload(dm, "foto.png", pngBytes(t, 8, 8))
	if up.status != http.StatusCreated {
		t.Fatalf("upload: %d %s", up.status, up.raw)
	}
	fileID, _ := up.body["id"].(string)
	c.owner.mustSay(dm, "olha isso", fileID)

	real := c.bo.get("/api/files/" + fileID)
	fake := c.bo.get("/api/files/AAAAAAAAAAAAAAAA")
	if real.status != http.StatusNotFound || real.raw != fake.raw {
		t.Errorf("Bo read an image from a DM he is not in: %d %q (an invented id gives %d %q)",
			real.status, real.raw, fake.status, fake.raw)
	}
	if got := c.alice.get("/api/files/" + fileID); got.status != http.StatusOK {
		t.Errorf("Alice cannot read an image from her own DM: %d", got.status)
	}

	// The file on disk is named after the row id and nothing else. Not the name
	// the uploader chose, not an extension: nothing a person typed reaches the
	// filesystem.
	entries, err := os.ReadDir(c.h.filesDir)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 1 || entries[0].Name() != fileID {
		names := []string{}
		for _, e := range entries {
			names = append(names, e.Name())
		}
		t.Errorf("files on disk = %v, want exactly [%s]", names, fileID)
	}
}

// A file that is gone from the disk is said to be gone. The move off the
// Contabo (18/09/2026) carried the database and not the upload folder, and the
// pictures from before it turned into a bare file name that led to a 404.
func TestAnAttachmentWhoseFileIsGoneSaysSo(t *testing.T) {
	c := newCast(t)
	dm := c.owner.openDM("alice")
	kept := c.owner.upload(dm, "fica.png", pngBytes(t, 8, 8))
	lost := c.owner.upload(dm, "some.png", pngBytes(t, 8, 8))
	keptID, _ := kept.body["id"].(string)
	lostID, _ := lost.body["id"].(string)
	c.owner.mustSay(dm, "duas fotos", keptID, lostID)
	if err := os.Remove(filepath.Join(c.h.filesDir, lostID)); err != nil {
		t.Fatal(err)
	}

	list := c.alice.history(dm)
	files, _ := list[len(list)-1]["attachments"].([]any)
	if len(files) != 2 {
		t.Fatalf("attachments = %v, want two", files)
	}
	for _, f := range files {
		a := f.(map[string]any)
		gone, _ := a["missing"].(bool)
		if want := a["id"] == lostID; gone != want {
			t.Errorf("attachment %v: missing = %v, want %v", a["id"], gone, want)
		}
	}
}

// ---------------------------------------------------------------------------
// "Uploads: tipo e tamanho validados no servidor, servidos com cabeçalhos que
//  impedem execução; nunca por caminho que o usuário escolha."
// ---------------------------------------------------------------------------

func TestUploadsAreCheckedByTheirBytesAndNotByTheirName(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()

	// An HTML file is the dangerous one, because a browser would run it. It is
	// refused whatever it is called.
	res := c.owner.upload(general, "pagina.gif", []byte("<html><script>alert(1)</script></html>"))
	if res.status != http.StatusUnsupportedMediaType || res.errorCode() != "bad_file_type" {
		t.Errorf("an HTML file was accepted as an image: %d %s", res.status, res.raw)
	}
	if entries, _ := os.ReadDir(c.h.filesDir); len(entries) != 0 {
		t.Errorf("a refused upload left %d file(s) on disk", len(entries))
	}

	// Since the D4i this service takes more than pictures, so a text file
	// called .png is no longer refused for being text — but the name still
	// decides nothing. It is stored, and served back, as what its bytes say it
	// is: a text file, which a browser is told to keep rather than to open.
	res = c.owner.upload(general, "inocente.png", []byte("isto aqui e um texto, nao uma imagem"))
	if res.status != http.StatusCreated {
		t.Fatalf("a text file was refused: %d %s", res.status, res.raw)
	}
	if res.body["mime"] != "text/plain" {
		t.Errorf("a text file named .png was stored as %v, want text/plain", res.body["mime"])
	}
	got := c.owner.get(res.body["url"].(string))
	if ct := got.header.Get("Content-Type"); ct != "text/plain" {
		t.Errorf("served as %q, want text/plain", ct)
	}
	if cd := got.header.Get("Content-Disposition"); !strings.HasPrefix(cd, "attachment;") {
		t.Errorf("served as %q, want an attachment", cd)
	}

	// Too big is refused before it is written.
	before, _ := os.ReadDir(c.h.filesDir)
	res = c.owner.upload(general, "grande.png", bytes.Repeat([]byte{0x89}, MaxUploadBytes+1024))
	if res.status != http.StatusRequestEntityTooLarge {
		t.Errorf("an oversized upload = %d, want 413", res.status)
	}
	if entries, _ := os.ReadDir(c.h.filesDir); len(entries) != len(before) {
		t.Errorf("an oversized upload left %d file(s) on disk", len(entries)-len(before))
	}

	// A real image is let through, and what comes back describes it.
	res = c.owner.upload(general, "foto.png", pngBytes(t, 12, 7))
	if res.status != http.StatusCreated {
		t.Fatalf("a real PNG was refused: %d %s", res.status, res.raw)
	}
	if res.body["mime"] != "image/png" || res.body["width"].(float64) != 12 || res.body["height"].(float64) != 7 {
		t.Errorf("upload described as %v", res.body)
	}
}

func TestAnImageIsServedSoThatNothingInItCanRun(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	up := c.owner.upload(general, "foto.png", pngBytes(t, 4, 4))
	if up.status != http.StatusCreated {
		t.Fatalf("upload: %d %s", up.status, up.raw)
	}
	res := c.owner.get("/api/files/" + up.body["id"].(string))
	if res.status != http.StatusOK {
		t.Fatalf("GET the image: %d", res.status)
	}
	want := map[string]string{
		"Content-Type":            "image/png",
		"X-Content-Type-Options":  "nosniff",
		"Content-Security-Policy": "default-src 'none'; sandbox",
	}
	for header, value := range want {
		if got := res.header.Get(header); got != value {
			t.Errorf("%s = %q, want %q", header, got, value)
		}
	}
	if cd := res.header.Get("Content-Disposition"); !strings.HasPrefix(cd, "inline;") {
		t.Errorf("Content-Disposition = %q", cd)
	}
}

// An attachment belongs to one person, one channel and one message. Without
// that, an id seen once could be re-posted anywhere.
func TestAnAttachmentCannotBeMovedToAnotherChannelOrPerson(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	dm := c.owner.openDM("alice")

	up := c.owner.upload(dm, "foto.png", pngBytes(t, 4, 4))
	fileID := up.body["id"].(string)

	if res := c.owner.say(general, "movido", fileID); res.status != http.StatusBadRequest {
		t.Errorf("an upload made for a DM was posted into a public channel: %d %s", res.status, res.raw)
	}
	if res := c.alice.say(dm, "roubado", fileID); res.status != http.StatusBadRequest {
		t.Errorf("Alice posted somebody else's upload: %d %s", res.status, res.raw)
	}
	if res := c.owner.say(dm, "certo", fileID); res.status != http.StatusCreated {
		t.Fatalf("the owner could not post their own upload: %d %s", res.status, res.raw)
	}
	// And not twice: an attachment is spent when it lands on a message.
	if res := c.owner.say(dm, "de novo", fileID); res.status != http.StatusBadRequest {
		t.Errorf("the same upload was posted twice: %d %s", res.status, res.raw)
	}
}

// ---------------------------------------------------------------------------
// "Limites por pessoa (mensagens por minuto, uploads por dia)"
// ---------------------------------------------------------------------------

func TestAPersonCannotFloodTheChannel(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	for i := 0; i < MessagesPerMinute; i++ {
		if res := c.owner.say(general, fmt.Sprintf("linha %d", i)); res.status != http.StatusCreated {
			t.Fatalf("message %d was refused: %d %s", i, res.status, res.raw)
		}
	}
	res := c.owner.say(general, "uma a mais")
	if res.status != http.StatusTooManyRequests || res.errorCode() != "message_limit" {
		t.Fatalf("message %d = %d %s, want 429 message_limit", MessagesPerMinute+1, res.status, res.raw)
	}
	// The limit is per person, not per channel and not for everybody: Alice is
	// unaffected by the owner's flood.
	if res := c.alice.say(general, "eu ainda falo"); res.status != http.StatusCreated {
		t.Errorf("Alice was caught by somebody else's limit: %d %s", res.status, res.raw)
	}
	// And it is a window, not a wall: with the clock moved on, the owner is
	// back.
	c.h.st.SetClock(func() time.Time { return time.Now().Add(2 * time.Minute) })
	if res := c.owner.say(general, "depois de um minuto"); res.status != http.StatusCreated {
		t.Errorf("the owner is still locked out after the window: %d %s", res.status, res.raw)
	}
}

func TestUploadsAreLimitedPerPersonPerDay(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	// Count the rows straight into the store rather than sending sixty real
	// files: what is being tested is the limit, and the row is what it counts.
	for i := 0; i < UploadsPerDay; i++ {
		if _, err := c.h.st.CreateAttachment(1, general, "x.png", "image/png", 10, 1, 1); err != nil {
			t.Fatal(err)
		}
	}
	res := c.owner.upload(general, "mais.png", pngBytes(t, 4, 4))
	if res.status != http.StatusTooManyRequests || res.errorCode() != "upload_limit" {
		t.Fatalf("upload past the daily limit = %d %s, want 429 upload_limit", res.status, res.raw)
	}
	if res := c.alice.upload(general, "minha.png", pngBytes(t, 4, 4)); res.status != http.StatusCreated {
		t.Errorf("Alice was caught by the owner's upload limit: %d %s", res.status, res.raw)
	}
}

// ---------------------------------------------------------------------------
// Mentions, unread counts, and the history itself
// ---------------------------------------------------------------------------

func TestAMentionCountsOnlyForThePersonNamed(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	c.owner.mustSay(general, "bom dia a todos")
	c.owner.mustSay(general, "@alice voce viu aquilo?")

	alice, _ := c.alice.channelByID(general)
	if alice["unread"].(float64) != 2 || alice["mentions"].(float64) != 1 {
		t.Errorf("Alice sees unread=%v mentions=%v, want 2 and 1", alice["unread"], alice["mentions"])
	}
	bo, _ := c.bo.channelByID(general)
	if bo["unread"].(float64) != 2 || bo["mentions"].(float64) != 0 {
		t.Errorf("Bo sees unread=%v mentions=%v, want 2 and 0", bo["unread"], bo["mentions"])
	}
	// The writer has not made anything unread for themselves.
	owner, _ := c.owner.channelByID(general)
	if owner["unread"].(float64) != 0 {
		t.Errorf("the writer has unread=%v in their own channel", owner["unread"])
	}

	// Reading clears both counts, and only for the person who read.
	last := c.alice.history(general)
	newest := last[len(last)-1]["id"].(float64)
	if res := c.alice.do(http.MethodPost, "/api/channels/"+general+"/read",
		map[string]any{"last_read_id": int64(newest)}); res.status != http.StatusOK {
		t.Fatalf("mark read: %d %s", res.status, res.raw)
	}
	alice, _ = c.alice.channelByID(general)
	if alice["unread"].(float64) != 0 || alice["mentions"].(float64) != 0 {
		t.Errorf("after reading, Alice still sees unread=%v mentions=%v", alice["unread"], alice["mentions"])
	}
	bo, _ = c.bo.channelByID(general)
	if bo["unread"].(float64) != 2 {
		t.Errorf("Alice reading changed Bo's count to %v", bo["unread"])
	}
}

// Naming somebody who cannot open the conversation must not put a badge on a
// door they have no key to.
func TestAMentionOfSomebodyOutsideTheConversationNotifiesNobody(t *testing.T) {
	c := newCast(t)
	dm := c.owner.openDM("alice")
	c.owner.mustSay(dm, "@bruno nem sabe que isto existe")

	for _, ch := range c.bo.channels() {
		if ch["mentions"].(float64) != 0 || ch["unread"].(float64) != 0 {
			t.Errorf("Bo has unread=%v mentions=%v on %v after being named in a DM he is not in",
				ch["unread"], ch["mentions"], ch["name"])
		}
	}
	// The mention did happen for the person who is in it.
	msgs := c.alice.history(dm)
	got, _ := msgs[0]["mentions"].([]any)
	if len(got) != 0 {
		t.Errorf("the DM recorded a mention of somebody outside it: %v", got)
	}
}

// The acceptance line of the sprint: history that survives restarting the
// server. The database is closed and reopened from the same file, which is
// what a restart is.
func TestHistorySurvivesARestart(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	c.owner.mustSay(general, "primeira")
	c.alice.mustSay(general, "segunda")
	c.bo.mustSay(general, "terceira")
	c.h.srv.Close()
	if err := c.h.st.Close(); err != nil {
		t.Fatal(err)
	}

	reopened, err := store.Open(c.h.dbPath)
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.Close()
	msgs, err := reopened.Messages(1, general, 0, 50)
	if err != nil {
		t.Fatal(err)
	}
	if len(msgs) != 3 {
		t.Fatalf("after a restart the channel holds %d messages, want 3", len(msgs))
	}
	want := []string{"primeira", "segunda", "terceira"}
	for i, m := range msgs {
		if m.Body != want[i] {
			t.Errorf("message %d is %q, want %q", i, m.Body, want[i])
		}
		if i > 0 && m.ID <= msgs[i-1].ID {
			t.Errorf("message ids are not in order: %d after %d", m.ID, msgs[i-1].ID)
		}
	}
}

// "Cada mensagem tem id e ordem definidos pelo servidor; edição e remoção ficam
// registradas (quem, quando)."
func TestEditAndRemovalStayOnTheRecord(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	posted := c.alice.mustSay(general, "texto original")
	id := fmt.Sprintf("%.0f", posted["id"].(float64))

	// Somebody else's message is not editable, owner or not.
	if res := c.bo.do(http.MethodPatch, "/api/messages/"+id, map[string]any{"body": "eu que escrevi"}); res.status != http.StatusNotFound {
		t.Errorf("Bo edited Alice's message: %d %s", res.status, res.raw)
	}
	if res := c.owner.do(http.MethodPatch, "/api/messages/"+id, map[string]any{"body": "corrigindo voce"}); res.status != http.StatusNotFound {
		t.Errorf("the owner edited somebody else's words: %d %s", res.status, res.raw)
	}

	edited := c.alice.do(http.MethodPatch, "/api/messages/"+id, map[string]any{"body": "texto corrigido"})
	if edited.status != http.StatusOK || edited.body["edited_at"] == nil {
		t.Fatalf("edit = %d %s, and it must record when", edited.status, edited.raw)
	}

	removed := c.owner.do(http.MethodDelete, "/api/messages/"+id, nil)
	if removed.status != http.StatusOK {
		t.Fatalf("remove: %d %s", removed.status, removed.raw)
	}
	if removed.body["deleted"] != true || removed.body["deleted_by"] != "tales" || removed.body["deleted_at"] == nil {
		t.Errorf("a removal must record who and when: %s", removed.raw)
	}
	// The tombstone keeps the id and the order; the words are gone.
	after := c.alice.history(general)
	if len(after) != 1 || after[0]["body"] != "" || after[0]["deleted"] != true {
		t.Errorf("after removal the history is %v", after)
	}

	// Gone from the file as well, not just from the answer.
	c.h.srv.Close()
	if err := c.h.st.Close(); err != nil {
		t.Fatal(err)
	}
	for _, suffix := range []string{"", "-wal", "-shm"} {
		raw, err := os.ReadFile(c.h.dbPath + suffix)
		if err != nil {
			continue
		}
		for _, gone := range []string{"texto original", "texto corrigido"} {
			if bytes.Contains(raw, []byte(gone)) {
				t.Errorf("%s still holds the text of a removed message (%q)", filepath.Base(c.h.dbPath+suffix), gone)
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Who may shape the place
// ---------------------------------------------------------------------------

func TestOnlyTheOwnerCreatesChannels(t *testing.T) {
	c := newCast(t)
	res := c.alice.do(http.MethodPost, "/api/channels", map[string]string{"name": "meu-canal", "topic": ""})
	if res.status != http.StatusForbidden || res.errorCode() != "not_allowed" {
		t.Fatalf("a member created a channel: %d %s", res.status, res.raw)
	}
	// The two channels every account starts with: #geral and the voice room
	// migration 3 added. Anything more means the refusal did not refuse.
	if list := c.alice.channels(); len(list) != 2 {
		t.Errorf("the refused channel appeared anyway: %v", list)
	}

	created := c.owner.do(http.MethodPost, "/api/channels", map[string]string{"name": "Jogos da Semana", "topic": "o que a gente joga"})
	if created.status != http.StatusCreated {
		t.Fatalf("the owner could not create a channel: %d %s", created.status, created.raw)
	}
	if created.body["name"] != "jogos-da-semana" {
		t.Errorf("channel name = %v, want jogos-da-semana", created.body["name"])
	}
	// A text channel is everybody's, the moment it exists.
	if _, ok := c.bo.channelByID(created.body["id"].(string)); !ok {
		t.Error("a new text channel did not appear for the other members")
	}
	// And the answer says who may do this, so the client does not have to
	// guess.
	if res := c.alice.get("/api/channels"); res.body["can_manage"] != false {
		t.Errorf("can_manage for a member = %v, want false", res.body["can_manage"])
	}
	if res := c.owner.get("/api/channels"); res.body["can_manage"] != true {
		t.Errorf("can_manage for the owner = %v, want true", res.body["can_manage"])
	}
}

// Opening the same conversation twice is the same conversation, whoever
// presses first.
func TestOpeningADirectMessageTwiceIsOneConversation(t *testing.T) {
	c := newCast(t)
	first := c.alice.openDM("bruno")
	second := c.bo.openDM("alice")
	if first != second {
		t.Fatalf("two conversations for one pair: %s and %s", first, second)
	}
	if c.alice.openDM("bruno") != first {
		t.Error("reopening made a third one")
	}
	// A conversation with somebody who does not exist is not a way to find out
	// that they do not exist.
	real := c.alice.do(http.MethodPost, "/api/dms", map[string]string{"username": "ninguem"})
	self := c.alice.do(http.MethodPost, "/api/dms", map[string]string{"username": "alice"})
	if real.status != http.StatusNotFound || self.status != http.StatusNotFound {
		t.Errorf("dm with a stranger = %d, with yourself = %d, want 404 for both", real.status, self.status)
	}
}

// ---------------------------------------------------------------------------
// "Logs sem conteúdo de mensagem"
// ---------------------------------------------------------------------------

func TestTheLogsNeverHoldWhatAnybodyWrote(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	dm := c.owner.openDM("alice")
	secrets := []string{"a senha do wifi e batatinha", "combinamos as sete", "@alice sozinha"}
	c.owner.mustSay(general, secrets[0])
	c.owner.mustSay(dm, secrets[1])
	c.owner.mustSay(general, secrets[2])
	up := c.owner.upload(general, "recibo-do-banco.png", pngBytes(t, 4, 4))
	if up.status != http.StatusCreated {
		t.Fatal(up.raw)
	}

	logs := c.h.logs.String()
	for _, secret := range secrets {
		if strings.Contains(logs, secret) {
			t.Errorf("a message body reached the logs: %q", secret)
		}
	}
	// Not the file name either: what somebody called a picture is theirs.
	if strings.Contains(logs, "recibo-do-banco") {
		t.Error("an uploaded file name reached the logs")
	}
}

// ---------------------------------------------------------------------------
// The live connection
// ---------------------------------------------------------------------------

// The socket fans out by membership, exactly like the reads do. Bo holds an
// open connection while the other two talk in private, and nothing arrives.
func TestTheLiveConnectionDeliversOnlyToMembers(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	dm := c.owner.openDM("alice")

	sock := c.bo.dial(t)
	defer sock.Close()

	c.owner.mustSay(dm, "isto e so nosso")
	c.owner.mustSay(general, "isto e de todos")

	// The public one arrives; the private one is not behind it, because events
	// are delivered in order on one socket.
	//
	// Presence events are skipped rather than asserted on: since D4, opening a
	// socket tells everybody listening that somebody arrived, and Bo's own
	// arrival is the first thing Bo hears about. What this test is about is
	// what does *not* arrive, and that is unchanged.
	var got gatewayEvent
	for {
		_ = sock.SetReadDeadline(time.Now().Add(5 * time.Second))
		if err := sock.ReadJSON(&got); err != nil {
			t.Fatalf("nothing arrived on the socket: %v", err)
		}
		if got.Type != "presence" {
			break
		}
	}
	if got.Type != "message" {
		t.Fatalf("first event is %q, want message", got.Type)
	}
	data, _ := got.Data.(map[string]any)
	if data["channel_id"] != general {
		t.Fatalf("the first event Bo received is from channel %v; the DM leaked", data["channel_id"])
	}
	if body, _ := data["body"].(string); body != "isto e de todos" {
		t.Fatalf("event body = %q", body)
	}
}

// A socket needs a session, and it needs to have been opened from this site.
func TestTheLiveConnectionNeedsASessionFromThisSite(t *testing.T) {
	h := newHarness(t)
	stranger := h.device("browser")
	if res := stranger.get("/ws"); res.status != http.StatusUnauthorized {
		t.Errorf("a signed-out visitor got %d from /ws, want 401", res.status)
	}
	owner := h.join(h.bootstrapInvite(), "tales", goodPass)
	res := owner.get("/ws", func(r *http.Request) {
		r.Header.Set("Origin", "https://nao-e-o-harmony.example")
		r.Header.Set("Connection", "Upgrade")
		r.Header.Set("Upgrade", "websocket")
		r.Header.Set("Sec-WebSocket-Version", "13")
		r.Header.Set("Sec-WebSocket-Key", base64.StdEncoding.EncodeToString(make([]byte, 16)))
	})
	if res.status != http.StatusForbidden {
		t.Errorf("a socket opened from another site got %d, want 403", res.status)
	}
}

// ---------------------------------------------------------------------------
// "responder uma mensagem" (D4c)
// ---------------------------------------------------------------------------

// A reply names one particular message, and the quotation drawn above it is
// read from that message every time rather than copied when the reply is
// written. Both halves are tested here: the one that keeps conversations apart
// (a reply may only ever point inside its own channel) and the one that makes
// the quotation honest (editing and removing the original show through).
func TestAReplyOnlyEverQuotesItsOwnConversation(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	original := c.owner.mustSay(general, "alguem vai jogar hoje?")
	originalID := int64(original["id"].(float64))

	answer := c.alice.do(http.MethodPost, "/api/channels/"+general+"/messages",
		map[string]any{"body": "eu vou", "attachments": []string{}, "reply_to": originalID})
	if answer.status != http.StatusCreated {
		t.Fatalf("replying inside a channel both people are in: %d %s", answer.status, answer.raw)
	}
	quote, _ := answer.body["reply_to"].(map[string]any)
	if quote == nil {
		t.Fatalf("the reply came back without the line it answers: %s", answer.raw)
	}
	if quote["body"] != "alguem vai jogar hoje?" {
		t.Errorf("quoted body = %q, want the original", quote["body"])
	}
	if who, _ := quote["author"].(map[string]any); who == nil || who["username"] != "tales" {
		t.Errorf("quoted author = %v, want tales", quote["author"])
	}

	// The part that is a rule and not a feature. Bruno is in #geral, so he may
	// write there — but the message he points at is from a conversation he is
	// not in, and a server that quoted it would have published a private line
	// into a public channel.
	dm := c.owner.openDM("alice")
	private := c.owner.mustSay(dm, "isso e so entre a gente")
	privateID := int64(private["id"].(float64))

	real := c.bo.do(http.MethodPost, "/api/channels/"+general+"/messages",
		map[string]any{"body": "olha isso", "attachments": []string{}, "reply_to": privateID})
	fake := c.bo.do(http.MethodPost, "/api/channels/"+general+"/messages",
		map[string]any{"body": "olha isso", "attachments": []string{}, "reply_to": 999999})
	if real.status != http.StatusNotFound {
		t.Errorf("Bruno replied to a message from a DM he is not in: %d %s", real.status, real.raw)
	}
	// And it has to fail the *same way* an invented number does, or the code
	// alone would confirm that id 4 exists somewhere on this server.
	if real.status != fake.status || real.raw != fake.raw {
		t.Errorf("a real id from another conversation answers %d %q and an invented one %d %q; they must be identical",
			real.status, real.raw, fake.status, fake.raw)
	}
	if strings.Contains(real.raw, "so entre a gente") {
		t.Error("the refusal carried the text of the message it refused to quote")
	}
	// Nothing was written either: a refused reply is not a message.
	for _, m := range c.bo.history(general) {
		if m["body"] == "olha isso" {
			t.Error("the refused reply was posted anyway")
		}
	}

	// The quotation is the original as it is now, not as it was. Editing it
	// shows through everywhere it is quoted...
	if res := c.owner.do(http.MethodPatch, fmt.Sprintf("/api/messages/%d", originalID),
		map[string]any{"body": "alguem vai jogar amanha?"}); res.status != http.StatusOK {
		t.Fatalf("editing the original: %d %s", res.status, res.raw)
	}
	if got := quotedIn(t, c.alice.history(general), "eu vou"); got["body"] != "alguem vai jogar amanha?" {
		t.Errorf("after the edit the quotation still reads %q", got["body"])
	}

	// ...and so does removing it, which must leave a tombstone rather than the
	// text living on inside somebody's answer.
	if res := c.owner.do(http.MethodDelete, fmt.Sprintf("/api/messages/%d", originalID), nil); res.status != http.StatusOK {
		t.Fatalf("removing the original: %d %s", res.status, res.raw)
	}
	got := quotedIn(t, c.alice.history(general), "eu vou")
	if got["deleted"] != true {
		t.Errorf("the quotation of a removed message says deleted=%v", got["deleted"])
	}
	if got["body"] != "" {
		t.Errorf("the quotation of a removed message still carries %q", got["body"])
	}
	// The answer itself survives: removing one line does not remove the
	// replies to it (which is why the column is not ON DELETE CASCADE).
	if quotedIn(t, c.alice.history(general), "eu vou") == nil {
		t.Error("the reply disappeared when the message it answered was removed")
	}
}

// quotedIn finds the message with this body in a page of history and returns
// the line it is answering.
func quotedIn(t *testing.T, page []map[string]any, body string) map[string]any {
	t.Helper()
	for _, m := range page {
		if m["body"] == body {
			q, _ := m["reply_to"].(map[string]any)
			if q == nil {
				t.Fatalf("message %q came back with no reply_to", body)
			}
			return q
		}
	}
	t.Fatalf("no message %q in the page", body)
	return nil
}

// ---------------------------------------------------------------------------
// "silenciar um canal" e "fechar uma conversa" (D4e)
// ---------------------------------------------------------------------------

// Both of these are one person's opinion about one channel, so both have to be
// exactly that: invisible to everybody else, and impossible to apply to a
// channel you are not in.
func TestSilencingAndClosingAreYourOwnAndNobodyElseIsTouched(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	dm := c.owner.openDM("alice")
	c.alice.mustSay(dm, "coisa que nao pode sumir")

	// Bruno is not in that conversation, so both of these have to fail the same
	// way an invented id fails — a different answer would already confirm it
	// exists.
	for _, path := range []string{"/mute", "/hide"} {
		real := c.bo.do(http.MethodPost, "/api/channels/"+dm+path, map[string]any{"muted": true})
		fake := c.bo.do(http.MethodPost, "/api/channels/AAAAAAAAAAAAAAAA"+path, map[string]any{"muted": true})
		if real.status != http.StatusNotFound {
			t.Errorf("POST %s on somebody else's conversation = %d, want 404", path, real.status)
		}
		if real.status != fake.status || real.raw != fake.raw {
			t.Errorf("%s: a real id answers %d %q and an invented one %d %q; they must be identical",
				path, real.status, real.raw, fake.status, fake.raw)
		}
	}

	// Alice silences #geral. It is hers alone.
	if res := c.alice.do(http.MethodPost, "/api/channels/"+general+"/mute",
		map[string]any{"muted": true}); res.status != http.StatusOK {
		t.Fatalf("silencing a channel she is in: %d %s", res.status, res.raw)
	}
	c.bo.mustSay(general, "oi @alice, oi @bruno")

	alice, ok := c.alice.channelByID(general)
	if !ok {
		t.Fatal("#geral vanished from Alice's list when she silenced it")
	}
	if alice["muted"] != true {
		t.Errorf("Alice's #geral says muted=%v", alice["muted"])
	}
	// Silenced means it asks for nothing at all: not a count, not a mention.
	if alice["unread"] != float64(0) || alice["mentions"] != float64(0) {
		t.Errorf("a silenced channel still counts: unread=%v mentions=%v",
			alice["unread"], alice["mentions"])
	}
	// And it is still all there when she opens it.
	if got := len(c.alice.history(general)); got == 0 {
		t.Error("a silenced channel lost its messages")
	}
	// The owner, who silenced nothing, is counted as always.
	owner, _ := c.owner.channelByID(general)
	if owner["muted"] != false || owner["unread"] == float64(0) {
		t.Errorf("Alice's silence reached the owner: muted=%v unread=%v",
			owner["muted"], owner["unread"])
	}

	// Closing a conversation takes it out of the column and deletes nothing.
	if res := c.owner.do(http.MethodPost, "/api/channels/"+dm+"/hide", nil); res.status != http.StatusOK {
		t.Fatalf("closing his own conversation: %d %s", res.status, res.raw)
	}
	if _, ok := c.owner.channelByID(dm); ok {
		t.Error("a closed conversation is still in the column")
	}
	if _, ok := c.alice.channelByID(dm); !ok {
		t.Error("closing a conversation closed it for the other person too")
	}
	if got := len(c.owner.history(dm)); got == 0 {
		t.Error("closing a conversation lost its messages")
	}

	// It comes back when there is something in it — a closed conversation that
	// swallowed messages would be a blocklist nobody was told about.
	c.alice.mustSay(dm, "voltei")
	if _, ok := c.owner.channelByID(dm); !ok {
		t.Error("a message arrived in a closed conversation and it stayed closed")
	}

	// And closing it again, then asking for it, brings it straight back.
	if res := c.owner.do(http.MethodPost, "/api/channels/"+dm+"/hide", nil); res.status != http.StatusOK {
		t.Fatalf("closing it again: %d %s", res.status, res.raw)
	}
	if again := c.owner.openDM("alice"); again != dm {
		t.Errorf("reopening made a second conversation: %s and %s", again, dm)
	}
	if _, ok := c.owner.channelByID(dm); !ok {
		t.Error("reopening a closed conversation did not bring it back")
	}

	// A channel of the server is not a tab, so it cannot be closed: somebody
	// who could make #geral disappear would have no way back into it.
	if res := c.owner.do(http.MethodPost, "/api/channels/"+general+"/hide", nil); res.status != http.StatusNotFound {
		t.Errorf("a shared channel was closed: %d %s", res.status, res.raw)
	}
}

// ---------------------------------------------------------------------------
// "está digitando" e "o apelido que só você vê" (D4f)
// ---------------------------------------------------------------------------

// Typing is a fact about a channel, so it obeys the channel's rule: only
// members may say it, and only members are told.
func TestTypingOnlyReachesTheChannelItIsIn(t *testing.T) {
	c := newCast(t)
	dm := c.owner.openDM("alice")

	real := c.bo.do(http.MethodPost, "/api/channels/"+dm+"/typing", map[string]any{})
	fake := c.bo.do(http.MethodPost, "/api/channels/AAAAAAAAAAAAAAAA/typing", map[string]any{})
	if real.status != http.StatusNotFound {
		t.Errorf("Bruno said he was typing in a conversation he is not in: %d %s", real.status, real.raw)
	}
	if real.status != fake.status || real.raw != fake.raw {
		t.Errorf("a real id answers %d %q and an invented one %d %q; they must be identical",
			real.status, real.raw, fake.status, fake.raw)
	}

	// And a member of it is answered, without a word of it being stored: the
	// only way to know it happened is to be listening at the time.
	if res := c.alice.do(http.MethodPost, "/api/channels/"+dm+"/typing", map[string]any{}); res.status != http.StatusNoContent {
		t.Fatalf("Alice typing in her own conversation: %d %s", res.status, res.raw)
	}
}

// A nickname is one person's private name for another. "Private" here is not a
// promise the interface keeps — it is that no query takes anybody's id but the
// asker's — so this checks it from the only place it could leak: somebody
// else's list of people.
func TestANicknameIsOnlyEverSeenByThePersonWhoGaveIt(t *testing.T) {
	c := newCast(t)

	if res := c.owner.do(http.MethodPut, "/api/people/alice/nickname",
		map[string]any{"name": "chefinha"}); res.status != http.StatusOK {
		t.Fatalf("giving somebody a name: %d %s", res.status, res.raw)
	}

	nicknameIn := func(d *device, about string) string {
		d.h.t.Helper()
		res := d.get("/api/members")
		if res.status != http.StatusOK {
			d.h.t.Fatalf("GET /api/members: %d %s", res.status, res.raw)
		}
		raw, _ := res.body["members"].([]any)
		for _, m := range raw {
			person := m.(map[string]any)
			if person["username"] == about {
				name, _ := person["nickname"].(string)
				return name
			}
		}
		d.h.t.Fatalf("%s is not in the list of people", about)
		return ""
	}

	if got := nicknameIn(c.owner, "alice"); got != "chefinha" {
		t.Errorf("the owner's own list says %q, want chefinha", got)
	}
	// The two people it is not about see nothing — including Alice herself,
	// who is not told what other people call her.
	for _, d := range []*device{c.alice, c.bo} {
		if got := nicknameIn(d, "alice"); got != "" {
			t.Errorf("somebody else's list carries the owner's private name: %q", got)
		}
	}
	// Nothing of it appears anywhere in their answer, either.
	if res := c.bo.get("/api/members"); strings.Contains(res.raw, "chefinha") {
		t.Error("the word appears in a list it does not belong in")
	}

	// Each person has their own, and they do not collide.
	if res := c.bo.do(http.MethodPut, "/api/people/alice/nickname",
		map[string]any{"name": "a vizinha"}); res.status != http.StatusOK {
		t.Fatalf("Bruno giving her his own name for her: %d %s", res.status, res.raw)
	}
	if got := nicknameIn(c.owner, "alice"); got != "chefinha" {
		t.Errorf("Bruno's name overwrote the owner's: %q", got)
	}
	if got := nicknameIn(c.bo, "alice"); got != "a vizinha" {
		t.Errorf("Bruno's own name came back as %q", got)
	}

	// An empty name takes it away.
	if res := c.owner.do(http.MethodPut, "/api/people/alice/nickname",
		map[string]any{"name": "  "}); res.status != http.StatusOK {
		t.Fatalf("clearing it: %d %s", res.status, res.raw)
	}
	if got := nicknameIn(c.owner, "alice"); got != "" {
		t.Errorf("after clearing it, the list still says %q", got)
	}

	// And a name for somebody who does not exist answers the same as anything
	// else that does not exist, which is how nobody finds out who has an
	// account here.
	ghost := c.owner.do(http.MethodPut, "/api/people/ninguem/nickname", map[string]any{"name": "x"})
	if ghost.status != http.StatusNotFound {
		t.Errorf("naming a person who does not exist: %d %s", ghost.status, ghost.raw)
	}
}

// ---------------------------------------------------------------------------
// reacting to a message (D4f, second half)
// ---------------------------------------------------------------------------

// react presses (or unpresses) one emoji on one message, as this device.
func (d *device) react(messageID float64, emoji string, on bool) reply {
	d.h.t.Helper()
	return d.do(http.MethodPost, fmt.Sprintf("/api/messages/%d/reactions", int64(messageID)),
		map[string]any{"emoji": emoji, "on": on})
}

// reactionsOf reads the reactions off a message as the API answers with them:
// each emoji, and everybody who pressed it.
func reactionsOf(m map[string]any) map[string][]string {
	out := map[string][]string{}
	raw, _ := m["reactions"].([]any)
	for _, r := range raw {
		one := r.(map[string]any)
		names := []string{}
		people, _ := one["users"].([]any)
		for _, p := range people {
			names = append(names, p.(string))
		}
		out[one["emoji"].(string)] = names
	}
	return out
}

// The rule of the whole service, applied to the newest table: a reaction is
// authorised by the same read that authorises seeing the message at all, so
// somebody outside the conversation gets the answer an invented id gets —
// byte for byte, which is what keeps a 404 from being an oracle.
func TestAReactionOnlyReachesAMessageYouCanSee(t *testing.T) {
	c := newCast(t)
	dm := c.owner.openDM("alice")
	msg := c.owner.mustSay(dm, "só entre nós")
	id := msg["id"].(float64)

	real := c.bo.react(id, "👍", true)
	fake := c.bo.do(http.MethodPost, "/api/messages/999999/reactions",
		map[string]any{"emoji": "👍", "on": true})
	if real.status != http.StatusNotFound {
		t.Errorf("Bruno reacted to a message from a conversation he is not in: %d %s", real.status, real.raw)
	}
	if real.status != fake.status || real.raw != fake.raw {
		t.Errorf("a real id answers %d %q and an invented one %d %q; they must be identical",
			real.status, real.raw, fake.status, fake.raw)
	}
	// And nothing of it was written: the message Alice reads is untouched.
	if got := reactionsOf(c.alice.history(dm)[0]); len(got) != 0 {
		t.Errorf("the refused press left something behind: %v", got)
	}

	// The person who *is* in the conversation is let through.
	if res := c.alice.react(id, "👍", true); res.status != http.StatusOK {
		t.Fatalf("Alice reacting in her own conversation: %d %s", res.status, res.raw)
	}
}

// The button under a message is a switch, and the row it is in is shared.
func TestAReactionIsASwitchAndCountsEachPersonOnce(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	id := c.owner.mustSay(general, "olha isso")["id"].(float64)

	// Pressing it twice is the same fact arriving twice, not two reactions —
	// two devices of one person do exactly this.
	c.owner.react(id, "🔥", true)
	res := c.owner.react(id, "🔥", true)
	if got := reactionsOf(res.body)["🔥"]; len(got) != 1 || got[0] != "tales" {
		t.Errorf("pressing twice gave %v, want one press by tales", got)
	}

	// Somebody else joins it, and the names are there — "quem foi que riu
	// disso" is most of the point.
	c.alice.react(id, "🔥", true)
	got := reactionsOf(c.bo.history(general)[len(c.bo.history(general))-1])["🔥"]
	if len(got) != 2 {
		t.Fatalf("two people pressed it and the message says %v", got)
	}

	// Pressing again takes it back, and takes back only your own.
	if res := c.owner.react(id, "🔥", false); res.status != http.StatusOK {
		t.Fatalf("taking a reaction back: %d %s", res.status, res.raw)
	}
	after := reactionsOf(c.alice.history(general)[len(c.alice.history(general))-1])["🔥"]
	if len(after) != 1 || after[0] != "alice" {
		t.Errorf("after the owner took his back, it says %v, want alice alone", after)
	}

	// The last one out takes the emoji with it: a button showing zero is a
	// button that does nothing.
	c.alice.react(id, "🔥", false)
	if got := reactionsOf(c.owner.history(general)[len(c.owner.history(general))-1]); len(got) != 0 {
		t.Errorf("an emoji nobody is pressing is still there: %v", got)
	}
}

// A free-text column hanging off a message is a second message body — one with
// no length limit worth the name, no rate limit and no way to remove it. This
// is the check that keeps it from being one.
func TestOnlyAnEmojiMayBeAReaction(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	id := c.owner.mustSay(general, "reajam aí")["id"].(float64)

	refused := []struct{ what, why string }{
		{"", "nothing at all"},
		{"   ", "a space"},
		{"lol", "a word"},
		{"👍 isso aqui é um recado inteiro", "a sentence with an emoji at the front"},
		{"1", "a digit"},
		{"<b>oi</b>", "markup"},
		{"👍👍👍👍👍👍👍👍👍👍👍👍👍👍", "a wall of them"},
	}
	for _, bad := range refused {
		res := c.owner.react(id, bad.what, true)
		if res.status != http.StatusBadRequest {
			t.Errorf("%s was accepted as a reaction: %d %s", bad.why, res.status, res.raw)
		}
	}

	// And the ones a telephone keyboard actually produces are let in, several
	// code points and all: a skin tone is two, a family joined with zero-width
	// joiners is seven.
	for _, good := range []string{"👍", "❤️", "👏🏽", "👨‍👩‍👧‍👦", "🇧🇷"} {
		if res := c.owner.react(id, good, true); res.status != http.StatusOK {
			t.Errorf("%q is an emoji and was refused: %d %s", good, res.status, res.raw)
		}
	}

	// Six is the limit, and the seventh is refused rather than silently
	// dropped — the row under a message is shared, so the way to spoil it is
	// to fill it.
	if res := c.owner.react(id, "🎉", true); res.status != http.StatusOK {
		t.Fatalf("the sixth: %d %s", res.status, res.raw)
	}
	if res := c.owner.react(id, "😀", true); res.status != http.StatusBadRequest {
		t.Errorf("a seventh reaction from one person: %d %s", res.status, res.raw)
	}
	// Pressing one that is already lit keeps working at the limit: it changes
	// nothing, and refusing it would be refusing a button that is on.
	if res := c.owner.react(id, "👍", false); res.status != http.StatusOK {
		t.Errorf("taking one back while at the limit: %d %s", res.status, res.raw)
	}
}

// A tombstone has nothing to have an opinion about.
func TestARemovedMessageCarriesNoReactions(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	id := c.owner.mustSay(general, "isso vai embora")["id"].(float64)
	c.alice.react(id, "😂", true)

	if res := c.owner.do(http.MethodDelete, fmt.Sprintf("/api/messages/%d", int64(id)), nil); res.status != http.StatusOK {
		t.Fatalf("removing it: %d %s", res.status, res.raw)
	}
	last := c.alice.history(general)[len(c.alice.history(general))-1]
	if last["deleted"] != true {
		t.Fatalf("the message is not a tombstone: %v", last)
	}
	if got := reactionsOf(last); len(got) != 0 {
		t.Errorf("a removed message still carries %v", got)
	}
	if res := c.bo.react(id, "😂", true); res.status != http.StatusBadRequest {
		t.Errorf("reacting to a removed message: %d %s", res.status, res.raw)
	}
}

// ---------------------------------------------------------------------------
// D4i — the search box. The rule of the house on the newest surface: what is
// findable is exactly what is readable, and a word in somebody else's
// conversation is not distinguishable from a word nobody ever wrote.
// ---------------------------------------------------------------------------

func (d *device) search(q string) []map[string]any {
	d.h.t.Helper()
	res := d.get("/api/search?q=" + url.QueryEscape(q))
	if res.status != http.StatusOK {
		d.h.t.Fatalf("search %q: %d %s", q, res.status, res.raw)
	}
	out := []map[string]any{}
	list, _ := res.body["messages"].([]any)
	for _, one := range list {
		if m, ok := one.(map[string]any); ok {
			out = append(out, m)
		}
	}
	return out
}

func TestSearchOnlyFindsWhatThePersonCouldAlreadyRead(t *testing.T) {
	c := newCast(t)

	// A word written in a conversation between two other people.
	private := c.alice.openDM("bruno")
	c.alice.mustSay(private, "a senha do wifi e girassol")

	// And the same word in the channel everybody is in.
	general := c.owner.generalID()
	c.owner.mustSay(general, "o girassol do vizinho floresceu")

	// The owner of the server is not the owner of everybody's conversations.
	found := c.owner.search("girassol")
	if len(found) != 1 {
		t.Fatalf("the owner found %d messages, want only the public one: %v", len(found), found)
	}
	if found[0]["channel_id"] != general {
		t.Errorf("the owner found a message from %v", found[0]["channel_id"])
	}

	// Alice, who is in both, finds both.
	if got := len(c.alice.search("girassol")); got != 2 {
		t.Errorf("Alice found %d of her own messages, want 2", got)
	}

	// And a word that exists only in that private conversation answers the
	// owner exactly what a word nobody ever wrote answers.
	real := c.owner.search("senha")
	invented := c.owner.search("hipopotamo")
	if len(real) != 0 || len(real) != len(invented) {
		t.Errorf("a private word gave the owner %v; an invented one gives %v", real, invented)
	}
}

func TestSearchLeavesOutTombstonesAndTheServersOwnLines(t *testing.T) {
	c := newCast(t)
	dm := c.owner.openDM("alice")
	msg := c.owner.mustSay(dm, "isto vai ser apagado")
	id := fmt.Sprintf("%.0f", msg["id"].(float64))
	if res := c.owner.do(http.MethodDelete, "/api/messages/"+id, nil); res.status != http.StatusOK {
		t.Fatalf("delete: %d %s", res.status, res.raw)
	}
	if got := c.owner.search("apagado"); len(got) != 0 {
		t.Errorf("a removed message was found: %v", got)
	}

	// The line a missed call leaves is not something anybody searches for.
	ws := c.alice.dial(t)
	defer ws.Close()
	res := c.owner.call("alice")
	callID, _ := res.body["id"].(string)
	waitEvent(t, ws, "call")
	c.owner.do(http.MethodPost, "/api/calls/"+callID+"/hangup", map[string]any{"reason": "missed"})
	if got := c.owner.search("missed"); len(got) != 0 {
		t.Errorf("the server's own notice was found by searching: %v", got)
	}
}

// ---------------------------------------------------------------------------
// D4i — the line a call leaves behind. Only calls nobody answered leave one,
// and it lands in the conversation those two people have and in no other.
// ---------------------------------------------------------------------------

func TestACallNobodyAnsweredLeavesALineInTheConversation(t *testing.T) {
	c := newCast(t)
	ws := c.alice.dial(t)
	defer ws.Close()

	res := c.owner.call("alice")
	if res.status != http.StatusCreated {
		t.Fatalf("call: %d %s", res.status, res.raw)
	}
	id, _ := res.body["id"].(string)
	waitEvent(t, ws, "call")

	// Nobody picks up, and the caller's browser says so.
	if got := c.owner.do(http.MethodPost, "/api/calls/"+id+"/hangup",
		map[string]any{"reason": "missed"}); got.status != http.StatusOK {
		t.Fatalf("hangup: %d %s", got.status, got.raw)
	}

	// Both ends see it, in the conversation the two of them have.
	dm := c.alice.openDM("tales")
	lines := c.alice.history(dm)
	if len(lines) != 1 {
		t.Fatalf("the conversation has %d lines, want the missed call: %v", len(lines), lines)
	}
	if lines[0]["kind"] != "call" || lines[0]["body"] != "missed" {
		t.Errorf("the line is %v, want a missed call", lines[0])
	}
	if author, _ := lines[0]["author"].(map[string]any); author["username"] != "tales" {
		t.Errorf("the missed call is from %v, want the person who rang", author)
	}

	// And it is theirs: Bruno is not in that conversation and it does not exist
	// for him.
	if got := c.bo.get("/api/channels/" + dm + "/messages"); got.status != http.StatusNotFound {
		t.Errorf("Bruno read a missed call between two other people: %d %s", got.status, got.raw)
	}
	if got := c.bo.search("missed"); len(got) != 0 {
		t.Errorf("Bruno found it by searching: %v", got)
	}
}

func TestACallThatWasAnsweredLeavesNothingBehind(t *testing.T) {
	c := newCast(t)
	ws := c.alice.dial(t)
	defer ws.Close()

	res := c.owner.call("alice")
	id, _ := res.body["id"].(string)
	waitEvent(t, ws, "call")
	if got := c.alice.do(http.MethodPost, "/api/calls/"+id+"/accept", nil); got.status != http.StatusOK {
		t.Fatalf("accept: %d %s", got.status, got.raw)
	}
	if got := c.owner.do(http.MethodPost, "/api/calls/"+id+"/hangup",
		map[string]any{"reason": "hangup"}); got.status != http.StatusOK {
		t.Fatalf("hangup: %d %s", got.status, got.raw)
	}

	// Nothing anywhere says the two of them spoke. That is the D4b promise and
	// the D4i does not quietly take it back.
	dm := c.alice.openDM("tales")
	if lines := c.alice.history(dm); len(lines) != 0 {
		t.Errorf("an answered call left %v behind", lines)
	}
}

// ---------------------------------------------------------------------------
// D4i — "@todos", which is every member of this channel and nobody else.
// ---------------------------------------------------------------------------

func TestEveryoneMentionsThisChannelAndNotTheServer(t *testing.T) {
	c := newCast(t)
	dm := c.owner.openDM("alice")
	msg := c.owner.mustSay(dm, "@todos vamos jogar?")

	named := map[string]bool{}
	for _, one := range msg["mentions"].([]any) {
		named[one.(string)] = true
	}
	if !named["alice"] {
		t.Errorf("@todos in a conversation did not name the other person: %v", msg["mentions"])
	}
	if named["tales"] {
		t.Errorf("@todos named the person who wrote it: %v", msg["mentions"])
	}
	// Bruno is not in this conversation, so "everybody" does not include him —
	// a notification pointing at a conversation he cannot open is the bug this
	// rule exists to prevent.
	if named["bruno"] {
		t.Errorf("@todos reached outside the conversation: %v", msg["mentions"])
	}
}
