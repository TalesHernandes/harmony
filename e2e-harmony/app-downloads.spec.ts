import { test, expect, Person } from './fixtures';

const release = {
 version_code: 12, version_name: 'native', size: 1572864, sha256: 'a'.repeat(64),
 url: '/api/app/windows/harmony-12-native.exe',
};

test('settings offers Windows with clear name, size and the server download URL', async ({ browser }) => {
 const ana = await Person.create(browser, 'windownload');
 await ana.page.route('**/api/app/windows', route => route.fulfill({ json: release }));
 await ana.openSettings();
 await ana.page.locator('[data-tab="conta"]').click();
 const row = ana.page.locator('[data-windows-app]');
 await expect(ana.page.getByRole('heading', { name: 'Programa para Windows' })).toBeVisible();
 await expect(row).toContainText('harmony para Windows');
 await expect(row).not.toContainText('native');
 await expect(row).not.toContainText('12');
 await expect(row).toContainText('1,5 MB');
 await expect(row.getByRole('link', { name: 'Baixar' })).toHaveAttribute('href', release.url);
 await row.screenshot({ path: test.info().outputPath('windows-card.png') });
 await ana.close();
});

test('settings hides Windows when no program is published', async ({ browser }) => {
 const ana = await Person.create(browser, 'winmissing');
 await ana.page.route('**/api/app/windows', route => route.fulfill({ status: 404, json: { error: 'no_app' } }));
 await ana.openSettings();
 const response = ana.page.waitForResponse('**/api/app/windows');
 await ana.page.locator('[data-tab="conta"]').click();
 await response;
 await expect(ana.page.locator('[data-windows-app]')).toHaveCount(0);
 await ana.close();
});
