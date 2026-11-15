package push

import (
	"os"
	"strings"
	"testing"
)

// TestImageShipsATrustStore guards a failure that cost a real device test.
//
// The Harmony binary runs in a `scratch` image, which has no CA bundle.
// That was free for six sprints because the server only ever accepted
// connections; this package makes the first outbound HTTPS call in the
// project, and without the bundle every push dies with
// `x509: certificate signed by unknown authority`.
//
// Nothing else could catch it. The sender tests talk to an httptest TLS
// server through a client that already trusts its certificate, and the e2e
// endpoint does not resolve, so it fails at DNS long before TLS. The
// property is one of the *image*, not of the code, so the Dockerfile is what
// gets asserted.
func TestImageShipsATrustStore(t *testing.T) {
	raw, err := os.ReadFile("../../harmony.Dockerfile")
	if err != nil {
		t.Fatalf("read Dockerfile: %v", err)
	}
	dockerfile := string(raw)

	_, final, ok := strings.Cut(dockerfile, "FROM scratch")
	if !ok {
		t.Skip("the final stage is no longer scratch; a base image with its own trust store makes this moot")
	}
	if !strings.Contains(final, "ca-certificates.crt") {
		t.Fatal("the scratch stage does not copy a CA bundle: every Web Push will fail with " +
			"`x509: certificate signed by unknown authority`, and only the server log will say so")
	}
}
