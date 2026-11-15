package mail

import (
	"context"
	"io"
	"mime"
	stdmail "net/mail"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestFileMailHeadersPortugueseAndAtomicOutput(t *testing.T) {
	dir := t.TempDir()
	f := &File{Dir: dir, From: "harmony <harmony@example.org>"}
	if err := f.Send(context.Background(), "alice@example.org", "Código do harmony", "Seu código: 123456\nSe não foi você, ignore."); err != nil {
		t.Fatal(err)
	}
	files, err := os.ReadDir(dir)
	if err != nil || len(files) != 1 || !strings.HasSuffix(files[0].Name(), ".eml") {
		t.Fatal("mail not published atomically", err)
	}
	raw, err := os.ReadFile(filepath.Join(dir, files[0].Name()))
	if err != nil {
		t.Fatal(err)
	}
	msg, err := stdmail.ReadMessage(strings.NewReader(string(raw)))
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"From", "To", "Date", "Message-ID", "MIME-Version", "Content-Type"} {
		if msg.Header.Get(name) == "" {
			t.Fatal("header missing", name)
		}
	}
	subject, err := new(mime.WordDecoder).DecodeHeader(msg.Header.Get("Subject"))
	if err != nil || subject != "Código do harmony" {
		t.Fatal(subject, err)
	}
	body, _ := io.ReadAll(msg.Body)
	if !strings.Contains(string(body), "Se não foi você") {
		t.Fatal("body damaged")
	}
}
func TestMailRejectsHeaderInjection(t *testing.T) {
	f := &File{Dir: t.TempDir(), From: "harmony@example.org"}
	for _, v := range []struct{ to, subject string }{{"alice@example.org\r\nBcc: attacker@example.org", "ok"}, {"alice@example.org", "ok\r\nBcc: attacker@example.org"}} {
		if err := f.Send(context.Background(), v.to, v.subject, "body"); err == nil {
			t.Fatal("header injection accepted")
		}
	}
}
func TestMailConfigurationAndNoPlaintextPort(t *testing.T) {
	for _, k := range []string{"HARMONY_MAIL_DIR", "HARMONY_SMTP_HOST", "HARMONY_SMTP_PORT", "HARMONY_MAIL_FROM"} {
		t.Setenv(k, "")
	}
	if m, err := FromEnv(); m != nil || err != nil {
		t.Fatal("absent configuration fails boot")
	}
	t.Setenv("HARMONY_SMTP_HOST", "smtp.example.org")
	t.Setenv("HARMONY_MAIL_FROM", "harmony@example.org")
	t.Setenv("HARMONY_SMTP_PORT", "25")
	if _, err := FromEnv(); err == nil {
		t.Fatal("plaintext SMTP allowed")
	}
	t.Setenv("HARMONY_SMTP_PORT", "465")
	if m, err := FromEnv(); err != nil {
		t.Fatal(err)
	} else if from := m.(*SMTP).From; from != `"harmony" <harmony@example.org>` {
		t.Fatalf("a bare address should be sent as harmony's: %s", from)
	}
	t.Setenv("HARMONY_SMTP_PORT", "587")
	if _, err := FromEnv(); err != nil {
		t.Fatal(err)
	}
	t.Setenv("HARMONY_MAIL_DIR", t.TempDir())
	if m, err := FromEnv(); err != nil {
		t.Fatal(err)
	} else if _, ok := m.(*File); !ok {
		t.Fatal("file mode did not override SMTP")
	}
}
