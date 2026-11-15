// The W5 chat audit, end to end: a throwaway harmony on 8099 with three people
// (the one the program signs in as, bruna and carlos), a channel with history
// and emoji, a DM, and then `harmony-nativo.exe --chat-audit <dir>`, which
// signs in, walks the chat and saves its pictures and measures in <dir>.
// Usage: node windows/app/testes/chat-audit.mjs [dir]   (run through chat-audit.cmd)
import { start, stop, BASE } from '../../../e2e-harmony/server.mjs';
import { readFileSync, writeFileSync, existsSync, mkdirSync, rmSync, readdirSync } from 'node:fs';
import { spawn } from 'node:child_process';
import { deflateSync } from 'node:zlib';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const here = dirname(dirname(fileURLToPath(import.meta.url)));
const output = resolve(process.argv[2] || join(here, 'test-output/chat-audit'));
const password = readFileSync(resolve(here, '../../e2e-harmony/fixtures.ts'), 'utf8').match(/export const PASSWORD = '([^']+)';/)?.[1]; // harmony:allow-secret (reads the e2e fixture password, holds none)
if (!password) throw new Error('fixture password não encontrado');
async function call(method, path, body, cookie = '') {
  const r = await fetch(BASE + path, { method, headers: { 'Content-Type': 'application/json', Cookie: cookie }, body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(10000) });
  if (!r.ok) throw new Error(`${method} ${path}: ${r.status}`);
  const text = await r.text();
  return { json: text ? JSON.parse(text) : {}, cookie: r.headers.get('set-cookie')?.split(';')[0] };
}
// bruna sits in the voice room on the web with Chromium's fake microphone
// (a beep every second), so the program's voice stage has someone speaking.
async function speaker(cookie) {
  const { chromium } = await import('playwright');
  const browser = await chromium.launch({ args: ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--mute-audio', '--autoplay-policy=no-user-gesture-required', '--allow-loopback-in-peer-connection'] });
  const context = await browser.newContext({ permissions: ['microphone'] });
  const [name, value] = cookie.split('=');
  await context.addCookies([{ name, value, url: BASE }]);
  const page = await context.newPage(); await page.goto(BASE);
  await page.getByRole('button', { name: 'Agora não' }).click({ timeout: 5000 }).catch(() => {});
  await page.locator('button[title*="entrar na sala" i]:visible').first().click({ timeout: 15000 });
  await page.locator('[data-call="live"]').waitFor({ timeout: 30000 });
  return browser;
}
// A real 1600x900 fixture: metadata alone cannot change a square source's
// aspect ratio in WinUI's Image. Match source pixels to the sizing fixture.
function landscapeFixture(path) {
  const width = 1600, height = 900, pixels = Buffer.alloc((width * 4 + 1) * height);
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    const at = y * (width * 4 + 1) + 1 + x * 4;
    pixels[at] = x < 800 ? 4 : 23; pixels[at + 1] = y < 450 ? 133 : 90; pixels[at + 2] = 200; pixels[at + 3] = 255;
  }
  const chunk = (type, data) => {
    const payload = Buffer.concat([Buffer.from(type), data]); let crc = 0xffffffff;
    for (const byte of payload) { crc ^= byte; for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0); }
    const result = Buffer.alloc(data.length + 12); result.writeUInt32BE(data.length); payload.copy(result,4); result.writeUInt32BE((crc ^ 0xffffffff) >>> 0,result.length - 4); return result;
  };
  const header = Buffer.alloc(13); header.writeUInt32BE(width); header.writeUInt32BE(height,4); header[8] = 8; header[9] = 6;
  writeFileSync(path,Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]),chunk('IHDR',header),chunk('IDAT',deflateSync(pixels)),chunk('IEND',Buffer.alloc(0))]));
}
let native, code = 1, voice;
try {
  rmSync(output, { recursive: true, force: true }); mkdirSync(output, { recursive: true });
  const picture = join(output, 'imagem-paisagem-sintetica.png'); landscapeFixture(picture);
  const { invites } = await start(5);
  const suffix = Date.now().toString(36).slice(-4);
  const me = 'observador' + suffix, people = {};
  for (const [i, who] of [me, 'bruna', 'carlos'].entries()) {
    people[who] = (await call('POST', '/api/signup', { code: invites[i], username: who, display_name: who === me ? 'observador' : who, password, email: who + suffix + '@example.invalid' })).cookie;
  }
  // More channels than one, so a pointer has rows to pass over (the first account is the admin).
  for (const name of ['jogos', 'filmes', 'musica', 'memes', 'avisos', 'prints']) await call('POST', '/api/channels', { name, topic: '', kind: 'text' }, people[me]);
  const channels = (await call('GET', '/api/channels', undefined, people.bruna)).json.channels;
  const geral = channels.find((c) => c.kind !== 'dm' && c.kind !== 'voice') ?? channels[0];
  const lines = ['bom dia 😀', 'alguém vai jogar hoje? 🎮🔥', 'eu vou, depois das 21h', 'show 👍', 'manda o print', 'olha isso 😂😂', 'parabéns!! 🎉🎂', 'ok'];
  for (let i = 0; i < 24; i++, await new Promise((r) => setTimeout(r, 350))) await call('POST', `/api/channels/${geral.id}/messages`, { body: lines[i % lines.length] }, i % 3 ? people.bruna : people.carlos);
  if (process.env.HARMONY_AUDIT_COLUMN === '1') {
    // D12b: a folder holding #jogos and the voice room, and one pinned line.
    const folder = (await call('POST', '/api/categories', { name: 'Jogos' }, people[me])).json;
    const all = (await call('GET', '/api/channels', undefined, people[me])).json.channels.filter((c) => c.kind === 'text' || c.kind === 'voice');
    await call('PUT', '/api/layout', { categories: [folder.id], channels: all.map((c) => ({ id: c.id, category_id: c.name === 'jogos' || c.kind === 'voice' ? folder.id : '' })) }, people[me]);
    const page = (await call('GET', `/api/channels/${geral.id}/messages`, undefined, people[me])).json.messages;
    await call('POST', `/api/messages/${page[page.length - 3].id}/pin`, undefined, people[me]);
    // Opening lands: #memes never read lands on its end, #avisos read up to
    // its tenth line lands on the eleventh. Rows of mixed height, so the
    // list has to measure before the end is the end.
    const byName = Object.fromEntries(all.map((c) => [c.name, c.id]));
    const long = 'uma linha comprida que quebra em várias quando a coluna é estreita, como acontece quando alguém conta uma história inteira de uma vez só';
    // The server takes 20 lines a minute from one person: a third writer, and
    // a wait when the minute is full.
    const dani = (await call('POST', '/api/signup', { code: invites[4], username: 'dani', display_name: 'dani', password, email: 'dani' + suffix + '@example.invalid' })).cookie;
    const writers = [dani, people.carlos, people.bruna];
    const post = async (channel, body, who) => {
      for (;;) try { return (await call('POST', `/api/channels/${channel}/messages`, { body }, who)).json; }
      catch (e) { if (!String(e).includes(': 429')) throw e; await new Promise((r) => setTimeout(r, 3000)); }
    };
    for (const name of ['memes', 'avisos']) for (let i = 0; i < 30; i++) {
      const body = `${name} ${i + 1}: ` + (i % 3 === 0 ? long + '\n' + long : lines[i % lines.length]);
      const sent = await post(byName[name], body, writers[i % 3]);
      if (name === 'avisos' && i === 9) await call('POST', `/api/channels/${byName.avisos}/read`, { last_read_id: sent.id ?? sent.message?.id }, people[me]);
    }
    // Pictures at the end of #memes: each arrives after its row is laid out
    // and grows it, the case that left the list short of its end.
    for (let i = 0; i < 3; i++) {
      const form = new FormData(); form.append('file', new Blob([readFileSync(picture)], { type: 'image/png' }), 'paisagem.png');
      const r = await fetch(BASE + `/api/channels/${byName.memes}/uploads`, { method: 'POST', headers: { Cookie: dani }, body: form });
      if (!r.ok) throw new Error(`upload: ${r.status}`);
      const att = await r.json();
      await call('POST', `/api/channels/${byName.memes}/messages`, { body: `print ${i + 1}`, attachments: [att.id] }, dani);
      await post(byName.memes, `e aí ${i + 1}`, writers[1 + (i % 2)]);
    }
  }
  const dm = (await call('POST', '/api/dms', { username: me }, people.bruna)).json;
  const dmID = dm.id ?? dm.channel?.id;
  if (dmID) for (const body of ['oi, tudo bem? 🙂', 'me chama quando entrar']) await call('POST', `/api/channels/${dmID}/messages`, { body }, people.bruna);
  // The column run stops before the voice stage; no one speaks in it.
  if (process.env.HARMONY_AUDIT_COLUMN !== '1') voice = await speaker(people.bruna);
  const extraSessions = [];
  for (let i = 0; i < 3; i++) extraSessions.push((await call('POST', '/api/login', { username: me, password })).cookie);
  const own = (await call('GET', '/api/sessions', undefined, people[me])).json.sessions;
  const other = (await call('GET', '/api/sessions', undefined, people.bruna)).json.sessions;
  if (other.some((s) => own.some((mine) => mine.id === s.id))) throw new Error('session list crossed account boundary');
  const foreign = await fetch(BASE + '/api/sessions/' + encodeURIComponent(own[0].id), { method: 'DELETE', headers: { Cookie: people.bruna } });
  if (foreign.status !== 404) throw new Error('another account could revoke a foreign session');
  console.log('PASS: user B cannot list or revoke sessions of A');
  let emailCode='';
  for(let i=0;i<100 && !emailCode;i++) {
    const mail=readdirSync(process.env.HARMONY_E2E_MAIL_DIR).filter((f)=>f.endsWith('.eml')).map((f)=>readFileSync(join(process.env.HARMONY_E2E_MAIL_DIR,f),'utf8'))
      .find((s)=>s.includes('To: <'+me+suffix+'@example.invalid>') && s.includes('confirmar seu'));
    emailCode=mail?.match(/\b\d{6}\b/)?.[0] || '';
    if(!emailCode) await new Promise((r)=>setTimeout(r,100));
  }
  if(!emailCode) throw new Error('local verification fixture missing');
  native = spawn(resolve(here, 'out/harmony-nativo.exe'), ['--chat-audit', output], {
    env: { ...process.env, HARMONY_TEST_REQUIRE_CABLE: '1', HARMONY_URL: BASE, HARMONY_TEST_USER: me, HARMONY_TEST_PASSWORD: password, HARMONY_TEST_EMAIL_CODE: emailCode, HARMONY_TEST_INVITE: BASE+'/signup?code='+invites[3], HARMONY_TEST_SIGNUP_USER: 'cadastro'+suffix, HARMONY_TEST_PICTURE: resolve(here, '../../web/harmony/public/icon-192.png'), HARMONY_TEST_LANDSCAPE: picture }, // harmony:allow-secret (passes local fixtures only)
    stdio: ['ignore', 'ignore', 'pipe'],
  });
  let log = ''; native.stderr.on('data', (c) => { log += c; writeFileSync(join(output, 'nativo.log'), log); });
  let nativeTimeout;
  code = await Promise.race([new Promise((r) => native.once('exit', r)), new Promise((r) => { nativeTimeout = setTimeout(() => r('timeout'), 240000); })])
    .finally(() => clearTimeout(nativeTimeout));
  const result = existsSync(join(output, 'resultado.json')) ? readFileSync(join(output, 'resultado.json'), 'utf8') : 'sem resultado';
  console.log('exit', code, result);
  let passed = false;
  try { passed = JSON.parse(result).passed === true; } catch {}
  if (code !== 0 || !passed) process.exitCode = 1;
  // The column run (HARMONY_AUDIT_COLUMN) stops before the account stages these check.
  if (passed && process.env.HARMONY_AUDIT_COLUMN !== '1') {
    for (const secret of [password,emailCode,invites[3],'@example.invalid','harmony_session=']) {
      if (log.includes(secret)) throw new Error('native program log contains private account data');
    }
    for (const cookie of [...extraSessions, people[me]]) {
      const denied = await fetch(BASE + '/api/me', { headers: { Cookie: cookie } });
      if (denied.status !== 401) throw new Error('native account flow left another session active');
    }
    const old = await fetch(BASE + '/api/login', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ username: me, password }) });
    if (old.status !== 401) throw new Error('old fixture password still accepted');
    await call('POST', '/api/login', { username: me, password: password + '-changed' });
    console.log('PASS: native password change, old password refused, other sessions revoked');
    if (process.env.HARMONY_PARITY5 === '1') {
      const parity = JSON.parse(readFileSync(join(output, 'round5-result.json'), 'utf8'));
      if (parity.passed !== true) {
        console.error('FAIL: round5 visible cursor on direct entry; see round5-result.json');
        process.exitCode = 1;
      }
    }
  }
} catch (e) {
  console.error('chat-audit falhou:', e.message); process.exitCode = 1;
} finally {
  native?.kill(); await voice?.close().catch(() => {}); stop();
}
