// Pedidos de 24/09/2026, depois do item 7: três coisas fora de linha.
import { test, expect, Person } from './fixtures';

// The piece of surface an element sits on: its nearest ancestor with a colour.
const islandOf = (el: Element): DOMRect => {
	for (let e: Element | null = el; e; e = e.parentElement) {
		if (getComputedStyle(e).backgroundColor !== 'rgba(0, 0, 0, 0)') return e.getBoundingClientRect();
	}
	return document.body.getBoundingClientRect();
};

test('a caixa de escrever tem as bordas do cabeçalho e o pé na altura da barra da conta', async ({ browser }) => {
	const ana = await Person.create(browser, 'r6caixa' + test.info().repeatEachIndex);
	await ana.page.setViewportSize({ width: 1200, height: 700 });
	await ana.inChannel('geral').waitFor({ timeout: 15_000 });
	const edges = await ana.page.evaluate(`(() => {
		const islandOf = ${islandOf.toString()};
		const r = (b) => [b.left, b.right, b.top, b.bottom].map(Math.round);
		return {
			caixa: r(document.querySelector('[data-composer-island]').getBoundingClientRect()),
			cabecalho: r(document.querySelector('header').getBoundingClientRect()),
			conta: r(islandOf(document.querySelector('[data-account]'))),
		};
	})()`) as { caixa: number[]; cabecalho: number[]; conta: number[] };
	expect(edges.caixa.slice(0, 2), 'esquerda e direita do cabeçalho').toEqual(edges.cabecalho.slice(0, 2));
	expect(edges.caixa.slice(2), 'topo e pé da barra da conta').toEqual(edges.conta.slice(2));
	await ana.close();
});

test('o + de criar canal fica na borda direita das linhas de canal', async ({ browser }) => {
	const dono = await Person.signIn(browser, 'dono');
	await dono.openChannels();
	const right = await dono.page.evaluate(() => {
		const r = (q: string) => Math.round(document.querySelector(q)!.getBoundingClientRect().right);
		return { canal: r('nav button[aria-label="#geral"]'), mais: r('button[aria-label="novo canal"]'), maisVoz: r('button[aria-label="novo canal de voz"]') };
	});
	expect(right.mais).toBe(right.canal);
	expect(right.maisVoz).toBe(right.canal);
	await dono.close();
});

test('nos Ajustes, o conteúdo de toda aba começa na mesma altura', async ({ browser }) => {
	const ana = await Person.create(browser, 'r6ajustes' + test.info().repeatEachIndex);
	await ana.inChannel('geral').waitFor({ timeout: 15_000 });
	await ana.page.locator('button[aria-label="ajustes"]').click();
	const tops: Record<string, number> = {};
	for (const t of ['av', 'avisos', 'conta', 'diag']) {
		await ana.page.locator(`[data-settings] [data-tab="${t}"]`).click();
		// The first line of words in the pane that is not the close button.
		tops[t] = await ana.page.evaluate(() => {
			const pane = document.querySelector('[data-settings] .scroller')!;
			const first = [...pane.querySelectorAll('*')].find(
				(e) => !e.closest('button') && e.children.length === 0 && (e.textContent || '').trim(),
			)!;
			return Math.round(first.getBoundingClientRect().top - pane.getBoundingClientRect().top);
		});
	}
	expect(tops).toEqual({ av: tops.avisos, avisos: tops.avisos, conta: tops.avisos, diag: tops.avisos });
	await ana.close();
});
