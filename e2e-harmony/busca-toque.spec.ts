import type { Locator, Page } from '@playwright/test';
import { test, expect, Person } from './fixtures';

async function generalID(who: Person): Promise<string> {
	return who.page.evaluate(async () => {
		const { channels } = await fetch('/api/channels').then((r) => r.json());
		return channels.find((c: { name: string }) => c.name === 'geral').id;
	});
}

async function post(who: Person, channel: string, body: string): Promise<number> {
	return who.page.evaluate(async ({ channel, body }) => {
		const res = await fetch(`/api/channels/${channel}/messages`, {
			method: 'POST', headers: { 'Content-Type': 'application/json' },
			body: JSON.stringify({ body, attachments: [] }),
		});
		if (!res.ok) throw new Error(`post: ${res.status}`);
		return (await res.json()).id;
	}, { channel, body });
}

async function search(page: Page, text: string, id: number) {
	await page.getByRole('button', { name: 'procurar', exact: true }).first().click();
	await page.locator('[data-search-box]').fill(text);
	await page.locator(`[data-hit="${id}"]`).click();
}

async function centered(row: Locator) {
	await expect(row).toBeInViewport();
	await expect.poll(() => row.evaluate((el) => {
		const pane = el.closest('[data-history]')!;
		const r = el.getBoundingClientRect(), p = pane.getBoundingClientRect();
		return Math.abs((r.top + r.bottom) / 2 - (p.top + p.bottom) / 2);
	})).toBeLessThan(3);
}

test('busca centraliza uma mensagem antiga, pagina nos dois sentidos e separa o socket', async ({ browser }) => {
	const people: Person[] = [];
	for (let i = 0; i < 7; i++) people.push(await Person.create(browser, `jump_seed_${i}`));
	const ana = people[0], page = ana.page, channel = await generalID(ana);
	const ids: number[] = [];
	for (const who of people) {
		for (let i = 0; i < 18; i++) {
			const index = ids.length;
			ids.push(await post(who, channel, index === 55 ? 'jump-window-target @jump_seed_0\nsecond line\nthird line' : `jump-window-${index}\nsecond line\nthird line`));
		}
	}
	await page.reload();
	await ana.inChannel('geral').waitFor();
	await expect(page.locator(`[data-message="${ids[125]}"]`)).toBeVisible();
	await expect(page.locator(`[data-message="${ids[55]}"]`)).toHaveCount(0);
	const around = page.waitForResponse((r) => new URL(r.url()).searchParams.get('around') === String(ids[55]));
	await search(page, 'jump-window-target', ids[55]);
	expect((await around).ok()).toBe(true);
	const row = page.locator(`[data-message="${ids[55]}"]`);
	await centered(row);
	await expect(row).toHaveClass(/message-highlight/);
	await expect(row).toHaveClass(/mentioned/);
	expect(await row.evaluate((e) => getComputedStyle(e, '::before').width)).toBe('3px');
	expect(await row.locator('.message-highlight-layer').evaluate((e) => getComputedStyle(e).transitionProperty)).toBe('opacity');
	await expect(page.locator('[data-go-recent]')).toBeVisible();
	const fresh = await post(people[1], channel, 'jump-arrived-while-in-past');
	// A socket message must not be appended beside this old window.
	await page.waitForTimeout(350);
	await expect(page.locator(`[data-message="${fresh}"]`)).toHaveCount(0);
	await expect(row).not.toHaveClass(/message-highlight/, { timeout: 4000 });
	await expect.poll(() => row.locator('.message-highlight-layer').evaluate((e) => getComputedStyle(e).opacity)).toBe('0');
	const older = page.waitForResponse((r) => new URL(r.url()).searchParams.has('before'));
	await page.locator('[data-history]').evaluate((e) => { e.scrollTop = 0; });
	expect((await older).ok()).toBe(true);
	await expect(page.locator(`[data-message="${ids[0]}"]`)).toHaveCount(1);
	let releaseAfter!: () => void, capturedAfter!: () => void;
	const held = new Promise<void>((r) => { releaseAfter = r; });
	const captured = new Promise<void>((r) => { capturedAfter = r; });
	await page.route((url) => url.searchParams.has('after'), async (route) => {
		const response = await route.fetch();
		capturedAfter();
		await held;
		await route.fulfill({ response });
	});
	const newer = page.waitForResponse((r) => new URL(r.url()).searchParams.has('after'));
	await page.locator('[data-history]').evaluate((e) => { e.scrollTop = e.scrollHeight; });
	await captured;
	const raced = await post(people[1], channel, 'jump-arrived-during-forward-page');
	await page.waitForTimeout(350);
	await expect(page.locator(`[data-message="${raced}"]`)).toHaveCount(0);
	releaseAfter();
	expect((await newer).ok()).toBe(true);
	await expect(page.locator(`[data-message="${fresh}"]`)).toHaveCount(1);
	await expect(page.locator(`[data-message="${raced}"]`)).toHaveCount(1);
	await expect(page.locator('[data-go-recent]')).toHaveCount(0);
	await search(page, 'jump-window-target', ids[55]);
	await centered(row);
	await expect(row).toHaveClass(/message-highlight/);
	// Reloading recent messages starts another old window; the explicit exit
	// returns to the real tail, including the message held back from the socket.
	await page.reload();
	await expect(page.locator(`[data-message="${raced}"]`)).toBeVisible();
	await search(page, 'jump-window-target', ids[55]);
	await expect(page.locator('[data-go-recent]')).toBeVisible();
	await page.locator('[data-go-recent]').click();
	await expect(page.locator(`[data-message="${raced}"]`)).toBeInViewport();
	await expect(page.locator('[data-go-recent]')).toHaveCount(0);
});

test('um resultado já carregado e de outra conversa salta sem pedir around', async ({ browser }) => {
	const ana = await Person.create(browser, 'jump_cached');
	const bo = await Person.create(browser, 'jump_peer');
	await ana.openDMWith(bo.username);
	await ana.writeTo(bo.username, 'jump-cached-private-target');
	const row = ana.page.locator('[data-message]').filter({ hasText: 'jump-cached-private-target' });
	await expect(row).toBeVisible();
	const id = Number(await row.getAttribute('data-message'));
	await ana.openChannels();
	await ana.page.locator('nav button[data-space="server"]').first().click();
	await ana.page.locator('nav button[aria-label="#geral"]').first().click();
	await ana.inChannel('geral').waitFor();
	const requests: string[] = [];
	ana.page.on('request', (r) => { if (new URL(r.url()).searchParams.has('around')) requests.push(r.url()); });
	await search(ana.page, 'jump-cached-private-target', id);
	await ana.composerFor(bo.username).waitFor();
	await expect(ana.page.locator(`[data-message="${id}"]`)).toBeInViewport();
	await expect(ana.page.locator(`[data-message="${id}"]`)).toHaveClass(/message-highlight/);
	expect(requests).toEqual([]);
});

async function touchStart(page: Page, row: Locator) {
	// CDP injects raw coordinates; wait for the reply bar's layout as a real tap does.
	await row.locator('.body-text').tap({ trial: true });
	const box = (await row.locator('.body-text').boundingBox())!;
	const session = await page.context().newCDPSession(page);
	await session.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: box.x + 10, y: box.y + 8 }] });
	return session;
}

async function longPress(page: Page, row: Locator) {
	const session = await touchStart(page, row);
	await expect(row.locator('[data-actions]')).toBeVisible({ timeout: 1500 });
	await session.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
	await session.detach();
}

test('toque longo abre reagir, responder, editar e apagar; toque fora fecha', async ({ browser }) => {
	const ana = await Person.create(browser, 'touch_actions', { viewport: Person.PHONE, hasTouch: true });
	const page = ana.page;
	await ana.inChannel('geral').fill('touch-menu-target');
	await page.getByRole('button', { name: /^enviar$/i }).tap();
	const initial = page.locator('[data-message]').filter({ hasText: 'touch-menu-target' });
	await expect(initial).toBeVisible();
	const row = page.locator(`[data-message="${await initial.getAttribute('data-message')}"]`);
	await expect(row.locator('[data-actions]')).toBeHidden();
	await row.locator('.body-text').tap();
	await expect(row.locator('[data-actions]')).toBeHidden();
	await longPress(page, row);
	expect(await page.evaluate(() => getSelection()?.toString())).toBe('');
	expect(await row.evaluate((e) => getComputedStyle(e).userSelect)).toBe('none');
	for (const label of ['reagir', 'responder', 'editar', 'remover']) await expect(row.getByLabel(label, { exact: true })).toBeVisible();
	await ana.inChannel('geral').tap();
	await expect(row.locator('[data-actions]')).toBeHidden();
	await longPress(page, row);
	await row.getByLabel('reagir', { exact: true }).tap();
	await row.locator('[data-emoji-panel]').getByLabel('procurar emoji').fill('fogo');
	await row.locator('[data-emoji="🔥"]').tap();
	await expect(row.locator('[data-reaction="🔥"]')).toBeVisible();
	await longPress(page, row);
	await row.getByLabel('responder', { exact: true }).tap();
	await expect(page.locator('[data-composer]')).toContainText('touch-menu-target');
	await longPress(page, row);
	await row.getByLabel('editar', { exact: true }).tap();
	await row.locator('[data-editing] textarea').fill('touch-menu-target-edited');
	await row.locator('[data-edit-save]').tap();
	await expect(row).toContainText('touch-menu-target-edited');
	await longPress(page, row);
	await row.getByLabel('remover', { exact: true }).tap();
	await expect(page.locator('[data-confirm]')).toBeVisible();
	await page.locator('[data-confirm-yes]').tap();
	await expect(page.locator('[data-message]').filter({ hasText: 'Mensagem removida' }).last()).toBeVisible();
});

test('mover o dedo cancela o toque longo; mouse continua abrindo por hover', async ({ browser }) => {
	const ana = await Person.create(browser, 'touch_cancel', { viewport: Person.PHONE, hasTouch: true });
	await post(ana, await generalID(ana), 'touch-cancel-target');
	const row = ana.page.locator('[data-message]').filter({ hasText: 'touch-cancel-target' });
	await expect(row).toBeVisible();
	const session = await touchStart(ana.page, row);
	const box = (await row.locator('.body-text').boundingBox())!;
	await session.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x: box.x + 40, y: box.y + 8 }] });
	await ana.page.waitForTimeout(650);
	await session.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
	await session.detach();
	await expect(row.locator('[data-actions]')).toBeHidden();
	const desktop = await Person.create(browser, 'touch_desktop');
	await expect(desktop.page.locator('[data-message]').filter({ hasText: 'touch-cancel-target' })).toBeVisible();
	const otherRow = desktop.page.locator('[data-message]').filter({ hasText: 'touch-cancel-target' });
	await desktop.page.mouse.move(0, 0);
	await expect(otherRow.locator('[data-actions]')).toBeHidden();
	await otherRow.hover();
	await expect(otherRow.getByLabel('responder', { exact: true })).toBeVisible();
});
