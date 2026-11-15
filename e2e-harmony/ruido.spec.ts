import type { Page } from '@playwright/test';
import { Person, expect, test } from './fixtures';

// The noise suppressor (DeepFilterNet since 25/09/2026, suppressor.ts),
// measured where it matters: on the other side of the room.
//
// Until 25/09/2026 the only test of it read the diagnostic log for "microfone
// refeito pela limpeza" — a sentence the app writes about itself. The owner
// then reported that "basicamente todos [os ruídos] estão passando", and a test
// that asks the app whether it cleaned the voice cannot disagree with the app.
// This one gives the speaker a microphone that plays nothing but a loud fan and
// has the listener measure what arrives, with the suppressor on and then off.
//
// Chrome only. Firefox is the owner's browser, but it will not reach a media
// server on the loopback address from Windows even with its loopback
// preference on, so it never joins the room here. The suppressor itself was
// measured in Firefox 155 outside the app the same day: a fan at -30 dB came
// out at -87 dB.

// A fan: brown noise with a hum, looped, about -26 dBFS. It replaces the
// microphone for audio-only requests; everything else goes to the real thing.
function fanMicrophone(): void {
	const md = navigator.mediaDevices;
	const real = md.getUserMedia.bind(md);
	md.getUserMedia = async (c?: MediaStreamConstraints): Promise<MediaStream> => {
		if (!c?.audio || c.video) return real(c);
		const ctx = new AudioContext();
		await ctx.resume().catch(() => undefined);
		const sr = ctx.sampleRate;
		const buf = ctx.createBuffer(1, sr * 4, sr);
		const d = buf.getChannelData(0);
		let b = 0;
		for (let i = 0; i < d.length; i++) {
			b = 0.98 * b + 0.2 * (Math.random() * 2 - 1);
			d[i] = 0.05 * (b + 0.3 * Math.sin((2 * Math.PI * 120 * i) / sr));
		}
		const src = ctx.createBufferSource();
		src.buffer = buf;
		src.loop = true;
		const dst = ctx.createMediaStreamDestination();
		src.connect(dst);
		src.start();
		return dst.stream;
	};
}

// heard is the mean power of every voice arriving at this page over a few
// seconds, in dB, read off the decoded audio itself.
//
// Not from the connection's stats: totalAudioEnergy is counted at playout, and
// a headless browser with no sound card never plays anything out, so it reads
// silence whatever arrives.
async function heard(page: Page, seconds = 3): Promise<number> {
	return page.evaluate(async (seconds) => {
		const pc = (window as any).__harmonyPC as RTCPeerConnection;
		const ctx = new AudioContext();
		await ctx.resume();
		const taps = pc
			.getReceivers()
			.map((r) => r.track)
			.filter((t) => t.kind === 'audio' && t.readyState === 'live')
			.map((t) => {
				const a = ctx.createAnalyser();
				a.fftSize = 2048;
				ctx.createMediaStreamSource(new MediaStream([t])).connect(a);
				return a;
			});
		const buf = new Float32Array(2048);
		let sum = 0;
		let n = 0;
		const end = performance.now() + seconds * 1000;
		while (performance.now() < end) {
			await new Promise((r) => setTimeout(r, 40));
			for (const a of taps) {
				a.getFloatTimeDomainData(buf);
				for (const v of buf) sum += v * v;
				n += buf.length;
			}
		}
		await ctx.close();
		return 10 * Math.log10(sum / Math.max(1, n) + 1e-12);
	}, seconds);
}

const diag = (p: Person) => p.page.evaluate(() => (window as any).__harmonyDiag() as string);

test('o ventilador de quem fala não chega em quem ouve', async ({ browser }) => {
	const falante = await Person.create(browser, 'ventilador');
	const ouvinte = await Person.create(browser, 'ouvido');
	await falante.page.addInitScript(fanMicrophone);
	await falante.page.reload();
	await falante.page.locator('button[aria-label="pessoas"]').waitFor({ timeout: 20_000 });

	for (const p of [ouvinte, falante]) {
		await p.openChannels();
		await p.page.locator('button[title*="entrar na sala" i]:visible').first().click();
		await p.page.locator('[data-call="live"]').waitFor({ timeout: 60_000 });
	}
	await expect.poll(() => diag(falante), { timeout: 30_000 }).toContain('microfone refeito pela limpeza');
	// DeepFilterNet, not the fallback: RNNoise also clears a fan, so the
	// numbers below alone would not notice the fallback taking over.
	expect(await diag(falante)).toContain('supressor: DeepFilterNet pronto');
	expect(await diag(falante)).not.toContain('sem DeepFilterNet');
	await falante.page.waitForTimeout(2_000);
	const on = await heard(ouvinte.page);

	await falante.page.evaluate(() => (window as any).__harmonyVoice.setMicFilter('rnnoise', false));
	await expect.poll(() => diag(falante), { timeout: 15_000 }).toContain('filtro rnnoise desligado');
	await falante.page.waitForTimeout(2_000);
	const off = await heard(ouvinte.page);

	const rate = await falante.page.evaluate(() => (window as any).__harmonyVoice.ctx?.sampleRate);
	console.log(`ruido: com filtro ${on.toFixed(1)} dB, sem ${off.toFixed(1)} dB, contexto ${rate} Hz`);
	// With nothing done to it the fan arrives near -26 dB. A suppressor that
	// works takes most of it away; 15 dB is a floor well under what the bench
	// measured (46 dB), and well over what a filter that is not there does (0).
	expect(off, 'sem filtro o ventilador tem de chegar, ou a medida não mede nada').toBeGreaterThan(-45);
	expect(off - on, 'o filtro tirou menos de 15 dB do ventilador').toBeGreaterThan(15);

	await falante.close();
	await ouvinte.close();
});
