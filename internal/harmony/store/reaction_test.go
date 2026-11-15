package store

import (
	"os"
	"regexp"
	"testing"
)

// Every emoji the reaction panel offers is one the server takes. The list is
// read from the client's own source rather than typed out here, so an emoji
// added to the panel and refused by the server fails this test the day it is
// added — which is how "não estou conseguindo reagir com alguns emojis"
// (the owner, 24/09/2026) went unnoticed: the keycaps, ‼️, 〰️ and the arrows
// are not "symbols" to Unicode's categories, and the check only knew those.
func TestEveryEmojiInThePanelIsAValidReaction(t *testing.T) {
	src, err := os.ReadFile("../../../web/harmony/src/emoji.ts")
	if err != nil {
		t.Fatal(err)
	}
	entries := regexp.MustCompile(`\['([^']+)', '[^']*'\]`).FindAllStringSubmatch(string(src), -1)
	if len(entries) < 1000 {
		t.Fatalf("read only %d emoji from emoji.ts; the pattern no longer matches the file", len(entries))
	}
	var refused []string
	for _, e := range entries {
		if !ValidReaction(e[1]) {
			refused = append(refused, e[1])
		}
	}
	if len(refused) > 0 {
		t.Fatalf("%d emoji in the panel are refused as reactions: %q", len(refused), refused)
	}
}

// And the shape check still keeps text out: a reaction is a picture, not a
// word, a digit or punctuation.
func TestAReactionIsNotText(t *testing.T) {
	for _, s := range []string{"a", "ok", "1", "#", "!", "...", "1 ", "<b>", "#1", "‼x", "⃣", "‍", "️"} {
		if ValidReaction(s) {
			t.Errorf("%q was taken as a reaction", s)
		}
	}
	for _, s := range []string{"👍", "#️⃣", "1️⃣", "*⃣", "‼️", "〰️", "↔️", "🇧🇷", "👩‍❤️‍💋‍👨", "👍🏽", "🏴‍☠️"} {
		if !ValidReaction(s) {
			t.Errorf("%q was refused", s)
		}
	}
}
