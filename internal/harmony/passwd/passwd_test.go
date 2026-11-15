package passwd

import (
	"strings"
	"testing"
)

// cheap keeps the suite fast. Every test that cares about the *format* of a
// hash still reads it from these, so turning the cost down cannot hide a bug in
// the encoding.
var cheap = Params{Memory: 8 * 1024, Time: 1, Parallelism: 1, SaltLen: 16, KeyLen: 32}

func TestHashIsArgon2idPHCString(t *testing.T) {
	h, err := Hash("correct horse battery staple", cheap)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(h, "$argon2id$v=19$") {
		t.Fatalf("hash is not argon2id: %q", h)
	}
	if strings.Contains(h, "correct horse") {
		t.Fatal("the password itself is inside the hash")
	}
	if n := len(strings.Split(h, "$")); n != 6 {
		t.Fatalf("PHC string has %d fields, want 6: %q", n, h)
	}
}

func TestVerify(t *testing.T) {
	h, err := Hash("a password long enough", cheap)
	if err != nil {
		t.Fatal(err)
	}
	ok, err := Verify(h, "a password long enough")
	if err != nil || !ok {
		t.Fatalf("right password rejected: ok=%v err=%v", ok, err)
	}
	ok, err = Verify(h, "a password long enougH")
	if err != nil {
		t.Fatal(err)
	}
	if ok {
		t.Fatal("wrong password accepted")
	}
}

func TestSaltIsPerHash(t *testing.T) {
	a, _ := Hash("same password here", cheap)
	b, _ := Hash("same password here", cheap)
	if a == b {
		t.Fatal("two hashes of the same password are identical: the salt is not random")
	}
}

// A hash whose parameters were tampered with, or that is not argon2id at all,
// must be an error and never a match. Returning "false, nil" would be enough
// to make a corrupted row look like a wrong password instead of a broken
// database.
func TestMalformedHashIsAnError(t *testing.T) {
	good, _ := Hash("a password long enough", cheap)
	cases := map[string]string{
		"empty":          "",
		"not phc":        "hunter2",
		"wrong algo":     strings.Replace(good, "argon2id", "argon2i", 1),
		"wrong version":  strings.Replace(good, "v=19", "v=16", 1),
		"missing fields": "$argon2id$v=19$m=8,t=1,p=1$only-salt",
		"bad base64":     strings.Replace(good, "$m=", "$!!$m=", 1),
	}
	for name, h := range cases {
		t.Run(name, func(t *testing.T) {
			ok, err := Verify(h, "a password long enough")
			if err == nil {
				t.Fatalf("malformed hash accepted without error (ok=%v)", ok)
			}
			if ok {
				t.Fatal("malformed hash reported as a match")
			}
		})
	}
}

// The stored parameters, not the current Default, decide how a hash is checked.
// Without this a change of cost would lock everybody out of their account.
func TestVerifyUsesTheParametersInTheHash(t *testing.T) {
	old := Params{Memory: 8 * 1024, Time: 2, Parallelism: 1, SaltLen: 16, KeyLen: 32}
	h, err := Hash("a password long enough", old)
	if err != nil {
		t.Fatal(err)
	}
	ok, err := Verify(h, "a password long enough")
	if err != nil || !ok {
		t.Fatalf("hash made with older parameters no longer verifies: ok=%v err=%v", ok, err)
	}
}
