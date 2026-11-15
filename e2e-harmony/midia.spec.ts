// The media area of the 23/09/2026 audit — begun by Codex, whose quota ran out
// two minutes in, and finished by hand from the two leads it left. Every one
// was inverted — the fix taken out, the test watched going red — before it
// was kept.

import { test, expect, Person } from './fixtures';

type VoiceView = { micGain: number; room: string };
const view = (who: Person) =>
	who.page.evaluate(
		() => (window as unknown as { __harmonyVoice: { snapshot(): VoiceView } }).__harmonyVoice.snapshot(),
	);

// A browser that has never saved a microphone volume read "no preference" as
// Number(null) — which is 0, and 0 is inside the range that is accepted. So a
// friend on a new phone joined every room at 0%: the raw microphone went out
// until the first touch, and the first touch built the graph at that volume.
test('num aparelho novo o microfone entra a 100%, não a 0%', async ({ browser }) => {
	const ana = await Person.create(browser, 'm1ana');
	expect(await ana.page.evaluate(() => Object.keys(localStorage).filter((k) => k.includes('micgain')))).toEqual(
		[],
	);
	expect((await view(ana)).micGain).toBe(1);
	await ana.close();
});

// The same slip in the column width: a new browser opened on the narrowest
// rung instead of the default.
test('num navegador novo a coluna abre na largura padrão', async ({ browser }) => {
	const ana = await Person.create(browser, 'm1bana');
	expect(await ana.page.evaluate(() => localStorage.getItem('harmony.column'))).toBeNull();
	const width = await ana.page.locator('nav').first().evaluate((el) => el.getBoundingClientRect().width);
	// COLUMN_DEFAULT is w-64: 256 px.
	expect(Math.round(width)).toBe(256);
	await ana.close();
});

// peopleInVoice asks the server who is in any voice room right now.
async function peopleInVoice(who: Person): Promise<string[]> {
	return who.page.evaluate(async () => {
		const res = await fetch('/api/voice/rooms').then((r) => r.json());
		return Object.values(res.rooms as Record<string, { username: string }[]>)
			.flat()
			.map((p) => p.username);
	});
}

// holdCapture makes one kind of capture wait for the test: the real stream is
// made, kept where the test can look at it, and handed over only on release.
async function holdCapture(who: Person, kind: 'getUserMedia' | 'getDisplayMedia') {
	await who.page.evaluate((kind) => {
		const w = window as unknown as { __release?: () => void; __held: MediaStream | null };
		const md = navigator.mediaDevices;
		const real = md.getUserMedia.bind(md);
		w.__held = null;
		w.__release = undefined;
		md[kind] = (c?: MediaStreamConstraints) =>
			new Promise((resolve) => {
				w.__release = async () => {
					const s = await real(kind === 'getDisplayMedia' ? { video: true } : c);
					w.__held = s;
					resolve(s);
				};
			});
	}, kind);
}

async function release(who: Person) {
	await expect
		.poll(() => who.page.evaluate(() => typeof (window as unknown as { __release?: unknown }).__release))
		.toBe('function');
	await who.page.evaluate(() => (window as unknown as { __release: () => Promise<void> }).__release());
}

const heldEnded = (who: Person) =>
	who.page.evaluate(() => {
		const s = (window as unknown as { __held: MediaStream | null }).__held;
		return s ? s.getTracks().every((t) => t.readyState === 'ended') : null;
	});

type Snapshot = { state: string; sessionID: string; channelID: string; error: string; sharingCamera: boolean };
const snap = (who: Person) =>
	who.page.evaluate(
		() => (window as unknown as { __harmonyVoice: { snapshot(): Snapshot } }).__harmonyVoice.snapshot(),
	);

const leaveNow = (who: Person) =>
	who.page.evaluate(() =>
		(window as unknown as { __harmonyVoice: { leave(): Promise<void> } }).__harmonyVoice.leave(),
	);

// 2. Pressing "sair" while the browser was still asking for the microphone
// took the room off the screen — and then, when the permission came, the join
// carried on as if nothing had happened: into the room, microphone open.
test('sair enquanto o microfone abre não entra na sala depois', async ({ browser }) => {
	const ana = await Person.create(browser, 'm2ana');
	await ana.openChannels();
	await holdCapture(ana, 'getUserMedia');
	await ana.page.locator('button[title*="entrar na sala" i]:visible').first().click();
	await expect.poll(async () => (await snap(ana)).state).toBe('joining');
	await leaveNow(ana);
	await expect.poll(async () => (await snap(ana)).state).toBe('off');

	const joined = ana.page.waitForRequest('**/api/voice/*/join', { timeout: 3_000 }).then(
		() => true,
		() => false,
	);
	await release(ana);
	await expect.poll(() => heldEnded(ana)).toBe(true);
	expect(await joined).toBe(false);
	const s = await snap(ana);
	expect([s.state, s.channelID, s.error]).toEqual(['off', '', '']);
	expect(await peopleInVoice(ana)).not.toContain('m2ana');
	await ana.close();
});

// The same, one step later: leaving while the server was still making the
// session left it there, and nothing here ever gave it back — the room went on
// showing this person in it until the server gave up on its own.
test('sair enquanto o servidor responde devolve a sessão', async ({ browser }) => {
	const ana = await Person.create(browser, 'm3ana');
	await ana.openChannels();
	let open!: () => void;
	const opened = new Promise<void>((r) => (open = r));
	let arrived!: () => void;
	const held = new Promise<void>((r) => (arrived = r));
	await ana.page.route('**/api/voice/*/join', async (route) => {
		arrived();
		await opened;
		await route.continue();
	});
	await ana.page.locator('button[title*="entrar na sala" i]:visible').first().click();
	await held;
	await leaveNow(ana);
	const left = ana.page.waitForRequest('**/api/voice/leave', { timeout: 10_000 });
	open();
	await left;
	await expect.poll(() => peopleInVoice(ana)).not.toContain('m3ana');
	// And nothing of it left on this screen.
	const s = await snap(ana);
	expect([s.state, s.sessionID, s.error]).toEqual(['off', '', '']);
	await ana.close();
});

// A camera allowed after leaving came on for nobody: the light on, the
// picture in a room this device was no longer in.
test('câmera liberada depois de sair não liga', async ({ browser }) => {
	const ana = await Person.create(browser, 'm4ana');
	await ana.joinVoice();
	await holdCapture(ana, 'getUserMedia');
	await ana.startCamera();
	await expect
		.poll(() => ana.page.evaluate(() => typeof (window as unknown as { __release?: unknown }).__release))
		.toBe('function');
	await leaveNow(ana);
	await expect.poll(async () => (await snap(ana)).state).toBe('off');
	await release(ana);
	await expect.poll(() => heldEnded(ana)).toBe(true);
	const s = await snap(ana);
	expect([s.sharingCamera, s.error]).toEqual([false, '']);
	await ana.close();
});

// And the screen: the picker stays open for as long as somebody takes to
// choose, and a window chosen after leaving went out to a room nobody was in.
test('tela escolhida depois de sair não sobe', async ({ browser }) => {
	const ana = await Person.create(browser, 'm5ana');
	await ana.joinVoice();
	await holdCapture(ana, 'getDisplayMedia');
	await ana.shareScreen();
	await expect
		.poll(() => ana.page.evaluate(() => typeof (window as unknown as { __release?: unknown }).__release))
		.toBe('function');
	await leaveNow(ana);
	await expect.poll(async () => (await snap(ana)).state).toBe('off');
	await release(ana);
	await expect.poll(() => heldEnded(ana)).toBe(true);
	const s = await ana.page.evaluate(
		() =>
			(window as unknown as { __harmonyVoice: { snapshot(): { sharingScreen: boolean } } }).__harmonyVoice.snapshot()
				.sharingScreen,
	);
	expect(s).toBe(false);
	await ana.close();
});

// 3. A switch-off waiting on the screen that went away fired on the next one.
// Somebody sharing into an empty room is told nobody watches, and the layers
// are set to go off in five seconds; stopping and sharing another window
// inside that time had the new screen switched off. The server's own answer
// about the new screen would have cancelled it — but that answer only comes
// once a picture arrives, so here it is held back, the way it is for the
// moment between sharing and the first picture.
test('trocar de janela logo depois não desliga a tela nova', async ({ browser }) => {
	const ana = await Person.create(browser, 'm6ana');
	await ana.joinVoice();
	await ana.shareScreen();
	await expect.poll(() => ana.diag(), { timeout: 15_000 }).toContain('vai encolher para nenhuma');
	await ana.page.keyboard.press('Escape');
	await ana.stopScreen();
	await ana.page.evaluate(() => {
		const v = (window as unknown as { __harmonyVoice: { screenWatchers(d: unknown): Promise<void> } })
			.__harmonyVoice;
		v.screenWatchers = async () => undefined;
	});
	await ana.page.keyboard.press('Escape');
	await ana.shareScreen();
	await expect
		.poll(async () => (await snap(ana)) as unknown as { sharingScreen: boolean })
		.toMatchObject({ sharingScreen: true });
	// Past the five seconds the old switch-off would have waited.
	await ana.page.waitForTimeout(6_000);
	const active = await ana.page.evaluate(() => {
		const pc = (window as unknown as { __harmonyPC: RTCPeerConnection }).__harmonyPC;
		const tr = pc.getTransceivers().find((t) => t.sender.track?.kind === 'video' && t.sender.track.readyState === 'live');
		return tr ? tr.sender.getParameters().encodings.map((e) => e.active) : null;
	});
	expect(active).not.toBeNull();
	expect(active!.some((a) => a)).toBe(true);
	await ana.close();
});

// A screen shared while somebody else's camera was already arriving went to
// nobody: the server took the transceiver bringing that camera for the new
// screen's slot, and dropped the screen itself as a track on an unknown slot.
// Every other test here shares the screen before anybody turns a camera on.
test('tela compartilhada com a câmera de outra pessoa no ar chega', async ({ browser }) => {
	const ana = await Person.create(browser, 'm7ana');
	const bruno = await Person.create(browser, 'm7bruno');
	await ana.joinVoice();
	await bruno.joinVoice();
	await ana.startCamera();
	await ana.page.keyboard.press('Escape');
	await expect.poll(() => bruno.diag(), { timeout: 15_000 }).toContain('recebendo cam de m7ana');
	await bruno.shareScreen();
	await bruno.page.keyboard.press('Escape');
	await ana.watchScreen();
	await expect.poll(() => ana.diag(), { timeout: 15_000 }).toContain('recebendo screen de m7bruno');
	await ana.close();
	await bruno.close();
});

// The line's loss is a difference against the previous reading, and since the
// call's redraw two parts of the screen read it — the bars in the column and
// the chip in the call's bar. Two readings a moment apart would judge the line
// by the handful of packets between them. One reading serves both.
test('as duas leituras da conexão são a mesma medida', async ({ browser }) => {
	const ana = await Person.create(browser, 'm8ana');
	await ana.joinVoice();
	await expect(ana.page.locator('[data-quality]')).toHaveCount(2, { timeout: 30_000 });
	const same = await ana.page.evaluate(async () => {
		const v = (window as unknown as { __harmonyVoice: { link(): Promise<unknown> } }).__harmonyVoice;
		const [a, b] = await Promise.all([v.link(), v.link()]);
		const c = await v.link();
		// And it is a window, not a lid: a second later there is a new reading
		// (the Codex, 23/09/2026: a cache that never expired passed this).
		await new Promise((r) => setTimeout(r, 1000));
		const d = await v.link();
		return a !== null && a === b && b === c && d !== null && d !== c;
	});
	expect(same).toBe(true);
	await ana.close();
});

// Since 23/09/2026 the meter reads every second (the owner: "poderia atualizar
// mais rapidamente"), and one second of one voice is about fifty packets: one
// packet lost in it is 2%, which is the "média" line. Loss is still counted
// over two seconds, where the same packet is 1% and the bars stay green.
//
// The browser's numbers are replaced by a clock — fifty packets a second, one
// lost at 2.5s — and the reading that matters is taken right after the loss,
// fresh (linkOut cleared), with a sample forced at zero so that one two seconds
// old exists. Counted against the last reading instead, which is what the
// meter did before, the same packet reads 2% or more.
test('um pacote perdido não pinta a conexão de amarelo', async ({ browser }) => {
	const ana = await Person.create(browser, 'm9ana' + (test.info().repeatEachIndex || ''));
	await ana.joinVoice();
	await expect(ana.page.locator('[data-quality]').first()).toBeVisible({ timeout: 30_000 });
	const perda = await ana.page.evaluate(async () => {
		type V = {
			pc: RTCPeerConnection;
			linkSamples: unknown[];
			linkOut: unknown;
			link(): Promise<{ lost: number } | null>;
		};
		const v = (window as unknown as { __harmonyVoice: V }).__harmonyVoice;
		const t0 = Date.now();
		v.pc.getStats = async () => {
			const t = Date.now() - t0;
			const audio = {
				type: 'inbound-rtp',
				kind: 'audio',
				packetsReceived: Math.floor((t / 1000) * 50),
				packetsLost: t > 2500 ? 1 : 0,
				jitter: 0,
			};
			const pair = { type: 'candidate-pair', state: 'succeeded', currentRoundTripTime: 0.02 };
			return new Map<string, unknown>([
				['a', audio],
				['p', pair],
			]) as unknown as RTCStatsReport;
		};
		v.linkSamples = [];
		v.linkOut = null;
		await v.link();
		await new Promise((r) => setTimeout(r, 2800 - (Date.now() - t0)));
		v.linkOut = null;
		return (await v.link())!.lost;
	});
	expect(perda, 'a perda de um pacote, em %').toBe(1);
	// And what is drawn: both meters green (the Codex, 23/09/2026: the number
	// was checked and the colour was not).
	await expect(ana.page.locator('[data-quality="boa"]')).toHaveCount(2);
	await ana.close();
});

// The three below come from the Codex's audit of the D4u (23/09/2026), each
// with the browser's numbers replaced so that the case is exact. The fake
// report is built inside the page from plain data.
type Linky = {
	pc: RTCPeerConnection;
	linkSamples: { at: number; counts: Map<string, { packets: number; lost: number }> }[];
	linkOut: unknown;
	linkPending: unknown;
	link(): Promise<{ lost: number } | null>;
	leave(): Promise<void>;
};

// Installs a getStats that answers from window.__fakeVoices: [id, packets, lost].
function installFake() {
	const w = window as unknown as { __harmonyVoice: Linky; __fakeVoices: [string, number, number][] };
	w.__harmonyVoice.pc.getStats = async () =>
		new Map<string, unknown>([
			['p', { type: 'candidate-pair', state: 'succeeded', currentRoundTripTime: 0.02 }],
			...w.__fakeVoices.map(
				([id, packets, lost]) =>
					[id, { type: 'inbound-rtp', kind: 'audio', id, packetsReceived: packets, packetsLost: lost, jitter: 0 }] as [
						string,
						unknown,
					],
			),
		]) as unknown as RTCStatsReport;
}

// Somebody leaves: their counter goes out of the report. Summed, the total
// fell and the difference went negative, which read as 0% while the voice that
// stayed was losing one packet in ten.
test('quem sai da sala não apaga a perda de quem ficou', async ({ browser }) => {
	const ana = await Person.create(browser, 'm10ana');
	await ana.joinVoice();
	await expect(ana.page.locator('[data-quality]').first()).toBeVisible({ timeout: 30_000 });
	await ana.page.evaluate(installFake);
	const perda = await ana.page.evaluate(async () => {
		const w = window as unknown as { __harmonyVoice: Linky; __fakeVoices: [string, number, number][] };
		const v = w.__harmonyVoice;
		w.__fakeVoices = [
			['bia', 1000, 0],
			['caio', 5000, 0],
		];
		v.linkSamples = [];
		v.linkOut = null;
		await v.link();
		await new Promise((r) => setTimeout(r, 2100));
		w.__fakeVoices = [['bia', 1090, 10]];
		v.linkOut = null;
		return (await v.link())!.lost;
	});
	expect(perda).toBe(10);
	await ana.close();
});

// A tab that was frozen: the only sample old enough is a minute old, and a
// minute of good line diluted the last bad second to nothing. The base is never
// older than five seconds.
test('uma amostra velha demais não dilui a perda recente', async ({ browser }) => {
	const ana = await Person.create(browser, 'm11ana');
	await ana.joinVoice();
	await expect(ana.page.locator('[data-quality]').first()).toBeVisible({ timeout: 30_000 });
	await ana.page.evaluate(installFake);
	const perda = await ana.page.evaluate(async () => {
		const w = window as unknown as { __harmonyVoice: Linky; __fakeVoices: [string, number, number][] };
		const v = w.__harmonyVoice;
		w.__fakeVoices = [['bia', 3000, 10]];
		const agora = Date.now();
		v.linkSamples = [
			{ at: agora - 60_000, counts: new Map([['bia', { packets: 0, lost: 0 }]]) },
			{ at: agora - 1000, counts: new Map([['bia', { packets: 2950, lost: 0 }]]) },
		];
		v.linkOut = null;
		return (await v.link())!.lost;
	});
	// Against the second-old sample: 10 lost of 60. Against the minute: 0%.
	expect(perda).toBe(17);
	await ana.close();
});

// A reading the browser was still working on when you left belongs to the
// connection you left: it must not come back as the new one's.
test('uma leitura de antes de sair não volta depois', async ({ browser }) => {
	const ana = await Person.create(browser, 'm12ana');
	await ana.joinVoice();
	await expect(ana.page.locator('[data-quality]').first()).toBeVisible({ timeout: 30_000 });
	const depois = await ana.page.evaluate(async () => {
		const v = (window as unknown as { __harmonyVoice: Linky }).__harmonyVoice;
		let solta: () => void = () => {};
		const segura = new Promise<void>((r) => (solta = r));
		v.pc.getStats = async () => {
			await segura;
			return new Map<string, unknown>([
				['p', { type: 'candidate-pair', state: 'succeeded', currentRoundTripTime: 0.02 }],
			]) as unknown as RTCStatsReport;
		};
		v.linkOut = null;
		v.linkPending = null;
		const lendo = v.link();
		await v.leave();
		solta();
		const lida = await lendo;
		return { lida, cache: v.linkOut, pendente: v.linkPending };
	});
	expect(depois).toEqual({ lida: null, cache: null, pendente: null });
	await ana.close();
});
