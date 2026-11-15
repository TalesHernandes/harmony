// The fourteen the Codex audit of the web client found on 23/09/2026, one test
// each (the numbers are the report's). Every one was inverted — the fix taken
// out, the test watched going red — before it was kept.
//
// Most of them are about a request that is slow or that fails, which a test
// can only stand inside by holding the request: page.route() is the hand on the
// network, and a `gate` is the moment the test lets go.

import type { Page, Route } from '@playwright/test';
import { test, expect, Person, PASSWORD } from './fixtures';

// gate holds requests until it is opened.
function gate() {
	let open!: () => void;
	const opened = new Promise<void>((r) => (open = r));
	return { open, opened };
}

// channelID asks the server, as this person, for the id of one conversation.
async function channelID(who: Person, pick: { name?: string; kind?: string }): Promise<string> {
	return who.page.evaluate(async (want) => {
		const res = await fetch('/api/channels').then((r) => r.json());
		const c = res.channels.find(
			(x: { name: string; kind: string }) =>
				(!want.name || x.name === want.name) && (!want.kind || x.kind === want.kind),
		);
		return c?.id ?? '';
	}, pick);
}

async function say(who: Person, channel: string, body: string): Promise<number> {
	return who.page.evaluate(
		async ([id, text]) => {
			const res = await fetch(`/api/channels/${id}/messages`, {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ body: text, attachments: [] }),
			});
			return (await res.json()).id as number;
		},
		[channel, body] as const,
	);
}

async function settings(page: Page, tab: string): Promise<void> {
	await page.locator('button[aria-label="ajustes"]').first().click();
	await page.locator(`[data-tab="${tab}"]`).click();
}

// 1. A draft left by one account became the next account's opening line on the
// same browser, whenever the first session ended other than by "Sair".
test('o rascunho de uma conta não aparece para a próxima no mesmo navegador', async ({ browser }) => {
	const bruno = await Person.create(browser, 'c1bruno');
	await bruno.close();
	const ana = await Person.create(browser, 'c1ana');
	await ana.inChannel('geral').waitFor({ timeout: 15_000 });
	await ana.inChannel('geral').fill('frase da ana que ninguém mais pode ver');
	await ana.page.evaluate(() => window.dispatchEvent(new Event('pagehide')));

	// The session ends without "Sair": it expired, or was revoked elsewhere.
	await ana.context.clearCookies();
	await ana.page.goto('/login');
	await ana.page.locator('#u').fill('c1bruno');
	await ana.page.locator('#p').fill(PASSWORD);
	await ana.page.getByRole('button', { name: /^entrar/i }).click();
	await ana.inChannel('geral').waitFor({ timeout: 20_000 });

	await expect(ana.inChannel('geral')).toHaveValue('');
	// And the file is gone, unread.
	expect(await ana.page.evaluate(() => localStorage.getItem('harmony.drafts'))).toBeNull();
	await ana.close();
});

// 2. A call that ended while the screen picker was open got its screen anyway:
// shared, for a call that no longer existed, with nothing on the screen to stop
// it.
test('desligar com o seletor de tela aberto não deixa a tela compartilhada', async ({ browser }) => {
	const ana = await Person.create(browser, 'c2ana');
	const bruno = await Person.create(browser, 'c2bruno');
	await ana.openDMWith('c2bruno');
	await ana.page.locator('[data-call-start]').click();
	await bruno.page.locator('[data-call-accept]').click({ timeout: 20_000 });
	await expect(ana.page.locator('[data-call-screen]')).toBeVisible({ timeout: 20_000 });
	// Sharing is only offered once the connection exists; a press before that
	// is ignored, and there would be no picker to hold.
	await expect
		.poll(
			() =>
				ana.page.evaluate(
					() =>
						(window as unknown as { __harmonyDirectPC?: RTCPeerConnection }).__harmonyDirectPC
							?.connectionState ?? 'none',
				),
			{ timeout: 30_000 },
		)
		.toBe('connected');

	// The picker, held open: getDisplayMedia answers only when the test says.
	await ana.page.evaluate(() => {
		const w = window as unknown as { __release: () => void; __held: MediaStream | null };
		w.__held = null;
		navigator.mediaDevices.getDisplayMedia = () =>
			new Promise((resolve) => {
				w.__release = async () => {
					const s = await navigator.mediaDevices.getUserMedia({ video: true });
					w.__held = s;
					resolve(s);
				};
			});
	});
	await ana.page.locator('[data-call-screen] button[aria-label="tela"]').click();
	await expect
		.poll(() => ana.page.evaluate(() => typeof (window as unknown as { __release?: unknown }).__release))
		.toBe('function');

	// The other person hangs up while the picker is still open…
	await bruno.page.locator('button[aria-label="desligar"]').first().click();
	await expect(ana.page.locator('[data-call-screen]')).toBeHidden({ timeout: 15_000 });
	// …and only then is a window chosen.
	await ana.page.evaluate(() => (window as unknown as { __release: () => void }).__release());

	await expect
		.poll(() =>
			ana.page.evaluate(() => {
				const s = (window as unknown as { __held: MediaStream | null }).__held;
				return s ? s.getTracks().every((t) => t.readyState === 'ended') : null;
			}),
		)
		.toBe(true);
	const state = await ana.page.evaluate(() =>
		(window as unknown as { __harmonyDirect: { snapshot(): { screen: boolean } } }).__harmonyDirect.snapshot(),
	);
	expect(state.screen).toBe(false);
	await ana.close();
	await bruno.close();
});

// 3. A failed search wrote the words being searched for into the diagnostic
// log, which is copied and shared.
test('uma busca que falha não põe as palavras no diagnóstico', async ({ browser }) => {
	const ana = await Person.create(browser, 'c3ana');
	await ana.page.route('**/api/search**', (route) => route.abort());
	const tried = ana.page.waitForEvent('requestfailed', (r) => r.url().includes('/api/search'));
	await ana.page.locator('button[aria-label="procurar"]').first().click();
	await ana.page.locator('[data-search-box]').fill('palavrasecretadaana');
	await tried;
	await ana.page.keyboard.press('Escape');

	await settings(ana.page, 'diag');
	await expect(ana.page.getByText('Diagnóstico').first()).toBeVisible();
	const log = await ana.page.locator('body').innerText();
	expect(log).toContain('/api/search');
	expect(log).not.toContain('palavrasecretadaana');
	await ana.close();
});

// The send, held on its way to the server.
async function holdSends(page: Page) {
	const g = gate();
	let posts = 0;
	await page.route('**/api/channels/*/messages', async (route: Route) => {
		if (route.request().method() !== 'POST') return route.continue();
		posts++;
		await g.opened;
		await route.continue();
	});
	return { ...g, posts: () => posts };
}

// 5. Enter went straight past the "sending" that only greyed the button.
test('Enter duas vezes enquanto envia manda uma mensagem só', async ({ browser }) => {
	const ana = await Person.create(browser, 'c5ana');
	const box = ana.inChannel('geral');
	await box.waitFor({ timeout: 15_000 });
	const held = await holdSends(ana.page);
	await box.fill('só uma vez, por favor');
	await box.press('Enter');
	await box.press('Enter');
	await box.press('Enter');
	await ana.page.waitForTimeout(500);
	held.open();
	await expect(ana.page.locator('[data-message]', { hasText: 'só uma vez, por favor' })).toHaveCount(1, {
		timeout: 15_000,
	});
	expect(held.posts()).toBe(1);
	await ana.close();
});

// 4. The answer to a send cleared the box — including what was written while
// the message was on its way.
test('o que foi escrito enquanto a mensagem ia não é apagado quando ela chega', async ({ browser }) => {
	const ana = await Person.create(browser, 'c4ana');
	const box = ana.inChannel('geral');
	await box.waitFor({ timeout: 15_000 });
	const held = await holdSends(ana.page);
	await box.fill('primeira');
	await box.press('Enter');
	await expect.poll(() => held.posts()).toBe(1);
	await box.fill('segunda, ainda não enviada');
	const answered = ana.page.waitForResponse(
		(r) => r.request().method() === 'POST' && /\/api\/channels\/[^/]+\/messages$/.test(new URL(r.url()).pathname),
	);
	held.open();
	await answered;
	// The page has had the answer; give its handler the frame it runs in.
	await ana.page.evaluate(() => new Promise((r) => requestAnimationFrame(() => setTimeout(r, 50))));
	await expect(box).toHaveValue('segunda, ainda não enviada', { timeout: 1 });
	await ana.close();
});

// 6. A failed first load left "Carregando…" on the screen for ever.
test('uma falha na primeira carga tenta de novo sozinha', async ({ browser }) => {
	const ana = await Person.create(browser, 'c6ana');
	// Every request fails until the page has said it is trying again, and not
	// only the first: the list of people is also asked for by the refresh that
	// runs when the socket opens, and when that one went first it took the
	// single failure (and rightly shrugged it off), the first load succeeded,
	// and the test waited for a message that had no reason to appear. It
	// failed once in a full run on 23/09/2026 and never alone.
	let broken = true;
	await ana.page.route('**/api/members', async (route) => {
		if (broken) return route.fulfill({ status: 500, body: '{"error":"server_error"}' });
		return route.continue();
	});
	await ana.page.reload();
	await expect(ana.page.getByText(/Tentando de novo/)).toBeVisible({ timeout: 10_000 });
	broken = false;
	await expect(ana.page.locator('button[aria-label="pessoas"]').first()).toBeVisible({ timeout: 20_000 });
	await ana.close();
});

// 7. Loading older messages rebuilt the page from the copy taken when the
// request left, throwing away what the socket delivered meanwhile.
test('carregar mensagens antigas não apaga a que chegou no meio', async ({ browser }) => {
	const ana = await Person.create(browser, 'c7ana');
	const bruno = await Person.create(browser, 'c7bruno');
	const carla = await Person.create(browser, 'c7carla');
	const geral = await channelID(ana, { name: 'geral' });
	// More than one page (50) in #geral, from three people, each under the
	// twenty a minute the server allows.
	for (const who of [ana, bruno, carla]) {
		for (let i = 0; i < 18; i++) await say(who, geral, `${who.username} ${i}`);
	}

	// Older pages are held.
	const g = gate();
	const isOlder = (url: URL) => url.pathname.endsWith('/messages') && url.search.includes('before=');
	await ana.page.route(isOlder, async (route) => {
		await g.opened;
		await route.continue();
	});
	await ana.page.reload();
	await ana.inChannel('geral').waitFor({ timeout: 20_000 });
	await expect(ana.page.locator('[data-message]').last()).toBeVisible({ timeout: 15_000 });
	// Up, until the older page is asked for.
	const older = ana.page.waitForRequest((r) => isOlder(new URL(r.url())), { timeout: 20_000 });
	await ana.page.locator('[data-message]').last().hover();
	for (let i = 0; i < 10; i++) {
		await ana.page.mouse.wheel(0, -6000);
		await ana.page.waitForTimeout(150);
	}
	await older;

	await say(bruno, geral, 'chegou no meio da carga');
	await expect(ana.page.locator('[data-message]', { hasText: 'chegou no meio da carga' })).toHaveCount(1, {
		timeout: 15_000,
	});
	const landed = ana.page.waitForResponse((r) => isOlder(new URL(r.url())));
	g.open();
	await landed;
	await ana.page.evaluate(() => new Promise((r) => requestAnimationFrame(() => setTimeout(r, 50))));
	await expect(ana.page.locator('[data-message]', { hasText: 'chegou no meio da carga' })).toHaveCount(1, {
		timeout: 1,
	});
	await ana.close();
	await bruno.close();
	await carla.close();
});

// 8. A reconnect refreshed only the conversation on the screen; every other one
// kept its copy from before the gap.
test('depois de cair a conexão, a outra conversa mostra o que foi dito na queda', async ({ browser }) => {
	const ana = await Person.create(browser, 'c8ana');
	const bruno = await Person.create(browser, 'c8bruno');

	// The hand on the socket: every connection goes through here, and while
	// `down` is set a new one is closed the moment it opens.
	let down = false;
	const live: { close: () => Promise<void> }[] = [];
	await ana.page.routeWebSocket(/\/ws$/, (ws) => {
		if (down) {
			void ws.close();
			return;
		}
		ws.connectToServer();
		live.push(ws);
	});
	await ana.page.reload();
	await ana.inChannel('geral').waitFor({ timeout: 20_000 });

	// Both conversations loaded, and ana back in #geral.
	await ana.openDMWith('c8bruno');
	await ana.composerFor('c8bruno').waitFor({ timeout: 15_000 });
	const dm = await channelID(ana, { kind: 'dm' });
	await ana.openChannels();
	await ana.page.locator('nav button[data-space="server"]').first().click();
	await ana.page.locator('nav button[aria-label="#geral"]').first().click();
	await ana.inChannel('geral').waitFor({ timeout: 15_000 });

	down = true;
	for (const ws of live.splice(0)) await ws.close();
	await ana.page.waitForTimeout(1_000);
	await say(bruno, dm, 'dito enquanto ela estava fora');
	await ana.page.waitForTimeout(500);
	down = false;
	// The next try gets through; the backoff is well under this.
	await expect.poll(() => live.length, { timeout: 20_000 }).toBeGreaterThan(0);
	await ana.page.waitForTimeout(1_500);

	await ana.openMessages();
	await ana.page.locator('nav button', { hasText: 'c8bruno' }).first().click();
	await expect(ana.page.locator('[data-message]', { hasText: 'dito enquanto ela estava fora' })).toBeVisible({
		timeout: 15_000,
	});
	await ana.close();
	await bruno.close();
});

// 9. A page of history taken before a deletion, landing after it, brought the
// deleted message back.
test('o histórico que chega atrasado não traz de volta uma mensagem apagada', async ({ browser }) => {
	const ana = await Person.create(browser, 'c9ana');
	const bruno = await Person.create(browser, 'c9bruno');
	await bruno.openDMWith('c9ana');
	await bruno.composerFor('c9ana').waitFor({ timeout: 15_000 });
	const dm = await channelID(bruno, { kind: 'dm' });
	const id = await say(ana, dm, 'isto vai ser apagado');
	await expect(bruno.page.locator('[data-message]', { hasText: 'isto vai ser apagado' })).toBeVisible({
		timeout: 15_000,
	});

	// Every page of history bruno asks for is fetched from the server at once
	// — a photograph with the message in it — and handed over only later.
	const g = gate();
	await bruno.page.route(`**/api/channels/${dm}/messages`, async (route) => {
		const response = await route.fetch();
		await g.opened;
		await route.fulfill({ response });
	});
	const asked = bruno.page.waitForRequest((r) => r.url().includes(`/api/channels/${dm}/messages`));
	// ?c= is how the app is told which conversation to open on arrival.
	await bruno.page.goto(`/?c=${dm}`);
	await asked;
	await bruno.page.waitForTimeout(3_000); // the socket, open, asking once more

	await ana.page.evaluate((m) => fetch(`/api/messages/${m}`, { method: 'DELETE' }), id);
	await bruno.page.waitForTimeout(1_500);
	g.open();

	await expect(bruno.page.locator('[data-message]', { hasText: 'mensagem removida' })).toBeVisible({
		timeout: 15_000,
	});
	await bruno.page.waitForTimeout(1_000);
	await expect(bruno.page.locator('[data-message]', { hasText: 'isto vai ser apagado' })).toHaveCount(0);
	await ana.close();
	await bruno.close();
});

// 10. A refused edit closed the box and lost the change without a word.
test('uma edição que falha deixa a caixa aberta e diz que não salvou', async ({ browser }) => {
	const ana = await Person.create(browser, 'c10ana');
	await ana.inChannel('geral').waitFor({ timeout: 15_000 });
	await ana.inChannel('geral').fill('texto original c10');
	await ana.page.keyboard.press('Enter');
	const msg = ana.page.locator('[data-message]', { hasText: 'texto original c10' });
	await expect(msg).toBeVisible({ timeout: 15_000 });

	await ana.page.route('**/api/messages/*', (route) =>
		route.request().method() === 'PATCH'
			? route.fulfill({ status: 500, contentType: 'application/json', body: '{"error":"server_error"}' })
			: route.continue(),
	);
	await msg.hover();
	await msg.locator('button[aria-label="editar"]').click();
	// While it is being edited its text is the box's value, not the page's,
	// so the box is found by being the only one inside a message.
	const editor = ana.page.locator('[data-message] textarea');
	// The box fills itself with the old text after it appears; typing before
	// that lands in front of it (a race no person's hands can win).
	await expect(editor).toHaveValue('texto original c10');
	await editor.fill('texto novo que não pode sumir');
	await editor.press('Enter');

	await expect(ana.page.getByText(/Não salvou/)).toBeVisible({ timeout: 10_000 });
	await expect(editor).toHaveValue('texto novo que não pode sumir');
	await ana.close();
});

// 11. A failed check left the notifications section at "Verificando…", with no
// switch to press, for as long as it was open.
test('os avisos dizem quando não conseguiram perguntar, e deixam tentar de novo', async ({ browser }) => {
	const ana = await Person.create(browser, 'c11ana');
	const supported = await ana.page.evaluate(
		() => 'serviceWorker' in navigator && 'PushManager' in window && 'Notification' in window,
	);
	test.skip(!supported, 'this browser has no push to ask about');
	let calls = 0;
	await ana.page.route('**/api/push/status**', (route) => {
		calls++;
		return route.fulfill({ status: 500, contentType: 'application/json', body: '{"error":"server_error"}' });
	});
	await settings(ana.page, 'avisos');
	await expect(ana.page.getByText(/Não consegui perguntar/)).toBeVisible({ timeout: 10_000 });
	const before = calls;
	await ana.page.getByRole('button', { name: /tentar de novo/i }).click();
	await expect.poll(() => calls).toBeGreaterThan(before);
	await ana.close();
});

// 12. The read mark moved on the screen before the server had it, and a refused
// one was never sent again.
test('a leitura que o servidor recusou é mandada de novo', async ({ browser }) => {
	const ana = await Person.create(browser, 'c12ana');
	const bruno = await Person.create(browser, 'c12bruno');
	await bruno.openDMWith('c12ana');
	await bruno.composerFor('c12ana').waitFor({ timeout: 15_000 });
	const dm = await channelID(bruno, { kind: 'dm' });
	const id = await say(bruno, dm, 'leia isto');

	await ana.page.route('**/api/channels/*/read', (route) =>
		route.fulfill({ status: 500, contentType: 'application/json', body: '{"error":"server_error"}' }),
	);
	await ana.openMessages();
	const refused = ana.page.waitForResponse((r) => r.url().endsWith('/read') && r.status() === 500);
	await ana.page.locator('nav button', { hasText: 'c12bruno' }).first().click();
	await expect(ana.page.locator('[data-message]', { hasText: 'leia isto' })).toBeVisible({ timeout: 15_000 });
	await refused;

	// The network is back. Leaving and coming back is enough of a reason to try.
	await ana.page.unroute('**/api/channels/*/read');
	await ana.openChannels();
	await ana.page.locator('nav button[data-space="server"]').first().click();
	await ana.page.locator('nav button[aria-label="#geral"]').first().click();
	await ana.inChannel('geral').waitFor({ timeout: 15_000 });
	await ana.openMessages();
	await ana.page.locator('nav button', { hasText: 'c12bruno' }).first().click();

	await expect
		.poll(
			() =>
				ana.page.evaluate(async (c) => {
					const res = await fetch('/api/channels').then((r) => r.json());
					return res.channels.find((x: { id: string }) => x.id === c)?.last_read_id ?? 0;
				}, dm),
			{ timeout: 10_000 },
		)
		.toBeGreaterThanOrEqual(id);
	await ana.close();
	await bruno.close();
});

// 13. Emptying the search box did not forget the search already in the air, and
// its answer landed under a box with nothing in it.
test('apagar a busca não deixa voltar o resultado da palavra apagada', async ({ browser }) => {
	const ana = await Person.create(browser, 'c13ana');
	await ana.inChannel('geral').waitFor({ timeout: 15_000 });
	await ana.inChannel('geral').fill('zebraquadrada c13');
	await ana.page.keyboard.press('Enter');
	await expect(ana.page.locator('[data-message]', { hasText: 'zebraquadrada' })).toBeVisible({ timeout: 15_000 });

	const g = gate();
	const asked = ana.page.waitForRequest((r) => r.url().includes('/api/search'));
	await ana.page.route('**/api/search**', async (route) => {
		await g.opened;
		await route.continue();
	});
	await ana.page.locator('button[aria-label="procurar"]').first().click();
	await ana.page.locator('[data-search-box]').fill('zebraquadrada');
	await asked;
	await ana.page.locator('[data-search-box]').fill('');
	const stale = ana.page.waitForResponse((r) => r.url().includes('/api/search'));
	g.open();
	await stale;
	await ana.page.evaluate(() => new Promise((r) => requestAnimationFrame(() => setTimeout(r, 50))));
	await expect(ana.page.locator('[data-search-results] [data-hit]')).toHaveCount(0, { timeout: 1 });
	await ana.close();
});

// 14. The account's own doors failed without a word: a device that could not
// be disconnected, a sign-out that did not happen.
test('desconectar e sair dizem quando não aconteceram', async ({ browser }) => {
	const ana = await Person.create(browser, 'c14ana');
	const outro = await Person.signIn(browser, 'c14ana');
	await ana.page.route('**/api/sessions/*', (route) =>
		route.request().method() === 'DELETE'
			? route.fulfill({ status: 500, contentType: 'application/json', body: '{"error":"server_error"}' })
			: route.continue(),
	);
	await ana.page.route('**/api/logout', (route) =>
		route.fulfill({ status: 500, contentType: 'application/json', body: '{"error":"server_error"}' }),
	);
	await settings(ana.page, 'conta');
	await ana.page.getByRole('button', { name: /^desconectar$/i }).first().click();
	await expect(ana.page.getByText(/Não desconectou/)).toBeVisible({ timeout: 10_000 });

	await ana.page.getByRole('button', { name: /sair desta conta/i }).click();
	// Leaving asks first (D4v); the answer is the red button in the box.
	await ana.page.locator('[data-confirm] [data-confirm-yes]').click();
	await expect(ana.page.getByText(/Não saiu/)).toBeVisible({ timeout: 10_000 });
	expect(ana.page.url()).not.toMatch(/\/login/);
	await ana.close();
	await outro.close();
});
