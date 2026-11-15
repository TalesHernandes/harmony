package store

import (
	"errors"
	"fmt"
	"reflect"
	"sync"
	"testing"
)

func TestConcurrentPinsKeepTheFiftyPinLimit(t *testing.T) {
	st := memStore(t)
	owner := signUp(t, st, "owner")
	var ids []int64
	for i := 0; i < MaxPinsPerChannel+1; i++ {
		msg, err := st.PostMessage(owner.ID, "geral", fmt.Sprint(i), nil, 0)
		if err != nil {
			t.Fatal(err)
		}
		ids = append(ids, msg.ID)
		if i < MaxPinsPerChannel-1 {
			if _, _, err := st.Pin(owner.ID, msg.ID, true, true); err != nil {
				t.Fatal(err)
			}
		}
	}
	start := make(chan struct{})
	results := make(chan error, 2)
	for _, id := range ids[MaxPinsPerChannel-1:] {
		go func(id int64) {
			<-start
			_, _, err := st.Pin(owner.ID, id, true, true)
			results <- err
		}(id)
	}
	close(start)
	var pinned, refused int
	for i := 0; i < 2; i++ {
		switch err := <-results; {
		case err == nil:
			pinned++
		case errors.Is(err, ErrPinLimit):
			refused++
		default:
			t.Fatal(err)
		}
	}
	list, err := st.Pins(owner.ID, "geral")
	if err != nil || len(list) != MaxPinsPerChannel || pinned != 1 || refused != 1 {
		t.Fatalf("pins=%d, successes=%d, refused=%d, err=%v", len(list), pinned, refused, err)
	}
}

func TestConcurrentLayoutsAreWholeAndDeletingAFolderKeepsRooms(t *testing.T) {
	st := memStore(t)
	owner := signUp(t, st, "owner")
	msg, err := st.PostMessage(owner.ID, "geral", "retained", nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	a, err := st.CreateCategory("First")
	if err != nil {
		t.Fatal(err)
	}
	b, err := st.CreateCategory("Second")
	if err != nil {
		t.Fatal(err)
	}
	orders := [][]string{{a.ID, b.ID}, {b.ID, a.ID}}
	rooms := [][]Placement{
		{{ID: "geral", CategoryID: a.ID}, {ID: "voz-geral", CategoryID: b.ID}},
		{{ID: "geral", CategoryID: b.ID}, {ID: "voz-geral", CategoryID: a.ID}},
	}
	start := make(chan struct{})
	results := make(chan error, 2)
	for i := range orders {
		go func(i int) { <-start; results <- st.SaveLayout(orders[i], rooms[i]) }(i)
	}
	close(start)
	for i := 0; i < 2; i++ {
		if err := <-results; err != nil {
			t.Fatal(err)
		}
	}
	cats, err := st.Categories()
	if err != nil {
		t.Fatal(err)
	}
	places, err := st.Placements()
	if err != nil {
		t.Fatal(err)
	}
	winner := 0
	if cats[0].ID == b.ID {
		winner = 1
	}
	if !reflect.DeepEqual(places, rooms[winner]) {
		t.Fatalf("mixed layout: folders=%v, rooms=%v", cats, places)
	}
	start = make(chan struct{})
	go func() { <-start; results <- st.SaveLayout(orders[0], rooms[0]) }()
	go func() { <-start; results <- st.DeleteCategory(a.ID) }()
	close(start)
	for i := 0; i < 2; i++ {
		if err := <-results; err != nil && !errors.Is(err, ErrStaleLayout) {
			t.Fatal(err)
		}
	}
	places, err = st.Placements()
	if err != nil || len(places) != 2 {
		t.Fatalf("rooms after deletion: %v, %v", places, err)
	}
	for _, p := range places {
		if p.CategoryID == a.ID {
			t.Fatal("room still references deleted folder")
		}
	}
	if got, err := st.MessageFor(owner.ID, msg.ID); err != nil || got.Body != "retained" {
		t.Fatalf("message after folder deletion: %+v, %v", got, err)
	}
}

func TestPinRacingDeletionLeavesNoPin(t *testing.T) {
	st := memStore(t)
	owner := signUp(t, st, "owner")
	room, err := st.createRoom(ChannelText, owner.ID, "pins", "")
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 200; i++ {
		msg, err := st.PostMessage(owner.ID, room.ID, "test", nil, 0)
		if err != nil {
			t.Fatal(err)
		}
		start := make(chan struct{})
		results := make(chan error, 2)
		var wg sync.WaitGroup
		wg.Add(2)
		go func() {
			defer wg.Done()
			<-start
			_, _, err := st.Pin(owner.ID, msg.ID, true, true)
			if errors.Is(err, ErrNotFound) {
				err = nil
			}
			results <- err
		}()
		go func() {
			defer wg.Done()
			<-start
			_, err := st.DeleteMessage(owner.ID, msg.ID, OthersAnywhere)
			results <- err
		}()
		close(start)
		wg.Wait()
		for j := 0; j < 2; j++ {
			if err := <-results; err != nil {
				t.Fatal(err)
			}
		}
		var count int
		if err := st.db.QueryRow(`SELECT COUNT(*) FROM pins WHERE message_id = ?`, msg.ID).Scan(&count); err != nil {
			t.Fatal(err)
		}
		if count != 0 {
			t.Fatalf("iteration %d: deleted message %d still has a pin", i, msg.ID)
		}
	}
}
