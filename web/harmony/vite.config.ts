import { fileURLToPath } from 'node:url';
import { cpSync, readdirSync } from 'node:fs';
import { defineConfig, type Plugin } from 'vite';
import preact from '@preact/preset-vite';
import tailwindcss from '@tailwindcss/vite';

// The harmony web client.
//
// It lives next to the 1:1 call app rather than inside it: the two are
// different programs on different machines, and the only thing they share is
// this folder's node_modules, so that the harmony client adds no dependency of
// its own (CLAUDE.md forbids one without asking) and stays on exactly the
// pinned versions the call app already uses.
//
// The build goes straight into the Go server's embedded assets, so a deploy is
// still one binary and there is no directory of files on the machine for a
// request to escape into. base is /static/ because that is the only path the
// server serves files from.
const outDir = fileURLToPath(new URL('../../internal/harmony/api/assets/app', import.meta.url));

// Twemoji (the owner, 24/09/2026, option 9A): every emoji drawn the same on
// every machine instead of Windows' flat set on one screen and Apple's on the
// next. The pictures come from the pinned @twemoji/svg package and are copied
// as they are into the build, beside the bundle: the CSP's img-src is 'self',
// so a CDN is not an option, and a copy here also means no third party learns
// who opened the app. The folder carries the Twemoji version in its name so the
// server can hand them out as immutable (they live under assets/) and a new
// version is a new address instead of a stale cache.
//
// Only .svg files are copied, so the package's own readme and licence never
// become something /static/ serves.
function twemoji(): Plugin {
	return {
		name: 'harmony-twemoji',
		apply: 'build',
		closeBundle() {
			const from = fileURLToPath(new URL('../node_modules/@twemoji/svg', import.meta.url));
			const to = outDir + '/assets/twemoji-15';
			for (const name of readdirSync(from)) {
				if (name.endsWith('.svg')) cpSync(from + '/' + name, to + '/' + name);
			}
		},
	};
}

export default defineConfig({
	root: fileURLToPath(new URL('.', import.meta.url)),
	base: '/static/',
	plugins: [preact(), tailwindcss(), twemoji()],
	build: {
		outDir,
		emptyOutDir: true,
		// Off on purpose: a source map would put the whole client source into
		// the deployed bundle, and the CSP has nowhere to load it from anyway.
		sourcemap: false,
	},
	server: {
		// `go run ./cmd/harmony` on 8080 for local work, so the browser sees
		// the same origin it will see behind Caddy.
		proxy: {
			'/api': 'http://127.0.0.1:8080',
			'/ws': { target: 'ws://127.0.0.1:8080', ws: true },
		},
	},
});
