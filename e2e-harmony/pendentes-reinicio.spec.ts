import { test, expect, Person } from './fixtures';
import { restart } from './server.mjs';

const state = (who: Person) => who.page.evaluate(() => (window as any).__harmonyVoice.snapshot());
const audioBytes = (who: Person) => who.page.evaluate(async () => {
	const stats = await (window as any).__harmonyPC.getStats();
	let bytes = 0;
	stats.forEach((s: any) => {
		if (s.type === 'inbound-rtp' && s.kind === 'audio') bytes += s.bytesReceived ?? 0;
	});
	return bytes;
});

test('a real server restart restores both room members and flowing audio within 15 seconds', async ({ browser }, info) => {
	const ana = await Person.create(browser, 'restartana');
	const bru = await Person.create(browser, 'restartbru');
	await ana.joinVoice();
	await bru.joinVoice();
	await expect.poll(() => audioBytes(ana)).toBeGreaterThan(0);
	const oldAna = (await state(ana)).sessionID;
	const oldBru = (await state(bru)).sessionID;
	const started = Date.now();
	await restart();
	await expect.poll(async () => {
		const a = await state(ana), b = await state(bru);
		return a.state === 'live' && b.state === 'live' && a.sessionID !== oldAna && b.sessionID !== oldBru;
	}, { timeout: 12_000 }).toBe(true);
	const a = await audioBytes(ana), b = await audioBytes(bru);
	await expect.poll(async () => (await audioBytes(ana)) > a && (await audioBytes(bru)) > b,
		{ timeout: 3000 }).toBe(true);
	const elapsedMs = Date.now() - started;
	expect(elapsedMs).toBeLessThan(15_000);
	await info.attach('restart-timing.json', { body: JSON.stringify({ elapsedMs }), contentType: 'application/json' });
	await ana.close();
	await bru.close();
});

test('gateway resync repairs a lone session before ICE fails and retries four unavailable joins', async ({ browser }) => {
	const ana = await Person.create(browser, 'restartsolo');
	const bru = await Person.create(browser, 'restartlater');
	await ana.joinVoice();
	await expect.poll(async () => (await state(ana)).state).toBe('live');
	const old = (await state(ana)).sessionID;
	// Make the gateway carry the proof: ICE may take longer to notice a loss.
	await ana.page.evaluate(() => {
		const pc = (window as any).__harmonyPC;
		pc.oniceconnectionstatechange = null;
		pc.onconnectionstatechange = null;
	});
	let joins = 0;
	await ana.context.route(/\/api\/voice\/[^/]+\/join$/, (route) => {
		if (++joins <= 4) return route.fulfill({ status: 503, contentType: 'application/json', body: '{"error":"voice_unavailable"}' });
		return route.continue();
	});
	const started = Date.now();
	await restart();
	await expect.poll(async () => {
		const s = await state(ana);
		return s.state === 'live' && s.sessionID !== old;
	}, { timeout: 22_000 }).toBe(true);
	expect(joins).toBe(5);
	await bru.joinVoice();
	await expect.poll(() => audioBytes(ana), { timeout: 3000 }).toBeGreaterThan(0);
	expect(Date.now() - started).toBeLessThan(25_000);
	await ana.close();
	await bru.close();
});

test('leaving during an unavailable rejoin cancels further attempts', async ({ browser }) => {
	const ana = await Person.create(browser, 'restartleave');
	await ana.joinVoice();
	let joins = 0;
	await ana.context.route(/\/api\/voice\/[^/]+\/join$/, (route) => {
		joins++;
		return route.fulfill({ status: 503, contentType: 'application/json', body: '{"error":"voice_unavailable"}' });
	});
	await ana.page.evaluate(() => {
		const v = (window as any).__harmonyVoice;
		v.reconcile({});
	});
	await expect.poll(() => joins).toBe(1);
	await ana.page.evaluate(() => (window as any).__harmonyVoice.leave());
	await ana.page.waitForTimeout(5500);
	expect(joins).toBe(1);
	expect((await state(ana)).state).toBe('off');
	await ana.close();
});
