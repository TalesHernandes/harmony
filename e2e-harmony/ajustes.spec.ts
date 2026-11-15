// D4v, os Ajustes no desenho novo: um painel no véu como os outros, que fecha
// do mesmo jeito que os outros, e que não perde as abas quando a conta tem
// muita coisa para mostrar.
import { test, expect, Person } from './fixtures';

async function open(ana: Person): Promise<void> {
	await ana.page.locator('button[aria-label="ajustes"]').first().click();
	await expect(ana.page.locator('[data-settings]')).toBeVisible({ timeout: 10_000 });
}

test('os ajustes fecham com esc, com o X e com um clique no véu', async ({ browser }) => {
	const ana = await Person.create(browser, 'ajfecha' + test.info().repeatEachIndex);
	const panel = ana.page.locator('[data-settings]');

	await open(ana);
	await ana.page.keyboard.press('Escape');
	await expect(panel).toHaveCount(0);

	await open(ana);
	await panel.getByRole('button', { name: 'fechar' }).click();
	await expect(panel).toHaveCount(0);

	// The veil is everything around the card: its own corner is outside it.
	await open(ana);
	const box = (await panel.boundingBox())!;
	await ana.page.mouse.click(box.x + box.width + 4, box.y + box.height / 2);
	await expect(panel).toHaveCount(0);

	// And a click inside the card is not a click on the veil.
	await open(ana);
	await panel.locator('[data-tab="diag"]').click();
	await expect(panel).toBeVisible();

	await ana.close();
});

// The owner's list of invites grew the card past the top of the screen, and
// the tabs and the X went with it: the grid's row was auto, so the card's
// percentage height had nothing to resolve against. The test account `dono`
// has one invite per account the suite ever created, which is the long list.
test('uma lista longa de convites não empurra as abas para fora da tela', async ({ browser }) => {
	const dono = await Person.signIn(browser, 'dono');
	await dono.page.setViewportSize({ width: 1440, height: 900 });
	await open(dono);
	await dono.page.locator('[data-tab="conta"]').click();
	await expect(dono.page.locator('[data-invites]')).toBeVisible({ timeout: 10_000 });

	const panel = dono.page.locator('[data-settings]');
	const card = (await panel.boundingBox())!;
	expect(card.y).toBeGreaterThanOrEqual(0);
	expect(card.y + card.height).toBeLessThanOrEqual(900);
	// The tab one would press next is on the screen and pressable.
	await panel.locator('[data-tab="diag"]').click({ timeout: 5_000 });
	await expect(panel.locator('[data-tab="diag"]')).toHaveAttribute('aria-pressed', 'true');

	await dono.close();
});

// At 375 pixels the card came out 429 wide and cut "@username" off on the
// right: the tab row cannot wrap, and the veil's grid column grew to fit it.
// The row is meant to slide sideways inside the card instead.
test('num telefone o painel cabe na tela e as abas deslizam', async ({ browser }) => {
	const ana = await Person.create(browser, 'ajtel' + test.info().repeatEachIndex, {
		viewport: { width: 375, height: 812 },
	});
	// On a telephone the gear lives in the channel drawer.
	await ana.page.locator('button[aria-label="canais"]').click();
	await ana.page.locator('button[aria-label="ajustes"]:visible').click();
	await expect(ana.page.locator('[data-settings]')).toBeVisible({ timeout: 10_000 });
	await ana.page.locator('[data-tab="conta"]').click();

	const panel = ana.page.locator('[data-settings]');
	const card = (await panel.boundingBox())!;
	expect(card.x).toBeGreaterThanOrEqual(0);
	expect(card.x + card.width).toBeLessThanOrEqual(375);

	const strip = panel.locator('[data-tab]').first().locator('..');
	const { client, scroll } = await strip.evaluate((el) => ({ client: el.clientWidth, scroll: el.scrollWidth }));
	expect(client).toBeLessThanOrEqual(card.width);
	expect(scroll).toBeGreaterThan(client);
	// The last tab is reachable: pressing it scrolls the row to it.
	await panel.locator('[data-tab="diag"]').click({ timeout: 5_000 });
	await expect(panel.locator('[data-tab="diag"]')).toHaveAttribute('aria-pressed', 'true');

	await ana.close();
});

test('sair da conta pergunta antes, e enter não responde por você', async ({ browser }) => {
	const ana = await Person.create(browser, 'ajsai' + test.info().repeatEachIndex);
	await open(ana);
	await ana.page.locator('[data-tab="conta"]').click();

	await ana.page.getByRole('button', { name: /sair desta conta/i }).click();
	const box = ana.page.locator('[data-confirm]');
	await expect(box).toBeVisible();
	await expect(box).toContainText('@' + ana.username);

	// Enter with the red button focused does nothing; Escape cancels.
	await box.locator('[data-confirm-yes]').focus();
	await ana.page.keyboard.press('Enter');
	await ana.page.waitForTimeout(500);
	expect(ana.page.url()).not.toMatch(/\/login/);
	await ana.page.keyboard.press('Escape');
	await expect(box).toHaveCount(0);
	// The confirmation took its own Escape: the settings under it stay.
	await expect(ana.page.locator('[data-settings]')).toBeVisible();

	await ana.page.getByRole('button', { name: /sair desta conta/i }).click();
	await box.getByRole('button', { name: 'Cancelar' }).click();
	await expect(box).toHaveCount(0);
	expect(ana.page.url()).not.toMatch(/\/login/);

	await ana.page.getByRole('button', { name: /sair desta conta/i }).click();
	await box.locator('[data-confirm-yes]').click();
	// Logout and an in-flight 401 may both navigate here. Assert the final
	// document instead of subscribing to the load event of the first navigation.
	await expect(ana.page).toHaveURL(/\/login/, { timeout: 15_000 });
	await expect(ana.page.getByRole('button', { name: 'Entrar', exact: true })).toBeVisible({ timeout: 15_000 });

	await ana.close();
});
