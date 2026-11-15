package api

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

// A program's diagnostic report, sent by its "Enviar diagnóstico" button so
// the owner no longer has to photograph the performance screen (22/10/2026).
// It is a write-only drop box: nothing here serves a report back over HTTP;
// they are read on the machine. The client sends counters and its technical
// log, which by construction carry no message text, password or e-mail; the
// server still never logs the body, only who sent how many bytes.
const (
	maxDiagnosticBytes  = 4 << 20 // a whole call of the program's log (29/10/2026)
	diagnosticInterval  = time.Minute
	keptDiagnosticFiles = 40
)

type diagnosticGate struct {
	mu   sync.Mutex
	last map[int64]time.Time
}

func (g *diagnosticGate) allow(user int64, now time.Time) bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.last == nil {
		g.last = map[int64]time.Time{}
	}
	if previous, ok := g.last[user]; ok && now.Sub(previous) < diagnosticInterval {
		return false
	}
	g.last[user] = now
	return true
}

func (s *Server) handleDiagnostic(w http.ResponseWriter, r *http.Request, cu ctxUser) {
	if s.cfg.DiagnosticsDir == "" {
		fail(w, http.StatusServiceUnavailable, "diagnostics_unavailable")
		return
	}
	// JSON {"report": "..."}, the shape every request of the program has;
	// the report is stored as the plain text it is, to be read as such.
	r.Body = http.MaxBytesReader(w, r.Body, maxDiagnosticBytes)
	raw, err := io.ReadAll(r.Body)
	if err != nil {
		fail(w, http.StatusRequestEntityTooLarge, "too_large")
		return
	}
	var req struct {
		Report string `json:"report"`
	}
	if err := json.Unmarshal(raw, &req); err != nil {
		fail(w, http.StatusBadRequest, "bad_request")
		return
	}
	body := []byte(req.Report)
	if len(strings.TrimSpace(req.Report)) == 0 {
		fail(w, http.StatusBadRequest, "empty")
		return
	}
	if !s.diagnostics.allow(cu.user.ID, time.Now()) {
		fail(w, http.StatusTooManyRequests, "rate_limited")
		return
	}
	if err := os.MkdirAll(s.cfg.DiagnosticsDir, 0o700); err != nil {
		s.log.Error("diagnostics directory", "err", err)
		fail(w, http.StatusInternalServerError, "internal")
		return
	}
	// The name is built from the account id and the clock only: nothing a
	// person typed ever becomes part of a path.
	name := fmt.Sprintf("%s-u%d.txt", time.Now().UTC().Format("20060102-150405"), cu.user.ID)
	if err := os.WriteFile(filepath.Join(s.cfg.DiagnosticsDir, name), body, 0o600); err != nil {
		s.log.Error("diagnostic write", "err", err)
		fail(w, http.StatusInternalServerError, "internal")
		return
	}
	pruneDiagnostics(s.cfg.DiagnosticsDir)
	s.log.Info("diagnostic received", "user", cu.user.ID, "bytes", len(body))
	writeJSON(w, http.StatusCreated, map[string]string{"name": name})
}

// pruneDiagnostics keeps the newest reports; names sort by time.
func pruneDiagnostics(dir string) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return
	}
	var names []string
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".txt") {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)
	for len(names) > keptDiagnosticFiles {
		_ = os.Remove(filepath.Join(dir, names[0]))
		names = names[1:]
	}
}
