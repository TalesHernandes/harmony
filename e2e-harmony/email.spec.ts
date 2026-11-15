import { test, expect, Person, readMail, PASSWORD } from './fixtures';
import { createHmac } from 'node:crypto';

function totp(secret: string): string {
 const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
 let bits = '';
 for (const c of secret) bits += alphabet.indexOf(c).toString(2).padStart(5, '0');
 const key = Buffer.from(bits.match(/.{8}/g)!.map((b) => parseInt(b, 2)));
 const counter = Buffer.alloc(8);
 counter.writeBigUInt64BE(BigInt(Math.floor(Date.now() / 30_000)));
 const h = createHmac('sha1', key).update(counter).digest();
 const offset = h[h.length - 1] & 15;
 return String((h.readUInt32BE(offset) & 0x7fffffff) % 1_000_000).padStart(6, '0');
}

async function signOut(person: Person): Promise<void> {
 const page = person.page;
 const leave = page.getByRole('button', { name: 'Sair desta conta', exact: true });
 if (!(await leave.isVisible())) {
  await person.openSettings();
  await page.locator('[data-tab="conta"]').click();
 }
 await leave.click();
 await page.locator('[data-confirm-yes]').click();
 await expect(page.locator('#u')).toBeVisible();
}

test('confirma e-mail e recupera a senha pelo link recebido', async ({ browser }) => {
 const person = await Person.create(browser, 'email-flow', { verifyEmail: false });
 const page = person.page;
 const verification = await readMail('email-flow@example.org', 'confirmar seu');
 const code = verification.match(/\b\d{6}\b/)?.[0];
 expect(code).toBeTruthy();
 await page.getByLabel('Código', { exact: true }).fill(code!);
 await page.getByRole('button', { name: 'Confirmar e-mail', exact: true }).click();
 await expect(page.getByRole('button', { name: 'pessoas', exact: true })).toBeVisible();
 const me = await page.request.get('/api/me');
 expect((await me.json()).user.email_verified).toBe(true);
 await signOut(person);
 await page.getByRole('button', { name: 'Esqueci a senha' }).click();
 await page.getByLabel('Nome ou e-mail').fill('email-flow@example.org');
 await page.getByRole('button', { name: 'Enviar link' }).click();
 await expect(page.getByText('Se a conta tem e-mail confirmado', { exact: false })).toBeVisible();
 const reset = await readMail('email-flow@example.org', '#reset=');
 const link = reset.match(/http[^\s]+\/#reset=[A-Za-z0-9_-]+/)?.[0];
 expect(link).toBeTruthy();
 await page.goto(link!);
 await expect(page.getByRole('heading', { name: 'Nova senha' })).toBeVisible();
 expect(page.url()).not.toContain('#reset=');
 await page.getByLabel('Nova senha', { exact: true }).fill('nova-senha-email-456');
 await page.getByRole('button', { name: 'Salvar nova senha' }).click();
 await page.getByRole('link', { name: 'Entrar', exact: true }).click();
 await page.locator('#u').fill('email-flow@example.org');
 await page.locator('#p').fill('nova-senha-email-456');
 await page.getByRole('button', { name: 'Entrar', exact: true }).click();
 await expect(page.getByRole('button', { name: 'pessoas', exact: true })).toBeVisible();
 await person.close();
});

test('liga TOTP nos ajustes e entra com um código de recuperação', async ({ browser }) => {
 const person = await Person.create(browser, 'email-totp');
 const page = person.page;
 await person.openSettings();
 await page.locator('[data-tab="conta"]').click();
 const security = page.locator('section[aria-label="Conta e segurança"]');
 await security.getByRole('button', { name: 'Ligar', exact: true }).click();
 await security.getByLabel('Senha atual', { exact: true }).fill(PASSWORD);
 const setupReply = page.waitForResponse((r) => r.url().endsWith('/api/me/2fa/totp/setup'));
 await security.getByRole('button', { name: 'Por app autenticador (recomendado)' }).click();
 const setup = await (await setupReply).json();
 await expect(security.getByRole('img', { name: 'QR code para o app autenticador' })).toBeVisible();
 await expect(security.getByRole('link', { name: 'Abrir no app autenticador' })).toHaveAttribute('href', /^otpauth:\/\/totp\/harmony:/);
 await security.getByLabel('Código que o app mostra', { exact: true }).fill(totp(setup.secret));
 await security.getByRole('button', { name: 'Confirmar e ligar' }).click();
 await expect(security.getByText('Por app autenticador', { exact: true })).toBeVisible();
 const codes = (await security.locator('pre').innerText()).trim().split('\n');
 expect(codes).toHaveLength(10);
 // Copied by hand from paper: never a pair people misread (store/security.go).
 for (const c of codes) expect(c).toMatch(/^[ACDEFGHJKMNPQRTUVWXYZ234679]{4}(-[ACDEFGHJKMNPQRTUVWXYZ234679]{4}){3}$/);
 await signOut(person);
 await page.locator('#u').fill('email-totp@example.org');
 await page.locator('#p').fill(PASSWORD);
 await page.getByRole('button', { name: 'Entrar', exact: true }).click();
 await page.getByRole('button', { name: 'Usar um código de recuperação' }).click();
 // Typed the way a person types it from paper: lower case, spaces for dashes.
 await page.getByLabel('Código de recuperação', { exact: true }).fill(codes[0].toLowerCase().replaceAll('-', ' '));
 await page.getByRole('button', { name: 'Entrar', exact: true }).click();
 await expect(page.getByRole('button', { name: 'pessoas', exact: true })).toBeVisible();
 await page.request.post('/api/logout');
 const login = await page.request.post('/api/login', { data: { username: 'email-totp', password: PASSWORD } });
 const challenge = (await login.json()).challenge;
 const reused = await page.request.post('/api/login/2fa', { data: { challenge, code: codes[0] } });
 expect(reused.status()).toBe(400);
 await person.close();
});
