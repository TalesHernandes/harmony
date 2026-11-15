package api

import (
	"fmt"
	"net/http"
	"runtime"
	"testing"

	"call/internal/harmony/store"
)

func TestLayoutSnapshotKeepsRoomsWithTheirFolders(t *testing.T) {
	h := newHarness(t)
	rooms, err := h.st.Placements()
	if err != nil {
		t.Fatal(err)
	}
	a, err := h.st.CreateCategory("First")
	if err != nil {
		t.Fatal(err)
	}
	b, err := h.st.CreateCategory("Second")
	if err != nil {
		t.Fatal(err)
	}
	save := func(first, second string) error {
		for i := range rooms {
			rooms[i].CategoryID = first
		}
		return h.st.SaveLayout([]string{first, second}, rooms)
	}
	if err := save(a.ID, b.ID); err != nil {
		t.Fatal(err)
	}
	stop := make(chan struct{})
	finished := make(chan error, 1)
	go func() {
		first, second := b.ID, a.ID
		for {
			select {
			case <-stop:
				finished <- nil
				return
			default:
			}
			if err := save(first, second); err != nil {
				finished <- err
				return
			}
			first, second = second, first
		}
	}()
	defer func() {
		close(stop)
		if err := <-finished; err != nil {
			t.Error(err)
		}
	}()
	for i := 0; i < 500; i++ {
		data, err := h.api.layoutJSON()
		if err != nil {
			t.Fatal(err)
		}
		first := data["categories"].([]map[string]any)[0]["id"].(string)
		for _, room := range data["channels"].([]map[string]any) {
			if room["category_id"] != first {
				t.Fatalf("iteration %d: folder order and room placement came from different layouts", i)
			}
		}
		runtime.Gosched()
	}
}

// D12a: renaming rooms, folders, the order of the column, and pins.

func (d *device) categories() []map[string]any {
	d.h.t.Helper()
	res := d.get("/api/channels")
	if res.status != http.StatusOK {
		d.h.t.Fatalf("GET /api/channels: %d %s", res.status, res.raw)
	}
	raw, _ := res.body["categories"].([]any)
	out := make([]map[string]any, 0, len(raw))
	for _, c := range raw {
		out = append(out, c.(map[string]any))
	}
	return out
}

func (d *device) mustCategory(name string) string {
	d.h.t.Helper()
	res := d.do(http.MethodPost, "/api/categories", map[string]string{"name": name})
	if res.status != http.StatusCreated {
		d.h.t.Fatalf("create category: %d %s", res.status, res.raw)
	}
	return res.body["id"].(string)
}

func (d *device) mustRoom(kind, name string) string {
	d.h.t.Helper()
	res := d.do(http.MethodPost, "/api/channels", map[string]string{"name": name, "kind": kind})
	if res.status != http.StatusCreated {
		d.h.t.Fatalf("create %s: %d %s", kind, res.status, res.raw)
	}
	return res.body["id"].(string)
}

type place struct{ id, folder string }

// layout builds the body of PUT /api/layout.
func layout(folders []string, rooms ...place) map[string]any {
	chans := make([]map[string]string, 0, len(rooms))
	for _, r := range rooms {
		chans = append(chans, map[string]string{"id": r.id, "category_id": r.folder})
	}
	if folders == nil {
		folders = []string{}
	}
	return map[string]any{"categories": folders, "channels": chans}
}

// Every new route that changes the column, asked by somebody who is not the
// owner, is refused — and the column is exactly what it was.
func TestOnlyTheOwnerShapesTheColumn(t *testing.T) {
	c := newCast(t)
	general, voice := c.owner.generalID(), c.owner.voiceID()
	folder := c.owner.mustCategory("Jogos")
	before := c.owner.get("/api/channels").raw

	attempts := []struct {
		method, path string
		body         any
	}{
		{http.MethodPatch, "/api/channels/" + general, map[string]string{"name": "renomeado"}},
		{http.MethodPatch, "/api/channels/" + voice, map[string]string{"name": "renomeada"}},
		{http.MethodPost, "/api/categories", map[string]string{"name": "Minha"}},
		{http.MethodPatch, "/api/categories/" + folder, map[string]string{"name": "Outra"}},
		{http.MethodDelete, "/api/categories/" + folder, nil},
		{http.MethodPut, "/api/layout", layout([]string{folder}, place{general, folder}, place{voice, folder})},
	}
	for _, member := range []*device{c.alice, c.bo} {
		for _, a := range attempts {
			res := member.do(a.method, a.path, a.body)
			if res.status != http.StatusForbidden || res.body["error"] != "not_allowed" {
				t.Fatalf("%s %s by a member: %d %s", a.method, a.path, res.status, res.raw)
			}
		}
	}
	if after := c.owner.get("/api/channels").raw; after != before {
		t.Fatalf("a refused request changed the column:\n%s\n%s", before, after)
	}
}

func TestRenamingAChannelReachesEverybodyWithTheirOwnCounters(t *testing.T) {
	c := newCast(t)
	general, voice := c.owner.generalID(), c.owner.voiceID()
	c.owner.mustSay(general, "uma")
	c.owner.mustSay(general, "duas")

	ws := c.alice.dial(t)
	defer ws.Close()
	waitEvent(t, ws, "presence")

	// Both rooms called geral are renamed, each on its own.
	res := c.owner.do(http.MethodPatch, "/api/channels/"+general, map[string]string{"name": "Sala Principal"})
	if res.status != http.StatusOK || res.body["name"] != "sala-principal" {
		t.Fatalf("rename text: %d %s", res.status, res.raw)
	}
	ev := eventData(t, waitEvent(t, ws, "channel"))
	if ev["id"] != general || ev["name"] != "sala-principal" {
		t.Fatalf("event: %v", ev)
	}
	// Alice's own counters, not zeros: the apps replace the whole row.
	if ev["unread"].(float64) != 2 || ev["last_message_id"].(float64) == 0 {
		t.Fatalf("the event lost alice's counters: %v", ev)
	}
	if res := c.owner.do(http.MethodPatch, "/api/channels/"+voice, map[string]string{"name": "papo"}); res.status != http.StatusOK {
		t.Fatalf("rename voice: %d %s", res.status, res.raw)
	}
	if ch, _ := c.bo.channelByID(voice); ch["name"] != "papo" {
		t.Fatalf("voice not renamed for bruno: %v", ch)
	}

	// The slug rule and the per-kind uniqueness of creating one.
	other := c.owner.mustRoom("text", "minecraft")
	if res := c.owner.do(http.MethodPatch, "/api/channels/"+other, map[string]string{"name": "sala-principal"}); res.status != http.StatusConflict {
		t.Fatalf("taken name: %d %s", res.status, res.raw)
	}
	if res := c.owner.do(http.MethodPatch, "/api/channels/"+other, map[string]string{"name": "papo"}); res.status != http.StatusOK {
		t.Fatalf("a text channel may share a voice room's name: %d %s", res.status, res.raw)
	}
	if res := c.owner.do(http.MethodPatch, "/api/channels/"+other, map[string]string{"name": "!!!"}); res.status != http.StatusBadRequest {
		t.Fatalf("bad name: %d %s", res.status, res.raw)
	}

	// A conversation is never renamed — not even one the owner is in — and
	// one between two others answers like an invented id.
	mine := c.owner.openDM("alice")
	theirs := c.alice.openDM("bruno")
	fake := c.owner.do(http.MethodPatch, "/api/channels/invented", map[string]string{"name": "x"})
	if fake.status != http.StatusNotFound {
		t.Fatalf("invented: %d %s", fake.status, fake.raw)
	}
	for _, id := range []string{mine, theirs} {
		res := c.owner.do(http.MethodPatch, "/api/channels/"+id, map[string]string{"name": "x"})
		if res.status != fake.status || res.raw != fake.raw {
			t.Fatalf("dm %s: %d %s", id, res.status, res.raw)
		}
	}
}

func TestFoldersHoldTextAndVoiceAndDeletingOneKeepsTheRooms(t *testing.T) {
	c := newCast(t)
	general, voice := c.owner.generalID(), c.owner.voiceID()
	mine := c.owner.mustRoom("text", "minecraft")
	c.owner.mustSay(mine, "a historia fica")

	ws := c.bo.dial(t)
	defer ws.Close()
	waitEvent(t, ws, "presence")

	empty := c.owner.mustCategory("  Vazia  ")
	ev := eventData(t, waitEvent(t, ws, "layout"))
	if cats := ev["categories"].([]any); len(cats) != 1 || cats[0].(map[string]any)["name"] != "Vazia" {
		t.Fatalf("an empty folder is a folder: %v", ev)
	}
	games := c.owner.mustCategory("Jogos")
	waitEvent(t, ws, "layout")
	if res := c.owner.do(http.MethodPost, "/api/categories", map[string]string{"name": "jogos"}); res.status != http.StatusConflict {
		t.Fatalf("same heading twice: %d %s", res.status, res.raw)
	}
	if res := c.owner.do(http.MethodPost, "/api/categories", map[string]string{"name": "   "}); res.status != http.StatusBadRequest {
		t.Fatalf("empty heading: %d %s", res.status, res.raw)
	}

	// Jogos above Vazia; minecraft and the voice room inside Jogos.
	res := c.owner.do(http.MethodPut, "/api/layout", layout([]string{games, empty},
		place{general, ""}, place{mine, games}, place{voice, games}))
	if res.status != http.StatusOK {
		t.Fatalf("layout: %d %s", res.status, res.raw)
	}
	waitEvent(t, ws, "layout")
	for _, d := range []*device{c.alice, c.bo} {
		cats := d.categories()
		if len(cats) != 2 || cats[0]["id"] != games || cats[1]["id"] != empty {
			t.Fatalf("folder order: %v", cats)
		}
		for _, id := range []string{mine, voice} {
			if ch, _ := d.channelByID(id); ch["category_id"] != games {
				t.Fatalf("%s not in Jogos: %v", id, ch)
			}
		}
	}
	if ids, _ := c.h.st.VoiceChannelIDs(1); len(ids) != 1 {
		t.Fatalf("voice ids: %v", ids)
	}

	// Renaming a folder.
	if res := c.owner.do(http.MethodPatch, "/api/categories/"+games, map[string]string{"name": "Jogatina"}); res.status != http.StatusNoContent {
		t.Fatalf("rename folder: %d %s", res.status, res.raw)
	}
	waitEvent(t, ws, "layout")

	// Deleting it deletes no room and no message: both land at the top.
	if res := c.owner.do(http.MethodDelete, "/api/categories/"+games, nil); res.status != http.StatusNoContent {
		t.Fatalf("delete folder: %d %s", res.status, res.raw)
	}
	ev = eventData(t, waitEvent(t, ws, "layout"))
	if len(ev["categories"].([]any)) != 1 {
		t.Fatalf("folder still listed: %v", ev)
	}
	for _, id := range []string{mine, voice} {
		ch, ok := c.alice.channelByID(id)
		if !ok || ch["category_id"] != "" {
			t.Fatalf("room lost with its folder: %v %v", ok, ch)
		}
	}
	if h := c.alice.history(mine); len(h) != 1 || h[0]["body"] != "a historia fica" {
		t.Fatalf("history after deleting the folder: %v", h)
	}
	if res := c.owner.do(http.MethodDelete, "/api/categories/"+games, nil); res.status != http.StatusNotFound {
		t.Fatalf("delete twice: %d %s", res.status, res.raw)
	}
}

// The order is written whole or not at all, and only when it names exactly
// the folders and rooms there are.
func TestTheOrderIsWrittenWholeOrNotAtAll(t *testing.T) {
	c := newCast(t)
	general, voice := c.owner.generalID(), c.owner.voiceID()
	a := c.owner.mustRoom("text", "a")
	b := c.owner.mustRoom("text", "b")
	folder := c.owner.mustCategory("Pasta")
	dm := c.alice.openDM("bruno")
	before := c.owner.get("/api/channels").raw

	stale := []map[string]any{
		layout([]string{folder}, place{general, folder}, place{a, folder}, place{b, folder}, place{b, folder}),          // same size, duplicate discovered after writes
		layout([]string{folder}, place{general, folder}, place{a, folder}, place{b, folder}, place{dm, folder}),         // same size, a DM replaces the voice room
		layout([]string{folder}, place{general, folder}, place{a, folder}, place{b, folder}, place{"invented", folder}), // same size, invented room
		layout([]string{folder}, place{general, ""}, place{a, ""}, place{voice, ""}),                                    // b missing
		layout([]string{folder}, place{general, ""}, place{a, ""}, place{b, ""}, place{b, ""}, place{voice, ""}),        // b twice
		layout([]string{}, place{general, ""}, place{a, ""}, place{b, ""}, place{voice, ""}),                            // folder missing
		layout([]string{folder, "inventada"}, place{general, ""}, place{a, ""}, place{b, ""}, place{voice, ""}),         // invented folder
		layout([]string{folder}, place{general, "inventada"}, place{a, ""}, place{b, ""}, place{voice, ""}),             // into an invented folder
		layout([]string{folder}, place{general, ""}, place{a, ""}, place{b, ""}, place{voice, ""}, place{dm, folder}),   // somebody's DM
	}
	for i, body := range stale {
		res := c.owner.do(http.MethodPut, "/api/layout", body)
		if res.status != http.StatusConflict || res.body["error"] != "stale_layout" {
			t.Fatalf("case %d: %d %s", i, res.status, res.raw)
		}
	}
	if after := c.owner.get("/api/channels").raw; after != before {
		t.Fatal("a refused order was partly written")
	}

	// b above a, both in the folder; the list comes back in that order, and
	// a text channel stays above a voice room in the same folder.
	res := c.owner.do(http.MethodPut, "/api/layout", layout([]string{folder},
		place{general, ""}, place{voice, folder}, place{b, folder}, place{a, folder}))
	if res.status != http.StatusOK {
		t.Fatalf("layout: %d %s", res.status, res.raw)
	}
	var texts []string
	for _, ch := range c.alice.channels() {
		if ch["kind"] == "text" {
			texts = append(texts, ch["id"].(string))
		}
	}
	if fmt.Sprint(texts) != fmt.Sprint([]string{general, b, a}) {
		t.Fatalf("order: %v", texts)
	}
}

func TestPinningFollowsWhoMayAndAStrangersPinLooksInvented(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	voice := c.owner.voiceID()
	voiceMessage, err := c.h.st.PostMessage(1, voice, "test", nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	for _, d := range []*device{c.owner, c.alice, c.bo} {
		for _, method := range []string{http.MethodPost, http.MethodDelete} {
			res := d.do(method, fmt.Sprintf("/api/messages/%d/pin", voiceMessage.ID), nil)
			fake := d.do(method, "/api/messages/999999/pin", nil)
			if res.status != http.StatusNotFound || res.raw != fake.raw {
				t.Fatalf("voice pin: %d %s; invented: %d %s", res.status, res.raw, fake.status, fake.raw)
			}
		}
	}
	public := fmt.Sprint(c.alice.mustSay(general, "fixa isso")["id"])
	ours := c.alice.openDM("bruno")
	private := fmt.Sprint(c.alice.mustSay(ours, "so nos dois")["id"])

	// A member who is not the owner cannot pin in a channel.
	if res := c.alice.do(http.MethodPost, "/api/messages/"+public+"/pin", nil); res.status != http.StatusForbidden {
		t.Fatalf("member pins in a channel: %d %s", res.status, res.raw)
	}
	if res := c.bo.do(http.MethodDelete, "/api/messages/"+public+"/pin", nil); res.status != http.StatusForbidden {
		t.Fatalf("member unpins in a channel: %d %s", res.status, res.raw)
	}

	// Pinning a message in somebody else's conversation answers exactly what
	// an invented id does — for the owner too.
	for _, method := range []string{http.MethodPost, http.MethodDelete} {
		fake := c.owner.do(method, "/api/messages/999999/pin", nil)
		res := c.owner.do(method, "/api/messages/"+private+"/pin", nil)
		if res.status != http.StatusNotFound || res.status != fake.status || res.raw != fake.raw {
			t.Fatalf("%s a stranger's pin: %d %s; invented: %d %s", method, res.status, res.raw, fake.status, fake.raw)
		}
	}
	fakeList := c.owner.get("/api/channels/invented/pins")
	if res := c.owner.get("/api/channels/" + ours + "/pins"); res.status != http.StatusNotFound || res.raw != fakeList.raw {
		t.Fatalf("a stranger's pin list: %d %s", res.status, res.raw)
	}

	// In a conversation of two, both pin and unpin, and the other side sees it.
	ws := c.alice.dial(t)
	defer ws.Close()
	waitEvent(t, ws, "presence")
	if res := c.bo.do(http.MethodPost, "/api/messages/"+private+"/pin", nil); res.status != http.StatusOK || res.body["pinned"] != true {
		t.Fatalf("bruno pins: %d %s", res.status, res.raw)
	}
	if ev := eventData(t, waitEvent(t, ws, "message_changed")); ev["pinned"] != true {
		t.Fatalf("event: %v", ev)
	}
	list := c.alice.get("/api/channels/" + ours + "/pins")
	if msgs := list.body["messages"].([]any); len(msgs) != 1 {
		t.Fatalf("alice's pin list: %s", list.raw)
	}
	if res := c.alice.do(http.MethodDelete, "/api/messages/"+private+"/pin", nil); res.status != http.StatusOK || res.body["pinned"] != nil {
		t.Fatalf("alice unpins: %d %s", res.status, res.raw)
	}

	// The owner pins in a channel; the message carries it in the history.
	if res := c.owner.do(http.MethodPost, "/api/messages/"+public+"/pin", nil); res.status != http.StatusOK {
		t.Fatalf("owner pins: %d %s", res.status, res.raw)
	}
	// The last line is the pin notice; the pinned message is the one before.
	if h := c.bo.history(general); h[len(h)-2]["pinned"] != true || h[len(h)-1]["kind"] != "pin" {
		t.Fatalf("history: %v", h[len(h)-2:])
	}
	// Removing a pinned message takes the pin away.
	if res := c.alice.do(http.MethodDelete, "/api/messages/"+public, nil); res.status != http.StatusOK || res.body["pinned"] != nil {
		t.Fatalf("remove: %d %s", res.status, res.raw)
	}
	if msgs := c.owner.get("/api/channels/" + general + "/pins").body["messages"].([]any); len(msgs) != 0 {
		t.Fatalf("a removed message is still pinned: %v", msgs)
	}
	if res := c.owner.do(http.MethodPost, "/api/messages/"+public+"/pin", nil); res.status != http.StatusNotFound {
		t.Fatalf("pinning a removed message: %d %s", res.status, res.raw)
	}
}

func TestAChannelHoldsAtMostItsCeilingOfPins(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	// Written through the store: that many posts through the API would trip
	// the per-minute limit, which is a different test.
	var ids []int64
	for i := 0; i <= store.MaxPinsPerChannel; i++ {
		m, err := c.h.st.PostMessage(1, general, fmt.Sprint("linha ", i), nil, 0)
		if err != nil {
			t.Fatal(err)
		}
		ids = append(ids, m.ID)
	}
	for _, id := range ids[:store.MaxPinsPerChannel] {
		if res := c.owner.do(http.MethodPost, fmt.Sprintf("/api/messages/%d/pin", id), nil); res.status != http.StatusOK {
			t.Fatalf("pin %d: %d %s", id, res.status, res.raw)
		}
	}
	// Pinning one already pinned is not one past the ceiling.
	if res := c.owner.do(http.MethodPost, fmt.Sprintf("/api/messages/%d/pin", ids[0]), nil); res.status != http.StatusOK {
		t.Fatalf("pin again: %d %s", res.status, res.raw)
	}
	last := fmt.Sprintf("/api/messages/%d/pin", ids[store.MaxPinsPerChannel])
	if res := c.owner.do(http.MethodPost, last, nil); res.status != http.StatusConflict || res.body["error"] != "pin_limit" {
		t.Fatalf("one past the ceiling: %d %s", res.status, res.raw)
	}
	c.owner.do(http.MethodDelete, fmt.Sprintf("/api/messages/%d/pin", ids[0]), nil)
	if res := c.owner.do(http.MethodPost, last, nil); res.status != http.StatusOK {
		t.Fatalf("after unpinning one: %d %s", res.status, res.raw)
	}
	list := c.alice.get("/api/channels/" + general + "/pins")
	msgs := list.body["messages"].([]any)
	if len(msgs) != store.MaxPinsPerChannel || fmt.Sprint(msgs[0].(map[string]any)["id"]) != fmt.Sprint(ids[store.MaxPinsPerChannel]) {
		t.Fatalf("pin list: %d, first %v", len(msgs), msgs[0])
	}
}

// A new pin leaves one line in the channel, from whoever pinned, answering the
// pinned message; pinning it again or unpinning it leaves nothing (30/10/2026).
func TestANewPinLeavesOneLineInTheChannel(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	said := fmt.Sprint(c.alice.mustSay(general, "o servidor abre às 20h")["id"])
	notes := func() []map[string]any {
		var out []map[string]any
		for _, m := range c.alice.history(general) {
			if m["kind"] == "pin" {
				out = append(out, m)
			}
		}
		return out
	}
	pin := "/api/messages/" + said + "/pin"
	if res := c.owner.do(http.MethodPost, pin, nil); res.status != http.StatusOK {
		t.Fatalf("pin: %d %s", res.status, res.raw)
	}
	got := notes()
	if len(got) != 1 {
		t.Fatalf("pin lines: %v", got)
	}
	note := got[0]
	if note["author"].(map[string]any)["username"] != "tales" || note["body"] != store.PinNoteBody ||
		fmt.Sprint(note["reply_to"].(map[string]any)["id"]) != said {
		t.Fatalf("pin line: %v", note)
	}
	c.owner.do(http.MethodPost, pin, nil)
	c.owner.do(http.MethodDelete, pin, nil)
	if got := notes(); len(got) != 1 {
		t.Fatalf("pinning again or unpinning wrote a line: %v", got)
	}
	// The line is not itself pinnable, and it is not something to find.
	if res := c.owner.do(http.MethodPost, fmt.Sprintf("/api/messages/%v/pin", note["id"]), nil); res.status == http.StatusOK {
		t.Fatalf("pinning the pin line: %d %s", res.status, res.raw)
	}
}

// Folders, order, names and pins are in the database, so a restart keeps them.
func TestTheColumnSurvivesARestart(t *testing.T) {
	c := newCast(t)
	general := c.owner.generalID()
	folder := c.owner.mustCategory("Jogos")
	mine := c.owner.mustRoom("text", "minecraft")
	voice := c.owner.voiceID()
	if res := c.owner.do(http.MethodPut, "/api/layout", layout([]string{folder},
		place{general, ""}, place{mine, folder}, place{voice, folder})); res.status != http.StatusOK {
		t.Fatalf("layout: %d %s", res.status, res.raw)
	}
	c.owner.do(http.MethodPatch, "/api/channels/"+mine, map[string]string{"name": "mine"})
	msg := c.owner.mustSay(mine, "fixada")
	c.owner.do(http.MethodPost, fmt.Sprintf("/api/messages/%v/pin", msg["id"]), nil)
	c.h.srv.Close()
	if err := c.h.st.Close(); err != nil {
		t.Fatal(err)
	}

	st, err := store.Open(c.h.dbPath)
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	cats, _ := st.Categories()
	if len(cats) != 1 || cats[0].Name != "Jogos" {
		t.Fatalf("folders: %v", cats)
	}
	list, _ := st.ListChannels(2)
	found := 0
	for _, ch := range list {
		if ch.ID == mine && ch.Name == "mine" && ch.CategoryID == folder {
			found++
		}
		if ch.ID == voice && ch.CategoryID == folder {
			found++
		}
	}
	if found != 2 {
		t.Fatalf("placement lost: %+v", list)
	}
	pins, _ := st.Pins(2, mine)
	if len(pins) != 1 || !pins[0].Pinned {
		t.Fatalf("pins: %+v", pins)
	}
}
