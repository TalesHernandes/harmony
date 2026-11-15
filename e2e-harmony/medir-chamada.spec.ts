// A régua da tela da chamada para o programa nativo (W5, o corte do sprint).
//
// Não é um teste: não afirma nada. Abre a sala com duas pessoas a 1280×800 e
// escreve a caixa desenhada de cada peça, mais um print, para o nativo ser
// copiado por medida (windows/app/medidas-web.md). Roda de propósito:
//
//     MEDIR=1 HARMONY_SHOTS=<pasta> npx playwright test --config playwright.harmony.config.ts --grep @chamada-medidas
import { writeFileSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import type { Page } from '@playwright/test';
import { test, Person } from './fixtures';

test.skip(!process.env.MEDIR, 'só quando alguém pede a régua');
test.setTimeout(120_000);

async function boxes(page: Page) {
	return page.evaluate(() => {
		const out: Record<string, unknown>[] = [];
		const add = (name: string, e: Element | null) => {
			if (!e) return;
			const r = e.getBoundingClientRect();
			const s = getComputedStyle(e);
			out.push({
				name,
				x: Math.round(r.x * 10) / 10,
				y: Math.round(r.y * 10) / 10,
				w: Math.round(r.width * 10) / 10,
				h: Math.round(r.height * 10) / 10,
				bg: s.backgroundColor,
				fg: s.color,
				font: `${s.fontSize}/${s.lineHeight} ${s.fontWeight}`,
				radius: s.borderRadius,
				pad: s.padding,
				text: (e as HTMLElement).innerText?.slice(0, 40) ?? '',
			});
		};
		const call = document.querySelector('[data-call]');
		add('call', call);
		add('header', document.querySelector('header'));
		document.querySelectorAll('header > *, header h1, header h2, header button').forEach((e, i) => add('header.' + i + '.' + e.tagName, e));
		call?.querySelectorAll('[data-tile]').forEach((t, i) => {
			add(`tile${i}`, t);
			add(`tile${i}.layer`, t.firstElementChild);
			add(`tile${i}.initials`, t.querySelector('[data-initials]'));
			add(`tile${i}.nome`, t.querySelector('[data-nome]'));
			t.querySelectorAll('[data-nome] svg').forEach((s, j) => add(`tile${i}.nome.svg${j}`, s));
			add(`tile${i}.assistir`, t.querySelector('[data-assistir]'));
			t.querySelectorAll('[data-assistir] svg').forEach((s, j) => add(`tile${i}.assistir.svg${j}`, s));
			add(`tile${i}.speaking`, t.querySelector('[data-speaking]'));
		});
		const bar = document.querySelector('[data-call-controls]');
		add('bar', bar);
		bar?.querySelectorAll(':scope > button, :scope > span, :scope > span button').forEach((b, i) => add(`bar.${i}.${b.getAttribute('aria-label') ?? b.className.slice(0, 20)}`, b));
		bar?.querySelectorAll('svg').forEach((s, i) => add(`bar.svg${i}`, s));
		return out;
	});
}

test('a régua da chamada @chamada-medidas', async ({ browser }) => {
	const dir = process.env.HARMONY_SHOTS ?? 'test-results/medidas-chamada';
	mkdirSync(dir, { recursive: true });
	const viewport = { width: 1280, height: 800 };
	const ana = await Person.create(browser, 'observador', { viewport });
	const bea = await Person.create(browser, 'visitante', { viewport });
	await ana.joinVoice();
	await bea.joinVoice();
	await ana.page.locator('[data-call] [data-tile]').nth(1).waitFor({ timeout: 20_000 });
	await ana.page.waitForTimeout(1500);
	await ana.page.mouse.move(5, 795);
	writeFileSync(join(dir, 'duas.json'), JSON.stringify(await boxes(ana.page), null, 1));
	await ana.page.screenshot({ path: join(dir, 'duas.png') });

	// Mudo e sem ouvir, para os ícones no nome e na barra.
	await bea.page.locator('[data-call-controls] button').nth(1).click();
	await ana.page.locator('[data-call-controls] button').first().click();
	await ana.page.waitForTimeout(1000);
	await ana.page.mouse.move(5, 795);
	writeFileSync(join(dir, 'mudos.json'), JSON.stringify(await boxes(ana.page), null, 1));
	await ana.page.screenshot({ path: join(dir, 'mudos.png') });
	await ana.page.locator('[data-call-controls] button').first().click();
	await bea.page.locator('[data-call-controls] button').nth(1).click();

	// Quem mostra a tela que não chega aqui: o "Assistir" debaixo do círculo.
	await bea.shareScreen();
	await ana.page.locator('[data-assistir]').first().waitFor({ timeout: 20_000 });
	await ana.page.waitForTimeout(1000);
	await ana.page.mouse.move(5, 795);
	writeFileSync(join(dir, 'assistir.json'), JSON.stringify(await boxes(ana.page), null, 1));
	await ana.page.screenshot({ path: join(dir, 'assistir.png') });
	await bea.close();
	await ana.close();
});
