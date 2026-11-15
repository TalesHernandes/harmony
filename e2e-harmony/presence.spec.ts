// Sprint D4 in two real browsers: the dot next to a name, and the doorbell.
//
// These are the parts no Go test can answer. Presence is a chain — one browser
// opens a socket, the server notices, a second browser is told, and a dot
// changes colour — and the only way to know the chain is whole is to watch the
// far end of it. The same for a call arriving on somebody else's screen.
//
// What is deliberately *not* here: the push itself. A push needs Google's or
// Mozilla's push service and a real phone; the server's half of it is proved
// against a fake push service in internal/harmony/api/notify_test.go, and the
// last step — "toca no bolso" — is the owner's to test.

import { test, expect, Person } from './fixtures';

// state reads the dot the people list is drawing for somebody.
async function state(who: Person, about: string): Promise<string> {
	return who.page
		.locator(`aside button[data-person="${about}"]`)
		.getAttribute('data-state')
		.then((s) => s ?? '');
}

// The button is a toggle, so a second call would close the list instead of
// opening it. Idempotent on purpose: a test that presses it twice is a test
// that fails for a reason that has nothing to do with what it is checking.
async function openPeople(who: Person): Promise<void> {
	// The question is whether the *panel* is open. The rows inside it arrive
	// from the server a moment later, so "no rows yet" is not "closed" — and
	// reading it that way presses the toggle and closes a panel that was
	// already open, which is the opposite of idempotent.
	const panel = who.page.locator('aside');
	const list = who.page.locator('aside button[data-person]').first();
	if (!(await panel.isVisible())) {
		await who.page.locator('button[aria-label="pessoas"]').first().click();
	}
	await list.waitFor({ timeout: 15_000 });
}

// hide is a phone whose screen went off, as far as a browser is concerned:
// document.visibilityState is what the app watches, and this is the only way to
// move it from a test.
async function hide(who: Person, hidden: boolean): Promise<void> {
	await who.page.evaluate((isHidden) => {
		Object.defineProperty(document, 'visibilityState', {
			configurable: true,
			get: () => (isHidden ? 'hidden' : 'visible'),
		});
		document.dispatchEvent(new Event('visibilitychange'));
	}, hidden);
}

test('online, fora da aba, em chamada e offline, vistos do outro navegador', async ({ browser }) => {
	const ana = await Person.create(browser, 'presana');
	const bruno = await Person.create(browser, 'presbru');
	await openPeople(ana);

	// Two people with the app open see each other as online.
	await expect.poll(() => state(ana, 'presbru'), { timeout: 15_000 }).toBe('online');

	// Bruno goes behind another window. Since 13/09/2026 that is not leaving:
	// "ausente no pc tem que ser 5 minutos fora da aba". He stays online, and
	// stays online for a good while.
	await hide(bruno, true);
	await ana.page.waitForTimeout(2_000);
	expect(await state(ana, 'presbru')).toBe('online');

	// The five minutes themselves are not proven here, and cannot be: it would
	// be a test that sleeps for five minutes, and the clock that decides is the
	// server's. Go moves that clock by hand instead —
	// TestHiddenTabStaysOnlineForFiveMinutes, and the two beside it.

	// And comes back.
	await hide(bruno, false);
	await expect.poll(() => state(ana, 'presbru'), { timeout: 15_000 }).toBe('online');

	// In a call beats everything else: it is the most useful thing to know
	// about somebody, and it comes from the room, not from the presence answer.
	await bruno.joinVoice();
	await expect.poll(() => state(ana, 'presbru'), { timeout: 30_000 }).toBe('in_call');

	// The app closes. Nothing is stored, so what is left is offline.
	await bruno.close();
	await expect.poll(() => state(ana, 'presbru'), { timeout: 30_000 }).toBe('offline');

	// And Ana never became anything she did not ask to be.
	expect(await state(ana, 'presana')).toBe('online');

	await ana.close();
});

test('a campainha chega na tela de quem está com o app aberto', async ({ browser }) => {
	const ana = await Person.create(browser, 'ringana');
	const bruno = await Person.create(browser, 'ringbru');

	// You call people to where you are, so Ana goes into the room first.
	await ana.joinVoice();
	await openPeople(ana);
	await ana.page.locator('aside button[data-person="ringbru"]').click();

	const profile = ana.page.getByRole('dialog');
	await profile.waitFor({ timeout: 10_000 });
	await profile.getByRole('button', { name: /chamar/i }).click();

	// On Bruno's screen, with no notification involved: he has the app open, so
	// the server deliberately does not wake his phone (notify.go).
	const ring = bruno.page.getByText(/está te chamando/i);
	await ring.waitFor({ timeout: 15_000 });

	// The box is drawn against the glass rather than against the app, so it is
	// outside the padding that keeps everything else off the notch and has to
	// keep off it on its own. Same trick as the test below: move the strip by
	// hand, because a test browser has no notch.
	await bruno.page.evaluate(() => {
		document.documentElement.style.setProperty('--safe-top', '40px');
	});
	const box = await bruno.page.locator('div.safe-fixed-top').boundingBox();
	const card = await ring.boundingBox();
	expect(box, 'the ring box is not anchored to the glass any more').not.toBeNull();
	expect(Math.round(card!.y), 'a call arriving lands under the notch').toBeGreaterThanOrEqual(40);

	await bruno.page.getByRole('button', { name: /^entrar$/i }).click();
	await bruno.page.locator('[data-call="live"]').waitFor({ timeout: 30_000 });

	// And the two of them are in the same room.
	await expect.poll(async () => (await ana.peopleInRoom()).sort(), { timeout: 30_000 }).toEqual([
		'ringana',
		'ringbru',
	]);

	// The diag log is the thing the owner is asked for when this goes wrong, so
	// it has to say that it happened.
	expect(await bruno.diag()).toContain('chamada de @ringana');

	await bruno.close();
	await ana.close();
});

test('o app se instala como PWA e sabe dizer se as notificações estão ligadas', async ({ browser }) => {
	const ana = await Person.create(browser, 'pwaana');

	// The service worker is what makes a notification with the app closed
	// possible at all, and it has to be served from the root or its scope
	// cannot cover the app.
	const scope = await ana.page.evaluate(async () => {
		const reg = await navigator.serviceWorker.ready;
		return reg.scope;
	});
	expect(new URL(scope).pathname).toBe('/');

	// The manifest is what "adicionar à tela de início" reads — the only way an
	// iPhone will ever show a notification from this app.
	const manifest = await ana.page.evaluate(async () => {
		const link = document.querySelector('link[rel="manifest"]') as HTMLLinkElement | null;
		if (!link) return null;
		const res = await fetch(link.href);
		return res.ok ? await res.json() : null;
	});
	expect(manifest?.scope, 'the manifest must claim the whole site').toBe('/');
	expect(manifest?.start_url).toBe('/');
	expect(manifest?.icons?.length, 'a home screen needs an icon').toBeGreaterThan(1);

	// Nothing was asked of anybody on the way in. Asking for notification
	// permission before somebody has used the app is how a browser gets told
	// "no" for ever, so the app only ever asks from a press — and the app's own
	// log is where that would show if it had.
	expect(await ana.diag()).not.toContain('permissao de notificacao');

	// A browser driven by a test refuses notifications outright — there is no
	// "ainda não perguntei" in it — so the one state a person actually arrives
	// in has to be faked to be looked at. This is a lie about the browser, not
	// about the app: everything after it is the app's own answer.
	await ana.page.evaluate(() => {
		Object.defineProperty(Notification, 'permission', { configurable: true, get: () => 'default' });
	});

	// The switch is in Ajustes, and what it says comes from the server as much
	// as from the browser: a doorbell that is off and looks fine is the failure
	// this whole feature is prone to.
	await ana.page.locator('button[aria-label="ajustes"]').first().click();
	// Since D4h the settings are four tabs instead of one long column, and the
	// doorbell is under the one named after it.
	await ana.page.locator('[data-tab="avisos"]').click();
	await expect(ana.page.getByText(/Desligado\. Ligue para ser avisado/i)).toBeVisible({
		timeout: 10_000,
	});
	await expect(ana.page.getByRole('button', { name: /ligar neste aparelho/i })).toBeVisible();

	await ana.close();
});

// The bug the owner hit the first time he opened the app from the home screen:
// the box you type in was not there. It could only ever happen there — in a
// browser tab the address bar and the toolbar sit in those strips, so nothing
// of ours was ever under them; installed, the app owns the whole glass and the
// last thing on the page ends up beneath the system's own bar.
//
// A test browser has no notch and no navigation bar, so the strips are moved by
// hand: the app reads them from CSS variables (src/style.css) exactly so that
// this is possible. Setting them through the CSSOM rather than a <style> tag is
// deliberate — the CSP has no unsafe-inline, and it blocks the tag.
test('o que o sistema cobre não come a caixa de escrever', async ({ browser }) => {
	const ana = await Person.create(browser, 'safeana', { viewport: Person.PHONE });
	const composer = ana.page.locator('[data-composer]');
	await composer.waitFor({ timeout: 15_000 });

	// With no strips — a computer, or a browser tab — the box sits on the very
	// bottom edge, which is where it belongs and where it has always been.
	const screen = await ana.page.evaluate(() => window.innerHeight);
	const before = await composer.boundingBox();
	expect(Math.round(before!.y + before!.height)).toBe(screen);

	// Now a telephone with a notch at the top and a navigation bar at the
	// bottom, which is what the installed app is looking at.
	const TOP = 40;
	const BOTTOM = 60;
	await ana.page.evaluate(
		({ top, bottom }) => {
			document.documentElement.style.setProperty('--safe-top', `${top}px`);
			document.documentElement.style.setProperty('--safe-bottom', `${bottom}px`);
		},
		{ top: TOP, bottom: BOTTOM },
	);

	const after = await composer.boundingBox();
	expect(
		Math.round(after!.y + after!.height),
		'the box you type in is underneath the system bar',
	).toBeLessThanOrEqual(screen - BOTTOM);

	// And the header did not go under the notch either.
	const header = await ana.page.locator('header').first().boundingBox();
	expect(Math.round(header!.y), 'the channel name is under the notch').toBeGreaterThanOrEqual(TOP);

	await ana.close();
});

// The measurement that exists because the fix above was a guess and the guess
// was wrong. Installed on the owner's telephone the composer was still not
// there, and nothing on that screen could say whether it was missing, empty or
// below the bottom edge. This proves the diag log tells those apart — including
// the case nobody can reproduce here, which is forced by hand.
test('o diaglog sabe dizer onde a caixa de escrever foi parar', async ({ browser }) => {
	const ana = await Person.create(browser, 'geoana', { viewport: Person.PHONE });
	await ana.page.locator('[data-composer]').waitFor({ timeout: 15_000 });

	const diag = await ana.diag();
	expect(diag, 'the log has to carry the measurement, not only the events').toContain('escrever-ate');
	expect(diag).toContain('janela 390x780');
	expect(diag).toContain('aba');
	// Which bundle answered is half of any "it did not update" question.
	expect(diag).toMatch(/pacote index-.*\.js/);

	// The number that matters: with the app the right height, the box ends at
	// the bottom edge and not past it.
	const line = /escrever-ate (\d+)/.exec(diag);
	expect(line, 'the log says the composer is not on the page at all').not.toBeNull();
	expect(Number(line![1])).toBeLessThanOrEqual(780);

	// Now force the shape of the owner's bug — an app taller than the glass, so
	// the last thing on the page falls off the bottom — and check the log
	// reports the box *past* the edge instead of quietly saying nothing.
	await ana.page.evaluate(() => {
		document.getElementById('app')!.style.height = '980px';
	});
	await ana.page.evaluate(() => {
		const w = window as unknown as { __harmonyDiag?: () => string };
		return w.__harmonyDiag?.();
	});
	const after = /escrever-ate (\d+)/.exec(await ana.diag());
	expect(Number(after![1]), 'a box pushed off the bottom must show as such').toBeGreaterThan(780);

	await ana.close();
});

// The bug the owner's diag log finally named, pinned as a rule.
//
// His installed app measured `janela 739` and `app 796`: the height the app
// asked for was 57px taller than the glass — the Android navigation bar — and
// the footer was drawn underneath it. A test browser will not reproduce that
// (its two answers agree), so what is checked here is the property that makes
// the app immune to it: **the app is never taller than what contains it, and
// never taller than what is visible**, whichever of the two is smaller.
test('o app nunca fica mais alto do que cabe na tela', async ({ browser }) => {
	const ana = await Person.create(browser, 'altana', { viewport: Person.PHONE });
	await ana.page.locator('[data-composer]').waitFor({ timeout: 15_000 });

	const heights = async () =>
		ana.page.evaluate(() => ({
			app: document.getElementById('app')!.offsetHeight,
			window: window.innerHeight,
			composer: Math.round(
				document.querySelector('[data-composer]')!.getBoundingClientRect().bottom,
			),
		}));

	const fits = await heights();
	expect(fits.app).toBe(fits.window);
	expect(fits.composer).toBeLessThanOrEqual(fits.window);

	// Now the shape of his telephone: something above the app claims more room
	// than the screen has. The app must take the screen's answer, not that one.
	await ana.page.evaluate(() => {
		document.body.style.height = '2000px';
	});
	const roomy = await heights();
	expect(roomy.app, 'the app grew past the glass because its parent said it could').toBe(
		roomy.window,
	);
	expect(
		roomy.composer,
		'the box you type in is off the bottom of the screen',
	).toBeLessThanOrEqual(roomy.window);

	// And the other direction still holds: given less room than the screen, the
	// app takes the room, not the screen.
	await ana.page.evaluate(() => {
		document.body.style.height = '300px';
	});
	expect((await heights()).app).toBe(300);

	await ana.close();
});

// "A barra de novas mensagens aparece por uma fração de segundo e desaparece."
//
// The line that says where you stopped reading has to stay where it was when
// you arrived — that is the whole use of it. Reading the channel is what moves
// the read mark, and the mark moving must not be allowed to erase the line
// drawn from where it used to be.
test('a barra de não lidas fica onde estava, e não pisca', async ({ browser }) => {
	const ana = await Person.create(browser, 'lidaana');
	const bruno = await Person.create(browser, 'lidabru');

	// First she has to have read something, or there is no "where I stopped" to
	// draw a line at — which is correct, and is why this needs two messages.
	await bruno.page.getByPlaceholder(/Escrever em/).fill('a primeira, que ela le');
	await bruno.page.getByRole('button', { name: /enviar/i }).click();
	await ana.page.getByText('a primeira, que ela le').waitFor({ timeout: 10_000 });

	// Then she walks away from the text channel — into the voice channel, which
	// has no history and so does not mark anything read.
	await ana.openChannels();
	await ana.page.locator('nav button[title*="entrar na sala" i]:visible').first().click();

	// And Bruno says something while she is not looking.
	await bruno.page.getByPlaceholder(/Escrever em/).fill('a segunda, que ela nao leu');
	await bruno.page.getByRole('button', { name: /enviar/i }).click();

	// She comes back to the channel.
	await ana.openChannels();
	await ana.page.locator('nav button[aria-label="#geral"]').first().click();

	const bar = ana.page.getByText('não lidas', { exact: true });
	await bar.waitFor({ timeout: 10_000 });

	// And it is still there a moment later, after reading it has moved the mark
	// on the server and the answer has come back down the socket.
	await ana.page.waitForTimeout(3000);
	await expect(bar, 'the unread line was erased by the reading of it').toBeVisible();

	// The other way in, and the one the owner described: the page is loaded
	// fresh and lands straight on a channel whose last message is somebody
	// else's. Here the mark comes from the server rather than from memory — and
	// she has to be *away* from the channel while it arrives, or reading it is
	// what happens instead.
	// Out of the room before reloading. A page that reloads while inside one
	// leaves a connection the media server takes about fifteen seconds to give
	// up on, and every test in this suite shares the same room — a handful of
	// those and the room is full, which is a failure in some other test file
	// that has nothing to do with this one.
	await ana.page.evaluate(async () => {
		const w = window as unknown as { __harmonyVoice?: { leave(): Promise<void> } };
		await w.__harmonyVoice?.leave();
	});
	await ana.openChannels();
	await ana.page.locator('nav button[aria-label="#geral"]').first().click();
	await ana.page.locator('[data-composer]').waitFor({ timeout: 10_000 });
	await ana.page.waitForTimeout(300);

	// She steps away again, this time without touching the room.
	await openPeople(ana);
	await ana.page.locator('aside button[data-person="dono"]').click();
	await ana.page.getByRole('dialog').getByRole('button', { name: /conversar no privado/i }).click();
	await bruno.page.getByPlaceholder(/Escrever em/).fill('a terceira, com ela fora do canal');
	await bruno.page.getByRole('button', { name: /enviar/i }).click();
	await ana.page.waitForTimeout(500);
	await ana.page.reload();

	const afterReload = ana.page.getByText('não lidas', { exact: true });
	await afterReload.waitFor({ timeout: 15_000 });
	await ana.page.waitForTimeout(3000);
	await expect(afterReload, 'the unread line vanished after a reload').toBeVisible();

	// And it survives the thing that can retake the photograph: whatever makes
	// the app look at the channel again while the mark has already moved on.
	// Forced here, because the owner's own trigger for it is not known.
	await ana.page.evaluate(() => window.dispatchEvent(new Event('resize')));
	await bruno.page.getByPlaceholder(/Escrever em/).fill('mais uma, com ela olhando');
	await bruno.page.getByRole('button', { name: /enviar/i }).click();
	await ana.page.waitForTimeout(2500);
	await expect(
		afterReload,
		'the line was erased by the conversation carrying on around it',
	).toBeVisible();

	await bruno.close();
	await ana.close();
});

// The 25ms the owner kept seeing, caught by watching every frame.
//
// His sharper description is what named it: the line flashes when he switches
// *away from* a channel whose last message is somebody else's. It was never the
// line being erased — it was a line drawn in the wrong conversation. Changing
// channel draws the new one first and takes the photograph of where you stopped
// in it a moment later, and in between the previous channel's mark was being
// laid over these messages.
//
// A human eye can miss that and a normal assertion certainly does, so this
// samples every animation frame for a second and a half and counts.
test('a linha de não lidas nunca aparece no canal errado', async ({ browser }) => {
	const ana = await Person.create(browser, 'flashana');
	const bruno = await Person.create(browser, 'flashbru');

	// Ana reads #geral, so she has a mark there…
	//
	// The words are this test's and nobody else's. #geral keeps its history and
	// every test in this file writes into it, so a line that reads like another
	// test's line is found the instant this one starts — in the *older*
	// message — and the wait returns before anything new has arrived. Which is
	// what "a primeira, que ela le", two tests up, used to do to this one.
	await bruno.page.getByPlaceholder(/Escrever em/).fill('flash: a lida de antes');
	await bruno.page.getByRole('button', { name: /enviar/i }).click();
	await ana.page.getByText('flash: a lida de antes').first().waitFor({ timeout: 10_000 });
	await ana.page.waitForTimeout(800);

	// …and Bruno opens a private conversation with her, which she has never
	// read. Nothing in it can ever be "unread since where I stopped", because
	// she never stopped anywhere in it.
	await openPeople(bruno);
	await bruno.page.locator('aside button[data-person="flashana"]').click();
	await bruno.page.getByRole('dialog').getByRole('button', { name: /conversar no privado/i }).click();
	await bruno.composerFor('flashana').fill('oi, so entre nos dois');
	await bruno.page.getByRole('button', { name: /enviar/i }).click();

	// The conversation lives on the other half of the column since D4c.
	await ana.openMessages();
	const conversation = ana.page.locator('nav button', { hasText: 'flashbru' }).first();
	await conversation.waitFor({ timeout: 10_000 });

	// Watch every frame from just before the switch until well after it.
	await ana.page.evaluate(() => {
		const w = window as unknown as { __unreadFrames: number };
		w.__unreadFrames = 0;
		const stop = performance.now() + 1500;
		const tick = () => {
			const seen = [...document.querySelectorAll('span')].some(
				(s) => s.textContent === 'não lidas',
			);
			if (seen) w.__unreadFrames++;
			if (performance.now() < stop) requestAnimationFrame(tick);
		};
		requestAnimationFrame(tick);
	});

	await conversation.click();
	await ana.page.waitForTimeout(1800);

	const frames = await ana.page.evaluate(
		() => (window as unknown as { __unreadFrames: number }).__unreadFrames,
	);
	expect(
		frames,
		'the unread line was drawn in a conversation it does not belong to',
	).toBe(0);

	await bruno.close();
	await ana.close();
});

// "Não encontrado" used to sit at the top of the conversation until the page was
// reloaded, which makes something that already passed look like something still
// wrong. The owner asked for five seconds and an X; this is both.
test('um aviso de erro sai sozinho, e sai no X', async ({ browser }) => {
	const ana = await Person.create(browser, 'avisoana');

	// A failure that is certain to happen, without needing the server to break.
	await ana.page.route('**/api/dms', (route) =>
		route.fulfill({ status: 404, contentType: 'application/json', body: '{"error":"not_found"}' }),
	);

	const open = async () => {
		await openPeople(ana);
		await ana.page.locator('aside button[data-person="dono"]').click();
		await ana.page.getByRole('dialog').getByRole('button', { name: /conversar no privado/i }).click();
	};

	await open();
	const warning = ana.page.getByRole('alert');
	await expect(warning).toContainText(/não encontrado/i, { timeout: 10_000 });
	await warning.getByRole('button', { name: /fechar aviso/i }).click();
	await expect(warning, 'the X did not close the warning').toBeHidden();

	// And left alone, it goes on its own.
	await open();
	await expect(ana.page.getByRole('alert')).toBeVisible({ timeout: 10_000 });
	await expect(ana.page.getByRole('alert'), 'the warning stayed on the screen').toBeHidden({
		timeout: 8000,
	});

	await ana.close();
});

// There is no conversation with yourself on this server, so the button that
// would open one is not drawn on your own profile. Asking for it answers the
// same 404 as asking for a conversation with a name that does not exist — which
// is on purpose, and is how nobody can find out who has an account here.
test('o seu próprio perfil não oferece uma conversa com você mesmo', async ({ browser }) => {
	const ana = await Person.create(browser, 'soana');

	await openPeople(ana);
	await ana.page.locator('aside button[data-person="soana"]').click();
	const mine = ana.page.getByRole('dialog');
	await mine.waitFor({ timeout: 10_000 });
	await expect(mine.getByRole('button', { name: /conversar no privado/i })).toHaveCount(0);

	// And on somebody else's profile it is there, so this is a rule and not a
	// button that quietly went missing.
	await ana.page.keyboard.press('Escape');
	await ana.page.locator('aside button[data-person="dono"]').click();
	await expect(
		ana.page.getByRole('dialog').getByRole('button', { name: /conversar no privado/i }),
	).toBeVisible();

	await ana.close();
});
