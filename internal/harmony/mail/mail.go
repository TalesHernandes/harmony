// Package mail delivers account notices only, never chat content.
package mail

import (
	"context"
	"crypto/rand"
	"crypto/tls"
	"encoding/hex"
	"errors"
	"fmt"
	"mime"
	"net"
	stdmail "net/mail"
	"net/smtp"
	"os"
	"strings"
	"time"
)

type Mailer interface {
	Send(ctx context.Context, to, subject, body string) error
}

var ErrConfiguration = errors.New("mail: invalid configuration")

type SMTP struct{ Host, Port, User, Pass, From string }
type File struct{ Dir, From string }

func FromEnv() (Mailer, error) {
	from := os.Getenv("HARMONY_MAIL_FROM")
	if dir := os.Getenv("HARMONY_MAIL_DIR"); dir != "" {
		if from == "" {
			from = "harmony <harmony@localhost>"
		}
		if _, err := stdmail.ParseAddress(from); err != nil {
			return nil, ErrConfiguration
		}
		if err := os.MkdirAll(dir, 0700); err != nil {
			return nil, ErrConfiguration
		}
		return &File{Dir: dir, From: from}, nil
	}
	host := os.Getenv("HARMONY_SMTP_HOST")
	if host == "" {
		return nil, nil
	}
	port := os.Getenv("HARMONY_SMTP_PORT")
	if port == "" {
		port = "587"
	}
	if port != "465" && port != "587" {
		return nil, ErrConfiguration
	}
	addr, err := stdmail.ParseAddress(from)
	if err != nil || strings.ContainsAny(from, "\r\n") {
		return nil, ErrConfiguration
	}
	// A bare address in the env file gets the service's name in front, so the
	// inbox says "harmony" without the file needing "<", ">" and spaces —
	// characters that break the moment anything reads it as shell.
	if addr.Name == "" {
		addr.Name = "harmony"
	}
	from = addr.String()
	return &SMTP{Host: host, Port: port, From: from, User: os.Getenv("HARMONY_SMTP_USER"), Pass: os.Getenv("HARMONY_SMTP_PASS")}, nil
}

func message(from, to, subject, body string) ([]byte, string, string, error) {
	if strings.ContainsAny(from+to+subject, "\r\n") {
		return nil, "", "", ErrConfiguration
	}
	f, err := stdmail.ParseAddress(from)
	if err != nil {
		return nil, "", "", ErrConfiguration
	}
	t, err := stdmail.ParseAddress(to)
	if err != nil {
		return nil, "", "", ErrConfiguration
	}
	var b [16]byte
	if _, err = rand.Read(b[:]); err != nil {
		return nil, "", "", err
	}
	// The id's right-hand side is the sender's own domain, as RFC 5322 asks;
	// a bare "@harmony" is a small spam signal for no gain.
	id := hex.EncodeToString(b[:]) + f.Address[strings.LastIndex(f.Address, "@"):]
	body = strings.ReplaceAll(strings.ReplaceAll(body, "\r\n", "\n"), "\n", "\r\n")
	raw := fmt.Sprintf("From: %s\r\nTo: %s\r\nDate: %s\r\nMessage-ID: <%s>\r\nSubject: %s\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n%s\r\n", f.String(), t.String(), time.Now().Format(time.RFC1123Z), id, mime.QEncoding.Encode("UTF-8", subject), body)
	return []byte(raw), f.Address, t.Address, nil
}

func (f *File) Send(ctx context.Context, to, subject, body string) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	raw, _, _, err := message(f.From, to, subject, body)
	if err != nil {
		return err
	}
	file, err := os.CreateTemp(f.Dir, "mail-*.tmp")
	if err != nil {
		return err
	}
	name := file.Name()
	defer os.Remove(name)
	if err = file.Chmod(0600); err == nil {
		_, err = file.Write(raw)
	}
	closeErr := file.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	return os.Rename(name, strings.TrimSuffix(name, ".tmp")+".eml")
}

func (m *SMTP) Send(ctx context.Context, to, subject, body string) error {
	raw, from, target, err := message(m.From, to, subject, body)
	if err != nil {
		return err
	}
	if m.Port != "465" && m.Port != "587" {
		return ErrConfiguration
	}
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	conn, err := (&net.Dialer{Timeout: 5 * time.Second}).DialContext(ctx, "tcp", net.JoinHostPort(m.Host, m.Port))
	if err != nil {
		return err
	}
	defer conn.Close()
	deadline, _ := ctx.Deadline()
	conn.SetDeadline(deadline)
	socket := conn
	stop := context.AfterFunc(ctx, func() { socket.Close() })
	defer stop()
	config := &tls.Config{ServerName: m.Host, MinVersion: tls.VersionTLS12}
	if m.Port == "465" {
		secure := tls.Client(conn, config)
		if err = secure.HandshakeContext(ctx); err != nil {
			return err
		}
		conn = secure
	}
	c, err := smtp.NewClient(conn, m.Host)
	if err != nil {
		return err
	}
	defer c.Close()
	if m.Port == "587" {
		if ok, _ := c.Extension("STARTTLS"); !ok {
			return errors.New("mail: STARTTLS required")
		}
		if err = c.StartTLS(config); err != nil {
			return err
		}
	}
	if m.User != "" {
		if err = c.Auth(smtp.PlainAuth("", m.User, m.Pass, m.Host)); err != nil {
			return err
		}
	}
	if err = c.Mail(from); err != nil {
		return err
	}
	if err = c.Rcpt(target); err != nil {
		return err
	}
	w, err := c.Data()
	if err != nil {
		return err
	}
	if _, err = w.Write(raw); err != nil {
		w.Close()
		return err
	}
	if err = w.Close(); err != nil {
		return err
	}
	return c.Quit()
}
