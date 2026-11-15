import { test, expect, Person } from './fixtures';
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

for (const type of ['wifi', undefined]) {
	test(`bandwidth change with ${type ?? 'unavailable'} connection type keeps healthy voice live`, async ({ browser }) => {
		const ana = await Person.create(browser, type ? 'bandana' : 'notypeana');
		await ana.context.addInitScript((type) => {
			const connection = Object.assign(new EventTarget(), { type, downlink: 10, rtt: 50, effectiveType: '4g' });
			Object.defineProperty(navigator, 'connection', { value: connection, configurable: true });
		}, type);
		await ana.page.reload();
		const bruno = await Person.create(browser, type ? 'bandbruno' : 'notypebruno');
		await bruno.joinVoice();
		await ana.joinVoice();
		await expect.poll(() => ana.page.evaluate(() => (window as any).__harmonyVoice.snapshot().state)).toBe('live');
		let restarts = 0;
		await ana.context.route('**/api/voice/restart', (route) => { restarts++; return route.continue(); });
		const before = await ana.diag();
		await ana.page.evaluate(() => {
			const w = window as any;
			w.__networkStates = [];
			w.__harmonyVoice.subscribe(() => w.__networkStates.push(w.__harmonyVoice.snapshot()));
			const connection = (navigator as any).connection;
			Object.assign(connection, { downlink: 1, rtt: 200, effectiveType: '3g' });
			connection.dispatchEvent(new Event('change'));
		});
		await ana.page.waitForTimeout(1000);
		expect(restarts).toBe(0);
		expect((await ana.diag()).slice(before.length)).not.toMatch(/ICE restart requested/);
		expect(await ana.page.evaluate(() => {
			const w = window as any;
			return w.__harmonyVoice.snapshot().state === 'live' &&
				w.__networkStates.every((s: any) => s.state === 'live' && s.notice !== 'Reconectando…');
		})).toBe(true);
		if (type) {
			// A real interface hint repairs ICE, but the request itself must
			// leave the healthy view alone until ICE reports a loss.
			expect(await ana.page.evaluate(() => {
				const connection = (navigator as any).connection;
				connection.type = 'cellular';
				connection.dispatchEvent(new Event('change'));
				const state = (window as any).__harmonyVoice.snapshot();
				return state.state === 'live' && state.notice !== 'Reconectando…';
			})).toBe(true);
			await expect.poll(() => restarts).toBe(1);
			await expect.poll(() => ana.diag()).toMatch(/ICE restart connected in/);
			await ana.page.evaluate(() => (navigator as any).connection.dispatchEvent(new Event('change')));
			await ana.page.waitForTimeout(500);
			expect(restarts).toBe(1);
		}
		await ana.close();
		await bruno.close();
	});
}

for (const missing of [false, true]) {
	test(`voice restart ${missing ? 'missing session immediately' : 'timeout'} rejoins automatically while preserving camera and screen capture`, async ({ browser }, info) => {
		const ana = await Person.create(browser, missing ? 'missingana' : 'fallbackana');
		const bruno = await Person.create(browser, missing ? 'missingbruno' : 'fallbackbruno');
		await ana.joinVoice();
		await bruno.joinVoice();
		await ana.startCamera();
		await ana.shareScreen();
		await bruno.watchScreen();
		await expect.poll(async () => (await bruno.videos()).filter((v) => v.w > 0 && v.live).length, { timeout: 20_000 }).toBeGreaterThan(0);
		await ana.page.evaluate(() => {
			const w = window as any;
			w.__fallbackBefore = {
				sid: w.__harmonyVoice.snapshot().sessionID,
				cam: w.__harmonyVoice.outgoing.get('cam'),
				screen: w.__harmonyVoice.outgoing.get('screen'),
			};
		});
		let restartRequests = 0;
		let rejoinAt = 0;
		ana.page.on('request', (request) => {
			if (/\/api\/voice\/[^/]+\/join$/.test(request.url())) rejoinAt = Date.now();
		});
		await ana.context.route('**/api/voice/restart', (route) => {
			restartRequests++;
			return route.fulfill({
				status: missing ? 404 : 503, contentType: 'application/json',
				body: missing ? '{"error":"not_found"}' : '{"error":"voice_unavailable"}',
			});
		});
		if (!missing) await ana.page.evaluate(() => { (window as any).__harmonyPC.getStats = async () => new Map(); });
		const startedAt = Date.now();
		await ana.page.evaluate(() => window.dispatchEvent(new Event('online')));
		if (missing) {
			await expect.poll(() => rejoinAt, { timeout: 2000 }).toBeGreaterThan(0);
			expect(rejoinAt - startedAt).toBeLessThan(2000);
			expect(restartRequests).toBe(1);
			await info.attach('missing-session-rejoin.json', {
				body: JSON.stringify({ rejoinAfterMs: rejoinAt - startedAt, restartRequests }), contentType: 'application/json',
			});
		}
		await expect.poll(() => ana.diag(), { timeout: 20_000 }).toMatch(/fallback resumed existing capture tracks/);
		expect(await ana.page.evaluate(() => {
			const w = window as any;
			const state = w.__harmonyVoice.snapshot();
			return state.sessionID !== w.__fallbackBefore.sid && state.sharingCamera && state.sharingScreen &&
				w.__harmonyVoice.outgoing.get('cam') === w.__fallbackBefore.cam &&
				w.__harmonyVoice.outgoing.get('screen') === w.__fallbackBefore.screen &&
				w.__fallbackBefore.cam.readyState === 'live' && w.__fallbackBefore.screen.readyState === 'live';
		})).toBe(true);
		await expect.poll(async () => (await bruno.videos()).some((v) => v.w > 0 && v.live), { timeout: 10_000 }).toBe(true);
		await ana.close();
		await bruno.close();
	});
}

test('network return restarts ICE without replacing the voice session or camera', async ({ browser }, info) => {
	const ana = await Person.create(browser, 'netana');
	const bruno = await Person.create(browser, 'netbruno');
	await ana.joinVoice();
	await bruno.joinVoice();
	await bruno.startCamera();
	await expect.poll(async () => (await ana.videos()).some((v) => v.w > 0 && v.live)).toBe(true);
	await ana.page.evaluate(() => {
		const w = window as any;
		w.__networkBefore = { pc: w.__harmonyPC, sid: w.__harmonyVoice.snapshot().sessionID };
	});
	const cdp = await ana.context.newCDPSession(ana.page);
	await cdp.send('Network.enable');
	const cutAt = Date.now();
	await cdp.send('Network.emulateNetworkConditions', {
		offline: true, latency: 0, downloadThroughput: -1, uploadThroughput: -1,
	});
	await expect.poll(() => ana.page.evaluate(() => navigator.onLine)).toBe(false);
	await ana.page.waitForTimeout(1000);
	const onlineAt = Date.now();
	await cdp.send('Network.emulateNetworkConditions', {
		offline: false, latency: 0, downloadThroughput: -1, uploadThroughput: -1,
	});
	const baseline = process.env.HARMONY_RECONNECTION_BASELINE === '1';
	if (baseline) await ana.page.waitForTimeout(1000);
	else await expect.poll(() => ana.diag(), { timeout: 15_000 }).toMatch(/ICE restart connected in/);
	const iceAt = baseline ? null : Date.now();
	expect(await ana.page.evaluate(() => {
		const w = window as any;
		return w.__networkBefore.pc === w.__harmonyPC && w.__networkBefore.sid === w.__harmonyVoice.snapshot().sessionID;
	})).toBe(true);
	// CDP takes HTTP offline; UDP may keep flowing. Verify real media counters
	// after recovery, but never call this a measured outage of the media route.
	const counters = () => ana.page.evaluate(async () => {
		const stats = await (window as any).__harmonyPC.getStats();
		const out = { audio: 0, video: 0 };
		stats.forEach((s: any) => {
			if (s.type === 'inbound-rtp' && (s.kind === 'audio' || s.kind === 'video')) {
				out[s.kind as 'audio' | 'video'] += s.bytesReceived ?? 0;
			}
		});
		return out;
	});
	const before = await counters();
	await expect.poll(async () => {
		const now = await counters();
		return now.audio > before.audio && now.video > before.video;
	}, { timeout: 10_000 }).toBe(true);
	await info.attach('network-timing.json', {
		body: JSON.stringify({ scenario: 'CDP HTTP offline; UDP outage not established', cutToICEms: iceAt === null ? null : iceAt - cutAt, onlineToICEms: iceAt === null ? null : iceAt - onlineAt }),
		contentType: 'application/json',
	});
	await info.attach('registro.txt', { body: await ana.diag(), contentType: 'text/plain' });
	await info.attach('recovered.png', { body: await ana.page.screenshot(), contentType: 'image/png' });
	const output = process.env.HARMONY_RECONNECTION_RESULTS_DIR;
	if (output) {
		mkdirSync(output, { recursive: true });
		writeFileSync(join(output, 'network-timing.json'), JSON.stringify({ scenario: 'CDP HTTP offline; UDP outage not established', cutToICEms: iceAt === null ? null : iceAt - cutAt, onlineToICEms: iceAt === null ? null : iceAt - onlineAt }));
		writeFileSync(join(output, 'registro-web.txt'), await ana.diag());
		writeFileSync(join(output, 'recovered-web.png'), await ana.page.screenshot());
	}
	await ana.close();
	await bruno.close();
});


test('restart completes with inbound media and no ICE state transition', async ({ browser }) => {
	const ana = await Person.create(browser, 'steadyana');
	const bruno = await Person.create(browser, 'steadybruno');
	await ana.joinVoice();
	await bruno.joinVoice();
	await bruno.startCamera();
	await expect.poll(async () => (await ana.videos()).some((v) => v.w > 0 && v.live)).toBe(true);
	await ana.page.evaluate(() => {
		const w = window as any;
		const pc = w.__harmonyPC;
		w.__steadyBefore = { pc, sid: w.__harmonyVoice.snapshot().sessionID };
		Object.defineProperty(pc, 'iceConnectionState', { configurable: true, get: () => 'connected' });
		pc.oniceconnectionstatechange = null;
		pc.onconnectionstatechange = null;
		// Exercise post-application samples, not spontaneous recovery while HTTP waits.
		const stats = pc.getStats.bind(pc);
		pc.getStats = () => w.__harmonyVoice.restartApplied ? stats() : Promise.resolve(new Map());
		window.dispatchEvent(new Event('online'));
	});
	await expect.poll(() => ana.diag()).toMatch(/same session; inbound RTP growing; applied=true/);
	await ana.page.evaluate(() => window.dispatchEvent(new Event('online')));
	await ana.page.waitForTimeout(9000);
	expect(await ana.page.evaluate(() => {
		const w = window as any;
		return w.__harmonyPC === w.__steadyBefore.pc && w.__harmonyVoice.snapshot().sessionID === w.__steadyBefore.sid &&
			w.__harmonyVoice.snapshot().state === 'live' && !w.__harmonyVoice.restartAt;
	})).toBe(true);
	expect(await ana.diag()).not.toMatch(/fallback rejoin/);
	expect((await ana.diag()).match(/ICE restart requested/g)).toHaveLength(1);
	await ana.close();
	await bruno.close();
});

test('deadline checks fresh inbound media before fallback even with restart HTTP failing', async ({ browser }) => {
	const ana = await Person.create(browser, 'deadlineana');
	const bruno = await Person.create(browser, 'deadlinebruno');
	await ana.joinVoice();
	await bruno.joinVoice();
	await bruno.startCamera();
	await expect.poll(async () => (await ana.videos()).some((v) => v.w > 0 && v.live)).toBe(true);
	await ana.context.route('**/api/voice/restart', (route) => route.fulfill({
		status: 503, contentType: 'application/json', body: '{"error":"voice_unavailable"}',
	}));
	await ana.page.evaluate(() => {
		const w = window as any;
		w.__deadlineBefore = { pc: w.__harmonyPC, sid: w.__harmonyVoice.snapshot().sessionID };
		// Keep recovery pending until the real eight-second deadline.
		w.__harmonyVoice.pollRestart = async () => undefined;
		window.dispatchEvent(new Event('online'));
	});
	await ana.page.waitForTimeout(7500);
	expect(await ana.page.evaluate(() => !!(window as any).__harmonyVoice.restartAt)).toBe(true);
	await expect.poll(() => ana.diag(), { timeout: 4000 }).toMatch(/same session; inbound RTP growing; applied=false/);
	expect(await ana.diag()).not.toMatch(/fallback rejoin/);
	expect(await ana.page.evaluate(() => {
		const w = window as any;
		return w.__harmonyPC === w.__deadlineBefore.pc && w.__harmonyVoice.snapshot().sessionID === w.__deadlineBefore.sid &&
			w.__harmonyVoice.snapshot().state === 'live';
	})).toBe(true);
	await ana.close();
	await bruno.close();
});


test('spontaneous media recovery cancels restart and still answers a late server offer', async ({ browser }) => {
	const ana = await Person.create(browser, 'lateana');
	const bruno = await Person.create(browser, 'latebruno');
	await ana.joinVoice();
	await bruno.joinVoice();
	await bruno.startCamera();
	await expect.poll(async () => (await ana.videos()).some((v) => v.w > 0 && v.live)).toBe(true);
	let answers = 0;
	let delivered = false;
	let releaseRestart!: () => void;
	const recovered = new Promise<void>((resolve) => { releaseRestart = resolve; });
	ana.page.on('response', (response) => {
		if (delivered && response.url().endsWith('/api/voice/answer') && response.status() === 204) answers++;
	});
	await ana.context.route('**/api/voice/restart', async (route) => {
		// Delay the request reaching the server. Creating fresh ICE credentials
		// immediately clears Pion's selected pair, so delaying only the response
		// makes spontaneous recovery depend on buffered RTP/stats and suite timing.
		await recovered;
		const response = await route.fetch();
		delivered = true;
		await route.fulfill({ response });
	});
	await ana.page.evaluate(() => {
		const w = window as any;
		w.__lateBefore = { pc: w.__harmonyPC, sid: w.__harmonyVoice.snapshot().sessionID };
		Object.defineProperty(w.__harmonyPC, 'iceConnectionState', { configurable: true, get: () => 'connected' });
		w.__harmonyPC.oniceconnectionstatechange = null;
		window.dispatchEvent(new Event('online'));
	});
	try {
		await expect.poll(() => ana.diag()).toMatch(/same session; inbound RTP growing; applied=false/);
	} finally {
		releaseRestart();
	}
	await expect.poll(() => answers).toBeGreaterThan(0);
	expect(await ana.page.evaluate(() => {
		const w = window as any;
		return !w.__harmonyVoice.restartAt && w.__lateBefore.pc === w.__harmonyPC &&
			w.__lateBefore.sid === w.__harmonyVoice.snapshot().sessionID;
	})).toBe(true);
	expect(await ana.diag()).not.toMatch(/fallback rejoin/);
	await ana.close();
	await bruno.close();
});
