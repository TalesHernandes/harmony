// The W6 round, end to end: a throwaway harmony with two people. The program
// (`harmony-nativo.exe --call-audit <dir>`) signs in as one and goes to the
// tray; the web, in Chromium with a fake microphone, is the other. The web
// writes a mention, a DM, a mention in a silenced channel and a DM with the
// notices off, then calls the program three times (it answers, it calls back,
// it refuses). Each side waits for the other's step; the program writes
// resultado.json, and this script adds what only the web can see: that the
// voice went both ways, and the mute.
// Usage: node windows/app/testes/call-audit.mjs [dir]   (run through call-audit.cmd)
import { start, stop, BASE } from '../../../e2e-harmony/server.mjs';
import { readFileSync, writeFileSync, existsSync, mkdirSync, rmSync } from 'node:fs';
import { spawn } from 'node:child_process';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const here = dirname(dirname(fileURLToPath(import.meta.url)));
const output = resolve(process.argv[2] || join(here, 'test-output/call-audit'));
const password = readFileSync(resolve(here, '../../e2e-harmony/fixtures.ts'), 'utf8').match(/export const PASSWORD = '([^']+)';/)?.[1];
if (!password) throw new Error('fixture password não encontrado');
async function call(method, path, body, cookie = '') {
  const r = await fetch(BASE + path, { method, headers: { 'Content-Type': 'application/json', Cookie: cookie }, body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(10000) });
  if (!r.ok) throw new Error(`${method} ${path}: ${r.status}`);
  const text = await r.text();
  return { json: text ? JSON.parse(text) : {}, cookie: r.headers.get('set-cookie')?.split(';')[0] };
}
const checks = [];
function check(name, ok, detail = '') {
  checks.push({ name, ok: !!ok, detail });
  console.log(ok ? 'ok  ' : 'FAIL', name, detail);
}
// What the web's connection has received and what the other end reports it
// received from the web (RTCP), audio only.
async function audio(page) {
  return page.evaluate(async () => {
    const pc = window.__harmonyDirectPC;
    if (!pc) return { state: 'none', inbound: 0, remote: 0 };
    let inbound = 0, remote = 0, level = 0;
    (await pc.getStats()).forEach((r) => {
      if (r.kind !== 'audio') return;
      if (r.type === 'inbound-rtp') inbound += r.bytesReceived ?? 0;
      if (r.type === 'outbound-rtp') remote += r.bytesSent ?? 0;
      if (r.type === 'inbound-rtp') level = Math.max(level, r.audioLevel ?? 0);
    });
    return { state: pc.connectionState, inbound, remote, level };
  });
}
// Video the web receives (the program's screen) and sends (its fake camera).
async function video(page) {
  return page.evaluate(async () => {
    const pc = window.__harmonyDirectPC;
    let inbound = 0, frames = 0, sent = 0;
    if (pc) (await pc.getStats()).forEach((r) => {
      if (r.kind !== 'video') return;
      if (r.type === 'inbound-rtp') { inbound += r.bytesReceived ?? 0; frames += r.framesDecoded ?? 0; }
      if (r.type === 'outbound-rtp') sent += r.bytesSent ?? 0;
    });
    const shown = [...document.querySelectorAll('[data-call-screen] video')].some((v) => v.videoWidth > 0 && !v.muted);
    return { inbound, frames, sent, shown };
  });
}
// The codec a slot's picture arrives in, read from that slot's own receiver:
// the camera's stream stays in the connection's statistics after it stops.
async function codecOf(page, slot) {
  return page.evaluate(async (slot) => {
    const tr = window.__harmonyDirectPC?.getTransceivers()[slot];
    if (!tr) return 'sem slot';
    const stats = await tr.receiver.getStats(); let mime = '';
    stats.forEach((r) => { if (r.type === 'inbound-rtp' && r.codecId) mime = stats.get(r.codecId)?.mimeType ?? mime; });
    return mime;
  }, slot);
}
let native, browser, log = '';
const waiters = [];
function waitLine(step, ms = 60000) {
  const line = `call-audit ${step}`;
  if (log.includes(line)) return Promise.resolve();
  return new Promise((res, rej) => {
    const timer = setTimeout(() => rej(new Error(`o programa não chegou a "${step}"`)), ms);
    waiters.push({ line, done: () => { clearTimeout(timer); res(); } });
  });
}
try {
  rmSync(output, { recursive: true, force: true }); mkdirSync(output, { recursive: true });
  const { invites } = await start(2);
  const suffix = Date.now().toString(36).slice(-4);
  const me = 'nativo' + suffix, peer = 'bruna' + suffix, people = {};
  for (const [i, who] of [me, peer].entries()) {
    people[who] = (await call('POST', '/api/signup', { code: invites[i], username: who, display_name: who === me ? 'nativo' : 'Bruna', password, email: who + '@example.invalid' })).cookie;
  }
  await call('POST', '/api/channels', { name: 'jogos', topic: '', kind: 'text' }, people[me]);
  const channels = (await call('GET', '/api/channels', undefined, people[peer])).json.channels;
  const geral = channels.find((c) => c.kind === 'text' && c.name !== 'jogos');
  const jogos = channels.find((c) => c.kind === 'text' && c.name === 'jogos');
  const dm = (await call('POST', '/api/dms', { username: me }, people[peer])).json;
  const dmID = dm.id ?? dm.channel?.id;
  await call('POST', `/api/channels/${dmID}/messages`, { body: 'oi' }, people[peer]);
  // The program's person silences #jogos, as the owner would from the menu.
  await call('POST', `/api/channels/${jogos.id}/mute`, { muted: true }, people[me]);

  const { chromium } = await import('playwright');
  browser = await chromium.launch({ args: ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--mute-audio', '--autoplay-policy=no-user-gesture-required', '--allow-loopback-in-peer-connection'] });
  const context = await browser.newContext({ viewport: { width: 960, height: 678 }, permissions: ['microphone', 'camera'] });
  const [name, value] = people[peer].split('=');
  await context.addCookies([{ name, value, url: BASE }]);
  // The web's own noise filters learn Chromium's fake beep as noise and fade
  // it out within seconds, which would read as the program losing sound.
  await context.addInitScript(() => localStorage.setItem('harmony.voice.micfilters', JSON.stringify({ noise: false, echo: false, agc: false, rnnoise: false })));
  const page = await context.newPage(); globalThis.auditPage = page;
  await page.goto(BASE);
  await page.getByRole('button', { name: 'Agora não' }).click({ timeout: 5000 }).catch(() => {});
  // The private conversation, from the person's row, as a person opens it
  // (e2e-harmony/fixtures.ts openDMWith).
  await page.locator(`aside button[data-person="${me}"]`).first().click({ timeout: 15000 });
  await page.getByRole('dialog').getByRole('button', { name: /conversar no privado/i }).click({ timeout: 10000 });
  await page.locator('[data-call-start]').waitFor({ timeout: 20000 });

  native = spawn(resolve(here, 'out/harmony-nativo.exe'), ['--call-audit', output], {
    env: { ...process.env, HARMONY_URL: BASE, HARMONY_TEST_USER: me, HARMONY_TEST_PASSWORD: password, HARMONY_TEST_PEER: peer, HARMONY_TEST_TONE: '1' },
    stdio: ['ignore', 'ignore', 'pipe'],
  });
  native.stderr.on('data', (c) => {
    log += c; writeFileSync(join(output, 'nativo.log'), log);
    for (const w of [...waiters]) if (log.includes(w.line)) { waiters.splice(waiters.indexOf(w), 1); w.done(); }
  });
  const exited = new Promise((r) => native.once('exit', r));

  await waitLine('hidden');
  await call('POST', `/api/channels/${geral.id}/messages`, { body: `@${me} olha isso SEGREDO123` }, people[peer]);
  await new Promise((r) => setTimeout(r, 1500));
  await call('POST', `/api/channels/${dmID}/messages`, { body: 'SEGREDO456 só entre nós' }, people[peer]);
  await new Promise((r) => setTimeout(r, 1500));
  await call('POST', `/api/channels/${jogos.id}/messages`, { body: `@${me} SEGREDO789 no canal silenciado` }, people[peer]);
  await waitLine('notices-off');
  // Past the server's 15 s between notices of one kind? Not for a socket that
  // is open: that interval is for push. This one must simply not show.
  await call('POST', `/api/channels/${dmID}/messages`, { body: 'SEGREDO000 com os avisos desligados' }, people[peer]);

  // The program is in the tray: the web calls it.
  await waitLine('ring-me');
  await page.locator('[data-call-start]').click();
  await waitLine('live-1');
  await page.locator('[data-call-screen]').waitFor({ timeout: 15000 });
  await page.screenshot({ path: join(output, 'web-chamada-atendida.png') });
  await page.waitForFunction(() => window.__harmonyDirectPC?.connectionState === 'connected', null, { timeout: 15000 }).catch(() => {});
  const a1 = await audio(page);
  await new Promise((r) => setTimeout(r, 2500));
  let a2 = await audio(page), loudest = a2.level;
  // The program beeps 200 ms a second: read the level over a second and a half.
  for (let i = 0; i < 6; i++) { await new Promise((r) => setTimeout(r, 250)); const x = await audio(page); loudest = Math.max(loudest, x.level); a2 = { ...x, level: loudest }; }
  check('chamada 1 conectada no web', a2.state === 'connected', a2.state);
  check('voz do programa chega ao web', a2.inbound > a1.inbound, `${a1.inbound} -> ${a2.inbound} bytes`);
  check('o web manda voz', a2.remote > a1.remote, `${a1.remote} -> ${a2.remote} bytes`);
  // What the program's speaker plays, from its own log: the web's fake
  // microphone beeps, so some second of the call must be loud.
  const played = [...log.matchAll(/call playout level ([0-9.]+) active/g)].map((m) => Number(m[1]));
  check('voz do web sai no alto-falante do programa', played.some((x) => x > 1), `max ${Math.max(0, ...played).toFixed(3)} em ${played.length} s`);
  check('o tom do programa tem som no web', a2.level > 0.01, `nível ${a2.level.toFixed(3)}`);
  // "Surdo" on the program: the web sees its microphone off, and what the
  // program's speaker plays falls to nothing; hearing again brings it back.
  await waitLine('deafened');
  const deafMark = await page.locator('[data-person-tile="peer"] [data-nome] [title="Microfone desligado"]').waitFor({ timeout: 5000 }).then(() => true, () => false);
  check('surdo no programa: o web vê o microfone dele desligado', deafMark);
  await page.screenshot({ path: join(output, 'web-chamada-sem-ouvir.png') });
  await waitLine('hearing');
  const during = log.slice(log.indexOf('call-audit deafened'), log.indexOf('call-audit hearing'));
  const deafLevels = [...during.matchAll(/call playout level ([0-9.]+) active/g)].map((m) => Number(m[1])).slice(1);
  check('surdo no programa: nada sai no alto-falante dele', deafLevels.length > 0 && deafLevels.every((x) => x < 0.5), `máx ${Math.max(0, ...deafLevels).toFixed(3)} em ${deafLevels.length} s`);
  await waitLine('camera-on-1');
  const back = log.slice(log.indexOf('call-audit hearing'), log.indexOf('call-audit camera-on-1'));
  const backLevels = [...back.matchAll(/call playout level ([0-9.]+) active/g)].map((m) => Number(m[1]));
  check('voltar a ouvir: o som volta no programa', backLevels.some((x) => x > 1), `máx ${Math.max(0, ...backLevels).toFixed(3)}`);
  // The program answered this one, so its answer chose the codecs.
  await new Promise((r) => setTimeout(r, 2000));
  const k1 = await video(page);
  await new Promise((r) => setTimeout(r, 2500));
  const k2 = await video(page);
  const k1Codec = await codecOf(page, 1);
  check('chamada 1 (o programa atende): câmera chega ao web', k2.frames > k1.frames + 10, `${k1.frames} -> ${k2.frames} quadros em 2,5 s, ${k1Codec}`);
  check('chamada 1: câmera não vai em H.264', k1Codec && !/h264/i.test(k1Codec), k1Codec);
  // The shared screen here is still (Windows captures only what changes), so
  // the proof is a picture decoded in the screen's own slot, not a rate.
  await waitLine('sharing-1');
  await new Promise((r) => setTimeout(r, 4000));
  const t1 = await page.evaluate(async () => {
    const stats = await window.__harmonyDirectPC.getTransceivers()[2].receiver.getStats(); let frames = 0;
    stats.forEach((r) => { if (r.type === 'inbound-rtp') frames += r.framesDecoded ?? 0; });
    return frames;
  });
  const t1Codec = await codecOf(page, 2);
  check('chamada 1: tela chega ao web', t1 > 0, `${t1} quadros decodificados no slot da tela`);
  check('chamada 1: tela vai em H.264', /h264/i.test(t1Codec), t1Codec);
  await waitLine('stopped-1');
  await waitLine('muted');
  // The speaker changed in the middle of the call and came back: both
  // switches worked, and the web's voice still plays after them.
  const switches = [...log.matchAll(/call device switched capture=0 default=\d ok=(\d)/g)].map((m) => m[1]);
  check('trocar o alto-falante no meio da chamada', switches.length === 2 && switches.every((x) => x === '1'), `${switches.length} trocas: ${switches.join(',')}`);
  const after = log.slice(log.indexOf('call-audit switched'));
  const afterLevels = [...after.matchAll(/call playout level ([0-9.]+) active/g)].map((m) => Number(m[1]));
  check('o som continua depois da troca', afterLevels.some((x) => x > 1), `máx ${Math.max(0, ...afterLevels).toFixed(3)} em ${afterLevels.length} s`);
  // The mark on the program's block, as a room shows it.
  const muted = await page.locator('[data-person-tile="peer"] [data-nome] [title="Microfone desligado"]').waitFor({ timeout: 10000 }).then(() => true, () => false);
  check('mudo do programa aparece no web', muted);
  // And it is silence, not only a word: the level the web receives falls.
  await new Promise((r) => setTimeout(r, 1500));
  const quiet = await audio(page);
  check('mudo do programa é silêncio no web', quiet.level < 0.005, `nível ${quiet.level.toFixed(4)}`);
  await page.locator('[data-call-hangup]').click();
  await waitLine('ended-1');

  // The program calls back; the web answers.
  await waitLine('calling');
  const box = page.locator('[data-incoming-call]');
  const rang = await box.waitFor({ timeout: 20000 }).then(() => true, () => false);
  check('ligação do programa toca no web', rang);
  if (rang) check('o web mostra quem liga', (await box.textContent()).includes('nativo'));
  await box.locator('[data-call-accept]').click();
  await waitLine('live-2');
  const b1 = await audio(page);
  await new Promise((r) => setTimeout(r, 2500));
  const b2 = await audio(page);
  check('chamada 2 conectada no web', b2.state === 'connected', b2.state);
  check('chamada 2: voz do programa chega ao web', b2.inbound > b1.inbound, `${b1.inbound} -> ${b2.inbound} bytes`);
  // Camera from the web to the program: its log counts the pictures that
  // arrive, and the call's tile shows them (the program's own step).
  await page.locator('[data-call-cam]').click();
  await waitLine('seen-camera');
  // The count is logged once a second: give it two.
  await new Promise((r) => setTimeout(r, 2500));
  const seen = [...log.matchAll(/call video in camera (\d+) screen \d+ fps/g)].map((m) => Number(m[1]));
  check('câmera do web chega ao programa', seen.some((x) => x >= 5), `máx ${Math.max(0, ...seen)} quadros/s em ${seen.length} s`);
  // The program's camera to the web, with the codec it went out in.
  await waitLine('pip-stop-camera');
  await page.locator('[data-call-cam]').click();
  await waitLine('pip-tested');
  await waitLine('camera-on');
  await new Promise((r) => setTimeout(r, 2500));
  const c1 = await video(page);
  await new Promise((r) => setTimeout(r, 2500));
  const c2 = await video(page);
  const camCodec = await codecOf(page, 1);
  check('câmera do programa chega ao web', c2.frames > c1.frames + 10, `${c1.frames} -> ${c2.frames} quadros em 2,5 s, ${camCodec}`);
  check('câmera do programa não vai em H.264', camCodec && !/h264/i.test(camCodec), camCodec);
  await waitLine('camera-off');
  // The program's screen to the web.
  await waitLine('sharing');
  await new Promise((r) => setTimeout(r, 2000));
  const v1 = await video(page);
  await new Promise((r) => setTimeout(r, 3000));
  const v2 = await video(page);
  check('tela do programa chega ao web', v2.frames > v1.frames + 10, `${v1.frames} -> ${v2.frames} quadros em 3 s`);
  check('o web mostra a tela do programa', v2.shown);
  const screenCodec = await codecOf(page, 2);
  check('a tela do programa vai em H.264', /h264/i.test(screenCodec), screenCodec);
  const big = await page.locator('[data-shown="peer"]').count();
  check('o web mostra a tela do programa em destaque', big === 1);
  await waitLine('stopped-sharing');
  await new Promise((r) => setTimeout(r, 2000));
  const v3 = await video(page);
  await new Promise((r) => setTimeout(r, 2000));
  const v4 = await video(page);
  check('parar a tela para no web', v4.frames - v3.frames < 5, `${v3.frames} -> ${v4.frames} quadros em 2 s`);
  await waitLine('ended-2');
  const gone = await page.locator('[data-call-screen]').waitFor({ state: 'detached', timeout: 10000 }).then(() => true, () => false);
  check('desligar no programa encerra no web', gone);

  // Once more, and the program refuses from the notice.
  await waitLine('ring-me-again');
  await page.locator('[data-call-start]').click();
  await waitLine('declined');
  const refused = await page.getByText('Chamada recusada').waitFor({ timeout: 10000 }).then(() => true, () => false);
  const ended = await page.locator('[data-call-screen]').waitFor({ state: 'detached', timeout: 10000 }).then(() => true, () => false);
  check('recusar no programa encerra no web', refused || ended, refused ? 'Chamada recusada' : 'tela fechada');

  const code = await Promise.race([exited, new Promise((r) => setTimeout(() => r('timeout'), 30000))]);
  const result = existsSync(join(output, 'resultado.json')) ? JSON.parse(readFileSync(join(output, 'resultado.json'), 'utf8')) : { passed: false, stage: 'sem resultado' };
  // No notice text, ever, in the program's log either.
  check('nenhum texto de mensagem no registro', !/SEGREDO/.test(log));
  result.web = checks;
  result.passed = result.passed === true && checks.every((c) => c.ok) && code === 0;
  writeFileSync(join(output, 'resultado.json'), JSON.stringify(result, null, 2));
  console.log('exit', code, JSON.stringify(result));
  if (!result.passed) process.exitCode = 1;
} catch (e) {
  console.error('call-audit falhou:', e.message); process.exitCode = 1;
  await globalThis.auditPage?.screenshot({ path: join(output, 'web-erro.png') }).catch(() => {});
  writeFileSync(join(output, 'erro.txt'), String(e.stack ?? e));
} finally {
  native?.kill(); await browser?.close().catch(() => {}); stop();
}
