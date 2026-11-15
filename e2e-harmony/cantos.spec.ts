// One corner on the whole page (the owner, 24/09/2026). The day before it was
// two — 16 for big pieces and 8 for what sits in them, each inner curve
// following the outer — and he said the bigger a piece the rounder it got,
// and the big ones stopped matching the small ones inside and beside them. So
// it is 8 everywhere. No button is round or a pill; round is for faces, dots
// and counters.
//
// And the room on a monitor is as wide as the header's island over it, with
// the islands' 8px between every two pieces.
import { test, expect, Person } from './fixtures';

// Buttons whose corner is half their height or more: circles and pills.
const roundButtons = () =>
	[...document.querySelectorAll('button')]
		.filter((b) => {
			const r = b.getBoundingClientRect();
			if (!r.width || !r.height) return false;
			return parseFloat(getComputedStyle(b).borderTopLeftRadius) >= r.height / 2 - 0.5;
		})
		.map((b) => b.getAttribute('aria-label') || b.getAttribute('title') || (b.textContent ?? '').trim().slice(0, 20));

// Every corner radius on the page that is not 8, faces and small marks aside.
const strayRadii = () => {
	const out = new Set<string>();
	for (const el of document.querySelectorAll('*')) {
		if (el.closest('[data-face], [data-initials]')) continue;
		const r = el.getBoundingClientRect();
		if (r.width < 24 || r.height < 24) continue;
		const v = parseFloat(getComputedStyle(el).borderTopLeftRadius) || 0;
		if (v && v !== 8)
			out.add(
				`${v}px ${el.tagName.toLowerCase()}` +
					(el.getAttribute('data-island') ? `[island=${el.getAttribute('data-island')}]` : '') +
					` ${(el.getAttribute('aria-label') ?? '').slice(0, 20)}`,
			);
	}
	return [...out];
};

async function check(ana: Person, where: string): Promise<void> {
	expect(await ana.page.evaluate(roundButtons), 'botões redondos ' + where).toEqual([]);
	expect(await ana.page.evaluate(strayRadii), 'cantos fora de 8 ' + where).toEqual([]);
}

test('um canto só na página inteira, e nenhum botão redondo', async ({ browser }) => {
	const ana = await Person.create(browser, 'cantos' + test.info().repeatEachIndex);
	await ana.page.setViewportSize({ width: 1440, height: 900 });
	await ana.page.locator('nav.channel-column button[aria-label="#geral"]').first().click();
	await expect(ana.page.locator('[data-composer]')).toBeVisible();
	await check(ana, 'na conversa');

	// The settings, with a list open: the panel and the list are pieces too.
	await ana.openSettings();
	await ana.page.locator('button[data-setting="microphone"]').click();
	await expect(ana.page.locator('[data-options="microphone"]')).toBeVisible();
	await check(ana, 'nos ajustes');
	await ana.page.keyboard.press('Escape');
	await ana.page.keyboard.press('Escape');

	await ana.joinVoice();
	await expect(ana.page.locator('[data-call="live"] [data-tilebar]').first()).toBeVisible({ timeout: 15_000 });
	await check(ana, 'na sala');

	const sala = await ana.page.evaluate(() => {
		const r = (el: Element) => el.getBoundingClientRect();
		const topo = r(document.querySelector('[data-island="topo"]')!);
		const tile = r(document.querySelector('[data-call="live"] [data-tile]')!);
		const barra = r(document.querySelector('[data-call-controls]')!);
		return {
			esquerda: [Math.round(tile.left - topo.left), Math.round(barra.left - topo.left)],
			direita: [Math.round(topo.right - tile.right), Math.round(topo.right - barra.right)],
			vaos: [Math.round(tile.top - topo.bottom), Math.round(barra.top - tile.bottom)],
			embaixo: Math.round(innerHeight - barra.bottom),
		};
	});
	// Since 24/09/2026 a block is 16:9 and centred in the room, as in Discord
	// (the owner's screenshot): alone in a wide room it spans the island's
	// width and the room's black shows above and below it, the same on both
	// sides and never less than the islands' 8.
	expect({ ...sala, vaos: undefined }).toEqual({ esquerda: [0, 0], direita: [0, 0], vaos: undefined, embaixo: 8 });
	expect(Math.abs(sala.vaos[0] - sala.vaos[1]), 'centrado na altura').toBeLessThanOrEqual(1);
	expect(Math.min(...sala.vaos)).toBeGreaterThanOrEqual(8);
	await ana.close();
});

test('a lista de escolha é nossa: abre, escolhe, e fecha no esc sem fechar o painel', async ({ browser }) => {
	const ana = await Person.create(browser, 'lista' + test.info().repeatEachIndex);
	await ana.joinVoice();
	await ana.openPanel('screen');
	const button = ana.page.locator('button[data-setting="fps"]');
	await expect(button).toBeVisible();
	// No native select is left for the operating system to draw.
	await expect(ana.page.locator('[role="dialog"] select')).toHaveCount(0);

	await button.click();
	const list = ana.page.locator('[data-options="fps"]');
	await expect(list).toBeVisible();
	await expect(list.locator('[aria-selected="true"]')).toHaveCount(1);
	await ana.page.keyboard.press('Escape');
	await expect(list).toHaveCount(0);
	// Escape took the list and left the panel.
	await expect(ana.page.getByRole('dialog')).toBeVisible();

	await button.click();
	const second = list.locator('[data-value]').nth(1);
	const value = await second.getAttribute('data-value');
	await second.click();
	await expect(list).toHaveCount(0);
	await expect(button).toHaveAttribute('data-value', value!);

	await ana.close();
});
