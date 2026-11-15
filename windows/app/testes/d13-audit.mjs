// D13: isolated port 8098, off-screen WinUI render, invoke providers, no audio devices.
import { start, stop, BASE } from '../../../e2e-harmony/server.mjs';
import { readFileSync, writeFileSync, existsSync, mkdirSync, readdirSync, copyFileSync } from 'node:fs';
import { spawn } from 'node:child_process';
import { resolve, join } from 'node:path';
const out = resolve(process.argv[2] || 'windows/app/test-output/d13-audit');
const auditRoot = resolve('windows/app/test-output') + '/';
if (!(out.replaceAll('\\', '/') + '/').startsWith(auditRoot.replaceAll('\\', '/'))) throw Error('D13 output must stay inside windows/app/test-output');
const photos = join(out, 'fotos');
const password = readFileSync('e2e-harmony/fixtures.ts', 'utf8').match(/export const PASSWORD = '([^']+)';/)?.[1]; // harmony:allow-secret (reads the existing e2e fixture password; stores no credential)
if (!password || BASE !== 'http://127.0.0.1:8098') throw Error('D13 requires fixture password and port 8098');
if (existsSync(out)) throw Error('D13 audit output already exists; preserve the previous evidence');
mkdirSync(out, { recursive: true }); mkdirSync(photos, { recursive: true });
const pause = ms => new Promise(r => setTimeout(r, ms));
async function call(method, path, body, cookie = '') {
  const r = await fetch(BASE + path, { method, headers: { 'Content-Type': 'application/json', Cookie: cookie }, body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(10000) });
  const text = await r.text();
  return { status: r.status, json: text ? JSON.parse(text) : {}, cookie: r.headers.get('set-cookie')?.split(';')[0] };
}
let native;
try {
  const { invites } = await start(3);
  const users = ['observador', 'bruna', 'carlos'], cookies = {};
  for (const [i, username] of users.entries()) {
    const r = await call('POST', '/api/signup', { code: invites[i], username, display_name: username, password, email: username + '@example.invalid' });
    if (r.status !== 201) throw Error('Fixture signup failed: ' + r.status);
    cookies[username] = r.cookie;
    let code;
    for (let n = 0; n < 100 && !code; ++n) {
      for (const file of readdirSync(process.env.HARMONY_E2E_MAIL_DIR)) {
        // The server writes a temporary file and renames it: it can vanish between the listing and the read.
        let text = '';
        try { text = readFileSync(join(process.env.HARMONY_E2E_MAIL_DIR, file), 'utf8'); } catch { continue; }
        if (text.includes('To: <' + username + '@example.invalid>')) code = text.match(/\b\d{6}\b/)?.[0];
      }
      if (!code) await pause(100);
    }
    if (!code || (await call('POST', '/api/me/email/verify', { code }, r.cookie)).status !== 200) throw Error('Fixture email verification failed');
  }
  const channels = (await call('GET', '/api/channels', undefined, cookies.observador)).json.channels;
  const text = channels.find(c => c.kind === 'text');
  const message = await call('POST', `/api/channels/${text.id}/messages`, { body: 'D13 fixture: mensagem para mostrar uma de cada vez.' }, cookies.bruna);
  const dm = await call('POST', '/api/dms', { username: 'bruna' }, cookies.observador);
  await call('POST', `/api/channels/${dm.json.id}/messages`, { body: 'O histórico continua disponível.' }, cookies.bruna);
  native = spawn(resolve('windows/app/out/harmony-nativo.exe'), ['--chat-audit', out], {
    env: { ...process.env, HARMONY_URL: BASE, HARMONY_TEST_USER: 'observador', HARMONY_TEST_PASSWORD: password, HARMONY_TEST_PEER: 'bruna', HARMONY_TEST_MESSAGE: String(message.json.id), HARMONY_AUDIT_PERSONAL: '1' }, // harmony:allow-secret (passes the existing local fixture variable only)
    windowsHide: true, stdio: ['ignore', 'ignore', 'pipe'],
  });
  let log = '', done = false;
  native.stderr.on('data', data => { log += data; writeFileSync(join(out, 'native.log'), log); });
  const exited = new Promise(r => native.once('exit', code => { done = true; r(code); }));
  for (let n = 0; n < 1800 && !done && !existsSync(join(out, 'external-ready')); ++n) await pause(100);
  if (existsSync(join(out, 'external-ready'))) {
    const own = (await call('GET', '/api/me/blocks', undefined, cookies.observador)).json.items;
    const other = (await call('GET', '/api/me/blocks', undefined, cookies.bruna)).json.items;
    if (!own.some(p => p.blocked && p.muted) || other.length) throw Error('Private state or native writes incorrect');
    if ((await call('POST', `/api/channels/${dm.json.id}/messages`, { body: 'must fail' }, cookies.bruna)).json.error !== 'delivery_failed') throw Error('DM generic refusal missing');
    await call('DELETE', '/api/me/blocks/bruna', undefined, cookies.observador);
    await call('DELETE', '/api/me/mutes/bruna', undefined, cookies.observador);
  }
  let timer;
  const code = await Promise.race([exited, new Promise(r => { timer = setTimeout(() => r('timeout'), 30000); })]).finally(() => clearTimeout(timer));
  if (code === 'timeout') native.kill();
  const result = existsSync(join(out, 'resultado.json')) ? JSON.parse(readFileSync(join(out, 'resultado.json'), 'utf8')) : {};
  if (log.includes(password) || log.includes('@example.invalid') || log.includes('harmony_session=')) throw Error('Private data found in native log');
  for (const file of readdirSync(out).filter(f => f.endsWith('.png') && f.startsWith('windows-'))) copyFileSync(join(out, file), join(photos, file));
  console.log(JSON.stringify({ exit: code, ...result }));
  if (code !== 0 || !result.passed) process.exitCode = 1;
} finally { if (native && native.exitCode === null) native.kill(); await stop(); }
