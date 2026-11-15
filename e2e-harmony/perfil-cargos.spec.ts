import { mkdirSync } from 'node:fs';
import { test, expect, Person } from './fixtures';
import type { Page } from '@playwright/test';

async function call(page: Page, method: string, path: string, body?: unknown) {
    return page.evaluate(async ([method, path, body]) => {
        const res = await fetch(path, { method, headers: { 'Content-Type': 'application/json' },
            body: body === undefined ? undefined : JSON.stringify(body) });
        if (!res.ok) throw new Error('fixture request: ' + res.status);
        return res.status === 204 ? null : res.json();
    }, [method, path, body] as const);
}

async function profile(viewer: Person, username: string) {
    if (!(await viewer.page.locator('aside').isVisible())) await viewer.page.getByRole('button', { name: 'pessoas', exact: true }).first().click();
    await viewer.page.locator(`[data-person="${username}"]`).click();
    await expect(viewer.page.getByRole('dialog')).toBeVisible();
}

test('profile grants and removes roles live; ordinary members only see chips', async ({ browser }) => {
    const n = Date.now() % 100000;
    const owner = await Person.signIn(browser, 'dono');
    const target = await Person.create(browser, 'profile' + n);
    const observer = await Person.create(browser, 'observe' + n);
    const photos = process.env.D12E_PHOTOS;
    if (photos) mkdirSync(photos, { recursive: true });
    const photo = async (page: Page, name: string) => { if (photos) await page.screenshot({ path: `${photos}/web-${name}.png` }); };
    const role = await call(owner.page, 'POST', '/api/roles', { name: 'Moderador ' + n, color: '#2ecc71', hoist: false, permissions: ['manage_messages'] });
    const other = await call(owner.page, 'POST', '/api/roles', { name: 'Amigo ' + n, color: '#3498db', hoist: false, permissions: [] });
    try {
        await profile(owner, target.username);
        await owner.page.getByRole('button', { name: '+ Dar cargo', exact: true }).click();
        await expect(owner.page.locator(`[data-profile-role-give="${role.name}"]`)).toBeVisible();
        await photo(owner.page, 'lista-cargos');
        await owner.page.locator(`[data-profile-role-give="${role.name}"]`).click();
        const chip = owner.page.locator(`[data-profile-role="${role.name}"]`);
        await expect(chip).toBeVisible();
        await expect(chip.getByTitle('Tirar o cargo', { exact: true })).toBeVisible();
        await expect(owner.page.getByRole('button', { name: '+ Dar cargo', exact: true })).toBeVisible();
        await photo(owner.page, 'perfil-dono');
        await profile(observer, target.username);
        await expect(observer.page.locator(`[data-profile-role="${role.name}"]`)).toBeVisible();
        await expect(observer.page.getByTitle('Tirar o cargo', { exact: true })).toHaveCount(0);
        await expect(observer.page.getByRole('button', { name: '+ Dar cargo', exact: true })).toHaveCount(0);
        await photo(observer.page, 'perfil-sem-permissao');
        await observer.page.keyboard.press('Escape');
        const name = observer.page.locator(`[data-person="${target.username}"] [data-name-color]`);
        await expect(name).toHaveAttribute('data-name-color', '#2ecc71');
        await expect(name).toHaveClass(/role-2ecc71/);
        await photo(observer.page, 'nome-verde');
        await chip.getByTitle('Tirar o cargo', { exact: true }).click();
        await expect(chip).toHaveCount(0);
        await expect(name).toHaveCount(0);
        await photo(owner.page, 'cargo-retirado');
    } finally {
        await call(owner.page, 'DELETE', `/api/roles/${role.id}`);
        await call(owner.page, 'DELETE', `/api/roles/${other.id}`);
        await observer.close(); await target.close(); await owner.close();
    }
});


test('profile moderator respects hierarchy, missing permissions and owner protection', async ({ browser }) => {
    const n = Date.now() % 100000;
    const owner = await Person.signIn(browser, 'dono');
    const manager = await Person.create(browser, 'manager' + n);
    const target = await Person.create(browser, 'limit' + n);
    const roles: any[] = [];
    async function create(name: string, permissions: string[]) {
        const r = await call(owner.page, 'POST', '/api/roles', { name: name + n, color: '', hoist: false, permissions });
        roles.push(r); return r;
    }
    try {
        const top = await create('Manager', ['manage_roles']);
        const forbidden = await create('Channels', ['manage_channels']);
        const allowed = await create('Friend', []);
        // New roles are appended below the existing ones.
        await call(owner.page, 'PUT', `/api/roles/${top.id}/members/${manager.username}`);
        await call(owner.page, 'PUT', `/api/roles/${forbidden.id}/members/${target.username}`);
        await profile(manager, target.username);
        await expect(manager.page.locator(`[data-profile-role="${forbidden.name}"]`).getByTitle('Tirar o cargo', { exact: true })).toBeVisible();
        await manager.page.getByRole('button', { name: '+ Dar cargo', exact: true }).click();
        await expect(manager.page.locator(`[data-profile-role-give="${allowed.name}"]`)).toBeVisible();
        await expect(manager.page.locator(`[data-profile-role-give="${top.name}"]`)).toHaveCount(0);
        await manager.page.keyboard.press('Escape');
        await manager.page.locator(`[data-profile-role="${forbidden.name}"]`).getByTitle('Tirar o cargo', { exact: true }).click();
        await expect(manager.page.locator(`[data-profile-role="${forbidden.name}"]`)).toHaveCount(0);
        await manager.page.getByRole('button', { name: '+ Dar cargo', exact: true }).click();
        await expect(manager.page.locator(`[data-profile-role-give="${forbidden.name}"]`)).toHaveCount(0);
        await manager.page.keyboard.press('Escape');
        await manager.page.keyboard.press('Escape');
        await call(owner.page, 'PUT', `/api/roles/${allowed.id}/members/dono`);
        await profile(manager, 'dono');
        await expect(manager.page.locator('[data-profile-roles]')).toContainText('Dono');
        await expect(manager.page.getByTitle('Tirar o cargo', { exact: true })).toHaveCount(0);
        await expect(manager.page.getByRole('button', { name: '+ Dar cargo', exact: true })).toHaveCount(0);
        await manager.page.keyboard.press('Escape');
        await profile(manager, manager.username);
        await expect(manager.page.locator(`[data-profile-role="${top.name}"]`).getByTitle('Tirar o cargo', { exact: true })).toHaveCount(0);
    } finally {
        for (const r of roles.reverse()) await call(owner.page, 'DELETE', `/api/roles/${r.id}`);
        await target.close(); await manager.close(); await owner.close();
    }
});
