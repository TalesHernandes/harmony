package api

import (
	"bytes"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func diagnosticHarness(t *testing.T) (*harness, string) {
	t.Helper()
	dir := filepath.Join(t.TempDir(), "diagnostics")
	h := newHarnessTweaked(t, func(c *Config) { c.DiagnosticsDir = dir })
	return h, dir
}

// sendDiagnostic posts a report as the program does: {"report": text}.
func sendDiagnostic(d *device, body string) reply {
	return d.do(http.MethodPost, "/api/diagnostics", map[string]string{"report": body})
}

func TestDiagnosticNeedsASession(t *testing.T) {
	h, dir := diagnosticHarness(t)
	if res := sendDiagnostic(h.device("anon"), "memória 120 MB"); res.status != http.StatusUnauthorized {
		t.Fatalf("report without a session: %d", res.status)
	}
	if entries, _ := os.ReadDir(dir); len(entries) != 0 {
		t.Fatalf("an anonymous report was stored: %d files", len(entries))
	}
}

func TestDiagnosticIsStoredOnceAMinuteAndNeverLogged(t *testing.T) {
	h, dir := diagnosticHarness(t)
	a := h.join(h.bootstrapInvite(), "ana", "senha-longa-da-ana-1")
	body := "harmony nativo 15\nmemória privada 431 MB\nconteúdo-que-não-pode-ir-ao-log"
	if res := sendDiagnostic(a, body); res.status != http.StatusCreated {
		t.Fatalf("report: %d %s", res.status, res.raw)
	}
	entries, err := os.ReadDir(dir)
	if err != nil || len(entries) != 1 {
		t.Fatalf("stored reports: %v %d", err, len(entries))
	}
	stored, _ := os.ReadFile(filepath.Join(dir, entries[0].Name()))
	if string(stored) != body {
		t.Fatalf("stored report differs")
	}
	if strings.Contains(entries[0].Name(), "ana") {
		t.Fatalf("a typed name became part of a path: %s", entries[0].Name())
	}
	if strings.Contains(h.logs.String(), "conteúdo-que-não-pode-ir-ao-log") {
		t.Fatal("the report body reached the server log")
	}
	if res := sendDiagnostic(a, body); res.status != http.StatusTooManyRequests {
		t.Fatalf("second report within a minute: %d", res.status)
	}
	// Another account has its own allowance, and cannot read anything back:
	// there is no route that serves a report.
	code, _ := a.inviteFrom("bia")
	b := h.join(code, "bia", "senha-longa-da-bia-1")
	if res := sendDiagnostic(b, "bia"); res.status != http.StatusCreated {
		t.Fatalf("other account's report: %d", res.status)
	}
	if res := b.get("/api/diagnostics"); res.status == http.StatusOK {
		t.Fatalf("reports can be read over HTTP: %d", res.status)
	}
}

func TestDiagnosticRefusesEmptyAndHuge(t *testing.T) {
	h, _ := diagnosticHarness(t)
	a := h.join(h.bootstrapInvite(), "ana", "senha-longa-da-ana-1")
	if res := sendDiagnostic(a, "   "); res.status != http.StatusBadRequest {
		t.Fatalf("empty report: %d", res.status)
	}
	huge := string(bytes.Repeat([]byte("x"), maxDiagnosticBytes+1))
	if res := sendDiagnostic(a, huge); res.status != http.StatusRequestEntityTooLarge {
		t.Fatalf("oversized report: %d", res.status)
	}
}

func TestDiagnosticsKeepOnlyTheNewest(t *testing.T) {
	dir := t.TempDir()
	for i := 0; i < keptDiagnosticFiles+5; i++ {
		name := filepath.Join(dir, "2026"+strings.Repeat("0", 2)+string(rune('a'+i%26))+string(rune('a'+i/26))+".txt")
		if err := os.WriteFile(name, []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	pruneDiagnostics(dir)
	if entries, _ := os.ReadDir(dir); len(entries) != keptDiagnosticFiles {
		t.Fatalf("kept %d reports", len(entries))
	}
}
