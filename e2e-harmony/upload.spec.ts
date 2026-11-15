// Sending a file: dropping it, watching it go, and changing your mind.
//
// The old test for uploads (d4i.spec.ts) proves the file arrives. These prove
// the part between choosing it and it arriving, which until now was a screen
// that did nothing at all for as long as the upload took.

import { test, expect, Person } from './fixtures';
import type { JSHandle, Page } from '@playwright/test';

// A file the server will accept, built inside the page because that is the only
// place a DataTransfer can exist. The bytes have to be a real PDF header: the
// server decides the type from them and from nothing else (api/files.go).
async function pdfOnClipboard(page: Page, name: string, bytes: number): Promise<JSHandle> {
	return page.evaluateHandle(
		({ name, bytes }) => {
			const head = new TextEncoder().encode('%PDF-1.4\n1 0 obj\n<<>>\nendobj\ntrailer\n<<>>\n%%EOF\n');
			const body = new Uint8Array(bytes);
			body.set(head, 0);
			const dt = new DataTransfer();
			dt.items.add(new File([body], name, { type: 'application/pdf' }));
			return dt;
		},
		{ name, bytes },
	);
}

test('arrastar um arquivo para a conversa mostra onde ele vai cair, e cai lá', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'arrastana');
	await ana.page.getByPlaceholder(/Escrever em/).waitFor({ timeout: 15_000 });

	const dt = await pdfOnClipboard(ana.page, 'contrato-arrastado.pdf', 64 * 1024);

	// Coming in over the page. The drop target is the window, so it does not
	// matter what is under the pointer — which is the whole point: somebody
	// aims at the conversation, not at the little box at the bottom.
	await ana.page.dispatchEvent('body', 'dragenter', { dataTransfer: dt });
	await ana.page.dispatchEvent('body', 'dragover', { dataTransfer: dt });

	const pano = ana.page.locator('[data-drop]');
	await expect(pano).toBeVisible({ timeout: 5_000 });
	// It says which conversation, because a server with twenty channels open in
	// twenty tabs is exactly where somebody drops a file into the wrong one.
	await expect(pano).toContainText('#geral');

	await ana.page.dispatchEvent('body', 'drop', { dataTransfer: dt });
	await expect(pano).toBeHidden({ timeout: 5_000 });

	const ladrilho = ana.page.locator('[data-upload]');
	await expect(ladrilho).toContainText('contrato-arrastado.pdf', { timeout: 15_000 });
	await expect(ladrilho).toContainText('Pronto para enviar', { timeout: 15_000 });

	await ana.page.getByRole('button', { name: /^enviar$/i }).click();

	// By name, not by position: #geral is the same channel for every test in the
	// run, and the first file card in it belongs to whichever test got here
	// first.
	const card = ana.page.locator('[data-file]').filter({ hasText: 'contrato-arrastado.pdf' });
	await expect(card).toBeVisible({ timeout: 15_000 });
	await expect(card.locator('[data-icon="file-arrow-down"]')).toBeVisible();

	await ana.close();
});

test('enquanto o arquivo sobe há uma barra, e ela é a do desenho', async ({ browser }) => {
	const ana = await Person.create(browser, 'barraana');
	await ana.page.getByPlaceholder(/Escrever em/).waitFor({ timeout: 15_000 });

	// Holding the answer back is the only way to stand still inside a moment
	// that, on this machine, lasts a few milliseconds.
	await ana.page.route('**/uploads', async (route) => {
		await new Promise((r) => setTimeout(r, 3_000));
		await route.continue();
	});

	const dt = await pdfOnClipboard(ana.page, 'demorado.pdf', 64 * 1024);
	await ana.page.dispatchEvent('body', 'drop', { dataTransfer: dt });

	const barra = ana.page.locator('.upload-bar');
	await expect(barra).toBeVisible({ timeout: 10_000 });

	// Nothing is sent while something is still on its way: a message that left
	// without its file cannot be mended afterwards.
	await expect(ana.page.getByRole('button', { name: /^enviar$/i })).toBeDisabled();

	// And the bar is drawn by this app, not by the operating system.
	//
	// It is asked in two halves because the filled part cannot be measured. The
	// browser builds it inside the element's own shadow tree, and getComputedStyle
	// refuses to report anything from in there: asked about
	// ::-webkit-progress-value it answers with the *element's* colours instead —
	// the colour of the track — which reads exactly like a stylesheet that never
	// arrived. So the element is measured, and the rule for the filled part is
	// read from the stylesheet, where it can be seen.
	const feitio = await barra.evaluate((el) => ({
		// The native bar off, and ours in its place: 4px and the surface colour.
		appearance: getComputedStyle(el).appearance,
		altura: getComputedStyle(el).height,
		trilho: getComputedStyle(el).backgroundColor,
		// The prefixed rule is the half that silently disappears — a build step
		// that prunes what it does not recognise, or somebody tidying up.
		preenchido: Array.from(document.styleSheets)
			.flatMap((f) => {
				try {
					return Array.from(f.cssRules);
				} catch {
					return [];
				}
			})
			.filter((r): r is CSSStyleRule => 'selectorText' in r)
			.filter((r) => r.selectorText === '.upload-bar::-webkit-progress-value')
			.map((r) => r.style.background),
	}));
	expect(feitio.appearance).toBe('none');
	expect(feitio.altura).toBe('4px');
	expect(feitio.trilho).toBe('rgb(35, 35, 37)');
	expect(feitio.preenchido).toEqual(['var(--color-accent)']);

	await expect(ana.page.locator('[data-upload]')).toContainText('Pronto para enviar', {
		timeout: 20_000,
	});

	await ana.close();
});

test('dá para desistir de um arquivo no meio da subida', async ({ browser }) => {
	const ana = await Person.create(browser, 'desistana');
	await ana.page.getByPlaceholder(/Escrever em/).waitFor({ timeout: 15_000 });

	// Held open, not held for ten seconds.
	//
	// A timer does not know that the test has ended: the old ten seconds
	// outlived this test by nine of them, woke up in the middle of
	// video.spec.ts and called route.continue() on a page that had been closed
	// since. Whether that landed inside a test or in the gap between two was
	// decided by the clock, and the gap is what the runner reports as "error
	// was not a part of any test" — the red run of 18/09/2026.
	//
	// A barrier is also the honest shape of what this test wants: not ten
	// seconds, but "still going when I press cancel", however slow the machine.
	let soltar = () => {};
	const preso = new Promise<void>((r) => (soltar = r));
	await ana.page.route('**/uploads', async (route) => {
		await preso;
		await route.continue().catch(() => undefined);
	});

	const dt = await pdfOnClipboard(ana.page, 'enorme.pdf', 64 * 1024);
	await ana.page.dispatchEvent('body', 'drop', { dataTransfer: dt });

	const ladrilho = ana.page.locator('[data-upload]');
	await expect(ladrilho).toContainText('enorme.pdf', { timeout: 10_000 });

	// The × during the upload is a cancellation and not a tidy-up: it goes now,
	// not in ten seconds when the answer that nobody is waiting for arrives.
	await ana.page.getByRole('button', { name: /^cancelar$/i }).click();
	await expect(ladrilho).toHaveCount(0, { timeout: 5_000 });

	// And giving up says nothing, because nothing went wrong.
	await expect(ana.page.locator('[data-composer]')).not.toContainText('Deu algo errado');
	await expect(ana.page.getByRole('button', { name: /^enviar$/i })).toBeDisabled();

	// Giving up was the point, so nobody is waiting for the held answer any
	// more. It is let go here, while this test still owns the page.
	soltar();
	await ana.close();
});
