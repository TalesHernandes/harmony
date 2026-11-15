import { defineConfig, devices } from '@playwright/test';

// The harmony browser tests, with their own local server and fixtures.
//
// What this config buys is the only thing the Go tests cannot: two real
// browsers, with a camera and a screen, watching each other through the media
// server. "Não consigo ver a tela de quem está compartilhando" is a sentence no
// unit test can answer.
export default defineConfig({
	testDir: './e2e-harmony',
	globalSetup: './e2e-harmony/global-setup.ts',
	globalTeardown: './e2e-harmony/global-teardown.ts',
	timeout: 90_000,
	fullyParallel: false,
	workers: 1, // one server, one room
	retries: 0,
	// Stop at the first failure.
	//
	// Playwright starts a fresh worker after a test fails, and a fresh worker is
	// a fresh everything — which on 11/09/2026 meant the invite handing rewound
	// to the start and every test after the first failure signed up with a code
	// that had already been spent, waited its full ninety seconds and failed for
	// a reason that had nothing to do with it. One broken test cost twenty
	// minutes and fifteen misleading messages.
	//
	// The handing is fixed (e2e-harmony/fixtures.ts), and this is the second
	// belt: with one server and one room, tests share a machine, so the first
	// failure is the one worth reading and everything after it is noise. A red
	// run now ends in seconds instead of grinding to the end.
	maxFailures: 1,
	reporter: [['list']],
	use: {
		baseURL: `http://127.0.0.1:${process.env.HARMONY_E2E_PORT || 8097}`,
	},
	projects: [
		{
			name: 'chromium',
			use: {
				...devices['Desktop Chrome'],
				launchOptions: {
					args: [
						'--use-fake-device-for-media-stream',
						'--use-fake-ui-for-media-stream',
						// getDisplayMedia without the native picker. The fake
						// screen comes with a fake audio track, which is the
						// "compartilhar o áudio" path.
						'--auto-select-desktop-capture-source=Entire screen',
						// ontrack calls play(), which in a real session sits
						// behind the click that joined the room. Headless has
						// no such activation.
						'--autoplay-policy=no-user-gesture-required',
						// The fake devices beep, and the suite runs on the
						// owner's PC while he uses it: nothing may reach his
						// headset (25/10/2026). Muting the output leaves the
						// received streams and their analysers intact.
						'--mute-audio',
						// The media server listens on the loopback alone when
						// the address it advertises is the loopback (sfu.go,
						// onThisMachine) — a test server has no business on
						// every interface of the machine. Chrome does not offer
						// 127.0.0.1 as a candidate unless asked, and without it
						// the two ends have nothing to pair: the server's only
						// address is one the browser refuses to try.
						//
						// It is not what quiets the firewall box, which was the
						// first guess and was wrong; that is the binary's path
						// (scripts/go-test.sh).
						'--allow-loopback-in-peer-connection',
					],
				},
			},
		},
		// The owner's browser. Only for what Firefox draws differently: the
		// scroll bar (24/09/2026), which Firefox will not let a stylesheet
		// shape, so the app draws its own and it is checked here.
		{
			name: 'firefox',
			testMatch: /rolagem\.spec\.ts/,
			use: { ...devices['Desktop Firefox'], launchOptions: { firefoxUserPrefs: { 'media.volume_scale': '0' } } },
		},
	],
});
