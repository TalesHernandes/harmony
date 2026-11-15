package api

import "mime"

// Go's built-in extension table does not know woff2, and the fallback is the
// operating system's: /etc/mime.types on the server, the registry on the
// Windows machine this is developed on. A container built from scratch has
// neither, and http.FileServer then sniffs the bytes and serves the font as
// application/octet-stream — which browsers do load, but only because a font is
// one of the few things X-Content-Type-Options: nosniff does not police.
//
// One line makes the answer the same everywhere and stops it depending on which
// packages happen to be installed next to the binary. The font itself is
// web/harmony/src/fonts/inter-latin.woff2, reached through /static/assets/.
//
// The same goes for the harmony's own sounds (web/harmony/src/sounds/, since
// 29/10/2026): a .wav and the ring's .mp3.
func init() {
	for ext, typ := range map[string]string{".woff2": "font/woff2", ".wav": "audio/wav", ".mp3": "audio/mpeg"} {
		if err := mime.AddExtensionType(ext, typ); err != nil {
			// AddExtensionType only rejects an extension that does not start with
			// a dot, so this is unreachable; panicking is still better than a
			// silent wrong Content-Type, and it fails at startup rather than in a
			// browser.
			panic(err)
		}
	}
}
