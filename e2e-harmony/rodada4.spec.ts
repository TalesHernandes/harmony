// Pedidos de 24/09/2026, a quarta volta do item 6: o convite da sala só sob o
// mouse, o botão de assistir cinza, o slider novo, o Procurar alinhado, a barra
// de rolagem 8B, a coluna 10A e os emojis Twemoji (9A).
import { test, expect, Person } from './fixtures';

test('o botão de chamar para a sala some quando o mouse sai, mesmo depois do clique que entrou', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'r4conv' + test.info().repeatEachIndex);
	await ana.joinVoice();
	const invite = ana.page.locator('nav button[data-open="invite"]').first();
	// The click that joined left focus on the room's row; with the pointer
	// somewhere else the button must not be there.
	await ana.page.mouse.move(900, 400);
	await expect(invite).toBeHidden();
	await ana.page.locator('nav button[title="Entrar na sala" i]').first().hover();
	await expect(invite).toBeVisible();
	await ana.page.mouse.move(900, 400);
	await expect(invite).toBeHidden();
	await ana.close();
});

test('assistir à tela é cinza e fica azul sob o mouse', async ({ browser }) => {
	const n = test.info().repeatEachIndex;
	const ana = await Person.create(browser, 'r4olha' + n);
	const bea = await Person.create(browser, 'r4mostra' + n);
	await ana.joinVoice();
	await bea.joinVoice();
	await bea.shareScreen();

	const panel = ana.page.locator('aside');
	if (!(await panel.isVisible())) await ana.page.locator('button[aria-label="pessoas"]').first().click();
	await ana.page.locator(`aside button[data-person="${bea.username}"]`).first().click();
	const watch = ana.page.getByRole('dialog').locator('button[data-watch]');
	await expect(watch).toBeVisible({ timeout: 20_000 });
	const bg = () => watch.evaluate((el) => getComputedStyle(el).backgroundColor);
	await ana.page.mouse.move(0, 0);
	expect(await bg(), 'parado: o cinza dos outros botões').toBe('rgb(39, 39, 42)');
	await watch.hover();
	await expect.poll(bg).toBe('rgb(4, 133, 247)');
	await bea.close();
	await ana.close();
});

test('o slider é um trilho com preenchimento, e o botão não arrasta cor para fora', async ({ browser }) => {
	const ana = await Person.create(browser, 'r4vol' + test.info().repeatEachIndex);
	await ana.page.locator('button[aria-label="ajustes"]').click();
	const mic = ana.page.locator('input[data-mic-volume]');
	await expect(mic).toBeVisible({ timeout: 10_000 });
	await mic.fill('0.2');
	const drawn = await mic.evaluate((el: HTMLInputElement) => {
		const fill = el.parentElement!.querySelector('progress');
		return {
			preenchimento: fill ? Number(fill.value) : null,
			sombra: getComputedStyle(el, '::-webkit-slider-thumb').boxShadow,
			fundo: getComputedStyle(el).backgroundColor,
		};
	});
	// The fill is the <progress> under it, at the same value, and the knob
	// throws no shadow: that shadow was the fill, and it was what spilled.
	expect(drawn).toEqual({ preenchimento: 0.2, sombra: 'none', fundo: 'rgba(0, 0, 0, 0)' });
	await ana.close();
});

test('no Procurar, a caixa e os resultados têm a mesma largura e o X é o dos outros painéis', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'r4busca' + test.info().repeatEachIndex);
	const word = 'alinhado' + (Date.now() % 100000);
	await ana.inChannel('geral').waitFor({ timeout: 15_000 });
	for (let i = 0; i < 2; i++) {
		await ana.inChannel('geral').fill(word + ' numero ' + i);
		await ana.page.keyboard.press('Enter');
		await expect(ana.page.locator('[data-message]', { hasText: word + ' numero ' + i })).toBeVisible({
			timeout: 15_000,
		});
	}
	await ana.page.locator('button[aria-label="procurar" i]').first().click();
	const panel = ana.page.locator('[data-search-panel]');
	await panel.locator('input').fill(word);
	const results = panel.locator('[data-search-results] button');
	await expect(results).toHaveCount(2, { timeout: 15_000 });

	const edges = await panel.evaluate((el) => {
		const r = (q: string) => el.querySelector(q)!.getBoundingClientRect();
		const box = el.querySelector('input')!.closest('label')!.getBoundingClientRect();
		const card = r('[data-search-results] button');
		const close = r('button[aria-label="fechar"]');
		return {
			esquerda: Math.round(card.left - box.left),
			direita: Math.round(card.right - box.right),
			xDireita: Math.round(close.right - box.right),
			x: [Math.round(close.width), Math.round(close.height)],
		};
	});
	expect(edges).toEqual({ esquerda: 0, direita: 0, xDireita: 0, x: [32, 32] });
	await ana.close();
});

// The scroll bar test that stood here read Chromium's ::-webkit-scrollbar
// rules. Since 24/09/2026 the app draws its own bar in every browser (the
// owner's Firefox would not take those rules), and rolagem.spec.ts checks it
// in Chromium and in Firefox.
test('a coluna anda de 8 em 8, a borda é cinza, e dois cliques voltam ao normal (10A)', async ({ browser }) => {
	const ana = await Person.create(browser, 'r4col' + test.info().repeatEachIndex);
	const column = ana.page.locator('.channel-column').first();
	const handle = ana.page.locator('[data-resizer]');
	await column.waitFor({ timeout: 15_000 });
	expect(Math.round((await column.boundingBox())!.width)).toBe(256);

	// The line stands in the middle of the gap between the channel islands and
	// the header island; it sat 1px off "# geral" until 17/10/2026.
	const line = handle.locator('[data-resizer-line]');
	const gap = await ana.page.evaluate(() => {
		const islands = [...document.querySelectorAll('.channel-column')[0].children]
			.filter((c) => c.classList.contains('bg-surface'))
			.map((c) => c.getBoundingClientRect().right);
		const header = document.querySelector('header')!.getBoundingClientRect().left;
		const l = document.querySelector('[data-resizer-line]')!.getBoundingClientRect();
		return { left: l.left - Math.max(...islands), right: header - l.right };
	});
	expect(gap).toEqual({ left: 3, right: 3 });

	const at = (await handle.boundingBox())!;
	await ana.page.mouse.move(at.x + at.width / 2, at.y + 100);
	await ana.page.mouse.down();
	await ana.page.mouse.move(at.x + at.width / 2 + 16, at.y + 100, { steps: 4 });
	// Mid-drag: the line is up, and grey, not the accent.
	await expect.poll(() => line.evaluate((el) => getComputedStyle(el).opacity)).toBe('1');
	expect(await line.evaluate((el) => getComputedStyle(el).backgroundColor)).toBe('rgb(82, 82, 91)');
	expect(await handle.evaluate((el) => getComputedStyle(el).backgroundColor)).toBe('rgb(6, 6, 7)');
	await ana.page.mouse.up();
	// 16px of pull is 16px of column: two steps of 8, where the old ladder
	// had nothing between 256 and 288.
	expect(Math.round((await column.boundingBox())!.width)).toBe(272);

	await handle.dblclick();
	expect(Math.round((await column.boundingBox())!.width)).toBe(256);
	await ana.close();
});

test('emojis viram as figuras do Twemoji na mensagem e na reação, e © continua letra', async ({ browser }) => {
	const ana = await Person.create(browser, 'r4emoji' + test.info().repeatEachIndex);
	const text = 'figuras ' + (Date.now() % 100000) + ' 🔥 ❤️ 👍🏽 ©';
	await ana.inChannel('geral').waitFor({ timeout: 15_000 });
	await ana.inChannel('geral').fill(text);
	await ana.page.keyboard.press('Enter');
	const msg = ana.page.locator('[data-message]', { hasText: text.split(' 🔥')[0] });
	await expect(msg).toBeVisible({ timeout: 15_000 });

	const pics = msg.locator('.body-text img[data-twemoji]');
	await expect(pics).toHaveCount(3);
	const drawn = await pics.evaluateAll((els) =>
		(els as HTMLImageElement[]).map((el) => [el.alt, el.getAttribute('src'), el.complete && el.naturalWidth > 0]),
	);
	expect(drawn).toEqual([
		['🔥', '/static/assets/twemoji-15/1f525.svg', true],
		['❤️', '/static/assets/twemoji-15/2764.svg', true],
		['👍🏽', '/static/assets/twemoji-15/1f44d-1f3fd.svg', true],
	]);
	await expect(msg.locator('.body-text')).toContainText('©');
	// A line with a picture in it is as tall as a line without.
	expect(Math.round((await msg.locator('.text-message').boundingBox())!.height)).toBe(24);

	await msg.hover();
	await msg.locator('button[aria-label="reagir" i]').click();
	await ana.page.locator('[data-emoji-search]').fill('fogo');
	await ana.page.locator('[data-emoji-panel] [data-emoji="🔥"]').click();
	await expect(msg.locator('[data-reaction="🔥"] img[data-twemoji="🔥"]')).toBeVisible({ timeout: 15_000 });

	// And the same picture where the message is found again.
	await ana.page.locator('button[aria-label="procurar" i]').first().click();
	await ana.page.locator('[data-search-panel] input').fill(text.split(' 🔥')[0]);
	await expect(ana.page.locator('[data-search-results] img[data-twemoji="🔥"]')).toBeVisible({ timeout: 15_000 });
	await ana.close();
});

test('os glifos novos do kit estão no lugar: convites, sair da conta e baixar a foto', async ({ browser }) => {
	const dono = await Person.signIn(browser, 'dono');
	await dono.page.locator('button[aria-label="ajustes"]').click();
	await dono.page.locator('[data-tab="conta"]').click();
	const invites = dono.page.locator('[data-invite-state]');
	await expect(invites.first()).toBeVisible({ timeout: 10_000 });
	// Each state has its mark: open is the clock, used is the check.
	const marks = await invites.evaluateAll((els) =>
		els.slice(0, 40).map((el) => [el.getAttribute('data-invite-state'), el.querySelector('[data-icon]')?.getAttribute('data-icon')]),
	);
	for (const [state, icon] of marks) expect(icon).toBe(state === 'usado' ? 'circle-check' : 'clock');

	await dono.page.getByRole('button', { name: /sair desta conta/i }).click();
	await expect(dono.page.locator('[data-confirm] [data-icon="arrow-right-from-square"]')).toBeVisible();
	await dono.page.keyboard.press('Escape');
	await dono.close();

	const ana = await Person.create(browser, 'r4foto' + test.info().repeatEachIndex);
	await ana.page.locator('input[type="file"]').setInputFiles({
		name: 'r4-foto.png',
		mimeType: 'image/png',
		buffer: Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aD1sAAAAASUVORK5CYII=', 'base64'),
	});
	await expect(ana.page.locator('[data-upload] img')).toBeVisible();
	await ana.page.getByRole('button', { name: /^enviar$/i }).click();
	await ana.page.locator('[data-image]').filter({ has: ana.page.locator('img[alt="r4-foto.png"]') }).last().click();
	await expect(
		ana.page.locator('[data-lightbox]').getByRole('link', { name: 'Baixar' }).locator('[data-icon="arrow-down-to-line"]'),
	).toBeVisible();
	await ana.close();
});
