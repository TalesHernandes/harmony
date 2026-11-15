// "valida tambem se tem coisas legadas da interface antiga que tem que mudar"
// (24/09/2026). The old names are gone from the stylesheet, so what guards
// against one coming back is that a class with no rule draws nothing - and
// that is checked here against what the build actually emitted, not against a
// list of names this file would have to keep in step.
import * as fs from 'node:fs';
import * as path from 'node:path';
import { test, expect, Person } from './fixtures';

const ASSETS = path.join(__dirname, '..', 'internal', 'harmony', 'api', 'assets', 'app', 'assets');

test('toda classe de cor que o app usa existe no CSS que ele carrega', () => {
	const read = (ext: string) =>
		fs
			.readdirSync(ASSETS)
			.filter((f) => f.endsWith(ext))
			.map((f) => fs.readFileSync(path.join(ASSETS, f), 'utf8'))
			.join('\n');
	const js = read('.js');
	const css = read('.css');
	const found = new Set<string>();
	const re =
		/(?<=[\s`'"])(?:[a-z-]+:)*((?:bg|text|border|ring|fill|stroke|divide|outline|placeholder|from|to|via|caret|decoration)-[a-z][a-z0-9-]*(?:\/\d+)?)(?=[\s`'"])/g;
	for (const m of js.matchAll(re)) found.add(m[1]);
	// Not classes: an SVG attribute name that happens to look like one.
	found.delete('fill-rule');
	expect(found.size, 'nenhuma classe encontrada: a leitura do pacote quebrou').toBeGreaterThan(40);
	const missing = [...found].filter((c) => {
		const sel = c.replace('/', '\\/');
		return !css.includes('.' + sel) && !css.includes(':' + sel);
	});
	expect(missing, 'classes sem regra nenhuma no CSS').toEqual([]);
});

test('a tela de entrada está no desenho novo: sem contorno, canto de 8, botão azul', async ({ browser }) => {
	const page = await browser.newPage();
	await page.goto('/');
	const field = page.locator('input#u');
	await expect(field).toBeVisible({ timeout: 15_000 });
	// Grey while there is nothing to send, like Enviar; blue once there is.
	await field.fill('alguem');
	await page.locator('input#p').fill('uma senha');
	const look = await page.evaluate(() => {
		const f = getComputedStyle(document.querySelector('input#u')!);
		const b = getComputedStyle(document.querySelector('form button')!);
		return {
			campo: [f.borderTopWidth, f.borderRadius, f.backgroundColor],
			botao: [b.borderRadius, b.backgroundColor, b.color],
		};
	});
	expect(look.campo).toEqual(['0px', '8px', 'rgb(24, 24, 27)']);
	expect(look.botao).toEqual(['8px', 'rgb(4, 133, 247)', 'rgb(252, 252, 252)']);
	await page.close();
});

test('o ponto de presença de quem está online é verde de verdade', async ({ browser }) => {
	const ana = await Person.create(browser, 'legado' + test.info().repeatEachIndex);
	await ana.page.locator('button[aria-label="pessoas"]').click();
	const dot = ana.page.locator('.avatar-dot').first();
	await expect(dot).toBeVisible({ timeout: 10_000 });
	// The dot was bg-live, an alias; with the alias gone it drew nothing.
	await expect
		.poll(() => ana.page.locator('.avatar-dot').evaluateAll((els) => els.map((e) => getComputedStyle(e).backgroundColor)))
		.toContain('rgb(23, 201, 100)');
	await ana.close();
});
