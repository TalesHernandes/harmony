import { test, expect, Person, PASSWORD } from './fixtures';

async function openAccounts(person: Person): Promise<void> {
	await person.page.locator('button[aria-label="ajustes"]').first().click();
	await person.page.locator('[data-settings] [data-tab="conta"]').click();
	await expect(person.page.locator('[data-invites]')).toBeVisible();
}

test('somente o dono vê Pessoas nos ajustes', async ({ browser }) => {
	const owner = await Person.signIn(browser, 'dono');
	const member = await Person.create(browser, 'd9visibility');
	await openAccounts(owner);
	await expect(owner.page.locator('[data-accounts]').getByRole('heading', { name: 'Pessoas', exact: true })).toBeVisible();
	await expect(owner.page.locator('[data-account="dono"] button')).toHaveCount(0);
	await member.page.locator('button[aria-label="ajustes"]').first().click();
	await member.page.locator('[data-tab="conta"]').click();
	await expect(member.page.locator('[data-settings]')).toBeVisible();
	await expect(member.page.locator('[data-accounts]')).toHaveCount(0);
	const denied = await member.context.request.get('/api/admin/accounts');
	expect(denied.status()).toBe(403);
	await owner.close();
	await member.close();
});

test('desativar desconecta a pessoa e reativar permite entrar com a senha antiga', async ({ browser }) => {
	const owner = await Person.signIn(browser, 'dono');
	const member = await Person.create(browser, 'd9disable');
	await openAccounts(owner);
	const row = owner.page.locator('[data-account="d9disable"]');
	await row.getByRole('button', { name: 'Desativar', exact: true }).click();
	await owner.page.locator('[data-confirm]').getByRole('button', { name: 'Cancelar', exact: true }).click();
	await expect(member.page.locator('button[aria-label="pessoas"]')).toBeVisible();
	await row.getByRole('button', { name: 'Desativar', exact: true }).click();
	await owner.page.locator('[data-confirm-yes]').click();
	await expect(row.getByText('desativada', { exact: true })).toBeVisible();
	await expect(member.page).toHaveURL(/\/login/);
	await member.page.locator('#u').fill(member.username);
	await member.page.locator('#p').fill(PASSWORD);
	await member.page.getByRole('button', { name: /^entrar/i }).click();
	await expect(member.page.getByText('Usuário ou senha não conferem.', { exact: true })).toBeVisible();
	await row.getByRole('button', { name: 'Reativar', exact: true }).click();
	await expect(row.getByRole('button', { name: 'Desativar', exact: true })).toBeVisible();
	await member.page.locator('#p').fill(PASSWORD);
	await member.page.getByRole('button', { name: /^entrar/i }).click();
	await expect(member.page.locator('button[aria-label="pessoas"]')).toBeVisible();
	await owner.close();
	await member.close();
});

test('nova senha aparece uma vez, desconecta sessões e substitui a senha antiga', async ({ browser }) => {
	const owner = await Person.signIn(browser, 'dono');
	const member = await Person.create(browser, 'd9password');
	const second = await Person.signIn(browser, member.username);
	await openAccounts(owner);
	const row = owner.page.locator('[data-account="d9password"]');
	await row.getByRole('button', { name: 'Nova senha', exact: true }).click();
	await owner.page.locator('[data-confirm]').getByRole('button', { name: 'Cancelar', exact: true }).click();
	await expect(owner.page.locator('[data-fresh-password]')).toHaveCount(0);
	await row.getByRole('button', { name: 'Nova senha', exact: true }).click();
	await owner.page.locator('[data-confirm-yes]').click();
	const fresh = owner.page.locator('[data-fresh-password]');
	await expect(fresh).toBeVisible();
	const password = (await fresh.textContent())!;
	expect(password).toMatch(/^[a-z2-9]{5}(?:-[a-z2-9]{5}){3}$/);
	await expect(owner.page.getByText('Passe para a pessoa por um caminho seu; ela entra e todas as sessões antigas dela já caíram.', { exact: true })).toBeVisible();
	await expect(member.page).toHaveURL(/\/login/);
	await expect(second.page).toHaveURL(/\/login/);
	await member.page.locator('#u').fill(member.username);
	await member.page.locator('#p').fill(PASSWORD);
	await member.page.getByRole('button', { name: /^entrar/i }).click();
	await expect(member.page.getByText('Usuário ou senha não conferem.', { exact: true })).toBeVisible();
	await member.page.locator('#p').fill(password);
	await member.page.getByRole('button', { name: /^entrar/i }).click();
	await expect(member.page.locator('button[aria-label="pessoas"]')).toBeVisible();
	await owner.page.getByRole('button', { name: 'Fechar senha', exact: true }).click();
	await expect(fresh).toHaveCount(0);
	await owner.page.locator('[data-tab="diag"]').click();
	await owner.page.locator('[data-tab="conta"]').click();
	await expect(fresh).toHaveCount(0);
	await owner.close();
	await member.close();
	await second.close();
});
