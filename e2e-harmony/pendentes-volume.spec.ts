import { test, expect, Person } from './fixtures';

test('a watched live has a hover and keyboard volume affecting only screen audio', async ({ browser }) => {
	const ana = await Person.create(browser, 'livevolumeana');
	const bru = await Person.create(browser, 'livevolumebru');
	await ana.joinVoice();
	await bru.joinVoice();
	await bru.shareScreen();
	await ana.watchScreen();
	const tile = ana.page.locator('[data-tile]').filter({ has: ana.page.locator('[data-live-volume]') }).first();
	const overlay = tile.locator('[data-live-volume]');
	const slider = overlay.locator('input[type="range"]');
	await expect(slider).toHaveCount(1);
	await ana.page.mouse.move(0, 0);
	await expect(overlay).toHaveCSS('opacity', '0');
	await tile.hover();
	await expect(overlay).toHaveCSS('opacity', '1');
	await ana.page.mouse.move(0, 0);
	await expect(overlay).toHaveCSS('opacity', '0');
	// Tab from the picture through its controls: opacity must not remove the
	// live slider from the keyboard order, and focusing must reveal it.
	await tile.locator('> div > button').first().focus();
	for (let i = 0; i < 12 && !(await slider.evaluate((el) => document.activeElement === el)); i++) {
		await ana.page.keyboard.press('Tab');
	}
	await expect(slider).toBeFocused();
	await expect(overlay).toHaveCSS('opacity', '1');
	await ana.page.keyboard.press('Home');
	await ana.page.keyboard.press('ArrowRight');
	await expect.poll(() => ana.page.evaluate((username) => {
		const voice = (window as any).__harmonyVoice;
		return { screen: voice.volume(username, 'screen_audio'), mic: voice.volume(username, 'mic') };
	}, bru.username)).toEqual({ screen: 0.05, mic: 1 });
	// Inspect the actual audio gain, not just the stored preference.
	await expect.poll(() => ana.page.evaluate((username) => {
		const voice = (window as any).__harmonyVoice;
		return [...voice.info].filter(([, v]: any) => v.username === username && v.source === 'screen_audio')
			.map(([mid]: any) => voice.gains.get(mid)?.gain.value);
	}, bru.username)).toEqual([expect.closeTo(0.05, 5)]);
	await ana.close();
	await bru.close();
});
