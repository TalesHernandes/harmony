import {
	test as base,
	expect,
	type Browser,
	type BrowserContext,
	type Locator,
	type Page,
} from '@playwright/test';
import { readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

export const PASSWORD = 'uma-senha-bem-longa-123';

export async function readMail(to: string, contains: string): Promise<string> {
	const dir = process.env.HARMONY_E2E_MAIL_DIR;
	if (!dir) throw new Error('e2e mail directory missing');
	let found = '';
	await expect.poll(() => {
		for (const f of readdirSync(dir).filter((f) => f.endsWith('.eml'))) {
			const raw = readFileSync(join(dir, f), 'utf8');
			if (raw.includes('To: <' + to.trim().toLowerCase() + '>') && raw.includes(contains)) found = raw;
		}
		return !!found;
	}, { timeout: 10_000 }).toBe(true);
	return found;
}

// The invites are single use, which is the point of them (PLANO-DISCORD.md §5),
// so a test takes one and it is gone.
//
// **Taken out of the file, not counted in memory**, and that distinction cost
// an evening. Playwright starts a fresh worker process after a test fails — it
// is how it guarantees one test cannot poison the next — and a counter living
// in that process goes back to zero with it. Every person created after the
// first failure then signed up with an invite that had already been spent, got
// the "Não existe" page, and timed out; one failing test turned into fifteen,
// each of them waiting the full ninety seconds, and none of the messages said
// anything about invites.
//
// The file is the state now, so it survives the worker that reads it. There is
// one worker by config (playwright.harmony.config.ts), so read-then-write needs
// no lock.
function takeInvite(): string {
	const path = join(__dirname, 'invites.json');
	const all: string[] = JSON.parse(readFileSync(path, 'utf8'));
	const code = all.shift();
	if (!code) {
		throw new Error('ran out of invites: mint more in global-setup.');
	}
	writeFileSync(path, JSON.stringify(all));
	return code;
}

// A person is one browser, with its own camera, its own microphone and its own
// cookie. Two of them in one test is the only way to answer "does the other one
// see my screen?".
// Everybody a test has opened and not yet closed.
//
// A test closes its own people on its last line, and that line is not reached
// when an assertion throws — so before 19/09/2026 a failing test left two live
// browsers behind, each still in the shared voice room, until the worker died.
// Playwright does not close a context made by hand: the advice it prints about
// browser.newContext() only makes sense because they survive the test.
//
// The teardown below empties this, so cleaning up no longer depends on the test
// having gone well.
const abertos = new Set<Person>();

export class Person {
	constructor(
		readonly context: BrowserContext,
		readonly page: Page,
		readonly username: string,
	) {
		abertos.add(this);
	}

	// A telephone, for the layout: the screen the owner photographed is about
	// this many pixels across, and it is where "cria um scroll" happens.
	static readonly PHONE = { width: 390, height: 780 };

	private static async open(
		browser: Browser,
		username: string,
		viewport?: { width: number; height: number },
		extra: string[] = [],
		hasTouch = false,
	): Promise<[BrowserContext, Page]> {
		const context = await browser.newContext({
			// Anything not granted here is *denied*, not "not asked yet" — which
			// is why a test about notifications has to say so explicitly.
			permissions: ['camera', 'microphone', ...extra],
			...(viewport ? { viewport } : {}),
			hasTouch,
		});
		const page = await context.newPage();
		page.on('console', (m) => {
			if (m.type() === 'error') console.log(`[${username}] console error: ${m.text()}`);
		});
		return [context, page];
	}

	private static async settled(page: Page, username: string): Promise<void> {
		try {
			// The button for the people list: it is in the header, so it is
			// there on a telephone as well, where the channel column is behind
			// a menu and everything in it is invisible.
			await page.locator('button[aria-label="pessoas"]').waitFor({ timeout: 20_000 });
		} catch (err) {
			console.log(`[${username}] url=${page.url()}`);
			console.log(`[${username}] body=${await page.evaluate(() => document.body.innerText)}`);
			throw err;
		}
	}

	static async create(
		browser: Browser,
		username: string,
		opts: {
			invite?: string;
			viewport?: { width: number; height: number };
			permissions?: string[];
			hasTouch?: boolean;
			verifyEmail?: boolean;
		} = {},
	): Promise<Person> {
		const { invite, viewport, permissions, hasTouch } = opts;
		const [context, page] = await Person.open(browser, username, viewport, permissions, hasTouch);
		// A browser that is open belongs to somebody from this line on. Signing
		// up can fail — a spent invite, a page that never settles — and before
		// 19/09/2026 that left a browser nobody held a reference to, so nothing
		// could close it.
		try {
			await page.goto(`/signup?code=${invite ?? takeInvite()}`);
			await page.getByPlaceholder('ex.: joao').fill(username);
			await page.getByPlaceholder('ex.: João').fill(username);
			await page.locator('#email').fill(username + '@example.org');
			await page.locator('input[type=password]').first().fill(PASSWORD);
			await page.getByRole('button', { name: /criar conta/i }).click();
			await page.getByRole('heading', { name: 'Confirme seu e-mail' }).waitFor();
			if (opts.verifyEmail !== false) {
				const raw = await readMail(username + '@example.org', 'confirmar seu');
				const code = raw.match(/\b\d{6}\b/)?.[0];
				if (!code) throw new Error('verification code missing');
				await page.getByLabel('Código', { exact: true }).fill(code);
				await page.getByRole('button', { name: 'Confirmar e-mail', exact: true }).click();
				await Person.settled(page, username);
			}
		} catch (err) {
			await context.close().catch(() => undefined);
			throw err;
		}
		return new Person(context, page, username);
	}

	// signIn is the same account arriving from another browser, which is how
	// "a mesma conta em dois lugares" is reproduced.
	static async signIn(browser: Browser, username: string): Promise<Person> {
		const [context, page] = await Person.open(browser, username);
		try {
			await page.goto('/login');
			await page.locator('#u').fill(username);
			await page.locator('#p').fill(PASSWORD);
			await page.getByRole('button', { name: /^entrar/i }).click();
			await Person.settled(page, username);
		} catch (err) {
			await context.close().catch(() => undefined);
			throw err;
		}
		return new Person(context, page, username);
	}

	// openChannels is the menu button a telephone has and a desktop does not.
	// Everything in the channel column is behind it there.
	// composerFor is the box you type in, asked for **by the name of the
	// conversation it belongs to**.
	//
	// There is one box and its placeholder carries the name of whatever is
	// open. Asking for "Conversar com" alone therefore matches the box of the
	// channel that happens to be on the screen — so a test that opens somebody's
	// conversation and types on the next line writes into #geral whenever the
	// conversation has not finished opening. The message is really sent, to the
	// wrong place, and the failure lands on the *other* person's assertion
	// several lines later, looking like a message that went missing.
	//
	// Naming the conversation makes fill() wait for the right box instead.
	composerFor(name: string): Locator {
		return this.page.getByPlaceholder(new RegExp('Conversar com ' + name));
	}

	// writeTo says something in the conversation with somebody, once that
	// conversation is the one on the screen.
	async writeTo(name: string, text: string): Promise<void> {
		const box = this.composerFor(name);
		await box.waitFor({ timeout: 15_000 });
		await box.fill(text);
		await this.page.keyboard.press('Enter');
	}

	// openDMWith opens the private conversation with somebody, from their row in
	// the list of people — the way a person does it.
	//
	// It asks the **panel** whether it is open, not the row. The list of people
	// is drawn empty and filled when the server answers, so a row that is not
	// there yet is not a closed panel — reading it as one presses "pessoas",
	// closes the panel that was already open, and then waits for a row it has
	// just hidden.
	async openDMWith(username: string): Promise<void> {
		const row = this.page.locator(`aside button[data-person="${username}"]`).first();
		const panel = this.page.locator('aside');
		if (!(await panel.isVisible())) {
			await this.page.locator('button[aria-label="pessoas"]').first().click();
			await expect(panel).toBeVisible({ timeout: 10_000 });
		}
		await row.waitFor({ timeout: 15_000 });
		await row.click();
		await this.page
			.getByRole('dialog')
			.getByRole('button', { name: /conversar no privado/i })
			.click();
	}

	// inChannel is the same box named the other way: a channel is written *in*
	// and a person is talked *to*, and the placeholder says which.
	inChannel(name: string): Locator {
		return this.page.getByPlaceholder(new RegExp('Escrever em #' + name));
	}

	async openChannels(): Promise<void> {
		const menu = this.page.locator('button[aria-label="canais"]');
		if (await menu.isVisible()) await menu.click();
	}

	// openMessages is the other half of the column, since D4c: the server and
	// the conversations with people are two places, and a private conversation
	// only appears in the second one.
	async openMessages(): Promise<void> {
		await this.openChannels();
		await this.page.locator('nav button[data-space="dms"]').first().click();
	}

	// joinVoice presses the voice channel the way a person does.
	async joinVoice(): Promise<void> {
		await this.openChannels();
		await this.page.locator('button[title*="entrar na sala" i]:visible').first().click();
		// The call itself says whether it is connected. The strip in the column
		// says so too, but the column does not exist on a telephone.
		await this.page.locator('[data-call="live"]').waitFor({ timeout: 30_000 });
	}

	// The screen and the camera are started, changed and stopped in a panel of
	// their own, opened by the button that starts them. These four do what a
	// person does: press the button, then press the thing in the panel.
	dialog() {
		return this.page.getByRole('dialog');
	}

	// `:visible` matters: the same button exists in the channel column and under
	// the call, and on a telephone the column's copy is there but invisible.
	async openPanel(what: 'screen' | 'camera'): Promise<void> {
		await this.page.locator(`button[data-open="${what}"]:visible`).first().click();
		await this.dialog().waitFor({ timeout: 10_000 });
	}

	async shareScreen(): Promise<void> {
		await this.openPanel('screen');
		await this.dialog().getByRole('button', { name: /compartilhar a tela/i }).click();
	}

	// watchScreen é o clique em "assistir", debaixo da pessoa que está
	// mostrando. Desde 18/09/2026 entrar no canal não começa a assistir
	// sozinho, então todo teste sobre uma tela chegando passa por aqui — o que
	// é o comportamento, e não um estorvo do arcabouço.
	async watchScreen(): Promise<void> {
		await this.page
			.locator('[data-call] button[aria-label="assistir à tela"]')
			.first()
			.click({ timeout: 20_000 });
	}

	// focusMyScreen presses the block of this person's own screen. Since
	// 24/09/2026 a screen being shown is one more block of the grid, as in
	// Discord, and it is big only when somebody presses it.
	async focusMyScreen(): Promise<void> {
		const block = this.page.locator('[data-call] [data-tile^="eu:"]').filter({ hasText: 'Tela de' }).first();
		await block.locator('> div > button').first().click({ timeout: 20_000 });
		await this.page.locator('[data-call][data-layout="foco"]').waitFor({ timeout: 10_000 });
	}

	async stopScreen(): Promise<void> {
		await this.openPanel('screen');
		await this.dialog().getByRole('button', { name: /parar a tela/i }).click();
	}

	async startCamera(): Promise<void> {
		await this.openPanel('camera');
		await this.dialog().getByRole('button', { name: /ligar a câmera/i }).click();
	}

	async stopCamera(): Promise<void> {
		await this.openPanel('camera');
		await this.dialog().getByRole('button', { name: /desligar a câmera/i }).click();
	}

	// openSettings is the gear at the bottom of the column: where the same
	// choices live for somebody who wants to set them up once.
	async openSettings(): Promise<void> {
		await this.page.locator('button[aria-label="ajustes"]').first().click();
		// Since D4h these are tabs, and the tab you left open is remembered per
		// device — so asking for the one this is about beats trusting a default.
		await this.page.locator('[data-tab="av"]').click();
		await this.page.locator('[data-setting="microphone"]').waitFor({ timeout: 10_000 });
	}

	// peopleInRoom is the list of names the channel column shows under the
	// voice channel, which is what the owner was looking at when he saw the
	// same name twice.
	async peopleInRoom(): Promise<string[]> {
		return this.page.evaluate(() =>
			[...document.querySelectorAll('nav button[data-person]')].map(
				(b) => (b as HTMLElement).dataset.person ?? '',
			),
		);
	}

	// videos reports what is actually painting: a <video> with a size and a
	// stream is a picture somebody can see; one without is a black box.
	async videos(): Promise<
		{ w: number; h: number; live: boolean; ready: number; boxW: number; parked: boolean }[]
	> {
		return this.page.evaluate(() =>
			[...document.querySelectorAll('video')].map((v) => ({
				w: v.videoWidth,
				h: v.videoHeight,
				live: !!v.srcObject && !v.paused,
				ready: v.readyState,
				boxW: v.offsetWidth,
				parked: !!v.closest('[hidden]'),
			})),
		);
	}

	// The choices are drawn by the page since 24/09/2026 (Modal.tsx Choice),
	// so choosing is what a person does: open the list, press the line.
	async choose(setting: string, value: string): Promise<void> {
		const button = this.page.locator(`button[data-setting="${setting}"]`).first();
		if ((await button.getAttribute('data-value')) === value) return;
		await button.click();
		await this.page.locator(`[data-options="${setting}"] [data-value="${value}"]`).click();
		await this.page
			.locator(`button[data-setting="${setting}"][data-value="${value}"]`)
			.first()
			.waitFor();
	}

	async options(setting: string): Promise<string[]> {
		const button = this.page.locator(`button[data-setting="${setting}"]`).first();
		await button.click();
		const list = await this.page
			.locator(`[data-options="${setting}"] [data-value]`)
			.evaluateAll((els) => els.map((e) => (e as HTMLElement).dataset.value ?? ''));
		await button.click();
		return list;
	}

	// sending is what this browser is really putting on the wire: the ceiling
	// the encoder was given, and the size and rate the camera or the screen
	// actually settled on. A setting that does not show up here is a decoration.
	async sending(): Promise<
		{ maxBitrate: number; maxFramerate: number; width: number; height: number; fps: number }[]
	> {
		return this.page.evaluate(() => {
			const pc = (window as unknown as { __harmonyPC?: RTCPeerConnection }).__harmonyPC;
			if (!pc) return [];
			return pc
				.getSenders()
				.filter((s) => s.track && s.track.kind === 'video')
				.map((s) => {
					const enc = s.getParameters().encodings?.[0] ?? {};
					const set = s.track!.getSettings();
					return {
						maxBitrate: enc.maxBitrate ?? 0,
						maxFramerate: enc.maxFramerate ?? 0,
						width: set.width ?? 0,
						height: set.height ?? 0,
						fps: Math.round(set.frameRate ?? 0),
					};
				});
		});
	}

	// holding is how long each arriving picture is being held before it is
	// shown, in seconds, as this browser reports it.
	async holding(): Promise<number[]> {
		return this.page.evaluate(() => {
			const pc = (window as unknown as { __harmonyPC?: RTCPeerConnection }).__harmonyPC;
			if (!pc) return [];
			return pc
				.getReceivers()
				.filter((r) => r.track?.kind === 'video')
				.map((r) => (r as RTCRtpReceiver & { playoutDelayHint?: number }).playoutDelayHint ?? 0);
		});
	}

	// markConnection stamps the live PeerConnection, so that a later check can
	// tell whether it is still the same object. Swapping a camera must never
	// rebuild it (CLAUDE.md — replaceTrack).
	async markConnection(mark: string): Promise<void> {
		await this.page.evaluate((m) => {
			const w = window as unknown as { __harmonyPC?: RTCPeerConnection & { __mark?: string } };
			if (w.__harmonyPC) w.__harmonyPC.__mark = m;
		}, mark);
	}

	async connectionMark(): Promise<string> {
		return this.page.evaluate(() => {
			const w = window as unknown as { __harmonyPC?: RTCPeerConnection & { __mark?: string } };
			return w.__harmonyPC?.__mark ?? '';
		});
	}

	// diag is the app's own log, which is what the owner is asked to send when
	// something goes wrong. If a test fails, this is what says why.
	async diag(): Promise<string> {
		return this.page.evaluate(() => {
			const w = window as unknown as { __harmonyDiag?: () => string };
			return w.__harmonyDiag ? w.__harmonyDiag() : '(sem diaglog)';
		});
	}

	// Hanging up before closing the browser is what a person does, and it is
	// also what keeps the room empty for the next test: a connection that is
	// only dropped takes fifteen seconds to be noticed, and every test uses the
	// same room. Ten tests of that and the room is full.
	async close(): Promise<void> {
		abertos.delete(this);
		// Stop intercepting BEFORE the page goes.
		//
		// A route handler that is still sleeping when the context closes wakes
		// up to a page that no longer exists, and `route.continue()` then
		// throws inside Playwright's own machinery — "may result in unhandled
		// error", in the words of its documentation for the default behaviour.
		// Where that error lands depends on the clock: inside whatever test is
		// running by then, or, if it falls in the gap between two, nowhere at
		// all, which the runner reports as "error was not a part of any test".
		//
		// upload.spec.ts holds an upload for ten seconds and cancels it after
		// three, so seven seconds of handler outlive that test every single
		// run — and the three seconds after it are video.spec.ts. That is the
		// red run of 18/09/2026, and it is a coin toss by construction.
		await this.page.unrouteAll({ behavior: 'ignoreErrors' }).catch(() => undefined);
		await this.page
			.evaluate(async () => {
				const w = window as unknown as { __harmonyVoice?: { leave(): Promise<void> } };
				await w.__harmonyVoice?.leave();
			})
			.catch(() => undefined);
		await this.context.close();
	}
}

export function invites(): string[] {
	return JSON.parse(readFileSync(join(__dirname, 'invites.json'), 'utf8'));
}

// The teardown that runs whether the test passed or not.
//
// It is an automatic fixture and not an afterEach on purpose: a hook declared
// in this file would attach itself to the suite of whichever spec imported it
// first, because the module is evaluated once per worker. A fixture belongs to
// the `test` object, so it reaches every file that uses it.
export const test = base.extend<{ limpeza: void }>({
	launchOptions: [async ({ launchOptions, browserName }, use) => {
		await use(browserName === 'firefox' ? { ...launchOptions,
			firefoxUserPrefs: { ...launchOptions.firefoxUserPrefs, 'media.volume_scale': '0' },
		} : launchOptions);
	}, { scope: 'worker' }],
	limpeza: [
		async ({}, use, info) => {
			await use();
			for (const quem of [...abertos]) {
				// A test that failed is asked for the app's own log first. It is
				// what the owner is asked for when something goes wrong, and it
				// is the difference between "the assertion failed" and knowing
				// why.
				if (info.status !== info.expectedStatus) {
					const diag = await quem.diag().catch(() => '(página já fechada)');
					console.log(`[${quem.username}] diaglog:
${diag}`);
				}
				await quem.close().catch(() => undefined);
			}
			abertos.clear();
		},
		{ auto: true },
	],
});
export { expect } from '@playwright/test';
