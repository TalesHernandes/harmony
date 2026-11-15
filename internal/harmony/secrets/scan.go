// Package secrets scans files for credentials that should never be committed.
//
// PLANO-DISCORD.md §5 promises that secrets live only in environment variables
// on the machine, "nunca no código nem no git. O repositório é verificado por
// isso". This package is that verification. It is deliberately narrow: it looks
// for the shapes a real leak takes here — a filled-in secret environment
// variable, a private key block, a committed .env — rather than guessing at
// entropy, because a scanner that cries wolf gets switched off.
package secrets

import (
	"bufio"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// Finding is one suspicious line.
type Finding struct {
	Path   string
	Line   int
	Reason string
	// Value is what matched. It is short by construction and is what an
	// allow-list entry names, so an exception is about one exact string and not
	// about a whole file.
	Value string
}

// assignment matches a filled-in secret variable in any file that uses
// shell-style assignment: .env files, Dockerfiles, compose files, Makefiles,
// scripts. An empty value — the shape of a .example template — is a template
// and not a finding.
// The name has to be SCREAMING_SNAKE, the shape an environment variable takes.
// Matching case-insensitively would flag every Go identifier called "secret" in
// the server packages and the scanner would be switched off inside a week.
//
// Three things this pattern got wrong until 20/09/2026, and all three were the
// names this very project uses. The prefix allowed **one** segment, so
// TURN_SECRET matched and HARMONY_TURN_SECRET did not — and there is no word
// boundary inside an underscored name for the match to restart at. The list
// said PRIVATE_KEY, so HARMONY_VAPID_PRIVATE went straight through. And only a
// double quote could open a value, so TURN_SECRET='...' went through too. A
// scanner that cannot see the shape of its own secrets is a green light, which
// is worse than no scanner at all.
// PRIVATE is its own alternative, with nothing allowed after it but _KEY,
// because a name that goes on — TURN_PRIVATE_IP — is an address and not a key.
var assignment = regexp.MustCompile(
	`\b([A-Z0-9_]*(?:SECRET|PASSPHRASE|PASSWORD|API_KEY|ACCESS_KEY)[A-Z0-9_]*|[A-Z0-9_]*PRIVATE(?:_KEY)?)\s*[=:]\s*(["']?)([^"'\s#]+)`)

// keyBlock is the header of a PEM private key, the age identity file, and an
// OpenSSH private key — the three kinds of key file this project handles.
var keyBlock = regexp.MustCompile(`-----BEGIN [A-Z ]*PRIVATE KEY-----|AGE-SECRET-KEY-1[0-9A-Z]+`)

// placeholders are values that are obviously not credentials: the empty
// template, the shell expansions a compose file uses to pass a variable through
// without ever holding it, and the format verbs of a line that prints one.
var placeholders = regexp.MustCompile(`^(\$|\{|<|%|\\|\.\.\.|xxx+|TODO|CHANGEME|""|''|your[-_])`)

// SkipDirs are trees that are not ours to police: dependencies, build output
// and the git database itself.
var SkipDirs = map[string]bool{
	".git": true, "node_modules": true, "dist": true,
	"test-results": true, "playwright-report": true, ".pw-browsers": true,
}

// Scan reads each path (relative to root) and reports what looks like a
// committed credential.
func Scan(root string, paths []string) ([]Finding, error) {
	var out []Finding
	for _, rel := range paths {
		if skip(rel) {
			continue
		}
		f, err := os.Open(filepath.Join(root, filepath.FromSlash(rel)))
		if err != nil {
			if os.IsNotExist(err) {
				continue // deleted but still in the index
			}
			return nil, err
		}
		findings, err := scanFile(rel, f)
		f.Close()
		if err != nil {
			return nil, err
		}
		out = append(out, findings...)

		// A file called .env is a finding on its own: the rule is that it never
		// leaves the machine, so its presence in the list is the problem, not
		// anything written inside it.
		// A `.example` is the template that documents which variables exist and
		// is meant to be committed; anything else called .env is the filled-in
		// one, which never leaves the machine.
		base := filepath.Base(rel)
		if base == ".env" || strings.HasPrefix(base, ".env.") && !strings.HasSuffix(base, ".example") {
			out = append(out, Finding{Path: rel, Reason: "an environment file is committed", Value: base})
		}
	}
	return out, nil
}

func skip(rel string) bool {
	for _, part := range strings.Split(filepath.ToSlash(rel), "/") {
		if SkipDirs[part] {
			return true
		}
	}
	// Lock files are long lists of hashes and no secrets, and they set the
	// entropy heuristics of any scanner on fire.
	switch filepath.Base(rel) {
	case "go.sum", "package-lock.json", "pnpm-lock.yaml":
		return true
	}
	return false
}

func scanFile(rel string, f *os.File) ([]Finding, error) {
	var out []Finding
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 0, 64*1024), 4*1024*1024)
	for line := 1; sc.Scan(); line++ {
		text := sc.Text()
		if strings.Contains(text, "harmony:allow-secret") {
			continue // an inline, reviewed exception, e.g. a sample in the docs
		}
		if m := keyBlock.FindString(text); m != "" {
			out = append(out, Finding{Path: rel, Line: line, Reason: "a private key is in the file", Value: m})
		}
		for _, m := range assignment.FindAllStringSubmatch(text, -1) {
			name, value := m[1], m[3]
			if value == "" || placeholders.MatchString(value) {
				continue
			}
			out = append(out, Finding{
				Path: rel, Line: line,
				Reason: name + " has a value in the file", Value: value,
			})
		}
	}
	return out, sc.Err()
}
