// Sprint D4b — the direct call, with two real browsers.
//
// This is the one thing no Go test can answer. The server's part of a direct
// call is an introduction, and the Go tests prove the introduction is private
// and is not read; what is left over is the part that matters to the two people
// on the call, which is whether the audio and the picture actually arrive —
// **without going through the server at all**.
//
// So the assertions here are about the connection itself: that it reaches
// "connected", that bytes are flowing over it, and that the pair of addresses it
// chose is a direct one. If a future change quietly routed a call through the
// media server, the third of those is what would catch it.

import { test, expect, Person } from './fixtures';

// pc reads something off the connection the call is actually using. It is the
// same object the app holds; direct.ts publishes it for exactly this.
async function connectionState(who: Person): Promise<string> {
	return who.page.evaluate(
		() => (window as unknown as { __harmonyDirectPC?: RTCPeerConnection }).__harmonyDirectPC?.connectionState ?? 'none',
	);
}

// flowing reports how many bytes have arrived over the connection, and by which
// kind of path. "host" and "srflx" are direct; "relay" would mean a TURN server
// in the middle, which this test does not have and must not need.
async function flowing(who: Person): Promise<{ bytes: number; path: string }> {
	return who.page.evaluate(async () => {
		const pc = (window as unknown as { __harmonyDirectPC?: RTCPeerConnection }).__harmonyDirectPC;
		if (!pc) return { bytes: 0, path: 'none' };
		const stats = await pc.getStats();
		let bytes = 0;
		let path = 'none';
		const byID = new Map<string, any>();
		stats.forEach((r) => byID.set(r.id, r));
		stats.forEach((r: any) => {
			if (r.type === 'inbound-rtp') bytes += r.bytesReceived ?? 0;
			if (r.type === 'candidate-pair' && r.state === 'succeeded' && r.nominated) {
				const local = byID.get(r.localCandidateId);
				if (local) path = local.candidateType;
			}
		});
		return { bytes, path };
	});
}

// openDMWith opens the private conversation with somebody, from their row in
// the list of people — the way a person does it.
async function openDMWith(who: Person, username: string): Promise<void> {
	const row = who.page.locator(`aside button[data-person="${username}"]`).first();
	// Ask the *panel* whether it is open, not the row. The list of people is
	// drawn empty and filled when the server answers, so a row that is not
	// there yet is not a closed panel — and this used to read it as one, press
	// "pessoas", and close the panel that was already open. It then waited ten
	// seconds for a row it had just hidden. That lost its coin flip once in
	// fifty runs, which is the worst kind of test.
	const panel = who.page.locator('aside');
	if (!(await panel.isVisible())) {
		await who.page.locator('button[aria-label="pessoas"]').first().click();
		await expect(panel).toBeVisible({ timeout: 10_000 });
	}
	await row.waitFor({ timeout: 15_000 });
	await row.click();
	await who.page
		.getByRole('dialog')
		.getByRole('button', { name: /conversar no privado/i })
		.click();
}

test('simultaneous network changes preserve a direct call through perfect negotiation', async ({ browser }) => {
	const ana = await Person.create(browser, 'iceana' + test.info().repeatEachIndex);
	const bruno = await Person.create(browser, 'icebruno' + test.info().repeatEachIndex);
	await openDMWith(ana, bruno.username);
	await ana.page.locator('[data-call-start]').click();
	await bruno.page.locator('[data-call-accept]').click();
	for (const who of [ana, bruno]) {
		await expect.poll(() => connectionState(who), { timeout: 30_000 }).toBe('connected');
		await who.page.evaluate(() => {
			const w = window as any;
			w.__directBefore = { pc: w.__harmonyDirectPC, sdp: w.__harmonyDirectPC.localDescription.sdp };
		});
		// Hold the replacement gateway while HTTP signalling still succeeds:
		// offers sent in that gap have no receiving socket and must be replayed.
		await who.context.routeWebSocket(/\/ws$/, async (ws) => {
			await new Promise((resolve) => setTimeout(resolve, 1000));
			ws.connectToServer();
		});
	}
	await Promise.all([ana, bruno].map((who) => who.page.evaluate(() => window.dispatchEvent(new Event('online')))));
	for (const who of [ana, bruno]) {
		await expect.poll(() => who.page.evaluate(() => {
			const w = window as any;
			const pc = w.__harmonyDirectPC;
			const credential = (sdp: string) => sdp.match(/a=ice-ufrag:([^\r\n]+)/)?.[1];
			return pc === w.__directBefore.pc && pc.signalingState === 'stable' &&
				credential(pc.localDescription.sdp) !== credential(w.__directBefore.sdp);
		}), { timeout: 15_000 }).toBe(true);
		const before = (await flowing(who)).bytes;
		await expect.poll(async () => (await flowing(who)).bytes, { timeout: 10_000 }).toBeGreaterThan(before);
	}
	await ana.close();
	await bruno.close();
});

test('uma liga para a outra, atende, e os dois se falam direto', async ({ browser }) => {
	const ana = await Person.create(browser, 'ligana');
	const bruno = await Person.create(browser, 'ligbru');

	// The button lives in the conversation with a person, because calling is
	// something you do to a person and not to a channel.
	await openDMWith(ana, 'ligbru');
	const callButton = ana.page.locator('[data-call-start]');
	await expect(callButton).toBeVisible({ timeout: 15_000 });
	await callButton.click();

	// It rings over there, on the screen he already has open.
	const box = bruno.page.locator('[data-incoming-call]');
	await expect(box).toBeVisible({ timeout: 20_000 });
	await expect(box).toContainText('ligana');
	await box.locator('[data-call-accept]').click();

	// Both are on the call.
	await expect(ana.page.locator('[data-call-screen]')).toBeVisible({ timeout: 20_000 });
	await expect(bruno.page.locator('[data-call-screen]')).toBeVisible({ timeout: 20_000 });

	// And the connection is up on both sides. This is the assertion the whole
	// sprint is about: two browsers found each other.
	for (const who of [ana, bruno]) {
		await expect.poll(() => connectionState(who), { timeout: 30_000 }).toBe('connected');
	}

	// Sound is arriving, and it did not go through anybody's server: a relayed
	// path would say "relay", and there is no relay in this test at all.
	for (const who of [ana, bruno]) {
		await expect
			.poll(async () => (await flowing(who)).bytes, { timeout: 30_000 })
			.toBeGreaterThan(0);
		expect((await flowing(who)).path, 'the call went through a relay').not.toBe('relay');
	}

	// The clock says it is up rather than still trying.
	await expect(ana.page.locator('[data-call-status]')).toContainText(':', { timeout: 15_000 });

	await ana.close();
	await bruno.close();
});

test('a câmera e a tela entram no meio da chamada, sem refazer a conexão', async ({ browser }) => {
	const ana = await Person.create(browser, 'telana');
	const bruno = await Person.create(browser, 'telbru');

	await openDMWith(ana, 'telbru');
	await ana.page.locator('[data-call-start]').click();
	await bruno.page.locator('[data-call-accept]').click({ timeout: 20_000 });
	await expect.poll(() => connectionState(ana), { timeout: 30_000 }).toBe('connected');

	// The connection as it is now. Everything below has to happen without this
	// changing: replaceTrack into a slot that was negotiated when the call
	// began, never a second PeerConnection (CLAUDE.md).
	const same = () =>
		ana.page.evaluate(
			() =>
				(window as unknown as { __harmonyDirectPC?: RTCPeerConnection }).__harmonyDirectPC
					?.connectionState ?? 'none',
		);

	// Her camera, then her screen.
	await ana.page.locator('[data-call-cam]').click();
	await ana.page.locator('[data-call-share]').click();

	// He sees a picture arriving — the screen, which wins over a face because it
	// is the thing being pointed at.
	await expect(bruno.page.locator('[data-call-screen] video').first()).toBeVisible({
		timeout: 25_000,
	});
	await expect
		.poll(
			async () =>
				bruno.page.evaluate(async () => {
					const pc = (window as unknown as { __harmonyDirectPC?: RTCPeerConnection })
						.__harmonyDirectPC;
					if (!pc) return 0;
					let frames = 0;
					(await pc.getStats()).forEach((r: any) => {
						if (r.type === 'inbound-rtp' && r.kind === 'video') frames += r.framesReceived ?? 0;
					});
					return frames;
				}),
			{ timeout: 30_000 },
		)
		.toBeGreaterThan(0);

	// And it is still the same connection it was before any of that.
	expect(await same()).toBe('connected');

	// Hanging up takes the call off both screens.
	await ana.page.locator('[data-call-hangup]').click();
	await expect(ana.page.locator('[data-call-screen]')).toHaveCount(0, { timeout: 15_000 });
	await expect(bruno.page.locator('[data-call-screen]')).toHaveCount(0, { timeout: 15_000 });

	await ana.close();
	await bruno.close();
});

test('recusar encerra a chamada dos dois lados', async ({ browser }) => {
	const ana = await Person.create(browser, 'recana');
	const bruno = await Person.create(browser, 'recbru');

	await openDMWith(ana, 'recbru');
	await ana.page.locator('[data-call-start]').click();
	await bruno.page.locator('[data-call-reject]').click({ timeout: 20_000 });

	await expect(bruno.page.locator('[data-incoming-call]')).toHaveCount(0, { timeout: 15_000 });
	await expect(ana.page.locator('[data-call-screen]')).toHaveCount(0, { timeout: 15_000 });

	await ana.close();
	await bruno.close();
});

// A call is not a modal. The first version of this covered the whole app until
// somebody hung up, which is the owner's complaint: "eu fico preso na tela de
// chamada". What matters is not only that the screen goes away, but that the
// call survives it going away — an unmounted <audio> is a call you can no
// longer hear.
test('dá para usar o app durante a chamada, e ela continua', async ({ browser }) => {
	const ana = await Person.create(browser, 'usoana');
	const bruno = await Person.create(browser, 'usobru');

	await openDMWith(ana, 'usobru');
	await ana.page.locator('[data-call-start]').click();
	await bruno.page.locator('[data-call-accept]').click({ timeout: 20_000 });
	await expect.poll(() => connectionState(ana), { timeout: 30_000 }).toBe('connected');

	// Out of the way.
	await ana.page.locator('[data-call-shrink]').click();
	await expect(ana.page.locator('[data-call-screen]')).toHaveCount(0);
	await expect(ana.page.locator('[data-call-strip]')).toBeVisible();

	// And the app is hers again: she writes a message while talking, and reads
	// it back in the conversation. (It is checked on her own screen, not on his:
	// he never opened this conversation, and a message in a conversation nobody
	// has open is not drawn anywhere — which is the app working, not failing.)
	await ana.writeTo('usobru', 'falando e escrevendo');
	await expect(ana.page.getByText('falando e escrevendo')).toBeVisible({ timeout: 15_000 });

	// The call never noticed. This is the assertion that matters: the sound is
	// still arriving, with the screen that was drawing it gone.
	expect(await connectionState(ana)).toBe('connected');
	await expect.poll(async () => (await flowing(ana)).bytes, { timeout: 15_000 }).toBeGreaterThan(0);

	// And back.
	await ana.page.locator('[data-call-open]').click();
	await expect(ana.page.locator('[data-call-screen]')).toBeVisible();

	await ana.close();
	await bruno.close();
});
