// The D12e audit, end to end: the D12c criterion repeated in the Windows
// program. A throwaway harmony on 8099 with the owner (observador), bruna and
// carlos. First run: the program signs in as the owner and, through the
// Cargos tab, makes "Moderador" green with Gerenciar mensagens, shows it apart,
// gives it to bruna, drags a second role above it and deletes a third. Second
// run: the program signs in as bruna, her name is green, she deletes one of
// carlos's messages; the owner takes the role away mid-run and the button and
// the colour go. She never sees Convites or Cargos. Nothing here plays sound.
// Usage: node windows/app/testes/roles-audit.mjs [dir]   (HARMONY_E2E_PORT=8099)
import { start, stop, BASE } from '../../../e2e-harmony/server.mjs';
import { readFileSync, writeFileSync, existsSync, mkdirSync, rmSync, readdirSync } from 'node:fs';
import { spawn } from 'node:child_process';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const here = dirname(dirname(fileURLToPath(import.meta.url)));
const output = resolve(process.argv[2] || join(here, 'test-output/roles-audit'));
const password = readFileSync(resolve(here, '../../e2e-harmony/fixtures.ts'), 'utf8').match(/export const PASSWORD = '([^']+)';/)?.[1]; // harmony:allow-secret (reads the e2e fixture password, holds none)
if (!password) throw new Error('fixture password não encontrado');
async function call(method, path, body, cookie = '') {
  const r = await fetch(BASE + path, { method, headers: { 'Content-Type': 'application/json', Cookie: cookie }, body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(10000) });
  if (!r.ok) throw new Error(`${method} ${path}: ${r.status}`);
  const text = await r.text();
  return { json: text ? JSON.parse(text) : {}, cookie: r.headers.get('set-cookie')?.split(';')[0] };
}
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
async function run(dir, user, mode, during) {
  mkdirSync(dir, { recursive: true });
  const native = spawn(resolve(here, 'out/harmony-nativo.exe'), ['--chat-audit', dir], {
    env: { ...process.env, HARMONY_URL: BASE, HARMONY_TEST_USER: user, HARMONY_TEST_PASSWORD: password, HARMONY_AUDIT_ROLES: mode }, // harmony:allow-secret (passes local fixtures only)
    stdio: ['ignore', 'ignore', 'pipe'],
  });
  let log = ''; native.stderr.on('data', (c) => { log += c; writeFileSync(join(dir, 'nativo.log'), log); });
  let done = false; const exited = new Promise((r) => native.once('exit', (c) => { done = true; r(c); }));
  if (during) during(() => done).catch((e) => { console.error('during:', e.message); native.kill(); });
  let timer;
  const code = await Promise.race([exited, new Promise((r) => { timer = setTimeout(() => r('timeout'), 180000); })]).finally(() => clearTimeout(timer));
  if (code === 'timeout') native.kill();
  const result = existsSync(join(dir, 'resultado.json')) ? readFileSync(join(dir, 'resultado.json'), 'utf8') : 'sem resultado';
  console.log(mode, 'exit', code, result);
  let passed = false; try { passed = JSON.parse(result).passed === true; } catch {}
  if (log.includes(password) || log.includes('@example.invalid') || log.includes('harmony_session=')) throw new Error('native log holds private account data');
  return passed && code === 0;
}
try {
  rmSync(output, { recursive: true, force: true }); mkdirSync(output, { recursive: true });
  const { invites } = await start(3);
  const suffix = Date.now().toString(36).slice(-4);
  const me = 'observador' + suffix, people = {};
  for (const [i, who] of [me, 'bruna', 'carlos'].entries()) {
    people[who] = (await call('POST', '/api/signup', { code: invites[i], username: who, display_name: who === me ? 'observador' : who, password, email: who + suffix + '@example.invalid' })).cookie;
  }
  const channels = (await call('GET', '/api/channels', undefined, people.bruna)).json.channels;
  const geral = channels.find((c) => c.kind === 'text') ?? channels[0];
  for (const body of ['bom dia', 'alguém joga hoje?', 'manda o print', 'ok']) await call('POST', `/api/channels/${geral.id}/messages`, { body }, people.carlos);
  await call('POST', `/api/channels/${geral.id}/messages`, { body: 'eu vou' }, people.bruna);

  let ok = await run(join(output, 'dono'), me, 'dono');
  const roles = (await call('GET', '/api/roles', undefined, people[me])).json.roles;
  const mod = roles.find((r) => r.name === 'Moderador');
  const check = (cond, what) => { if (!cond) { console.error('FAIL:', what); ok = false; } else console.log('PASS:', what); };
  check(mod && mod.color === '#2ecc71' && mod.hoist && mod.permissions.join() === 'manage_messages' && mod.members.join() === 'bruna',
    'the owner made Moderador green, shown apart, Gerenciar mensagens, given to bruna, through the program');
  check(roles.length === 2 && roles[0].name === 'Ajudante' && !roles.some((r) => r.name === 'Temporário'), 'the drag put Ajudante on top, and Temporário was deleted');

  // bruna: the role is taken away when the program says it is ready.
  const amigo = join(output, 'amigo');
  ok = (await run(amigo, 'bruna', 'amigo', async (done) => {
    while (!done() && !existsSync(join(amigo, 'amigo-pronto'))) await wait(100);
    if (done()) return;
    await call('DELETE', `/api/roles/${encodeURIComponent(mod.id)}/members/bruna`, undefined, people[me]);
    console.log('the owner took Moderador from bruna');
  })) && ok;
  // The server agrees: without the role bruna's delete is refused.
  const page = (await call('GET', `/api/channels/${geral.id}/messages`, undefined, people.carlos)).json.messages;
  const left = page.filter((m) => m.author.username === 'carlos' && !m.deleted);
  const gone = page.filter((m) => m.author.username === 'carlos' && m.deleted);
  check(gone.length === 1, 'bruna deleted one of carlos\'s messages with the role');
  const refused = await fetch(BASE + '/api/messages/' + left[0].id, { method: 'DELETE', headers: { Cookie: people.bruna } });
  check(refused.status === 403 || refused.status === 404, 'without the role the server refuses her delete (' + refused.status + ')');
  const invite = await fetch(BASE + '/api/invites', { method: 'POST', headers: { Cookie: people.bruna, 'Content-Type': 'application/json' }, body: '{}' });
  check(invite.status === 403, 'bruna never makes an invite');
  if (!ok) process.exitCode = 1;
} catch (e) {
  console.error('roles-audit falhou:', e.message); process.exitCode = 1;
} finally {
  stop();
}
