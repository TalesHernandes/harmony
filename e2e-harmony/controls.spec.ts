// The controls people actually press, in a real browser.
//
// These are the owner's sentences after using the thing, turned into
// assertions: a fullscreen with no way out and a strip of numbers stuck under
// it, no way to ask for less when the line is bad, no way to choose a camera or
// a microphone, and buttons too small to hit. Every one of them is a sentence
// about the browser, so this is the only place they can be checked.

import { test, expect, Person } from './fixtures';

// What the code allows each size and rate (the SCREEN_BITRATE table in
// web/harmony/src/media.ts). If these numbers ever disagree with that table,
// one of them is wrong and this is where it shows.
//
// They are the owner's own, given cell by cell on 11/09/2026 — so a change to
// them is a decision of his, and a test failing here means somebody changed
// the table without meaning to.
// The START table, not the ceiling — they were the same number until
// 18/09/2026 and are now two. The start is 60% of this.
const START_1080_30 = 8_000_000;
// The ceiling for 720p at 30. The owner raised the table on 18/09/2026, lowered
// it on 19/09/2026 once three layers made a 40 Mbit layer mean something
// different from a 40 Mbit stream, and raised it again the same night when a
// real game hit the new ceiling and the picture shrank to fit.
const CEILING_720_30 = 9_000_000;

test('the quality that is chosen is the quality that goes out', async ({ browser }) => {
	const ana = await Person.create(browser, 'quali');
	await ana.joinVoice();
	await ana.shareScreen();

	await expect.poll(async () => (await ana.sending()).length, { timeout: 30_000 }).toBeGreaterThan(0);

	// With the screen already up, the same panel is where it is changed and
	// where it is switched off.
	await ana.openPanel('screen');
	await expect(ana.dialog().getByRole('button', { name: /parar a tela/i })).toBeVisible();
	await ana.choose('quality', '720');
	await ana.choose('fps', '30');

	// The ceiling the encoder was given, and the size the capture settled on.
	// The pair, waited for together: 720p at "automático" has the same
	// ceiling as 720p at 30, so waiting on the ceiling alone could read the
	// frame rate before the second choice had arrived.
	await expect
		.poll(
			async () => {
				const s = (await ana.sending())[0];
				return [s?.maxBitrate, s?.maxFramerate];
			},
			{ timeout: 15_000, message: 'the frame rate was not capped' },
		)
		.toEqual([CEILING_720_30, 30]);
	const sending = (await ana.sending())[0];
	expect(sending.height, 'the capture is bigger than what was asked for').toBeLessThanOrEqual(720);
	expect(sending.fps, 'the capture is faster than what was asked for').toBeLessThanOrEqual(31);

	// Asking for less asks for less in the ceiling at once; the capture's
	// part is checked in the test below, as a request.
	await ana.choose('quality', '480');
	await expect
		.poll(async () => (await ana.sending())[0]?.maxBitrate, { timeout: 15_000 })
		.toBe(6_000_000);

	// And it is remembered — under this person's name, not the browser's, so
	// that two people sharing a machine do not inherit each other's settings.
	expect(
		await ana.page.evaluate(() => localStorage.getItem('harmony.voice.quali.quality')),
	).toBe('480');
	await ana.page.reload();
	// The heading is 'Voz' since 20/09/2026 ('Canais de voz' under a tab
	// that already said Servidor was saying it twice). What this line is
	// really waiting for is the column being drawn again after the reload.
	await ana.page.getByText('Voz', { exact: true }).first().waitFor();
	await ana.joinVoice();
	await ana.openPanel('screen');
	expect(await ana.page.locator('button[data-setting="quality"]').getAttribute('data-value')).toBe('480');

	await ana.close();
});

// Stepping a live share down to 480p asks the capture for 480. What the test
// browser then *delivers* cannot be checked here: Chromium's fake capture
// device only knows 96x96, 320x240, 640x480, 1280x720, 1920x1080 and
// 3840x2160 (media/capture/video/fake_video_capture_device_factory.cc,
// kDefaultResolutions), and for a request of 853x480 it picks the first one
// at least that wide — 1280x720. The earlier version of the test above read
// the size in the few seconds getSettings still echoed the request, and so
// passed on that echo; on 24/09/2026 it read later, saw 720, and looked like a
// defect in the app for an hour. The app's part is the request, so the
// request is what is checked.
test('descer de 720p para 480p no meio da tela pede 480 à captura', async ({ browser }) => {
	const ana = await Person.create(browser, 'desce' + test.info().repeatEachIndex);
	await ana.joinVoice();
	await ana.shareScreen();
	await expect.poll(async () => (await ana.sending()).length, { timeout: 30_000 }).toBeGreaterThan(0);
	await ana.openPanel('screen');
	await ana.choose('quality', '720');
	await ana.choose('quality', '480');
	const asked = () =>
		ana.page.evaluate(() => {
			const pc = (window as unknown as { __harmonyPC?: RTCPeerConnection }).__harmonyPC;
			const track = pc?.getSenders().find((s) => s.track?.kind === 'video')?.track;
			const c = track?.getConstraints() as { height?: { max?: number }; width?: { max?: number } };
			return [c?.width?.max, c?.height?.max];
		});
	await expect.poll(asked, { timeout: 10_000 }).toEqual([853, 480]);
	await ana.close();
});

test('a screen is told to start at the size it was asked for', async ({ browser }) => {
	// The owner's log: 320×180, then 960×540, then 1280×720, and 1080p only
	// forty seconds in. A browser told nothing starts at about three hundred
	// kilobits and feels its way up; this is the line that tells it otherwise,
	// written into the description before it is answered.
	const ana = await Person.create(browser, 'comeco');
	await ana.joinVoice();
	await ana.shareScreen();
	await expect.poll(async () => (await ana.sending()).length, { timeout: 30_000 }).toBeGreaterThan(0);

	// What is sent can be the microphone before the screen's renegotiation
	// has been answered; the description to read is the one with a picture.
	const described = () =>
		ana.page.evaluate(() => {
			const pc = (window as unknown as { __harmonyPC?: RTCPeerConnection }).__harmonyPC;
			return pc?.remoteDescription?.sdp ?? '';
		});
	await expect.poll(async () => (await described()).includes('m=video'), { timeout: 30_000 }).toBe(true);
	const sdp = await described();
	// Sixty per cent of the eight megabits a 1080p screen at thirty STARTS at
	// by default (START_SHARE, in kilobits). The ceiling is five times this and
	// deliberately does not appear here: starting at sixty per cent of fifty
	// megabits would be the exact mistake this line exists to prevent.
	expect(sdp, 'the description says nothing about where to start').toContain(
		`x-google-start-bitrate=${(START_1080_30 * 0.6) / 1000}`,
	);
	// And it was only written on the picture slots: audio is untouched.
	const audio = sdp.slice(sdp.indexOf('m=audio'), sdp.indexOf('m=video'));
	expect(audio).not.toContain('x-google-start-bitrate');

	await ana.close();
});

test('an arriving screen is held long enough to replace a lost packet', async ({ browser }) => {
	// The last thing that was still lurching. The reply to "send that packet
	// again" has to arrive before the browser has given up waiting, or every
	// lost packet becomes a visible stop. Holding the picture a little longer is
	// what gives the reply time to arrive.
	//
	// The number came down from 400 ms to 150 on 18/09/2026, and to 80 on
	// 19/09/2026 when the owner asked whether more could come off; the
	// threshold below came down with it. What is proved here never changes: a
	// screen is held and a face is not. Four hundred was written for a server in Germany
	// 211 ms away; the server is now in Sao Paulo, 15 ms away, and the owner was
	// feeling the difference as delay — his telephone read `espera=336-493ms` on
	// every sample while he said the live was far behind. What is proved here is
	// unchanged: a screen is held, and a face is not.
	const ana = await Person.create(browser, 'segura1');
	const bento = await Person.create(browser, 'segura2');
	await ana.joinVoice();
	await bento.joinVoice();
	await ana.shareScreen();
	await bento.watchScreen();

	await expect
		.poll(async () => (await bento.holding()).filter((s) => s > 0.04).length, { timeout: 30_000 })
		.toBeGreaterThan(0);

	// A camera is not held: a face is part of the conversation, and a
	// conversation is where delay is felt.
	await bento.startCamera();
	await expect
		.poll(async () => (await ana.holding()).filter((s) => s > 0).length, { timeout: 30_000 })
		.toBe(0);

	await ana.close();
	await bento.close();
});

test('the camera and the microphone can be switched without rebuilding the connection', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'camsel');
	await ana.joinVoice();
	await ana.startCamera();
	await expect.poll(async () => (await ana.sending()).length, { timeout: 30_000 }).toBeGreaterThan(0);

	await ana.markConnection('antes');

	// The camera, in the panel that turned it on.
	await ana.openPanel('camera');
	const cameras = await ana.options('camera');
	expect(cameras.length, 'no camera in the selector: ' + cameras.join(',')).toBeGreaterThan(1);
	await ana.choose('camera', cameras[1]);
	await ana.page.keyboard.press('Escape');

	// The microphone and the speakers, in Ajustes, where somebody sets this up
	// once and forgets about it.
	await ana.openSettings();
	const mics = await ana.options('microphone');
	expect(mics.length, 'no microphone in the selector: ' + mics.join(',')).toBeGreaterThan(1);
	await ana.choose('microphone', mics[1]);

	// Still sending a picture, still in the room, and still the same
	// connection: swapping a device is a replaceTrack, never a new
	// PeerConnection (CLAUDE.md).
	await expect
		.poll(async () => (await ana.videos()).filter((v) => v.w > 0 && v.live).length, {
			timeout: 20_000,
		})
		.toBeGreaterThan(0);
	expect(await ana.connectionMark(), 'the connection was rebuilt to change a device').toBe('antes');
	expect(await ana.page.evaluate(() => localStorage.getItem('harmony.voice.camsel.camera'))).toBe(
		cameras[1],
	);
	expect(await ana.page.evaluate(() => localStorage.getItem('harmony.voice.camsel.mic'))).toBe(
		mics[1],
	);

	await ana.close();
});

test('fullscreen has a way out, and nothing else stays on the screen', async ({ browser }) => {
	const ana = await Person.create(browser, 'cheia');
	await ana.joinVoice();
	await ana.shareScreen();
	// Full screen is on the block in focus (Discord's room, 24/09/2026).
	await ana.focusMyScreen();

	const enter = ana.page.locator('button[aria-label="tela cheia"]').first();
	await enter.waitFor({ timeout: 30_000 });

	// The owner's other complaint, measured: the button has to be big enough to
	// hit with a thumb.
	const box = await enter.boundingBox();
	expect(box, 'no fullscreen button').not.toBeNull();
	expect(box!.height, 'the fullscreen button is too small').toBeGreaterThanOrEqual(28);
	expect(box!.width, 'the fullscreen button is too small').toBeGreaterThanOrEqual(28);

	await enter.click();
	await expect
		.poll(async () => ana.page.evaluate(() => !!document.fullscreenElement), { timeout: 10_000 })
		.toBe(true);

	// "Em fullscreen, tá ficando a barra de stats, não é para ficar."
	await expect(ana.page.locator('[data-tilebar]').first()).toBeHidden();

	const controls = ana.page.locator('[data-fullscreen-controls]');
	await expect(controls).toHaveAttribute('data-fullscreen-controls', 'visivel');

	// Left alone, the controls get out of the way too, the way every video
	// player does.
	await expect(controls).toHaveAttribute('data-fullscreen-controls', 'escondido', {
		timeout: 10_000,
	});

	// Any sign of a person brings them back.
	await ana.page.mouse.move(300, 300);
	await ana.page.mouse.move(320, 310);
	await expect(controls).toHaveAttribute('data-fullscreen-controls', 'visivel', { timeout: 5_000 });

	// And the way out works. This is the thing that was missing.
	await ana.page.locator('button[aria-label="sair da tela cheia"]').first().click();
	await expect
		.poll(async () => ana.page.evaluate(() => !!document.fullscreenElement), { timeout: 10_000 })
		.toBe(false);
	await expect(ana.page.locator('[data-tilebar]').first()).toBeVisible();

	await ana.close();
});

test('the call has the controls under it', async ({ browser }) => {
	const ana = await Person.create(browser, 'barra');
	await ana.joinVoice();

	// The five things people press during a call, where the eyes already are.
	const call = ana.page.locator('[data-call]');
	for (const label of [
		'desligar o microfone',
		'não ouvir ninguém',
		'ligar a câmera',
		'compartilhar a tela',
		'sair da chamada',
	]) {
		const button = call.locator(`button[aria-label="${label}"]`);
		await expect(button, `no button for ${label}`).toBeVisible();
		const box = await button.boundingBox();
		expect(box!.height, `"${label}" is too small to press`).toBeGreaterThanOrEqual(40);
	}

	// And hanging up from there really leaves the room.
	await call.locator('button[aria-label="sair da chamada"]').click();
	await expect(ana.page.locator('[data-call="live"]')).toBeHidden({ timeout: 15_000 });

	await ana.close();
});

test('the small buttons are big enough to press', async ({ browser }) => {
	// The owner: "tem alguns ícones que estão muito pequenos, como o de
	// adicionar um canal". Adding a channel is only drawn for the owner, which
	// is why this test signs in as them.
	const dono = await Person.signIn(browser, 'dono');

	for (const label of ['novo canal', 'novo canal de voz', 'pessoas', 'ajustes']) {
		const button = dono.page.locator(`button[aria-label="${label}"]`).first();
		await button.waitFor({ timeout: 10_000 });
		const box = await button.boundingBox();
		expect(box, `no button for ${label}`).not.toBeNull();
		expect(box!.height, `"${label}" is too small to press`).toBeGreaterThanOrEqual(28);
		expect(box!.width, `"${label}" is too small to press`).toBeGreaterThanOrEqual(28);
	}

	await dono.close();
});

test('a sala conecta antes do supressor de ruído, e o supressor entra depois', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'antesdo');
	await ana.openChannels();

	// Production is 211 ms away from the owner's machine, measured on
	// 18/09/2026. Localhost hides every round trip in the join chain and hides
	// the downloads the noise suppressor needs — 157 KB of neural network and
	// 64 KB of worklet when it was RNNoise, which over that link were 1,6 s and
	// 1,1 s, and 24 MB since it is DeepFilterNet (25/09/2026). This
	// puts the distance back so that the *order* is the thing being tested.
	await ana.page.route('**/*', async (route) => {
		await new Promise((r) => setTimeout(r, 105));
		await route.continue();
	});

	await ana.page.locator('button[title*="entrar na sala" i]:visible').first().click();
	await ana.page.locator('[data-call="live"]').waitFor({ timeout: 60_000 });

	// First: the suppressor is late but it does arrive. The microphone in the
	// connection is swapped for the clean one once it is ready, without the
	// connection being rebuilt (CLAUDE.md — replaceTrack).
	//
	// This is waited for **before** the timings are read, and the order of
	// these two blocks is the test's own bug from the first day it existed: the
	// whole point of the change is that the download no longer happens before
	// the room is joined, so reading the list of requests the instant the call
	// goes live catches a download that has not started yet. It passed once and
	// failed on the next run.
	await expect
		.poll(async () => await ana.page.evaluate(() => (window as any).__harmonyDiag() as string), {
			timeout: 30_000,
		})
		.toContain('microfone refeito pela limpeza');

	// And now the assertion, which is about order and nothing else: the request
	// that joins the room went out **before** the network the suppressor runs
	// on was fetched. Until 18/09/2026 it was the other way round, and the
	// column said "Conectando…" through a download that has nothing to do with
	// connecting.
	const when = await ana.page.evaluate(() =>
		performance
			.getEntriesByType('resource')
			.filter((e) => /\/join$|df_bg.*\.wasm$/.test(e.name))
			.map((e) => ({ what: /wasm$/.test(e.name) ? 'rnnoise' : 'join', at: e.startTime })),
	);
	const join = when.find((e) => e.what === 'join');
	const rnnoise = when.find((e) => e.what === 'rnnoise');
	expect(join, 'o pedido de entrar na sala').toBeTruthy();
	expect(rnnoise, 'o download do supressor').toBeTruthy();
	expect(rnnoise!.at).toBeGreaterThan(join!.at);

	await ana.close();
});
