// Pedidos de 24/09/2026, depois da rodada 4: o painel de emoji sem rolagem
// para o lado, os cinco últimos glifos do kit, e reagir com qualquer emoji do
// painel.
import { test, expect, Person } from './fixtures';

test('o painel de emoji tem todos os grupos numa lista só, um trilho ao lado, e nada rola para o lado', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'r5emoji' + test.info().repeatEachIndex);
	await ana.page.locator('[data-emoji-open]').click();
	const panel = ana.page.locator('[data-emoji-panel]');
	const list = panel.locator('[data-emoji-list]');
	const rail = panel.locator('[data-emoji-rail] button');
	await expect(rail.first()).toBeVisible();
	// Only the groups near the visible part fetch their pictures: all 1,867
	// at once was 8 MB on opening, and the page choked on the downloads.
	// The groups in view are marked a frame after the panel opens, so the
	// count is waited for rather than read once.
	const pics = panel.locator('[data-emoji-list] img[data-twemoji]');
	await expect.poll(() => pics.count()).toBeGreaterThan(20);
	await ana.page.waitForTimeout(500);
	expect(await pics.count()).toBeLessThan(400);

	// Every group is in the one list, not one at a time.
	expect(await list.locator('section').count()).toBe(await rail.count());
	expect(await list.locator('section').count()).toBeGreaterThanOrEqual(10);

	// The rail jumps to a group and lights it; scrolling back lights the first.
	const flags = panel.locator('[data-emoji-rail] button[aria-label="Bandeiras"]');
	await flags.click();
	await expect(flags).toHaveAttribute('aria-pressed', 'true');
	await expect(list.locator('section[data-section] h3', { hasText: 'Bandeiras' })).toBeInViewport();
	await list.evaluate((el) => el.scrollTo(0, 0));
	await expect(rail.first()).toHaveAttribute('aria-pressed', 'true');

	// Nothing in the panel is wider than its box.
	const wide = await panel.evaluate((root) =>
		[root, ...root.querySelectorAll('*')].filter((el) => el.scrollWidth > el.clientWidth + 1).map((el) => el.className),
	);
	expect(wide).toEqual([]);
	await ana.close();
});

test('no telefone o painel de emoji encolhe as casas em vez de rolar para o lado', async ({ browser }) => {
	const ana = await Person.create(browser, 'r5fone' + test.info().repeatEachIndex, { viewport: Person.PHONE });
	await ana.page.locator('[data-emoji-open]').click();
	const panel = ana.page.locator('[data-emoji-panel]');
	await expect(panel.locator('[data-emoji]').first()).toBeVisible();
	const box = (await panel.boundingBox())!;
	expect(box.x + box.width).toBeLessThanOrEqual(Person.PHONE.width);
	const wide = await panel.evaluate((root) =>
		[root, ...root.querySelectorAll('*')].filter((el) => el.scrollWidth > el.clientWidth + 1).map((el) => el.className),
	);
	expect(wide).toEqual([]);
	await ana.close();
});

test('os cinco últimos glifos do kit estão no lugar', async ({ browser }) => {
	const n = test.info().repeatEachIndex;
	const ana = await Person.create(browser, 'r5glifo' + n);
	await ana.openChannels();
	// The hash before a text channel, in the column and over the conversation.
	await expect(ana.page.locator('nav button[aria-label="#geral"] [data-icon="hashtag"]').first()).toBeVisible();
	await expect(ana.page.locator('[data-icon="hashtag"]').nth(1)).toBeVisible();
	await ana.close();

	const dono = await Person.signIn(browser, 'dono');
	await dono.openChannels();
	await expect(
		dono.page.locator('nav button[title="Novo canal" i] [data-icon="plus"], nav button[aria-label="novo canal" i] [data-icon="plus"]').first(),
	).toBeVisible();
	await dono.close();

	const bea = await Person.create(browser, 'r5tela' + n);
	await bea.joinVoice();
	// Leaving is the handset with the arrow going out.
	await expect(bea.page.locator('button[aria-label="sair da chamada"] [data-icon="handset-arrow-out"]')).toBeVisible();
	await expect(bea.page.locator('button[data-open="screen"]:visible [data-icon="display"]').first()).toBeVisible();
	await bea.shareScreen();
	// Your own screen on the air is the display with the pulse.
	await expect(bea.page.locator('button[data-open="screen"]:visible [data-icon="display-pulse"]').first()).toBeVisible({
		timeout: 15_000,
	});
	await bea.close();
});

test('a caixa de escrever é uma ilha só, sem fio em cima, sem botão contornado, e Enviar cinza até ter o que mandar', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'r5caixa' + test.info().repeatEachIndex);
	const box = ana.inChannel('geral');
	await box.waitFor({ timeout: 15_000 });
	const composer = ana.page.locator('[data-composer]');
	const drawn = await composer.evaluate((el) => {
		const s = (x: Element) => getComputedStyle(x);
		const island = el.querySelector('[data-composer-island]')!;
		const bordered = [...el.querySelectorAll('button, label, textarea')].filter(
			(b) => parseFloat(s(b).borderTopWidth) > 0 || parseFloat(s(b).borderLeftWidth) > 0,
		).length;
		return {
			fioEmCima: s(el).borderTopWidth,
			ilha: [s(island).backgroundColor, s(island).borderTopLeftRadius],
			contornados: bordered,
			// Everything is inside the island: attach, the text, emoji, Enviar.
			// (Enviar is the last button; the last child is the typing box's
			// scroll bar layer, scrollbar.ts.)
			dentro: ['label', 'textarea', '[data-emoji-open]', 'button:last-of-type'].every((q) => island.querySelector(q)),
		};
	});
	expect(drawn).toEqual({ fioEmCima: '0px', ilha: ['rgb(24, 24, 27)', '8px'], contornados: 0, dentro: true });

	const send = ana.page.getByRole('button', { name: /^enviar$/i });
	const bg = () => send.evaluate((el) => [getComputedStyle(el).backgroundColor, getComputedStyle(el).opacity]);
	expect(await bg(), 'vazio: o cinza de controle parado, sem transparência').toEqual(['rgb(39, 39, 42)', '1']);
	await box.fill('agora tem');
	expect(await bg()).toEqual(['rgb(4, 133, 247)', '1']);
	await ana.close();
});
