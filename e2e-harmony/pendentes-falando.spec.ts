import { test, expect, Person } from './fixtures';

test('self and remote microphones light for quiet float samples while live audio does not', async ({ browser }) => {
	const ana = await Person.create(browser, 'quietmeterana');
	const result = await ana.page.evaluate(() => {
		const v = (window as any).__harmonyVoice;
		// Exercise the production readMeters path without capture or playback:
		// samples at 0.0011 cannot be represented by the old byte analyser.
		v.frame = new Float32Array(512);
		const meter = { fftSize: 512, getFloatTimeDomainData: (buf: Float32Array) => buf.fill(0.0011) };
		v.meters.set('eu', meter);
		v.meters.set('remote-mic', meter);
		v.meters.set('live-sound', meter);
		v.info.set('remote-mic', { username: 'remote', source: 'mic' });
		v.info.set('live-sound', { username: 'film', source: 'screen_audio' });
		v.readMeters();
		const talking = [...v.talking].sort();
		v.meters.clear();
		v.info.clear();
		v.lastLoud.clear();
		return talking;
	});
	expect(result).toEqual(['quietmeterana', 'remote']);
	await ana.close();
});
