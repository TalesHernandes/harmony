// A lista de mensagens, que é o coração — o desenho dela, medido.
//
// These are the numbers from "harmony - Etapa 2", section 02, and the reason
// they are asserted rather than trusted is that they are invisible when they
// go wrong: a name that quietly falls back to 14px, or a grouped line that
// starts four pixels to the left of the one above it, looks like nothing at
// all in a screenshot and like a different app after a week of reading it.

import { test, expect, Person } from './fixtures';

// What the browser actually computed, which is the only honest answer about a
// style: a class can be present and lose to another one.
async function drawn(who: Person, selector: string, props: string[]) {
	return who.page.evaluate(
		({ selector, props }) => {
			const el = document.querySelector(selector);
			if (!el) return null;
			const s = getComputedStyle(el);
			const out: Record<string, string> = {};
			for (const p of props) out[p] = s.getPropertyValue(p);
			return out;
		},
		{ selector, props },
	);
}

test('a lista é desenhada na escala do desenho, e a coluna do rosto alinha tudo', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'escalana');
	const bru = await Person.create(browser, 'escalabru');
	await ana.page.getByPlaceholder(/Escrever em/).waitFor({ timeout: 15_000 });
	await bru.page.getByPlaceholder(/Escrever em/).waitFor({ timeout: 15_000 });

	const say = async (who: Person, text: string) => {
		await who.inChannel('geral').fill(text);
		await who.page.getByRole('button', { name: /^enviar$/i }).click();
		await who.page.waitForTimeout(300);
	};

	// Two lines from the same person, seconds apart: the second is a grouped
	// line, drawn with no name and no face.
	await say(bru, 'primeira linha do bloco da escala');
	await say(bru, 'segunda linha do mesmo bloco da escala');

	const primeira = ana.page
		.locator('[data-message]')
		.filter({ hasText: 'primeira linha do bloco' })
		.last();
	const segunda = ana.page
		.locator('[data-message]')
		.filter({ hasText: 'segunda linha do mesmo bloco' })
		.last();
	await expect(segunda).toBeVisible({ timeout: 15_000 });

	// The head is drawn once for the block. The second line has no name on it,
	// which is what makes a run of messages read as one paragraph.
	await expect(primeira.locator('[data-author]')).toHaveCount(1);
	await expect(segunda.locator('[data-author]')).toHaveCount(0);

	// And it still starts at exactly the same place. This is the whole purpose
	// of a 48px column that is empty half the time: without it the second line
	// would slide left to where the face was, and every block would be a little
	// staircase.
	const x = async (row: typeof primeira, texto: string) => {
		const box = await row.locator('.body-text', { hasText: texto }).boundingBox();
		return Math.round(box!.x);
	};
	expect(await x(segunda, 'segunda linha')).toBe(await x(primeira, 'primeira linha'));

	// The scale, as the browser computed it. 16/24 for what was written — a step
	// above the kit's body, on the owner's call, because the conversation is the
	// one screen that is nothing but text being read for an hour — and the name
	// at the same size, carried by its weight alone.
	//
	// Same size is the assertion that matters. A name set smaller than the
	// message under it reads as a caption over the words rather than as the
	// person who wrote them, and it gives the block two rhythms where it should
	// have one.
	const nome = await drawn(ana, '[data-author]', ['font-size', 'font-weight', 'line-height']);
	expect(nome).toEqual({ 'font-size': '16px', 'font-weight': '600', 'line-height': '24px' });
	const corpo = await drawn(ana, '[data-message] .body-text', ['font-size', 'line-height']);
	expect(corpo).toEqual({ 'font-size': '16px', 'line-height': '24px' });

	// The 36px face is centred on the name line plus the first line of text
	// (2 x 24px): 6px of room above it and 6px below, including rows with a
	// reply. Flush with the name it rode 6px high (owner, 03/10/2026).
	const rosto = await ana.page.evaluate(() => {
		const row = [...document.querySelectorAll('[data-message]')]
			.reverse()
			.find((el) => el.textContent?.includes('primeira linha do bloco')) as HTMLElement;
		const face = row.querySelector('[data-message-person] [data-face]')!.getBoundingClientRect();
		const head = row.querySelector('[data-author]')!.parentElement!.getBoundingClientRect();
		return {
			linha: Math.round(head.height),
			acima: Math.round(face.top - head.top),
			abaixo: Math.round(head.top + 2 * head.height - face.bottom),
		};
	});
	expect(rosto.linha).toBe(24);
	expect(rosto.acima).toBe(6);
	expect(rosto.abaixo).toBe(6);

	// With a quoted line above it the face stays beside the *name*, because the
	// quotation is a grid row of its own. Inside the column of text it pushed
	// the name down a line and left the circle level with somebody else's
	// words — the same staircase, one line lower.
	await primeira.hover();
	await primeira.getByRole('button', { name: 'responder', exact: true }).click();
	await say(ana, 'respondendo para medir o rosto');
	const resposta = ana.page
		.locator('[data-message]')
		.filter({ hasText: 'respondendo para medir o rosto' })
		.last();
	await expect(resposta.locator('[data-quote]')).toBeVisible({ timeout: 15_000 });
	const citada = await ana.page.evaluate(() => {
		const row = [...document.querySelectorAll('[data-message]')]
			.reverse()
			.find((el) => el.textContent?.includes('respondendo para medir o rosto')) as HTMLElement;
		const face = row.querySelector('[data-message-person] [data-face]')!.getBoundingClientRect();
		const cita = row.querySelector('[data-quote]')!.getBoundingClientRect();
		const head = row.querySelector('[data-author]')!.parentElement!.getBoundingClientRect();
		return {
			abaixoDaCitacao: face.top >= cita.bottom,
			acima: Math.round(face.top - head.top),
		};
	});
	expect(citada.abaixoDaCitacao).toBe(true);
	expect(citada.acima).toBe(rosto.acima);

	// And the band that lights up under the pointer is centred on the message.
	// Equal padding above and below is the assertion, because the gap between
	// blocks lives in the margin: put it in the padding instead and every
	// hovered message sits low in its own highlight.
	const espaco = await drawn(ana, '[data-message]', ['padding-top', 'padding-bottom']);
	expect(espaco!['padding-top']).toBe(espaco!['padding-bottom']);

	// A message that names you: the accent at 12% behind it, and the accent
	// bar of a selected channel, 3px wide at the left edge (the owner,
	// 23/09/2026 — the bar like the channel's, and the blue kept). The bar is a ::before outside the row's box, not a border,
	// because a border would move the text of that one message three pixels
	// to the right of everything else — so the author's left edge is checked
	// against an unmarked message's.
	await say(bru, '@escalana confere essa aqui, é a da barra');
	const marcada = ana.page.locator('[data-message].mentioned').filter({ hasText: 'da barra' });
	await expect(marcada).toBeVisible({ timeout: 15_000 });
	const marca = await ana.page.evaluate(() => {
		const el = [...document.querySelectorAll('[data-message].mentioned')].pop()!;
		const outra = [...document.querySelectorAll('[data-message]:not(.mentioned)')].pop()!;
		const barra = getComputedStyle(el, '::before');
		const texto = (m: Element) => m.querySelector('[data-author]')?.getBoundingClientRect().left ?? -1;
		return {
			fundo: getComputedStyle(el).backgroundColor,
			barra: [barra.width, barra.backgroundColor],
			texto: [texto(el), texto(outra)],
		};
	});
	expect(marca.fundo).toBe('rgba(4, 133, 247, 0.12)');
	expect(marca.barra).toEqual(['3px', 'rgb(4, 133, 247)']);
	expect(marca.texto[0]).toBeGreaterThan(0);
	expect(marca.texto[0]).toBe(marca.texto[1]);

	await bru.close();
	await ana.close();
});
