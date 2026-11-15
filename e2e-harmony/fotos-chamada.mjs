// Photos of the two calls side by side, for looking at them: the room and the
// direct call, on a monitor (Ana) and on a telephone-sized window (Bruna).
// Usage: node e2e-harmony/fotos-chamada.mjs [dir]
// Needs PLAYWRIGHT_BROWSERS_PATH (CLAUDE.md) and the web built (build:harmony).
import { start, stop, BASE } from './server.mjs';
import { readFileSync, mkdirSync } from 'node:fs';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const here = dirname(fileURLToPath(import.meta.url));
const out = resolve(process.argv[2] || join(here, '../test-results/fotos-chamada'));
const password = readFileSync(join(here, 'fixtures.ts'), 'utf8').match(/export const PASSWORD = '([^']+)';/)[1]; // harmony:allow-secret (reads the e2e fixture password, holds none)

async function call(method, path, body, cookie = '') {
	const r = await fetch(BASE + path, { method, headers: { 'Content-Type': 'application/json', Cookie: cookie }, body: body === undefined ? undefined : JSON.stringify(body) });
	if (!r.ok) throw new Error(`${method} ${path}: ${r.status}`);
	const text = await r.text();
	return { json: text ? JSON.parse(text) : {}, cookie: r.headers.get('set-cookie')?.split(';')[0] };
}
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
let browser, pages = [];
try {
	mkdirSync(out, { recursive: true });
	const { invites } = await start(2);
	const s = Date.now().toString(36).slice(-4);
	const ana = 'ana' + s, bruna = 'bruna' + s, cookies = {};
	for (const [i, who] of [ana, bruna].entries())
		cookies[who] = (await call('POST', '/api/signup', { code: invites[i], username: who, display_name: who.slice(0, -4), password, email: who + '@example.invalid' })).cookie;

	const { chromium } = await import('playwright');
	browser = await chromium.launch({ args: ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--mute-audio', '--autoplay-policy=no-user-gesture-required', '--allow-loopback-in-peer-connection', '--auto-select-desktop-capture-source=Entire screen'] });
	const open = async (who, phone) => {
		const context = await browser.newContext({
			permissions: ['microphone', 'camera'],
			...(phone ? { viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true } : { viewport: { width: 1440, height: 900 } }),
		});
		const [name, value] = cookies[who].split('=');
		await context.addCookies([{ name, value, url: BASE }]);
		const page = await context.newPage();
		await page.goto(BASE);
		await page.getByRole('button', { name: 'Agora não' }).click({ timeout: 4000 }).catch(() => {});
		return page;
	};
	const pc = await open(ana, false), phone = await open(bruna, true); pages = [pc, phone];
	const shot = async (page, name) => { await wait(1500); await page.screenshot({ path: join(out, name + '.png') }); console.log('foto', name); };

	// The room.
	for (const page of [pc, phone]) {
		const menu = page.locator('button[aria-label="canais"]');
		if (await menu.isVisible().catch(() => false)) await menu.click();
		await page.locator('button[title*="entrar na sala" i]:visible').first().click();
		await page.locator('[data-call="live"]').waitFor({ timeout: 30000 });
	}
	await shot(pc, 'sala-pc');
	await shot(phone, 'sala-celular');
	for (const page of [pc, phone]) await page.locator('[data-call-controls] button[title*="Sair" i], [data-call-controls] [data-leave]').first().click().catch(() => {});
	await wait(1500);

	// The direct call: Ana calls Bruna from the person's row.
	await pc.reload(); await pc.getByRole('button', { name: 'Agora não' }).click({ timeout: 4000 }).catch(() => {});
	await pc.locator(`aside button[data-person="${bruna}"]`).first().click({ timeout: 15000 });
	await pc.getByRole('dialog').getByRole('button', { name: /conversar no privado/i }).click();
	await pc.locator('[data-call-start]').click();
	await phone.locator('[data-incoming-call]').waitFor({ timeout: 20000 });
	await shot(phone, 'tocando-celular');
	await phone.locator('[data-call-accept]').click();
	await pc.locator('[data-call-screen]').waitFor({ timeout: 20000 });
	await wait(3000);
	await shot(pc, 'direta-pc');
	await shot(phone, 'direta-celular');
	await pc.locator('[data-call-cam]').click().catch(() => {});
	await wait(3000);
	await shot(pc, 'direta-pc-camera');
	await shot(phone, 'direta-celular-camera');
	await phone.locator('[data-call-deafen]').click();
	await shot(phone, 'direta-celular-surdo');
	await shot(pc, 'direta-pc-outro-surdo');
	await phone.locator('[data-call-deafen]').click();
	await pc.locator('[data-call-share]').click();
	await wait(3000);
	await shot(pc, 'direta-pc-tela');
	await shot(phone, 'direta-celular-tela');
} catch (e) {
	console.error('fotos falharam:', e.message); process.exitCode = 1;
	for (const [i, p] of pages.entries()) await p.screenshot({ path: join(out, 'erro-' + i + '.png') }).catch(() => {});
} finally {
	await browser?.close().catch(() => {}); stop();
}
