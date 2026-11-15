// "deixe o scroll sempre mais grosso desse jeito e remova as setinhas" - and
// the owner uses Firefox (24/09/2026), which draws its own bar whatever the
// stylesheet says. So this file runs in Firefox as well as in Chromium
// (playwright.harmony.config.ts), and checks the bar the app draws.
import type { Browser, Page } from '@playwright/test';
import { test, expect, PASSWORD } from './fixtures';

// Signed in by hand and not through Person: Person asks for camera and
// microphone permissions, which Firefox's automation does not have.
async function dono(browser: Browser): Promise<Page> {
	const context = await browser.newContext({ viewport: { width: 1200, height: 700 } });
	const page = await context.newPage();
	await page.goto('/login');
	await page.locator('#u').fill('dono');
	await page.locator('#p').fill(PASSWORD);
	await page.getByRole('button', { name: /^entrar/i }).click();
	await page.locator('button[aria-label="pessoas"]').waitFor({ timeout: 20_000 });
	return page;
}

// Enough lines in #geral that the list has to scroll - in two messages, not
// many: the server takes 20 a minute from one person (chat.go), and dono is
// the account other tests write with too.
async function fill(page: Page, tag: string) {
	await page.evaluate(async (tag) => {
		const body = Array.from({ length: 40 }, (_, i) => 'linha ' + (i + 1)).join('\n');
		for (let i = 0; i < 2; i++) {
			await fetch('/api/channels/geral/messages', {
				method: 'POST',
				credentials: 'same-origin',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ body: tag + ' ' + i + '\n' + body, attachments: [], reply_to: 0 }),
			});
		}
	}, tag);
}

const measure = (page: Page) =>
	page.evaluate(() => {
		const list = document.querySelector<HTMLElement>('[data-message]')!.closest('.scroller') as HTMLElement;
		const thumb = list.parentElement!.querySelector<SVGRectElement>(':scope > .scroll-layer [data-scroll-thumb]')!;
		const l = list.getBoundingClientRect();
		const t = thumb.getBoundingClientRect();
		return {
			// The browser's own bar takes no room: the list's inner width is its
			// whole width (a native bar, thin or not, would take some).
			nativo: list.offsetWidth - list.clientWidth,
			largura: Math.round(t.width),
			visivel: thumb.getAttribute('visibility'),
			direita: Math.round(l.right - t.right),
			topo: Math.round(t.top - l.top),
			pe: Math.round(l.bottom - t.bottom),
			altura: Math.round(t.height),
			// What the thumb's height should be: the share of the list on screen.
			esperado: Math.round((list.clientHeight * (list.clientHeight - 4)) / list.scrollHeight),
			rolado: list.scrollTop,
			fim: list.scrollHeight - list.clientHeight,
			cor: getComputedStyle(thumb).fill,
		};
	});

test('a barra de rolagem é a do app: grossa, sempre à mostra, sem setas, e segue a lista', async ({ browser }) => {
	const page = await dono(browser);
	const tag = 'rolagem' + Date.now();
	await fill(page, tag);
	await expect(page.locator('[data-message]', { hasText: tag + ' 1' })).toBeVisible({ timeout: 15_000 });
	await expect.poll(async () => (await measure(page)).visivel).toBe('visible');

	// At the bottom, where a conversation opens.
	await expect.poll(async () => (await measure(page)).pe).toBe(2);
	const fundo = await measure(page);
	expect(fundo.nativo, 'o navegador ainda desenha a barra dele').toBe(0);
	expect(fundo.largura).toBe(8);
	expect(fundo.direita).toBe(2);
	expect(Math.abs(fundo.altura - Math.max(40, fundo.esperado))).toBeLessThanOrEqual(1);
	expect(fundo.cor).toBe('rgb(78, 78, 86)');

	// Scrolled up, the thumb goes where the list's position puts it. Not
	// "at the top": near the top a long history fetches the older page and the
	// list grows above the reader (Messages.tsx, onScroll), which is how the
	// first version of this line failed inside the full suite and passed alone.
	await page.evaluate(() => {
		const list = document.querySelector('[data-message]')!.closest('.scroller')!;
		list.scrollTop = 0;
	});
	await expect
		.poll(async () => {
			const m = await measure(page);
			const track = m.topo + m.altura + m.pe - 4;
			return Math.abs(m.topo - (2 + ((track - m.altura) * m.rolado) / m.fim)) <= 1 && m.topo < fundo.topo;
		})
		.toBe(true);

	// Dragging it down scrolls the list down.
	const thumb = page.locator('[data-scroll-thumb]').filter({ visible: true }).first();
	const box = (await thumb.boundingBox())!;
	await page.mouse.move(box.x + 4, box.y + 10);
	await page.mouse.down();
	await page.mouse.move(box.x + 4, box.y + 10 + 120, { steps: 6 });
	await page.mouse.up();
	await expect.poll(async () => (await measure(page)).rolado).toBeGreaterThan(100);

	// And the wheel over the thumb scrolls the list, not the page around it.
	const before = (await measure(page)).rolado;
	const now = (await thumb.boundingBox())!;
	await page.mouse.move(now.x + 4, now.y + 10);
	await page.mouse.wheel(0, 200);
	await expect.poll(async () => (await measure(page)).rolado).toBeGreaterThan(before);

	await page.context().close();
});

test('a caixa de escrever ganha a barra quando o texto passa da altura dela, e perde quando se apaga', async ({
	browser,
}) => {
	const page = await dono(browser);
	const box = page.getByPlaceholder(/Escrever em/);
	const bar = () =>
		box.evaluate((el) => el.parentElement!.querySelector(':scope > .scroll-layer [data-scroll-thumb]')?.getAttribute('visibility') ?? 'sem barra');
	await box.click();
	for (let i = 0; i < 12; i++) {
		await page.keyboard.type('linha ' + i);
		await page.keyboard.press('Shift+Enter');
	}
	await expect.poll(bar).toBe('visible');
	// Deleted with the keyboard, the way somebody gives up on a long text: no
	// node changes, only the text inside the box.
	await page.keyboard.press('ControlOrMeta+a');
	await page.keyboard.press('Delete');
	await expect.poll(bar).toBe('hidden');
	await page.context().close();
});

test('uma lista que cabe inteira não mostra barra', async ({ browser }) => {
	const page = await dono(browser);
	// The channel column: one text channel and one room, nothing to scroll.
	const drawn = () =>
		page.evaluate(() => {
			const col = document.querySelector('nav.channel-column .scroller') as HTMLElement;
			const thumb = col.parentElement!.querySelector(':scope > .scroll-layer [data-scroll-thumb]');
			return { cabe: col.scrollHeight <= col.clientHeight, barra: thumb?.getAttribute('visibility') ?? 'sem barra' };
		});
	await expect.poll(drawn).toEqual({ cabe: true, barra: 'hidden' });
	await page.context().close();
});

// Keep this in rolagem.spec.ts: the Firefox project runs this file too.
for (const viewport of [{ width: 1280, height: 800 }, { width: 1440, height: 900 }]) {
	test(`settings keep the close button and scrollbar clear of the card edges at ${viewport.width}x${viewport.height}`, async ({ browser }, testInfo) => {
		const page = await dono(browser);
		try {
			await page.setViewportSize(viewport);
			await page.locator('button[aria-label="ajustes"]').first().click();
			const panel = page.locator('[data-settings]');
			await panel.locator('[data-tab="conta"]').click();
			await expect(panel.locator('[data-invites]')).toBeVisible();
			const list = panel.locator('.scroller').first();
			const thumb = panel.locator('[data-scroll-thumb]').first();
			const close = panel.getByRole('button', { name: 'fechar', exact: true }).filter({ visible: true });
			await expect(thumb).toHaveAttribute('visibility', 'visible');
			await list.evaluate((el) => { el.scrollTop = 0; });
			const card = (await panel.boundingBox())!;
			const button = (await close.boundingBox())!;
			const scroll = (await list.boundingBox())!;
			const style = await panel.evaluate((el) => {
				const css = getComputedStyle(el);
				return { radius: css.borderTopRightRadius, padding: css.paddingRight };
			});
			// One corner on the whole page (cantos.spec.ts); the room is in the inset.
			expect(style).toEqual({ radius: '8px', padding: '24px' });
			expect(scroll.y - (button.y + button.height)).toBeGreaterThanOrEqual(8);
			expect(card.x + card.width - (scroll.x + scroll.width)).toBeGreaterThanOrEqual(24);
			expect(card.y + card.height - (scroll.y + scroll.height)).toBeGreaterThanOrEqual(24);
			await expect.poll(async () => (await thumb.boundingBox())!.y).toBeCloseTo(scroll.y + 2, 0);
			await page.screenshot({ path: testInfo.outputPath('settings-top.png') });

			// Scrolling over the bar still reaches the final account actions;
			// the close button stays visible above it throughout.
			const top = (await thumb.boundingBox())!;
			await page.mouse.move(top.x + top.width / 2, top.y + top.height / 2);
			await page.mouse.wheel(0, 10000);
			await expect.poll(() => list.evaluate((el) => el.scrollTop)).toBeGreaterThan(0);
			await list.evaluate((el) => { el.scrollTop = el.scrollHeight; });
			await expect.poll(async () => {
				const bottom = (await thumb.boundingBox())!;
				return card.y + card.height - (bottom.y + bottom.height);
			}).toBeCloseTo(26, 0);
			await expect(panel.getByRole('button', { name: /sair desta conta/i })).toBeInViewport();
			await page.screenshot({ path: testInfo.outputPath('settings-bottom.png') });
			await close.click();
			await expect(panel).toHaveCount(0);
		} finally {
			await page.context().close();
		}
	});
}
