import type { Locator, Page } from '@playwright/test';
import { test, expect, Person } from './fixtures';

test('call notices use message typography and align with message bodies', async ({ browser }) => {
	const ana = await Person.create(browser, 'callnotetextana');
	const bruno = await Person.create(browser, 'callnotetextbru');
	await ana.openDMWith(bruno.username);
	await ana.writeTo(bruno.username, 'Message beside a call notice');
	// The composer refuses another submit while the first request is pending.
	await expect(ana.page.locator('[data-message]')).toHaveCount(1);
	await ana.writeTo(bruno.username, 'Grouped message beside a call notice');
	await expect(ana.page.locator('[data-message]')).toHaveCount(2);
	await ana.page.locator('[data-call-start]').click();
	const incoming = bruno.page.locator('[data-incoming-call]');
	await expect(incoming).toBeVisible({ timeout: 20_000 });
	await incoming.locator('[data-call-reject]').click();
	await bruno.openDMWith(ana.username);
	for (const person of [ana, bruno]) {
		const note = person.page.locator('[data-call-note]');
		await expect(note).toBeVisible({ timeout: 15_000 });
		await expect(note).toContainText(/recusou/i);
		for (const viewport of [{ width: 1100, height: 760 }, Person.PHONE]) {
			await person.page.setViewportSize(viewport);
			const body = person.page.locator('[data-message] .text-message').last();
			await expect(body).toBeVisible();
			const message = await body.evaluate((el) => ({
				fontSize: getComputedStyle(el).fontSize,
				lineHeight: getComputedStyle(el).lineHeight,
				left: el.getBoundingClientRect().left,
				paddingTop: getComputedStyle(el.closest('[data-message]')!).paddingTop,
				paddingBottom: getComputedStyle(el.closest('[data-message]')!).paddingBottom,
			}));
			const notice = await note.locator('[data-call-note-text]').evaluate((el) => ({
				fontSize: getComputedStyle(el).fontSize,
				lineHeight: getComputedStyle(el).lineHeight,
				left: el.getBoundingClientRect().left,
				paddingTop: getComputedStyle(el.closest('[data-call-note]')!).paddingTop,
				paddingBottom: getComputedStyle(el.closest('[data-call-note]')!).paddingBottom,
			}));
			expect(notice.fontSize).toBe(message.fontSize);
			expect(Math.abs(notice.left - message.left)).toBeLessThanOrEqual(1);
			expect(notice.lineHeight).toBe(message.lineHeight);
			expect(notice.paddingTop).toBe(message.paddingTop);
			expect(notice.paddingBottom).toBe(message.paddingBottom);
			expect(await note.locator('svg').evaluate((el) => el.getBoundingClientRect().width)).toBe(20);
		}
	}
	await ana.close();
	await bruno.close();
});

async function seed(who: Person): Promise<number[]> {
	return who.page.evaluate(async () => {
		const { channels } = await fetch('/api/channels').then((res) => res.json());
		const channel = channels.find((c: { name: string }) => c.name === 'geral').id;
		const ids: number[] = [];
		// Stay below the per-person burst limit; four lines still fill a history.
		for (let i = 0; i < 16; i++) {
			const res = await fetch(`/api/channels/${channel}/messages`, {
				method: 'POST', headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ body: `reaction-panel-${i}\nsecond line\nthird line\nfourth line`, attachments: [] }),
			});
			if (!res.ok) throw new Error(`post: ${res.status}`);
			ids.push((await res.json()).id);
		}
		return ids;
	});
}

async function place(row: Locator, edge: 'top' | 'bottom') {
	await row.evaluate((el, edge) => {
		const pane = el.closest<HTMLElement>('[data-history]')!;
		const p = pane.getBoundingClientRect(), r = el.getBoundingClientRect();
		const target = edge === 'top' ? p.top + 40 : p.bottom - r.height - 40;
		pane.scrollTop += r.top - target;
	}, edge);
	await expect.poll(() => row.evaluate((el, edge) => {
		const p = el.closest('[data-history]')!.getBoundingClientRect(), r = el.getBoundingClientRect();
		return Math.abs((edge === 'top' ? r.top - p.top : p.bottom - r.bottom) - 40);
	}, edge)).toBeLessThan(2);
}

async function open(page: Page, row: Locator) {
	await row.hover({ position: { x: 16, y: 16 } });
	const button = row.locator('[data-react]');
	await expect(button).toBeVisible();
	const point = (await button.boundingBox())!;
	const history = page.locator('[data-history]');
	const before = await history.evaluate((el) => el.scrollTop);
	// A raw pointer click avoids Playwright's own scrollIntoView hiding the bug.
	await page.mouse.click(point.x + point.width / 2, point.y + point.height / 2);
	const panel = row.locator('.emoji-panel');
	await expect(panel).toBeVisible();
	await expect(panel.locator('[data-emoji-search]')).toBeFocused();
	// Let delayed dialog focus and the next paint happen before reading scroll.
	await page.evaluate(() => new Promise<void>((resolve) => requestAnimationFrame(() => requestAnimationFrame(() => resolve()))));
	return { history, before, panel };
}

async function inside(panel: Locator) {
	const bounds = await panel.evaluate((el) => {
		const pane = el.closest<HTMLElement>('[data-history]')!;
		const p = pane.getBoundingClientRect(), r = el.getBoundingClientRect();
		return {
			top: r.top - p.top - pane.clientTop,
			bottom: p.top + pane.clientTop + pane.clientHeight - r.bottom,
			left: r.left - p.left - pane.clientLeft,
			right: p.left + pane.clientLeft + pane.clientWidth - r.right,
		};
	});
	for (const gap of Object.values(bounds)) expect(gap).toBeGreaterThanOrEqual(-1);
}

test('opening reactions at either visible edge preserves scroll and contains the panel', async ({ browser }) => {
	const ana = await Person.create(browser, 'reactionpanelbounds' + test.info().repeatEachIndex, { viewport: { width: 1100, height: 760 } });
	const ids = await seed(ana);
	await ana.page.reload();
	await ana.inChannel('geral').waitFor();
	await expect(ana.page.locator(`[data-message="${ids[15]}"]`)).toBeVisible();
	const history = ana.page.locator('[data-history]');
	expect(await history.evaluate((el) => el.scrollHeight - el.clientHeight)).toBeGreaterThan(500);
	for (const height of [760, 400]) {
		// The short viewport forces a cap rather than just choosing another side.
		await ana.page.setViewportSize({ width: 1100, height });
		for (const edge of ['top', 'bottom'] as const) {
			const row = ana.page.locator(`[data-message="${ids[edge === 'top' ? 3 : 12]}"]`);
			await place(row, edge);
			const { before, panel } = await open(ana.page, row);
			expect(await history.evaluate((el) => el.scrollTop)).toBe(before);
			await inside(panel);
			const list = panel.locator('[data-emoji-list]');
			expect(await list.evaluate((el) => el.scrollHeight > el.clientHeight)).toBe(true);
			await list.evaluate((el) => { el.scrollTop = 200; });
			expect(await history.evaluate((el) => el.scrollTop)).toBe(before);
			await ana.page.keyboard.press('Escape');
			await expect(panel).toHaveCount(0);
			expect(await history.evaluate((el) => el.scrollTop)).toBe(before);
		}
	}
	await ana.close();
});

test('an open reaction panel paints above a later hovered message', async ({ browser }) => {
	const ana = await Person.create(browser, 'reactionpanelstack' + test.info().repeatEachIndex, { viewport: { width: 1100, height: 760 } });
	const ids = await seed(ana);
	await ana.page.reload();
	await ana.inChannel('geral').waitFor();
	await expect(ana.page.locator(`[data-message="${ids[15]}"]`)).toBeVisible();
	const row = ana.page.locator(`[data-message="${ids[3]}"]`);
	await place(row, 'top');
	// Leave enough headroom for the old autofocus bug too: this test must
	// reach the overlapping hit test independently of the scroll regression.
	await row.evaluate((el) => { el.closest<HTMLElement>('[data-history]')!.scrollTop -= 260; });
	await expect.poll(() => row.evaluate((el) => el.getBoundingClientRect().top - el.closest('[data-history]')!.getBoundingClientRect().top)).toBe(300);
	const { panel } = await open(ana.page, row);
	const overlap = await panel.evaluate((el) => {
		const r = el.getBoundingClientRect(), pane = el.closest('[data-history]')!;
		const x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
		const owner = el.closest('[data-message]')!;
		const next = [...pane.querySelectorAll<HTMLElement>('[data-message]')].find((msg) => {
			const m = msg.getBoundingClientRect();
			return !!(owner.compareDocumentPosition(msg) & Node.DOCUMENT_POSITION_FOLLOWING) && m.top < y && m.bottom > y;
		});
		if (!next) throw new Error('no later message under the panel centre');
		return { x, y, id: next.dataset.message!, hoverX: pane.getBoundingClientRect().left + 60 };
	});
	// Hover the exposed left side of the row underneath, without asking
	// Playwright to move a covered element into view or through the panel.
	await ana.page.mouse.move(overlap.hoverX, overlap.y);
	await expect.poll(() => ana.page.locator(`[data-message="${overlap.id}"]`).evaluate((el) => el.matches(':hover'))).toBe(true);
	// Before the fix each isolated later row paints over the earlier row's
	// entire panel. Hit testing at this overlapping centre returns that row.
	expect(await panel.evaluate((el, point) => el.contains(document.elementFromPoint(point.x, point.y)), overlap)).toBe(true);
	await ana.close();
});
