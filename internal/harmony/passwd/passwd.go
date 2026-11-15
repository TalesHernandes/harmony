// Package passwd hashes and verifies account passwords with argon2id.
//
// The encoded form is the standard PHC string, the same one Django, PHP and
// the argon2 reference CLI produce:
//
//	$argon2id$v=19$m=65536,t=3,p=2$<b64 salt>$<b64 hash>
//
// Storing the parameters next to the hash is what makes them changeable later:
// a stronger cost can be rolled in without invalidating every old password,
// because each hash still says how it was made.
package passwd

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"strings"

	"golang.org/x/crypto/argon2"
)

// Params are the argon2id cost settings.
type Params struct {
	Memory      uint32 // KiB
	Time        uint32
	Parallelism uint8
	SaltLen     uint32
	KeyLen      uint32
}

// Default is sized for the 4 vCPU / 8 GB box: 64 MiB and 3 passes take about
// 60 ms there, which is slow enough to make offline cracking expensive and
// fast enough that twenty friends logging in never notice.
var Default = Params{Memory: 64 * 1024, Time: 3, Parallelism: 2, SaltLen: 16, KeyLen: 32}

var (
	ErrFormat    = errors.New("passwd: malformed hash")
	ErrAlgorithm = errors.New("passwd: unsupported algorithm")
	ErrVersion   = errors.New("passwd: unsupported argon2 version")
)

// MinLength is the shortest password the service accepts. Length beats
// character classes; there is no "must contain a symbol" rule.
const MinLength = 10

// MaxLength caps the work a single request can ask of the CPU.
const MaxLength = 200

// Hash derives an argon2id hash of password with p and returns its PHC string.
func Hash(password string, p Params) (string, error) {
	salt := make([]byte, p.SaltLen)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}
	key := argon2.IDKey([]byte(password), salt, p.Time, p.Memory, p.Parallelism, p.KeyLen)
	enc := base64.RawStdEncoding
	return fmt.Sprintf("$argon2id$v=%d$m=%d,t=%d,p=%d$%s$%s",
		argon2.Version, p.Memory, p.Time, p.Parallelism,
		enc.EncodeToString(salt), enc.EncodeToString(key)), nil
}

// Verify reports whether password matches the encoded PHC hash. A malformed
// hash is an error, never a match.
func Verify(encoded, password string) (bool, error) {
	p, salt, want, err := decode(encoded)
	if err != nil {
		return false, err
	}
	got := argon2.IDKey([]byte(password), salt, p.Time, p.Memory, p.Parallelism, uint32(len(want)))
	return subtle.ConstantTimeCompare(got, want) == 1, nil
}

func decode(encoded string) (p Params, salt, key []byte, err error) {
	parts := strings.Split(encoded, "$")
	if len(parts) != 6 || parts[0] != "" {
		return p, nil, nil, ErrFormat
	}
	if parts[1] != "argon2id" {
		return p, nil, nil, ErrAlgorithm
	}
	var version int
	if _, err := fmt.Sscanf(parts[2], "v=%d", &version); err != nil {
		return p, nil, nil, ErrFormat
	}
	if version != argon2.Version {
		return p, nil, nil, ErrVersion
	}
	if _, err := fmt.Sscanf(parts[3], "m=%d,t=%d,p=%d", &p.Memory, &p.Time, &p.Parallelism); err != nil {
		return p, nil, nil, ErrFormat
	}
	enc := base64.RawStdEncoding
	if salt, err = enc.DecodeString(parts[4]); err != nil {
		return p, nil, nil, ErrFormat
	}
	if key, err = enc.DecodeString(parts[5]); err != nil {
		return p, nil, nil, ErrFormat
	}
	if len(salt) == 0 || len(key) == 0 {
		return p, nil, nil, ErrFormat
	}
	p.SaltLen, p.KeyLen = uint32(len(salt)), uint32(len(key))
	return p, salt, key, nil
}
