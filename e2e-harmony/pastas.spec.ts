// D12a: folders, dragging, renaming and pinning, watched from a second
// browser. The owner's acceptance in one run: create "Jogos", drag a text
// channel and a voice room into it, rename a channel, pin a message — and the
// other person sees each of them arrive, and sees the same after a reload.
import type { Locator, Page } from '@playwright/test';
import { test, expect, Person } from './fixtures';

async function call<T>(page: Page, method: string, path: string, body?: unknown): Promise<T> {
	return page.evaluate(
		async ([method, path, body]) => {
			const res = await fetch(path as string, {
				method: method as string,
				credentials: 'same-origin',
				headers: { 'Content-Type': 'application/json' },
				body: body === undefined ? undefined : JSON.stringify(body),
			});
			return res.status === 204 ? ({} as never) : ((await res.json()) as never);
		},
		[method, path, body] as const,
	);
}

// A drag the way a hand does it: press, move in steps past the six-pixel
// threshold, release over the target.
async function drag(page: Page, from: Locator, to: Locator) {
	const a = (await from.boundingBox())!;
	const b = (await to.boundingBox())!;
	await page.mouse.move(a.x + a.width / 2, a.y + a.height / 2);
	await page.mouse.down();
	await page.mouse.move(a.x + a.width / 2, a.y + a.height / 2 + 10, { steps: 3 });
	await page.mouse.move(b.x + b.width / 2, b.y + b.height / 2, { steps: 8 });
	await page.mouse.up();
}

test('o dono monta a pasta Jogos arrastando, renomeia e fixa, e o outro navegador vê tudo na hora', async ({ browser }) => {
	const n = Date.now() % 100000;
	const folderName = 'Jogos ' + n;
	const text = 'mine-' + n;
	const room = 'sala-' + n;
	const dono = await Person.signIn(browser, 'dono');
	const bea = await Person.create(browser, 'pastas' + test.info().repeatEachIndex);
	const textID = (await call<{ id: string }>(dono.page, 'POST', '/api/channels', { name: text })).id;
	const roomID = (await call<{ id: string }>(dono.page, 'POST', '/api/channels', { name: room, kind: 'voice' })).id;

	// The folder, made from the column heading's menu.
	await dono.page.locator('nav [data-drop-folder=""]').first().click({ button: 'right' });
	await dono.page.locator('[data-menu]').getByRole('menuitem', { name: 'Nova categoria' }).click();
	await dono.page.locator('[data-new-channel]').fill(folderName);
	await dono.page.getByRole('button', { name: 'Criar' }).click();
	const heading = (p: Person) => p.page.locator(`nav button[aria-label="categoria ${folderName}"]`);
	await expect(heading(dono)).toBeVisible({ timeout: 10_000 });
	await expect(heading(bea)).toBeVisible({ timeout: 10_000 });
	const folderID = (await heading(dono).locator('xpath=ancestor::*[@data-folder][1]').getAttribute('data-folder'))!;
	const inside = (p: Person, id: string) => p.page.locator(`nav [data-folder="${folderID}"] [data-drop-room="${id}"]`);

	// Both rooms dragged in by the owner; bea's column follows without a reload.
	const textRow = (p: Person) => p.page.locator(`nav [data-drop-room="${textID}"]`);
	const roomRow = (p: Person) => p.page.locator(`nav [data-drop-room="${roomID}"]`);
	await expect(textRow(bea)).toBeVisible({ timeout: 10_000 });
	await drag(dono.page, textRow(dono), heading(dono));
	await expect(inside(dono, textID)).toBeVisible({ timeout: 5_000 });
	await expect(inside(bea, textID)).toBeVisible({ timeout: 5_000 });
	await drag(dono.page, roomRow(dono), heading(dono));
	await expect(inside(bea, roomID)).toBeVisible({ timeout: 5_000 });
	// The release over the heading was not a click: the folder is still open.
	await expect(heading(dono)).toHaveAttribute('aria-expanded', 'true');

	// Renamed from the row's menu; bea sees the new name.
	const renamed = 'minecraft-' + n;
	await textRow(dono).locator('button').first().click({ button: 'right' });
	await dono.page.locator('[data-menu]').getByRole('menuitem', { name: 'Renomear canal' }).click();
	await dono.page.locator('[data-new-channel]').fill(renamed);
	await dono.page.getByRole('button', { name: 'Salvar' }).click();
	await expect(bea.page.locator(`nav button[aria-label="#${renamed}"]`)).toBeVisible({ timeout: 5_000 });

	// A pin, from the message's own button; bea finds it in the pinned list.
	const said = await call<{ id: number }>(dono.page, 'POST', `/api/channels/${textID}/messages`, { body: 'fixa essa ' + n, attachments: [] });
	await dono.page.locator(`nav button[aria-label="#${renamed}"]`).click();
	const line = dono.page.locator(`[data-pin="${said.id}"]`);
	await dono.page.getByText('fixa essa ' + n).hover();
	await line.click();
	await expect(line).toHaveAttribute('aria-pressed', 'true', { timeout: 5_000 });
	await bea.page.locator(`nav button[aria-label="#${renamed}"]`).click();
	// The pin leaves one line in the channel, which is how bea finds out.
	await expect(bea.page.locator('[data-pin-note-text]')).toHaveText(/fixou uma mensagem\.$/, { timeout: 5_000 });
	await expect(dono.page.locator('[data-pin-note-text]')).toHaveText('Você fixou uma mensagem.');
	await bea.page.locator('[data-pins-open]').click();
	await expect(bea.page.locator(`[data-pins-panel] [data-pinned="${said.id}"]`)).toBeVisible({ timeout: 5_000 });
	// Bea is not the owner: she cannot pin in a channel, so she gets no button.
	await expect(bea.page.locator('[data-pins-panel] [data-unpin]')).toHaveCount(0);
	await bea.page.keyboard.press('Escape');

	// Closing the folder is bea's own, and survives her reload; the owner's
	// stays open. After the reload everything is where it was put.
	await bea.page.locator('nav [data-drop-room]').first().click();
	await heading(bea).click();
	await expect(inside(bea, roomID)).toHaveCount(0);
	await bea.page.reload();
	await expect(heading(bea)).toHaveAttribute('aria-expanded', 'false', { timeout: 15_000 });
	await expect(inside(bea, roomID)).toHaveCount(0);
	await heading(bea).click();
	await expect(inside(bea, roomID)).toBeVisible();
	await expect(inside(bea, textID)).toBeVisible();
	await dono.page.reload();
	await expect(inside(dono, textID)).toBeVisible({ timeout: 15_000 });

	// Deleting the folder keeps both rooms, at the top of the column.
	await call(dono.page, 'DELETE', `/api/categories/${folderID}`);
	await expect(heading(bea)).toHaveCount(0, { timeout: 5_000 });
	await expect(textRow(bea)).toBeVisible();
	await expect(roomRow(bea)).toBeVisible();

	await call(dono.page, 'DELETE', `/api/channels/${textID}`);
	await call(dono.page, 'DELETE', `/api/channels/${roomID}`);
	await bea.close();
	await dono.close();
});

test('quem não é o dono não arrasta, não renomeia e não cria categoria', async ({ browser }) => {
	const bea = await Person.create(browser, 'naopasta' + test.info().repeatEachIndex);
	await bea.page.locator('nav button[aria-label="#geral"]').first().click({ button: 'right' });
	await expect(bea.page.locator('[data-menu]').getByRole('menuitem', { name: 'Renomear canal' })).toHaveCount(0);
	await bea.page.keyboard.press('Escape');
	await bea.page.locator('nav [data-drop-folder=""]').first().click({ button: 'right' });
	await expect(bea.page.locator('[data-menu]')).toHaveCount(0);
	const statuses = await bea.page.evaluate(async () => {
		const put = await fetch('/api/layout', { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: '{"categories":[],"channels":[]}' });
		const make = await fetch('/api/categories', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{"name":"x"}' });
		const rename = await fetch('/api/channels/geral', { method: 'PATCH', headers: { 'Content-Type': 'application/json' }, body: '{"name":"x"}' });
		return [put.status, make.status, rename.status];
	});
	expect(statuses).toEqual([403, 403, 403]);
	await bea.close();
});
