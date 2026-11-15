package secrets

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// allowed is the reviewed exception list: exact values that match the patterns
// but are not credentials. Every entry has to say why, and it names one value
// rather than a whole file, so a real secret landing in the same file still
// fails the test.
var allowed = map[string]string{
	// Native Windows test drivers that read the e2e password out of
	// e2e-harmony/fixtures.ts: the "value" is the capture group of that regex
	// and an env variable name, never a password.
	"windows/app/instalar/updater-e2e.mjs|([^":                        "regex reading the fixture password",
	"windows/app/testes/call-audit.mjs|([^":                           "regex reading the fixture password",
	"windows/app/testes/call-audit.mjs|password,":                     "variable holding the fixture password",
	"windows/app/instalar/installer-e2e.mjs|([^":                      "regex reading the fixture password",
	// The browser suite creates disposable accounts on its local server.
	"e2e-harmony/fixtures.ts|uma-senha-bem-longa-123": "the password every e2e account is created with",

	// The native Windows programme's self-tests and the bench (W0–W3, October
	// 2026). None of these is a credential: the scanner reads a variable name
	// (password, senha, SENHA) or a regular expression ("([^") as the value,
	// and the one literal is the password of the throwaway accounts the bench
	// creates on its own local server.
}

// TestNoSecretIsCommitted asks git what is actually in the repository, which is
// exactly the promise of PLANO-DISCORD.md §5: no key, password or token in the
// code or in git. Untracked files on this machine (a real .env, for one) are
// none of its business.
func TestNoSecretIsCommitted(t *testing.T) {
	root := repoRoot(t)
	paths := trackedFiles(t, root)
	if len(paths) < 10 {
		t.Fatalf("git listed only %d files; the scan would pass for the wrong reason", len(paths))
	}

	findings, err := Scan(root, paths)
	if err != nil {
		t.Fatal(err)
	}
	used := map[string]bool{}
	for _, f := range findings {
		key := f.Path + "|" + f.Value
		if _, ok := allowed[key]; ok {
			used[key] = true
			continue
		}
		t.Errorf("%s:%d — %s (%q)", f.Path, f.Line, f.Reason, f.Value)
	}
	// An exception nobody needs any more is an exception nobody is reading.
	for key, why := range allowed {
		if !used[key] {
			t.Errorf("the exception for %q (%s) no longer matches anything; delete it", key, why)
		}
	}
}

// And .env has to stay ignored, since that is the file the rule depends on.
func TestEnvFileIsIgnored(t *testing.T) {
	root := repoRoot(t)
	body, err := os.ReadFile(filepath.Join(root, ".gitignore"))
	if err != nil {
		t.Fatal(err)
	}
	// Read as lines, not as bytes: this repository is edited on Windows, where
	// a checkout writes CRLF, and a rule this important must not be reported
	// as missing because of the two characters at the end of the line.
	ignored := false
	for _, line := range strings.Split(string(body), "\n") {
		if strings.TrimSpace(line) == ".env" {
			ignored = true
		}
	}
	if !ignored {
		t.Fatal(".gitignore no longer ignores .env")
	}
}

// The scanner has to actually catch things; a scan that finds nothing because
// its patterns are broken looks exactly like a clean repository.
func TestScannerCatchesTheShapesItClaimsTo(t *testing.T) {
	dir := t.TempDir()
	write := func(name, body string) string {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o600); err != nil {
			t.Fatal(err)
		}
		return name
	}
	// Every fixture line carries the inline exception marker, because this file
	// is itself committed and TestNoSecretIsCommitted scans the whole
	// repository: without it, the test that proves the scanner works would be
	// reported by the scanner.
	//
	// One file per shape, and each one asked about **on its own**. Counting
	// the whole batch was a hole: `.env` is reported twice (once for the name,
	// once for the line inside it), so the total could still reach five with
	// one shape missed entirely — and three shapes were in fact missed, all
	// three of them names this project really uses. Audit of 2026-09-20, F11.
	planted := []struct{ what, name, body string }{
		{"a secret in a compose file", "compose.yml", "environment:\n  AUTH_SECRET: 3f9a2b7c1d\n"}, // harmony:allow-secret
		{"a shell export", "deploy.sh", "export TURN_SECRET=abc123def456\n"},                       // harmony:allow-secret
		{"an ssh private key", "id_ed25519", "-----BEGIN OPENSSH PRIVATE KEY-----\nabc\n"},         // harmony:allow-secret
		{"an age identity", "backup-identity.txt", "AGE-SECRET-KEY-1QQPQZRHW9F5X2\n"},              // harmony:allow-secret
		{"a committed .env", ".env", "PASSPHRASE=hunter2\n"},                                       // harmony:allow-secret
		// The three this project's own names take, none of which used to match.
		{"a name with three segments", "run.sh", "HARMONY_TURN_SECRET=abc123def456\n"},         // harmony:allow-secret
		{"a name ending in PRIVATE", "start.sh", "HARMONY_VAPID_PRIVATE=abc123def456\n"},       // harmony:allow-secret
		{"a value in single quotes", "compose2.yml", "  TURN_SECRET: 'abc123def456'\n"},        // harmony:allow-secret
		{"a key name in single quotes", "conf.ts", "VAPID_PRIVATE_KEY: 'abc123def456',\n"},     // harmony:allow-secret
		{"an address, which is not a key", "addr.sh", "TURN_PRIVATE_IP=10.0.0.165\n@ignorar@"}, // harmony:allow-secret
	}
	for _, p := range planted {
		name := write(p.name, p.body)
		findings, err := Scan(dir, []string{name})
		if err != nil {
			t.Fatal(err)
		}
		// The last one is the counter-example: an address is not a credential,
		// and a scanner that reports it is a scanner somebody switches off.
		wantFinding := !strings.Contains(p.body, "@ignorar@")
		if wantFinding && len(findings) == 0 {
			t.Errorf("%s (%s) was not caught", p.what, p.name)
		}
		if !wantFinding && len(findings) > 0 {
			t.Errorf("%s (%s) was reported and should not be: %+v", p.what, p.name, findings)
		}
	}

	clean := []string{
		write(".env.example", "AUTH_SECRET=\nPASSPHRASE=\n# openssl rand -hex 32\n"),
		write("docker-compose.yml", "command:\n  - \"--static-auth-secret=$$TURN_SECRET\"\n"),
		write("notes.md", "the VAPID_PRIVATE_KEY lives only on the server\n"),
		write("sample.md", "AUTH_SECRET=exemplo-nao-real  <!-- harmony:allow-secret -->\n"),
	}
	findings, err := Scan(dir, clean)
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range findings {
		t.Errorf("false positive in %s:%d — %s (%q)", f.Path, f.Line, f.Reason, f.Value)
	}
}

func repoRoot(t *testing.T) string {
	t.Helper()
	dir, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "go.mod")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			t.Fatal("could not find the repository root")
		}
		dir = parent
	}
}

func trackedFiles(t *testing.T, root string) []string {
	t.Helper()
	git, err := exec.LookPath("git")
	if err != nil {
		t.Skip("git is not on PATH; this test asks git what is committed")
	}
	cmd := exec.Command(git, "ls-files", "-z")
	cmd.Dir = root
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("git ls-files: %v", err)
	}
	var paths []string
	for _, p := range strings.Split(string(out), "\x00") {
		if p != "" {
			paths = append(paths, p)
		}
	}
	return paths
}
