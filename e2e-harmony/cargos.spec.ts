// D12c: roles. The owner's acceptance in one run, from his own screen: he
// creates "Moderador" in green and gives it to a friend; the friend's name
// turns green, and the friend removes a third person's message. The owner
// takes the role away and the friend cannot any more. With the role or
// without it, the friend never sees the invites.
import type { Page } from '@playwright/test';
import { test, expect, Person } from './fixtures';

const GREEN = '#2ecc71';

async function call(page: Page, method: string, path: string, body?: unknown): Promise<{ status: number; data: any }> {
	return page.evaluate(
		async ([method, path, body]) => {
			const res = await fetch(path as string, {
				method: method as string,
				credentials: 'same-origin',
				headers: { 'Content-Type': 'application/json' },
				body: body === undefined ? undefined : JSON.stringify(body),
			});
			return { status: res.status, data: res.status === 204 ? {} : await res.json() };
		},
		[method, path, body] as const,
	);
}

async function openSettings(p: Person, tab: string) {
	await p.page.locator('button[aria-label="ajustes"]').first().click();
	await expect(p.page.locator('[data-settings]')).toBeVisible({ timeout: 10_000 });
	await p.page.locator(`[data-settings] [data-tab="${tab}"]`).click();
}

async function closeSettings(p: Person) {
	await p.page.keyboard.press('Escape');
	await expect(p.page.locator('[data-settings]')).toHaveCount(0);
}

async function noInvites(p: Person) {
	await openSettings(p, 'conta');
	// The account tab draws; the invites section never does.
	await expect(p.page.locator('[data-settings] [data-tab="cargos"]')).toHaveCount(0);
	await p.page.waitForTimeout(500);
	await expect(p.page.locator('[data-invites]')).toHaveCount(0);
	await closeSettings(p);
}

test('o dono cria Moderador em verde, dá a um amigo, o amigo apaga a mensagem de outro, perde o cargo e não consegue mais', async ({ browser }) => {
	const n = Date.now() % 100000;
	const roleName = 'Moderador ' + n;
	const dono = await Person.signIn(browser, 'dono');
	const mod = await Person.create(browser, 'cargo' + n);
	const ter = await Person.create(browser, 'terceira' + n);
	const general = (await call(ter.page, 'GET', '/api/channels')).data.channels.find((c: any) => c.kind === 'text' && c.name === 'geral').id;

	await noInvites(mod);

	// The role, made on the owner's screen.
	await openSettings(dono, 'cargos');
	await dono.page.locator('[data-role-create]').click();
	const editor = dono.page.locator('[data-role-editor]');
	await expect(editor).toBeVisible({ timeout: 5_000 });
	await editor.locator('[data-role-name]').fill(roleName);
	await editor.locator('[data-role-name-save]').click();
	await expect(dono.page.locator(`[data-role="${roleName}"]`)).toBeVisible({ timeout: 5_000 });
	await editor.locator(`[data-role-color="${GREEN}"]`).click();
	await expect(editor.locator(`[data-role-color="${GREEN}"]`)).toHaveAttribute('aria-pressed', 'true', { timeout: 5_000 });
	await editor.locator('[data-role-permission="manage_messages"]').click();
	await expect(editor.locator('[data-role-permission="manage_messages"]')).toBeChecked({ timeout: 5_000 });
	await editor.locator('[data-role-member-pick]').selectOption(mod.username);
	await editor.locator('[data-role-member-add]').click();
	await expect(editor.locator(`[data-role-member="${mod.username}"]`)).toBeVisible({ timeout: 5_000 });
	await closeSettings(dono);

	// The friend's name is green on the third person's screen, without a reload.
	const listed = ter.page.locator(`[data-person="${mod.username}"] [data-name-color]`);
	if (!(await ter.page.locator('aside').isVisible())) {
		await ter.page.locator('button[aria-label="pessoas"]').first().click();
		await expect(ter.page.locator('aside')).toBeVisible({ timeout: 10_000 });
	}
	await expect(listed).toHaveAttribute('data-name-color', GREEN, { timeout: 10_000 });
	await expect(listed).toHaveClass(/role-2ecc71/);

	// The third person says something; the friend removes it from the screen.
	const said = (await call(ter.page, 'POST', `/api/channels/${general}/messages`, { body: 'apaga essa ' + n, attachments: [] })).data;
	await mod.page.locator('nav button[aria-label="#geral"]').click();
	await mod.page.getByText('apaga essa ' + n).hover();
	await mod.page.locator(`[data-remove="${said.id}"]`).click();
	await mod.page.locator('[data-confirm-yes]').click();
	await expect(ter.page.getByText('apaga essa ' + n)).toHaveCount(0, { timeout: 10_000 });
	// The author's name over the friend's own line is green too.
	await call(mod.page, 'POST', `/api/channels/${general}/messages`, { body: 'sou moderador ' + n, attachments: [] });
	await expect(ter.page.locator(`[data-author="${mod.username}"]`).last()).toHaveClass(/role-2ecc71/, { timeout: 10_000 });

	await noInvites(mod);

	// The owner takes it away; the friend's button goes, and the server refuses.
	await openSettings(dono, 'cargos');
	await dono.page.locator(`[data-role="${roleName}"]`).click();
	await dono.page.locator(`[data-role-member-remove="${mod.username}"]`).click();
	await expect(dono.page.locator(`[data-role-member="${mod.username}"]`)).toHaveCount(0, { timeout: 5_000 });
	await closeSettings(dono);

	const again = (await call(ter.page, 'POST', `/api/channels/${general}/messages`, { body: 'e essa ' + n, attachments: [] })).data;
	await expect(mod.page.getByText('e essa ' + n)).toBeVisible({ timeout: 10_000 });
	await mod.page.getByText('e essa ' + n).hover();
	await expect(mod.page.locator(`[data-remove="${again.id}"]`)).toHaveCount(0);
	expect((await call(mod.page, 'DELETE', `/api/messages/${again.id}`)).status).toBe(404);
	await expect(ter.page.locator(`[data-person="${mod.username}"] [data-name-color]`)).toHaveCount(0, { timeout: 10_000 });

	await noInvites(mod);

	// Tidy: the role goes, so the next run starts from the same list.
	const roles = (await call(dono.page, 'GET', '/api/roles')).data.roles as Array<{ id: string; name: string }>;
	for (const r of roles.filter((r) => r.name === roleName)) await call(dono.page, 'DELETE', `/api/roles/${r.id}`);

	await ter.close();
	await mod.close();
	await dono.close();
});
