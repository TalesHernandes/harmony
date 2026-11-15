// "botao no clique direito para deletar canal" (24/09/2026). The owner's
// alone, it asks first, and the channel leaves everybody's column at once -
// including the column of somebody who was reading it.
import type { Page } from '@playwright/test';
import { test, expect, Person } from './fixtures';

async function create(page: Page, name: string, kind: 'text' | 'voice' = 'text'): Promise<string> {
	return page.evaluate(
		async ([name, kind]) => {
			const res = await fetch('/api/channels', {
				method: 'POST',
				credentials: 'same-origin',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ name, kind }),
			});
			return ((await res.json()) as { id: string }).id;
		},
		[name, kind] as const,
	);
}

test('o dono apaga um canal pelo clique direito, e ele some também para quem estava lendo', async ({ browser }) => {
	const dono = await Person.signIn(browser, 'dono');
	const bea = await Person.create(browser, 'apagar' + test.info().repeatEachIndex);
	const name = 'some-' + (Date.now() % 100000);
	await create(dono.page, name);
	const row = (p: Person) => p.page.locator(`nav button[aria-label="#${name}"]`);
	await expect(row(dono)).toBeVisible({ timeout: 15_000 });
	await expect(row(bea)).toBeVisible({ timeout: 15_000 });

	// Bea is reading it when it goes.
	await row(bea).click();
	await expect(bea.page.getByPlaceholder('Escrever em #' + name)).toBeVisible();

	await row(dono).click({ button: 'right' });
	const item = dono.page.locator('[data-menu]').getByRole('menuitem', { name: 'Apagar canal' });
	await expect(item).toHaveAttribute('data-danger', '');
	await item.click();

	// It asks first, naming the channel; Cancelar leaves it where it was.
	const box = dono.page.locator('[data-confirm]');
	await expect(box).toContainText(name);
	await box.getByRole('button', { name: 'Cancelar' }).click();
	await expect(box).toHaveCount(0);
	await expect(row(dono)).toBeVisible();

	await row(dono).click({ button: 'right' });
	await dono.page.locator('[data-menu]').getByRole('menuitem', { name: 'Apagar canal' }).click();
	await dono.page.locator('[data-confirm] [data-confirm-yes]').click();

	await expect(row(dono)).toHaveCount(0, { timeout: 10_000 });
	await expect(row(bea)).toHaveCount(0, { timeout: 10_000 });
	// Bea landed on a channel that exists instead of on a pane for one that
	// does not.
	await expect(bea.page.getByPlaceholder('Escrever em #' + name)).toHaveCount(0);
	await expect(bea.page.getByPlaceholder(/^Escrever em #/)).toBeVisible();

	// And the name is free again.
	await create(dono.page, name);
	await expect(row(bea)).toBeVisible({ timeout: 15_000 });

	await bea.close();
	await dono.close();
});

test('quem não é o dono não vê "Apagar canal"', async ({ browser }) => {
	const bea = await Person.create(browser, 'naoapaga' + test.info().repeatEachIndex);
	await bea.page.locator('nav button[aria-label="#geral"]').first().click({ button: 'right' });
	const menu = bea.page.locator('[data-menu]');
	await expect(menu.getByRole('menuitem').first()).toBeVisible();
	await expect(menu.getByRole('menuitem', { name: 'Apagar canal' })).toHaveCount(0);
	// And the server says no by itself, whatever a page sends.
	const status = await bea.page.evaluate(async () => (await fetch('/api/channels/geral', { method: 'DELETE' })).status);
	expect(status).toBe(403);
	await bea.close();
});

test('o dono apaga uma sala de voz vazia pelo clique direito', async ({ browser }) => {
	const dono = await Person.signIn(browser, 'dono');
	const name = 'sala-' + (Date.now() % 100000);
	await create(dono.page, name, 'voice');
	const room = dono.page.locator('nav button[title="Entrar na sala"]', { hasText: name });
	await expect(room).toBeVisible({ timeout: 15_000 });
	await room.click({ button: 'right' });
	await dono.page.locator('[data-menu]').getByRole('menuitem', { name: 'Apagar canal' }).click();
	await expect(dono.page.locator('[data-confirm]')).toContainText(name);
	await dono.page.locator('[data-confirm] [data-confirm-yes]').click();
	await expect(room).toHaveCount(0, { timeout: 10_000 });
	await dono.close();
});
