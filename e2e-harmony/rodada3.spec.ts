// Pedidos de 24/09/2026: editar já com o cursor na caixa e com botões, o
// espaço entre mensagens seguidas, e criar canal sem a janela do navegador.
import { test, expect, Person } from './fixtures';

test('editar põe o cursor na caixa, no fim do texto, e tem Salvar e Cancelar', async ({ browser }) => {
	const ana = await Person.create(browser, 'r3edit' + test.info().repeatEachIndex);
	await ana.inChannel('geral').waitFor({ timeout: 15_000 });
	await ana.inChannel('geral').fill('texto que vai mudar');
	await ana.page.keyboard.press('Enter');
	const msg = ana.page.locator('[data-message]', { hasText: 'texto que vai mudar' });
	await expect(msg).toBeVisible({ timeout: 15_000 });

	await msg.hover();
	await msg.locator('button[aria-label="editar"]').click();
	const editor = ana.page.locator('[data-message] textarea');
	await expect(editor).toBeFocused();
	expect(
		await editor.evaluate((el: HTMLTextAreaElement) => [el.selectionStart, el.value.length]),
		'o cursor fica no fim do texto',
	).toEqual([19, 19]);

	// Typing goes straight in, with no click first.
	await ana.page.keyboard.type(' agora');
	await ana.page.locator('[data-edit-save]').click();
	await expect(ana.page.locator('[data-message]', { hasText: 'texto que vai mudar agora' })).toBeVisible({
		timeout: 10_000,
	});
	await expect(ana.page.locator('[data-editing]')).toHaveCount(0);

	// Cancelar leaves the message as it was.
	const again = ana.page.locator('[data-message]', { hasText: 'texto que vai mudar agora' });
	await again.hover();
	await again.locator('button[aria-label="editar"]').click();
	await ana.page.keyboard.type(' e mais');
	await ana.page.locator('[data-edit-cancel]').click();
	await expect(ana.page.locator('[data-editing]')).toHaveCount(0);
	await expect(ana.page.locator('[data-message]', { hasText: 'agora e mais' })).toHaveCount(0);

	await ana.close();
});

test('mensagens seguidas da mesma pessoa ficam mais juntas que blocos diferentes', async ({ browser }) => {
	const ana = await Person.create(browser, 'r3junto' + test.info().repeatEachIndex);
	await ana.inChannel('geral').waitFor({ timeout: 15_000 });
	for (const t of ['junto um', 'junto dois', 'junto tres']) {
		await ana.inChannel('geral').fill(t);
		await ana.page.keyboard.press('Enter');
		await expect(ana.page.locator('[data-message]', { hasText: t })).toBeVisible({ timeout: 15_000 });
	}
	await ana.page.locator('[data-message]', { hasText: 'junto dois' }).hover();
	const gaps = await ana.page.evaluate(() => {
		const rows = ['junto um', 'junto dois', 'junto tres'].map(
			(t) => [...document.querySelectorAll('[data-message]')].find((el) => el.textContent?.includes(t))!,
		);
		const text = (el: Element) => el.querySelector('.text-message')!.getBoundingClientRect();
		// The band is the row's ::after under the pointer, read from the page:
		// how far it reaches past the row, and that it is painted at all.
		const r = rows[1].getBoundingClientRect();
		const after = getComputedStyle(rows[1], '::after');
		if (after.content === 'none' || after.backgroundColor === 'rgba(0, 0, 0, 0)') return null;
		const band = { top: r.top + parseFloat(after.top), bottom: r.bottom - parseFloat(after.bottom) };
		return {
			seguidas: [text(rows[1]).top - text(rows[0]).bottom, text(rows[2]).top - text(rows[1]).bottom],
			// The band's four-pixel padding belongs to this row only.
			faixa: [text(rows[1]).top - band.top, band.bottom - text(rows[1]).bottom],
			contained: band.top >= r.top && band.bottom <= r.bottom,
			clearOfNeighbours: band.top >= rows[0].getBoundingClientRect().bottom &&
				band.bottom <= rows[2].getBoundingClientRect().top,
		};
	});
	expect(gaps, 'nenhuma faixa pintada sob o mouse').not.toBeNull();
	// Two lines of one block: 4px of the row below and 4px of the row above.
	expect(gaps!.seguidas.map(Math.round)).toEqual([8, 8]);
	expect(gaps!.faixa.map(Math.round), 'a faixa fica dentro da mensagem').toEqual([4, 4]);
	expect(gaps!.contained && gaps!.clearOfNeighbours, 'o realce não invade as mensagens vizinhas').toBe(true);

	// And between two people's blocks, 20 (it was 32, then 24: "diminua o
	// tamanho entre as mensagens", asked twice).
	const bea = await Person.create(browser, 'r3outro' + test.info().repeatEachIndex);
	await bea.inChannel('geral').waitFor({ timeout: 15_000 });
	await bea.inChannel('geral').fill('bloco de outra pessoa');
	await bea.page.keyboard.press('Enter');
	const other = ana.page.locator('[data-message]', { hasText: 'bloco de outra pessoa' });
	await expect(other).toBeVisible({ timeout: 15_000 });
	const entre = await ana.page.evaluate(() => {
		const rows = [...document.querySelectorAll('[data-message]')];
		const a = rows.find((el) => el.textContent?.includes('junto tres'))!;
		const b = rows.find((el) => el.textContent?.includes('bloco de outra pessoa'))!;
		return b.getBoundingClientRect().top - a.getBoundingClientRect().bottom;
	});
	// The margin alone: 12px, plus 4 + 4 of the two rows = 20 between texts.
	expect(Math.round(entre)).toBe(12);
	await bea.close();
	await ana.close();
});

test('criar canal abre um painel do app, e o nome se ajusta enquanto se digita', async ({ browser }) => {
	const dono = await Person.signIn(browser, 'dono');
	let native = false;
	dono.page.on('dialog', (d) => {
		native = true;
		void d.dismiss();
	});
	await dono.openChannels();
	await dono.page.locator('nav button[title="Novo canal" i], nav button[aria-label="novo canal" i]').first().click();
	const box = dono.page.locator('[data-new-channel]');
	await expect(box).toBeFocused();
	const name = 'Só Teste ' + (Date.now() % 100000);
	await dono.page.keyboard.type(name);
	const shaped = 'so-teste-' + (Date.now() % 100000).toString().slice(0, 0);
	await expect(box).toHaveValue(new RegExp('^' + shaped));
	await dono.page.keyboard.press('Enter');
	await expect(box).toHaveCount(0, { timeout: 10_000 });
	expect(native, 'o navegador abriu a janela dele').toBe(false);
	await dono.close();
});
