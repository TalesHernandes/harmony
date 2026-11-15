// D4v, a parte do Codex: os painéis da conversa (emoji, @, procurar, foto
// ampliada e a confirmação de apagar).
import { test, expect, Person } from './fixtures';
import type { Locator } from '@playwright/test';

// Geometry assertions protect the design as well as the click paths.
async function panelCorners(panel: Locator) {
	await expect(panel).toHaveCSS('border-top-left-radius', '8px');
	expect(await panel.locator('[style]').count()).toBe(0);
	expect(await panel.locator('button').evaluateAll((buttons) => buttons.filter((b) => {
		const r = b.getBoundingClientRect();
		return r.height > 0 && getComputedStyle(b).borderTopLeftRadius !== '8px';
	}).length)).toBe(0);
}

test('o emoji abre acima da caixa, em oito colunas, e lembra o que foi escolhido', async ({ browser }) => {
	const ana = await Person.create(browser, 'panel_emoji');
	const page = ana.page;
	await page.locator('[data-emoji-open]').click();
	const panel = page.locator('[data-emoji-panel]');
	await panelCorners(panel);
	await expect(panel).toHaveCSS('width', '384px');
	const box = await panel.boundingBox();
	const composer = await page.locator('[data-composer]').boundingBox();
	expect(box!.y + box!.height).toBeLessThanOrEqual(composer!.y - 8);
	await expect(panel.locator('.emoji-grid').first()).toHaveCSS('grid-template-columns', '36px 36px 36px 36px 36px 36px 36px 36px');
	await expect(panel.locator('[data-emoji]').first()).toHaveCSS('height', '36px');
	await expect(panel.locator('[data-emoji]').first()).toHaveCSS('font-size', '22px');
	await panel.getByLabel('procurar emoji').fill('fogo');
	await panel.locator('[data-emoji="🔥"]').click();
	await expect(ana.inChannel('geral')).toHaveValue('🔥');
	await page.locator('[data-emoji-open]').click();
	await expect(panel.locator('section').first()).toContainText('usados aqui');
	await expect(panel.locator('section').first().locator('[data-emoji="🔥"]')).toBeVisible();
	await panel.getByLabel('procurar emoji').fill('zzzzzz');
	await expect(panel.getByRole('status')).toContainText('Nenhum emoji');
	await panel.getByLabel('limpar busca de emoji').click();
	await expect(panel.getByRole('status')).toHaveCount(0);
	await page.keyboard.press('Escape');
	await expect(panel).toBeHidden();
	await page.locator('[data-emoji-open]').click();
	await ana.inChannel('geral').click();
	await expect(panel).toBeHidden();
});

test('o painel de reagir fica preso na barra da mensagem e cabe no telefone', async ({ browser }) => {
	const ana = await Person.create(browser, 'panel_react', { viewport: Person.PHONE });
	await ana.inChannel('geral').fill('panel reaction anchor');
	await ana.page.getByRole('button', { name: /^enviar$/i }).click();
	const row = ana.page.locator('[data-message]').filter({ hasText: 'panel reaction anchor' });
	await row.hover();
	await row.locator('[data-react]').click();
	const panel = row.locator('[data-actions] [data-emoji-panel]');
	await panelCorners(panel);
	const b = await panel.boundingBox();
	const a = await row.locator('[data-actions]').boundingBox();
	expect(b!.x).toBeGreaterThanOrEqual(0);
	expect(b!.x + b!.width).toBeLessThanOrEqual(Person.PHONE.width);
	expect(b!.y).toBeGreaterThanOrEqual(0);
	expect(b!.y + b!.height).toBeLessThanOrEqual(a!.y - 7);
	await panel.getByLabel('procurar emoji').fill('fogo');
	await panel.locator('[data-emoji="🔥"]').click();
	await expect(panel).toBeHidden();
	await expect(row.locator('[data-reaction="🔥"] img[data-twemoji="🔥"]')).toBeVisible();
});

test('o @ já vem com a primeira pessoa escolhida, anda pelas setas e diz quando não há ninguém', async ({ browser }) => {
	const ana = await Person.create(browser, 'panel_mention');
	const input = ana.inChannel('geral');
	await input.fill('@');
	const panel = ana.page.locator('[data-mentions]');
	await panelCorners(panel);
	const first = panel.getByRole('option').first();
	await expect(first).toHaveAttribute('aria-selected', 'true');
	await expect(first.locator('button')).toHaveCSS('height', '44px');
	await expect(first.locator('button')).toHaveCSS('background-color', 'rgb(39, 39, 42)');
	expect(await first.locator('button').evaluate((e) => getComputedStyle(e, '::before').width)).toBe('3px');
	const p = await panel.boundingBox();
	const c = await ana.page.locator('[data-composer]').boundingBox();
	expect(p!.y + p!.height).toBe(c!.y - 8);
	await input.press('ArrowDown');
	await expect(panel.getByRole('option').nth(1)).toHaveAttribute('aria-selected', 'true');
	await input.press('ArrowUp');
	await input.press('Enter');
	await expect(input).toHaveValue('@todos ');
	await expect(panel).toBeHidden();
	await input.fill('@zzzzzzzzzz');
	await expect(panel).toContainText('Ninguém com esse nome');
	await input.press('Escape');
	await expect(panel).toBeHidden();
	await expect(input).toHaveValue('@zzzzzzzzzz');
});

test('apagar mostra a mensagem, e enter não aperta o botão vermelho', async ({ browser }) => {
	const ana = await Person.create(browser, 'panel_delete');
	await ana.inChannel('geral').fill('preview before deleting');
	await ana.page.getByRole('button', { name: /^enviar$/i }).click();
	const row = ana.page.locator('[data-message]').filter({ hasText: 'preview before deleting' });
	await row.hover();
	await row.locator('[data-remove]').click();
	const panel = ana.page.locator('[data-confirm]');
	await panelCorners(panel);
	await expect(panel).toHaveCSS('width', '400px');
	await expect(panel).toContainText('preview before deleting');
	await expect(panel.locator('[data-icon="trash-bin"]')).toBeVisible();
	await panel.locator('[data-confirm-yes]').focus();
	await ana.page.keyboard.press('Enter');
	await expect(panel).toBeVisible();
	await expect(row).toContainText('preview before deleting');
	await ana.page.keyboard.press('Escape');
	await expect(panel).toBeHidden();
	await row.hover();
	await row.locator('[data-remove]').click();
	await ana.page.mouse.click(2, 2);
	await expect(panel).toBeHidden();
	await row.hover();
	await row.locator('[data-remove]').click();
	await panel.locator('[data-confirm-yes]').click();
	await expect(ana.page.getByText('mensagem removida').last()).toBeVisible();
});

test('a foto tem fundo opaco, baixar, crédito e três saídas iguais', async ({ browser }) => {
	const ana = await Person.create(browser, 'panel_photo');
	await ana.page.locator('input[type="file"]').setInputFiles({ name: 'panel-photo.png', mimeType: 'image/png', buffer: Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aD1sAAAAASUVORK5CYII=', 'base64') });
	await expect(ana.page.locator('[data-upload] img')).toBeVisible();
	await ana.page.getByRole('button', { name: /^enviar$/i }).click();
	const photo = ana.page.locator('[data-image]').filter({ has: ana.page.locator('img[alt="panel-photo.png"]') });
	const panel = ana.page.locator('[data-lightbox]');
	for (const exit of ['escape', 'button', 'background']) {
		await photo.click();
		await expect(panel).toHaveCSS('background-color', 'rgb(6, 6, 7)');
		await expect(panel).toHaveCSS('padding', '32px');
		await expect(panel.getByRole('link', { name: 'Baixar' })).toHaveAttribute('download', 'panel-photo.png');
		await expect(panel.locator('p')).toContainText('panel_photo');
		await expect(panel.locator('p')).toContainText('panel-photo.png');
		await panel.locator('img').click();
		await expect(panel).toBeVisible();
		if (exit === 'escape') await ana.page.keyboard.press('Escape');
		else if (exit === 'button') await panel.getByRole('button', { name: 'fechar' }).click();
		else await ana.page.mouse.click(2, 200);
		await expect(panel).toBeHidden();
	}
});

test('procurar vira folha de baixo no telefone, com os resultados em cartões', async ({ browser }) => {
	const ana = await Person.create(browser, 'panel_search', { viewport: Person.PHONE });
	await ana.inChannel('geral').fill('panelsearchunique');
	await ana.page.getByRole('button', { name: /^enviar$/i }).click();
	await expect(ana.page.locator('[data-message]').filter({ hasText: 'panelsearchunique' })).toBeVisible();
	await ana.page.locator('button[aria-label="procurar"]').first().click();
	const panel = ana.page.locator('[data-search-panel]');
	await panelCorners(panel);
	await expect(panel.locator('.search-handle')).toBeVisible();
	const bounds = await panel.boundingBox();
	expect(bounds!.y + bounds!.height).toBe(Person.PHONE.height - 8);
	await panel.locator('[data-search-box]').fill('panelsearchunique');
	const hit = panel.locator('[data-hit]').first();
	await expect(hit).toContainText('panelsearchunique');
	await expect(hit).toHaveCSS('border-radius', '8px');
	await expect(hit).toHaveCSS('background-color', 'rgb(35, 35, 37)');
	await hit.click();
	await expect(panel).toBeHidden();
	await ana.page.locator('button[aria-label="procurar"]').first().click();
	await panel.locator('[data-search-box]').fill('zzzznotfound');
	await expect(panel).toContainText('Nada com essa palavra');
	await ana.page.keyboard.press('Escape');
	await expect(panel).toBeHidden();
});
