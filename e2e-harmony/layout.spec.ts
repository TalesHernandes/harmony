// Sprint D4c — the round of changes the owner asked for after living in the app
// for a day. Seventeen of them, and these are the ones a browser can answer.
//
// Most of this file exists for one reason: everything in it is a thing that was
// there, was moved, and could silently come back or silently vanish in a later
// sprint. A button removed on purpose is exactly the kind of change nothing
// notices when it undoes itself.

import { test, expect, Person } from './fixtures';

// waitForTalk looks for a conversation in the column, re-opening the
// conversations half each time. The re-opening is the point: a conversation
// somebody else has just started arrives on the socket a moment later, and the
// app moves the column to wherever the channel it is *showing* lives — so a
// single click on "Mensagens" before the conversation exists can be undone
// before it arrives.
async function waitForTalk(who: Person, name: string) {
	await expect
		.poll(
			async () => {
				await who.openMessages();
				return who.page.locator('nav button').filter({ hasText: name }).count();
			},
			{ timeout: 25_000 },
		)
		.toBeGreaterThan(0);
	return who.page.locator('nav button').filter({ hasText: name }).first();
}

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

// -------------------------------------------------------------------------
// 2 — as conversas saem do menu do servidor
// -------------------------------------------------------------------------

test('o servidor e as conversas são dois lugares, e abrir uma conversa leva o trilho junto', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'layana');
	const bruno = await Person.create(browser, 'laybru');

	// The two choices sit inside the column, under the name of the service and
	// above the channels — the owner's second look at it, after the rail of
	// icons down the left edge took width from the only list that grows.
	const tabs = ana.page.locator('nav.channel-column button[data-space]');
	await expect(tabs).toHaveCount(2);
	await expect(ana.page.locator('nav button[data-space="server"]').first()).toHaveAttribute(
		'aria-pressed',
		'true',
	);
	await expect(ana.page.getByRole('button', { name: '#geral' })).toBeVisible();

	// The other side is a different column, and it starts empty.
	await ana.page.locator('nav button[data-space="dms"]').first().click();
	await expect(ana.page.getByText('Nenhuma conversa ainda.')).toBeVisible();
	await expect(ana.page.getByRole('button', { name: '#geral' })).toBeHidden();

	// Opening a conversation from the people list is the way in, and the rail
	// has to follow it: a conversation opened while the column is showing the
	// server would otherwise leave the column pointing somewhere else.
	await ana.page.locator('nav button[data-space="server"]').first().click();
	await openPeople(ana);
	await ana.page.locator('aside button[data-person="laybru"]').click();
	await ana.page.getByRole('button', { name: /Conversar no privado/ }).click();
	await expect(ana.page.locator('nav button[data-space="dms"]').first()).toHaveAttribute(
		'aria-pressed',
		'true',
	);
	// And the column really is showing that half: the channels are gone.
	await expect(ana.page.getByRole('button', { name: '#geral' })).toBeHidden();

	await bruno.context.close();
	await ana.context.close();
});

// -------------------------------------------------------------------------
// 3 — o menu do botão direito
// -------------------------------------------------------------------------

test('o botão direito numa pessoa abre o menu, e Escape o fecha', async ({ browser }) => {
	const ana = await Person.create(browser, 'menuana');
	const bruno = await Person.create(browser, 'menubru');
	await openPeople(ana);

	const bru = ana.page.locator('aside button[data-person="menubru"]');
	await bru.waitFor({ timeout: 15_000 });
	await bru.click({ button: 'right' });

	const menu = ana.page.locator('[data-menu]');
	await expect(menu).toBeVisible();
	await expect(menu.getByRole('menuitem', { name: 'Ver perfil' })).toBeVisible();
	await expect(menu.getByRole('menuitem', { name: 'Conversar no privado' })).toBeVisible();
	// Nobody is in a room, so there is nothing to call anybody into. The server
	// would refuse it anyway; not drawing it is the courtesy.
	await expect(menu.getByRole('menuitem', { name: /Chamar para a sala/ })).toBeHidden();

	// It has to be *on the screen*, not half outside it — the whole reason it
	// waits a frame before showing itself.
	const box = await menu.boundingBox();
	expect(box).not.toBeNull();
	expect(box!.x).toBeGreaterThanOrEqual(0);
	expect(box!.y).toBeGreaterThanOrEqual(0);
	const size = ana.page.viewportSize()!;
	expect(box!.x + box!.width).toBeLessThanOrEqual(size.width);
	expect(box!.y + box!.height).toBeLessThanOrEqual(size.height);

	await ana.page.keyboard.press('Escape');
	await expect(menu).toBeHidden();

	// And on your own name there is no conversation to offer, because there is
	// no conversation with yourself (the server answers 404, D4 sixth round).
	await ana.page.locator('aside button[data-person="menuana"]').click({ button: 'right' });
	await expect(
		ana.page.locator('[data-menu]').getByRole('menuitem', { name: 'Conversar no privado' }),
	).toBeHidden();

	await bruno.context.close();
	await ana.context.close();
});

// -------------------------------------------------------------------------
// 11 e 12 — responder uma mensagem, e a data nela
// -------------------------------------------------------------------------

test('responder cita a mensagem certa, e a citação leva até ela', async ({ browser }) => {
	const ana = await Person.create(browser, 'repana');
	const bruno = await Person.create(browser, 'repbru');

	await ana.page.getByPlaceholder(/Escrever em/).fill('alguem vai jogar hoje?');
	await ana.page.keyboard.press('Enter');
	const original = bruno.page.getByText('alguem vai jogar hoje?');
	await original.waitFor({ timeout: 15_000 });

	// The action only appears under the pointer, which is what stopped it
	// pushing every message down (D4 fifth round).
	await original.hover();
	await bruno.page.getByRole('button', { name: 'responder' }).first().click();

	// Who is being answered, above the box, before a word is typed.
	await expect(bruno.page.locator('[data-replying]')).toBeVisible();
	await expect(bruno.page.locator('[data-replying]')).toContainText('repana');

	await bruno.page.getByPlaceholder(/Escrever em/).fill('eu vou');
	await bruno.page.keyboard.press('Enter');

	// The bar clears itself: the next message is not another reply by accident.
	await expect(bruno.page.locator('[data-replying]')).toBeHidden();

	// And the answer carries the line it answers, on the other person's screen.
	const answer = ana.page.locator('[data-message]').filter({ hasText: 'eu vou' }).last();
	await answer.waitFor({ timeout: 15_000 });
	await expect(answer.locator('[data-quote]')).toContainText('alguem vai jogar hoje?');

	// Pressing the quotation goes to the original.
	await answer.locator('[data-quote]').click();
	await expect(ana.page.getByText('alguem vai jogar hoje?').first()).toBeInViewport();

	// Escape drops the answer instead of sending it.
	await ana.page.getByText('eu vou').first().hover();
	await ana.page.getByRole('button', { name: 'responder' }).first().click();
	await expect(ana.page.locator('[data-replying]')).toBeVisible();
	await ana.page.getByPlaceholder(/Escrever em/).press('Escape');
	await expect(ana.page.locator('[data-replying]')).toBeHidden();

	await bruno.context.close();
	await ana.context.close();
});

// -------------------------------------------------------------------------
// 14 — a coluna que se arrasta
// -------------------------------------------------------------------------

test('a coluna se arrasta, para entre os limites, e continua lá depois de recarregar', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'colana');
	const column = ana.page.locator('.channel-column').first();
	const before = (await column.boundingBox())!.width;

	const handle = ana.page.locator('[data-resizer]');
	const at = (await handle.boundingBox())!;
	await ana.page.mouse.move(at.x + at.width / 2, at.y + 100);
	await ana.page.mouse.down();
	await ana.page.mouse.move(at.x + 80, at.y + 100, { steps: 10 });
	await ana.page.mouse.up();

	const after = (await column.boundingBox())!.width;
	expect(after).toBeGreaterThan(before);

	// Pulled far past the end, it stops at the end. A column that can be
	// dragged to nothing is a column that can be broken by accident, and one
	// dragged across the window has eaten the conversation.
	const edge = async (dx: number) => {
		const now = (await handle.boundingBox())!;
		await ana.page.mouse.move(now.x + now.width / 2, now.y + 100);
		await ana.page.mouse.down();
		await ana.page.mouse.move(now.x + dx, now.y + 100, { steps: 10 });
		await ana.page.mouse.up();
		return (await column.boundingBox())!.width;
	};
	// The two numbers are the ends of the ladder in App.tsx, and they moved up
	// a rung with the type: 208 and 448, not 192 and 384.
	expect(await edge(2000)).toBeLessThanOrEqual(448);
	const narrow = await edge(-2000);
	expect(narrow).toBeGreaterThanOrEqual(208);

	// And it is remembered, which is the whole reason it is worth having.
	await ana.page.reload();
	await ana.page.locator('.channel-column').first().waitFor({ timeout: 20_000 });
	const back = (await ana.page.locator('.channel-column').first().boundingBox())!.width;
	expect(Math.abs(back - narrow)).toBeLessThan(2);

	await ana.context.close();
});

// -------------------------------------------------------------------------
// 7 e 8 — o que saiu da tela
// -------------------------------------------------------------------------

test('as explicações óbvias e o botão repetido não estão mais lá', async ({ browser }) => {
	const ana = await Person.create(browser, 'gonana');

	await openPeople(ana);
	await expect(ana.page.getByText(/Verde é online, amarelo é ausente/)).toHaveCount(0);
	await expect(ana.page.getByText(/voltar para a chamada/)).toHaveCount(0);

	await ana.page.locator('button[aria-label="ajustes"]').click();
	await expect(ana.page.getByText(/Fica guardado para você neste aparelho/)).toHaveCount(0);
	// The one that stayed, and the reason it stayed: it is a fact about a
	// notification that cannot be seen from the outside. Since D4h the settings
	// are tabs, so it lives under the one it belongs to instead of in a single
	// long column.
	await ana.page.locator('[data-tab="avisos"]').click();
	await expect(ana.page.getByText(/nunca o que foi escrito/)).toBeVisible();

	await ana.context.close();
});

// -------------------------------------------------------------------------
// 17 — a faixa de baixo não é paga duas vezes numa aba
// -------------------------------------------------------------------------

test('numa aba a faixa de baixo é do navegador, e o app não a paga de novo', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'safana', { viewport: Person.PHONE });

	// This is a tab, not an installed app. Whatever the browser reports for the
	// bottom inset, the app must add nothing below the box you type in — that
	// dead band is what the owner saw on Firefox for Android.
	const bottom = await ana.page.evaluate(() =>
		getComputedStyle(document.documentElement).getPropertyValue('--safe-bottom').trim(),
	);
	expect(bottom).toBe('0px');

	// And the box is still on the screen, which is the thing the padding was
	// brought in to protect in the first place (D4 second round).
	const box = (await ana.page.locator('[data-composer]').boundingBox())!;
	expect(box.y + box.height).toBeLessThanOrEqual(Person.PHONE.height + 1);

	await ana.context.close();
});

// -------------------------------------------------------------------------
// D4d — a segunda volta do visual
// -------------------------------------------------------------------------

// The nine ceilings are the owner's, cell by cell, and this is where a change
// to them stops being silent. It checks the two ends of the table: the most a
// screen may ever cost, and the least.
test('cada tamanho e cada taxa tem o teto que o dono escolheu', async ({ browser }) => {
	const ana = await Person.create(browser, 'tetoana');
	await ana.joinVoice();
	await ana.shareScreen();
	await expect.poll(async () => (await ana.sending()).length, { timeout: 30_000 }).toBeGreaterThan(0);

	await ana.openPanel('screen');

	// This test was inverted for about an hour on 18/09/2026 and then put back,
	// and the hour is the reason it now walks every cell instead of two corners.
	//
	// The owner asked for no ceiling at all. It was removed, measured, and the
	// picture got worse by two thirds: 6926 kbit/s of delivered video became
	// 2418. There is no "no ceiling" to be had — libwebrtc simply substitutes
	// its own default, 2500 kbit/s above 960x540, and the estimate stops
	// probing upward because probing is capped at twice the allocation. So the
	// ceiling came back, and the owner picked the number himself: "um limite
	// muito alto, por exemplo de 50mbps" — and then, on 19/09/2026, brought it
	// back down to ten: "pode voltar para 10mbps", and then to fifteen the same
	// night once a real game showed ten was no longer enough. Simulcast is what
	// changed the question. See SCREEN_CEILING.
	//
	// What is proved here is that every mode carries its own, because a mode
	// whose ceiling silently reverted to libwebrtc's default would look exactly
	// like the bad hour and would only be noticed in a call months from now.
	const table: Record<string, Record<string, number>> = {
		'1080': { '60': 15_000_000, '30': 12_000_000, '15': 9_000_000 },
		'720': { '60': 12_000_000, '30': 9_000_000, '15': 6_000_000 },
		'480': { '60': 6_000_000, '30': 6_000_000, '15': 6_000_000 },
	};
	for (const [quality, rates] of Object.entries(table)) {
		await ana.choose('quality', quality);
		for (const [fps, want] of Object.entries(rates)) {
			await ana.choose('fps', fps);
			// The pair, waited for together: two cells can share a ceiling
			// (480p is 6 Mbit/s at every rate), and waiting on the ceiling
			// alone let the check read the frame rate before it had arrived.
			await expect
				.poll(
					async () => {
						const s = (await ana.sending())[0];
						return [s?.maxBitrate, s?.maxFramerate];
					},
					{ timeout: 15_000, message: `${quality}p at ${fps} did not cap the frame rate` },
				)
				.toEqual([want, Number(fps)]);
		}
	}

	await ana.close();
});

// The + on the room you are in, and the thing it opens. Both are the owner's
// second look at the invite button: it was in the call and he wanted it in the
// column.
test('o + na sala em que estou chama alguém para ela', async ({ browser }) => {
	const ana = await Person.create(browser, 'convana');
	const bruno = await Person.create(browser, 'convbru');

	// Outside a room there is nothing to invite anybody into, so there is no +.
	await expect(ana.page.locator('[data-open="invite"]')).toHaveCount(0);

	await ana.joinVoice();
	const plus = ana.page.locator('nav [data-open="invite"]');
	await plus.waitFor({ timeout: 20_000 });
	await plus.click();

	const panel = ana.page.locator('[data-invite]');
	await expect(panel).toBeVisible();
	// Everybody who is not already in the room, and never yourself.
	await expect(panel.locator('[data-ring="convbru"]')).toBeVisible();
	await expect(panel.locator('[data-ring="convana"]')).toHaveCount(0);

	await panel.locator('[data-ring="convbru"]').click();
	// It says so, so that pressing it twice is not the way somebody finds out
	// it worked.
	await expect(panel.locator('[data-ring="convbru"]')).toBeDisabled();
	// And it really arrived on the other screen.
	await expect(bruno.page.getByText(/está te chamando|te chamou/i).first()).toBeVisible({
		timeout: 20_000,
	});

	await ana.page.keyboard.press('Escape');
	await expect(panel).toBeHidden();

	await bruno.close();
	await ana.close();
});

// The connection readout is an icon now, not a number: the owner asked for the
// one he already knows how to read. The number moved to the tooltip, which is
// where a number belongs.
test('a qualidade da chamada é um ícone, e diz qual é', async ({ browser }) => {
	const ana = await Person.create(browser, 'sinalana');
	await ana.joinVoice();

	// Two of them since the call's redraw: the bars beside "Voz conectada" in
	// the column, and the "Boa · 38 ms" chip in the call's own bar. Both are
	// the same reading, and both have to say it.
	const meters = ana.page.locator('[data-quality]');
	await expect(meters).toHaveCount(2, { timeout: 30_000 });
	for (const meter of await meters.all()) await checkMeter(meter);
	// And they agree (the Codex, 23/09/2026: each was checked on its own, so
	// one "boa" beside one "ruim" passed).
	await expect(async () => {
		const ditos = await meters.evaluateAll((els) => els.map((e) => e.getAttribute('data-quality')));
		expect(new Set(ditos).size).toBe(1);
	}).toPass();
	await ana.close();
});

async function checkMeter(meter: import('@playwright/test').Locator) {
	// Between two browsers on this machine it can only be good; what matters
	// here is that it says *something* from the closed set, and that the
	// milliseconds are still reachable.
	//
	// Asked with expect() and not read with getAttribute(): the element is
	// drawn as soon as the call is live, and the first reading of the link
	// arrives a moment later. A single read catches whichever of the two got
	// there first; expect() keeps asking until the answer settles.
	await expect(meter).toHaveAttribute('data-quality', /^(boa|media|ruim)$/);
	await expect(meter).toHaveAttribute('title', /\d+ ms/);
	// Three bars, always, so that "two lit" is readable as less than three.
	await expect(meter.locator('[data-bar]')).toHaveCount(3);
}

// -------------------------------------------------------------------------
// D4e — silenciar, fechar, e o que estava mentindo
// -------------------------------------------------------------------------

// Silencing is one person's opinion about a shared channel: it stops asking
// *them* for anything and changes nothing for anybody else.
test('silenciar um canal para de contar, e só para quem silenciou', async ({ browser }) => {
	const ana = await Person.create(browser, 'mudoana');
	const bruno = await Person.create(browser, 'mudobru');

	// Ana has to be reading somewhere *else* for #geral to accumulate — a
	// channel you are looking at is a channel you have read. The voice room is
	// the somewhere else that is on the same half of the column, so #geral
	// stays in front of her the whole time.
	await ana.joinVoice();
	await bruno.page.getByPlaceholder(/Escrever em/).fill('primeira, contada');
	await bruno.page.keyboard.press('Enter');

	const geral = ana.page.locator('nav button[aria-label="#geral"]').first();
	await expect(geral).toContainText('1', { timeout: 15_000 });

	await geral.click({ button: 'right' });
	await ana.page.locator('[data-menu]').getByRole('menuitem', { name: 'Silenciar' }).click();
	// It says so on the row, so that a silenced channel is not a channel that
	// looks broken.
	await expect(geral.locator('[data-icon="bell-slash"]')).toBeVisible({ timeout: 15_000 });

	// Back to the room, and from here #geral asks for nothing at all — not even
	// for a mention, which is the loudest thing it has.
	await ana.page.locator('nav button[title*="entrar na sala" i]').first().click();
	// The mention goes in the middle on purpose: a body that *ends* in "@nome"
	// leaves the composer's completion list open, and Enter then completes the
	// name instead of sending the message.
	await bruno.page.getByPlaceholder(/Escrever em/).fill('segunda, @mudoana e nao conta');
	await bruno.page.keyboard.press('Enter');
	await ana.page.waitForTimeout(2000);
	await expect(geral).not.toContainText('2');
	await expect(geral).not.toContainText('1');

	// But it is all still there when she opens it.
	await geral.click();
	await expect(ana.page.getByText('e nao conta')).toBeVisible({ timeout: 15_000 });

	// And Bruno, who silenced nothing, counts as always.
	await bruno.joinVoice();
	await ana.page.getByPlaceholder(/Escrever em/).fill('para o bruno contar');
	await ana.page.keyboard.press('Enter');
	await expect(bruno.page.locator('nav button[aria-label="#geral"]').first()).toContainText('1', {
		timeout: 15_000,
	});

	await bruno.close();
	await ana.close();
});

// Closing a conversation is closing a tab. The word that matters is in the
// assertions: nothing is deleted, and it comes back.
test('fechar uma conversa tira da coluna sem apagar nada, e ela volta', async ({ browser }) => {
	const ana = await Person.create(browser, 'fechana');
	const bruno = await Person.create(browser, 'fechbru');

	await openPeople(bruno);
	const list = bruno.page.locator('aside button[data-person="fechana"]');
	await list.waitFor({ timeout: 15_000 });
	await list.click();
	await bruno.page.getByRole('dialog').getByRole('button', { name: /conversar no privado/i }).click();
	await bruno.writeTo('fechana', 'isto nao pode sumir');

	const talk = await waitForTalk(ana, 'fechbru');
	await talk.click();
	await expect(ana.page.getByText('isto nao pode sumir')).toBeVisible({ timeout: 15_000 });

	await talk.click({ button: 'right' });
	const closeConversation = ana.page.locator('[data-menu]').getByRole('menuitem', { name: 'Fechar conversa' });
	await expect(closeConversation).toHaveCSS('background-color', 'rgb(219, 59, 62)');
	await expect(closeConversation).toHaveCSS('color', 'rgb(252, 252, 252)');
	await closeConversation.hover();
	await expect(closeConversation).toHaveCSS('background-color', 'rgb(225, 84, 81)');
	await expect(closeConversation).toHaveCSS('color', 'rgb(252, 252, 252)');
	await closeConversation.click();
	await expect(ana.page.locator('nav button', { hasText: 'fechbru' })).toHaveCount(0, {
		timeout: 15_000,
	});
	// Bruno still has his copy: closing is one person's tab, not a deletion.
	await expect(bruno.page.getByText('isto nao pode sumir')).toBeVisible();

	// And something new brings it back, with everything in it — a closed
	// conversation that swallowed messages would be a blocklist nobody was
	// told about.
	await bruno.writeTo('fechana', 'voltei');
	const back = await waitForTalk(ana, 'fechbru');
	await back.click();
	await expect(ana.page.getByText('isto nao pode sumir')).toBeVisible({ timeout: 15_000 });
	await expect(ana.page.getByText('voltei')).toBeVisible();

	await bruno.close();
	await ana.close();
});

// The volume sliders had a way of showing one person's number while pointing at
// another, and then writing it onto them. That is how the owner's whole room
// ended up at nought per cent, so it is worth a test of its own.
test('o volume mostrado é o da pessoa aberta, e não o da anterior', async ({ browser }) => {
	const ana = await Person.create(browser, 'volana');
	const bruno = await Person.create(browser, 'volbru');
	const carol = await Person.create(browser, 'volcar');

	await ana.joinVoice();
	await bruno.joinVoice();
	await carol.joinVoice();
	// Joining a room folds the people column away (24/09/2026), so it is
	// opened again, as a person would.
	await openPeople(ana);

	// Opened and closed by the two things that are always there: the name in the
	// list, and the ✕ in the corner. Escape closes it too and is what a person
	// uses, but a test that leans on a key is a test that fails for a reason
	// that has nothing to do with volumes.
	const dialog = () => ana.page.getByRole('dialog');
	const open = async (who: string) => {
		await ana.page.locator(`aside button[data-person="${who}"]`).first().click();
		await dialog().waitFor({ timeout: 10_000 });
	};
	const shut = async () => {
		await dialog().getByTitle('fechar').click();
		await expect(dialog()).toBeHidden({ timeout: 10_000 });
	};
	const slider = () => ana.page.locator('input[data-volume]').first();

	// Bruno goes down to nothing.
	await open('volbru');
	await slider().fill('0');
	await expect(slider()).toHaveValue('0');
	await shut();

	// Carol, opened next, must be her own hundred per cent — not Bruno's nought.
	await open('volcar');
	await expect(slider(), "Carol's slider is showing Bruno's number").toHaveValue('1');
	await shut();

	// And Bruno kept his.
	await open('volbru');
	await expect(slider()).toHaveValue('0');

	await carol.close();
	await bruno.close();
	await ana.close();
});

// -------------------------------------------------------------------------
// D4f — a terceira volta
// -------------------------------------------------------------------------

// Two things that only a second browser can answer: that the light comes on
// over there while somebody types here, and that it goes out on its own.
test('o outro lado vê que estou digitando, e a luz apaga sozinha', async ({ browser }) => {
	const ana = await Person.create(browser, 'digana');
	const bruno = await Person.create(browser, 'digbru');

	await ana.page.getByPlaceholder(/Escrever em/).fill('escrevendo devagar');
	await expect(bruno.page.locator('[data-typing]')).toContainText('digana', { timeout: 15_000 });

	// Nobody ever sends "I stopped": the person who shut their laptop
	// mid-sentence never would. It goes out by time, which is the only honest
	// way, and five seconds is what the page waits.
	await expect(bruno.page.locator('[data-typing]')).toHaveCount(0, { timeout: 15_000 });

	// And it is never about yourself.
	await expect(ana.page.locator('[data-typing]')).toHaveCount(0);

	await bruno.close();
	await ana.close();
});

// The nickname, and the sentence that matters about it: the other person does
// not see it. The Go test proves the server never hands it over; this proves
// the app never draws it.
test('o apelido aparece para quem deu, e para mais ninguém', async ({ browser }) => {
	const ana = await Person.create(browser, 'apeana');
	const bruno = await Person.create(browser, 'apebru');

	await ana.page.locator('aside button[data-person="apebru"]').first().click();
	const card = ana.page.getByRole('dialog');
	await card.waitFor({ timeout: 10_000 });
	await card.locator('input[data-nickname]').fill('o vizinho');
	await card.locator('[data-nickname-save]').click();
	await card.getByTitle('fechar').click();

	// Ana's column and her list of people now call him that.
	await expect(ana.page.locator('aside button[data-person="apebru"]')).toContainText('o vizinho', {
		timeout: 15_000,
	});
	// And his real name is still reachable — a nickname that replaced it
	// everywhere would make "quem é @apebru?" unanswerable.
	await ana.page.locator('aside button[data-person="apebru"]').first().click();
	await expect(ana.page.getByRole('dialog')).toContainText('apebru');
	await ana.page.getByRole('dialog').getByTitle('fechar').click();

	// Bruno sees nothing of it, anywhere.
	await expect(bruno.page.locator('aside button[data-person="apebru"]')).not.toContainText(
		'o vizinho',
	);
	expect(await bruno.page.locator('text=o vizinho').count()).toBe(0);

	await bruno.close();
	await ana.close();
});

// Removing a message asks first, and shows which one — the mistake it guards
// against is having the wrong message under the pointer.
test('excluir uma mensagem pergunta antes, e mostra qual', async ({ browser }) => {
	const ana = await Person.create(browser, 'apagana');

	await ana.page.getByPlaceholder(/Escrever em/).fill('mensagem que vai embora');
	await ana.page.keyboard.press('Enter');
	const mine = ana.page.getByText('mensagem que vai embora');
	await mine.waitFor({ timeout: 15_000 });

	await mine.hover();
	await ana.page.locator('[data-remove]').first().click();
	const box = ana.page.locator('[data-confirm-remove]');
	await expect(box).toBeVisible();
	await expect(box, 'the box does not show which message it is about').toContainText(
		'mensagem que vai embora',
	);

	// Cancelling is tested through the button, not through Escape, and that is
	// deliberate: the row of actions this was opened from only exists under the
	// pointer and is gone by the time the box is up, so where a key lands
	// depends on what the browser did with the focus of an element it hid.
	// Cancelar is what a person presses and is the same for everybody.
	await box.getByRole('button', { name: 'Cancelar' }).click();
	await expect(box).toBeHidden();
	await expect(ana.page.getByText('mensagem que vai embora')).toBeVisible();

	// Going through with it removes it.
	await ana.page.getByText('mensagem que vai embora').hover();
	await ana.page.locator('[data-remove]').first().click();
	await ana.page.locator('[data-confirm-yes]').click();
	await expect(ana.page.getByText('mensagem removida').first()).toBeVisible({ timeout: 15_000 });

	await ana.close();
});

// The X on the row, and the order of the column.
test('o X fecha a conversa, e a mais recente fica em cima', async ({ browser }) => {
	const ana = await Person.create(browser, 'ordana');
	const um = await Person.create(browser, 'ordum');
	const dois = await Person.create(browser, 'orddois');

	// Two conversations, the second one newer.
	for (const who of [um, dois]) {
		await openPeople(who);
		await who.page.locator('aside button[data-person="ordana"]').first().click();
		await who.page
			.getByRole('dialog')
			.getByRole('button', { name: /conversar no privado/i })
			.click();
		await who.writeTo('ordana', 'oi');
		await who.page.waitForTimeout(1100);
	}

	await ana.openMessages();
	const rows = ana.page.locator('nav button[title*="opções" i]');
	await expect(rows).toHaveCount(2, { timeout: 20_000 });
	// Newest first: the one who spoke last is the one you are most likely to
	// want.
	await expect(rows.first()).toContainText('orddois');

	// And the X takes one out of the column without deleting anything.
	const row = ana.page.locator('nav button', { hasText: 'orddois' }).first();
	await row.hover();
	await ana.page.locator('[data-close-dm]').first().click();
	await expect(ana.page.locator('nav button', { hasText: 'orddois' })).toHaveCount(0, {
		timeout: 15_000,
	});
	await expect(ana.page.locator('nav button', { hasText: 'ordum' })).toHaveCount(1);

	await dois.close();
	await um.close();
	await ana.close();
});

// Reacting: the one piece of the D4f list that waited for a session of its own,
// because it is a table, a live event and a change to how every message is
// drawn.
//
// Two browsers, because the whole point of a reaction is that somebody else
// sees it. What this proves is the count going up from the other side and back
// down again — the switch — and that the panel is reachable from the row of
// actions the way a person reaches it.
test('reagir aparece dos dois lados, e apertar de novo tira', async ({ browser }) => {
	const ana = await Person.create(browser, 'reagana');
	const bruno = await Person.create(browser, 'reagbru');

	await ana.page.getByPlaceholder(/Escrever em/).fill('alguém reage aí');
	await ana.page.keyboard.press('Enter');

	// Everything is scoped to *this* message and not to the first one on the
	// page. #geral is shared by every test in this file, so it is full of other
	// people's lines by now, and each one of them carries the same hidden row
	// of actions — a bare [data-react] finds the oldest message on the screen,
	// which nobody is pointing at.
	const line = (who: Person) =>
		who.page.locator('[data-message]').filter({ hasText: 'alguém reage aí' }).last();
	await line(ana).waitFor({ timeout: 15_000 });

	// The panel opens from the row of actions over the message, which only
	// exists under the pointer.
	await line(ana).hover();
	await line(ana).locator('[data-react]').click();
	// 🔥 can be on the panel twice now, in "usados aqui" and in its group,
	// since every group is in the one list (24/09/2026).
	await ana.page.locator('[data-emoji="🔥"]').first().click();

	// Hers, lit, counting one.
	const hers = line(ana).locator('[data-reaction="🔥"]');
	await expect(hers).toContainText('1', { timeout: 15_000 });

	// And it arrived on the other side without anybody reloading anything.
	const his = line(bruno).locator('[data-reaction="🔥"]');
	await expect(his).toContainText('1', { timeout: 15_000 });

	// He joins it by pressing the button itself — the second way in, and the
	// one people actually use.
	await his.click();
	await expect(hers).toContainText('2', { timeout: 15_000 });

	// Pressing again takes back your own and only your own.
	await his.click();
	await expect(hers).toContainText('1', { timeout: 15_000 });

	// The last one out takes the button with it: one showing zero does nothing.
	await hers.click();
	await expect(line(ana).locator('[data-reaction="🔥"]')).toHaveCount(0, { timeout: 15_000 });
	await expect(line(bruno).locator('[data-reaction="🔥"]')).toHaveCount(0, { timeout: 15_000 });

	await bruno.close();
	await ana.close();
});

// -------------------------------------------------------------------------
// D4h — a volta de uso depois da chamada direta
// -------------------------------------------------------------------------

// A linha de "não lidas" é sobre o que você não leu. A sua própria mensagem
// não é uma delas — e era: a marca é tirada quando a conversa abre e não se
// move enquanto ela está na tela, então tudo escrito depois contava como novo,
// inclusive o que você mesmo acabou de digitar.
test('a linha de não lidas nunca aparece na sua própria mensagem', async ({ browser }) => {
	const ana = await Person.create(browser, 'naolida');
	const bruno = await Person.create(browser, 'naolidb');

	// Bruno fala primeiro, para haver uma marca de verdade.
	await bruno.page.getByPlaceholder(/Escrever em/).fill('primeira do bruno');
	await bruno.page.keyboard.press('Enter');
	await ana.page.getByText('primeira do bruno').waitFor({ timeout: 15_000 });

	await ana.page.getByPlaceholder(/Escrever em/).fill('minha propria mensagem');
	await ana.page.keyboard.press('Enter');
	const mine = ana.page
		.locator('[data-message]')
		.filter({ hasText: 'minha propria mensagem' })
		.last();
	await mine.waitFor({ timeout: 15_000 });
	await expect(mine).not.toContainText('não lidas');

	// E continua funcionando para o que importa: a resposta dele, sim.
	await bruno.page.getByPlaceholder(/Escrever em/).fill('resposta do bruno');
	await bruno.page.keyboard.press('Enter');
	await expect(
		ana.page.locator('[data-message]').filter({ hasText: 'resposta do bruno' }).last(),
	).toBeVisible({ timeout: 15_000 });

	await bruno.close();
	await ana.close();
});

// O nome em cima da mensagem é o lugar mais natural do app para apertar numa
// pessoa, e era o único que não fazia nada.
test('clicar no nome de quem escreveu abre o perfil dela', async ({ browser }) => {
	const ana = await Person.create(browser, 'perfana');
	const bruno = await Person.create(browser, 'perfbru');

	await bruno.page.getByPlaceholder(/Escrever em/).fill('quem sou eu');
	await bruno.page.keyboard.press('Enter');
	await ana.page.getByText('quem sou eu').waitFor({ timeout: 15_000 });

	await ana.page.locator('[data-author="perfbru"]').first().click();
	const card = ana.page.getByRole('dialog');
	await expect(card).toBeVisible({ timeout: 10_000 });
	await expect(card).toContainText('perfbru');
	await ana.page.keyboard.press('Escape');
	await ana.page.locator('[data-message-person="perfbru"]').first().click();
	await expect(card).toBeVisible();
	await expect(card).toContainText('perfbru');
	await expect(card.locator('[data-face]')).toBeDisabled();

	await bruno.close();
	await ana.close();
});

// Os ajustes eram uma coluna só, de cinco seções empilhadas: o microfone que a
// pessoa veio procurar ficava quatro rolagens acima do log de diagnóstico, que
// se abre uma vez por mês.
test('os ajustes são abas, e a aba escolhida fica', async ({ browser }) => {
	const ana = await Person.create(browser, 'abasana');

	await ana.page.locator('button[title="ajustes" i], button[aria-label="ajustes"]').first().click();
	const panel = ana.page.locator('[data-settings]');
	await expect(panel).toBeVisible({ timeout: 10_000 });
	// Chega na aba de sempre, a de voz.
	await expect(panel.locator('[data-tab="av"]')).toHaveAttribute('aria-pressed', 'true');

	await panel.locator('[data-tab="diag"]').click();
	await expect(panel.locator('[data-tab="diag"]')).toHaveAttribute('aria-pressed', 'true');
	// E o que estava na primeira aba saiu da tela, em vez de continuar embaixo.
	await expect(panel.getByText('Quando você compartilha a tela')).toHaveCount(0);

	// Fechar e abrir volta onde estava.
	await panel.getByRole('button', { name: 'fechar' }).click();
	await ana.page.locator('button[title="ajustes" i], button[aria-label="ajustes"]').first().click();
	await expect(ana.page.locator('[data-settings] [data-tab="diag"]')).toHaveAttribute(
		'aria-pressed',
		'true',
	);

	await ana.close();
});

// No celular os dois menus têm que fechar do mesmo jeito. O de canais já
// fechava ao apertar fora; o de pessoas, não — e ainda comia metade da largura
// da conversa enquanto estava aberto.
test('no celular, apertar fora fecha a lista de pessoas', async ({ browser }) => {
	const ana = await Person.create(browser, 'celana', { viewport: Person.PHONE });

	// Num telefone ela começa fechada: abrir o app com um painel em cima da
	// conversa é abrir pedindo para ser dispensado.
	await expect(ana.page.locator('aside')).toHaveCount(0);

	await ana.page.locator('button[aria-label="pessoas"]').first().click();
	await expect(ana.page.locator('aside')).toBeVisible();

	// Apertar fora — o pano de fundo, igual ao do menu de canais.
	await ana.page.locator('button[aria-label="fechar"]').first().click();
	await expect(ana.page.locator('aside')).toHaveCount(0);

	await ana.close();
});

// E o menu de canais fica aberto ao trocar de metade: apertar "Mensagens" não
// pode jogar ninguém dentro de uma conversa e fechar o menu no mesmo gesto.
test('no celular, trocar para Mensagens não fecha o menu', async ({ browser }) => {
	const ana = await Person.create(browser, 'celmen', { viewport: Person.PHONE });

	await ana.page.locator('button[aria-label="canais"]').first().click();
	// Inside the menu, not in the copy of the column that a monitor would be
	// showing — both are in the page, and on a telephone that one is hidden.
	const dms = ana.page.locator('[data-drawer] button[data-space="dms"]');
	await dms.waitFor({ timeout: 10_000 });
	await dms.click();

	// O menu continua de pé, mostrando a outra metade.
	await expect(dms).toHaveAttribute('aria-pressed', 'true');
	await expect(dms).toBeVisible();

	await ana.close();
});

// The room's bar fits whatever width it is given, and "Sair" is always on the
// screen. The Codex found it past the edge at 320, 360 and 375px of telephone
// and at a 768px window with both columns open (23/09/2026); the bar now gives
// way by its own width (style.css, .bar-*).
test('a barra da sala cabe em qualquer largura, e o Sair está sempre na tela', async ({ browser }) => {
	const ana = await Person.create(browser, 'barracabe');
	await ana.joinVoice();
	for (const [w, h] of [
		[320, 640],
		[360, 740],
		[375, 812],
		[412, 915],
		[768, 900],
		[1024, 768],
		[1440, 900],
	]) {
		await ana.page.setViewportSize({ width: w, height: h });
		await expect(async () => {
			const r = await ana.page.evaluate(() => {
				const bar = [...document.querySelectorAll('[data-call-controls]')].find(
					(e) => (e as HTMLElement).offsetParent,
				)!;
				const sair = bar.querySelector('button[aria-label="sair da chamada"]')!.getBoundingClientRect();
				const b = bar.getBoundingClientRect();
				return {
					cabe: bar.scrollWidth <= bar.clientWidth + 1,
					sairDentro: sair.right <= b.right + 0.5 && sair.right <= innerWidth,
					sairTamanho: Math.round(sair.height),
				};
			});
			expect(r.cabe, `a barra transborda em ${w}px`).toBe(true);
			expect(r.sairDentro, `o Sair sai da tela em ${w}px`).toBe(true);
			expect(r.sairTamanho, `o Sair é pequeno demais em ${w}px`).toBeGreaterThanOrEqual(38);
		}).toPass({ timeout: 5000 });
	}
	await ana.close();
});
