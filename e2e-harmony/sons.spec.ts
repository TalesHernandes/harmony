// The harmony's own sounds (29/10/2026): which sound each moment asks for.
//
// What is checked is the sound *asked for* (sounds.ts keeps the names in
// __harmonySounds), not what comes out of a speaker: every test browser runs
// muted (AGENTS.md item 8), so nothing here ever plays in anybody's headphones.

import { test, expect, Person } from './fixtures';

async function sounds(who: Person): Promise<string[]> {
	return who.page.evaluate(() => [...((window as unknown as { __harmonySounds?: string[] }).__harmonySounds ?? [])]);
}

test('a room is heard: arriving, leaving, mute and a screen', async ({ browser }, info) => {
	test.setTimeout(150_000);
	const ana = await Person.create(browser, 'somana' + info.repeatEachIndex);
	const beto = await Person.create(browser, 'sombeto' + info.repeatEachIndex);

	// Heard at the click, before the connection is up (owner, 29/10/2026).
	const joined = ana.joinVoice();
	await expect.poll(() => sounds(ana), { message: 'ana did not hear herself arrive' }).toEqual(['entrar']);
	await joined;

	await beto.joinVoice();
	await expect.poll(() => sounds(beto)).toEqual(['entrar']);
	await expect.poll(() => sounds(ana), { message: 'ana did not hear beto arrive' }).toEqual(['entrar', 'entrar']);

	await ana.page.evaluate(() => (window as any).__harmonyVoice.setMuted(true));
	await expect.poll(() => sounds(ana)).toEqual(['entrar', 'entrar', 'mutar']);
	await ana.page.evaluate(() => (window as any).__harmonyVoice.setMuted(false));
	await expect.poll(() => sounds(ana)).toEqual(['entrar', 'entrar', 'mutar', 'desmutar']);

	const base = ['entrar', 'entrar', 'mutar', 'desmutar'];
	await beto.shareScreen();
	await expect.poll(() => sounds(ana), { timeout: 20_000, message: 'ana did not hear the screen start' })
		.toEqual([...base, 'tela-comecou']);
	// Whoever shares hears it too (owner, 29/10/2026).
	await expect.poll(() => sounds(beto), { message: 'beto did not hear his own screen start' })
		.toEqual(['entrar', 'tela-comecou']);
	// The end is heard by the whole room, watching it or not.
	await beto.stopScreen();
	await expect.poll(() => sounds(ana), { timeout: 10_000, message: 'ana did not hear the screen end' })
		.toEqual([...base, 'tela-comecou', 'tela-acabou']);
	await expect.poll(() => sounds(beto)).toEqual(['entrar', 'tela-comecou', 'tela-acabou']);

	// Shared again after the blink guard, and watched: the same two sounds.
	await ana.page.waitForTimeout(1200);
	await beto.shareScreen();
	await expect.poll(() => sounds(ana), { timeout: 20_000 })
		.toEqual([...base, 'tela-comecou', 'tela-acabou', 'tela-comecou']);
	await ana.watchScreen();
	await beto.stopScreen();
	await expect.poll(() => sounds(ana), { timeout: 10_000, message: 'ana did not hear the screen she watched end' })
		.toEqual([...base, 'tela-comecou', 'tela-acabou', 'tela-comecou', 'tela-acabou']);

	await beto.page.locator('button[aria-label="sair da sala"]:visible').first().click();
	await expect.poll(() => sounds(beto))
		.toEqual(['entrar', 'tela-comecou', 'tela-acabou', 'tela-comecou', 'tela-acabou', 'sair']);
	await expect.poll(() => sounds(ana), { message: 'ana did not hear beto leave' })
		.toEqual([...base, 'tela-comecou', 'tela-acabou', 'tela-comecou', 'tela-acabou', 'sair']);
});

test('a direct call rings on both sides and is heard when answered and hung up', async ({ browser }, info) => {
	const ana = await Person.create(browser, 'somcana' + info.repeatEachIndex);
	const beto = await Person.create(browser, 'somcbeto' + info.repeatEachIndex);
	const row = ana.page.locator(`aside button[data-person="${beto.username}"]`).first();
	if (!(await ana.page.locator('aside').isVisible())) await ana.page.locator('button[aria-label="pessoas"]').first().click();
	await row.click();
	await ana.page.getByRole('dialog').getByRole('button', { name: /conversar no privado/i }).click();

	await ana.page.locator('[data-call-start]').click();
	await expect.poll(() => sounds(ana)).toEqual(['toque-chamando']);
	await expect.poll(() => sounds(beto), { timeout: 15_000 }).toEqual(['toque']);

	await beto.page.locator('[data-call-accept]').click();
	await expect.poll(() => sounds(ana), { timeout: 30_000 }).toEqual(['toque-chamando', 'entrar']);
	await expect.poll(() => sounds(beto), { timeout: 30_000 }).toEqual(['toque', 'entrar']);

	await beto.page.evaluate(() => (window as any).__harmonyDirect.toggleMute());
	await expect.poll(() => sounds(beto)).toEqual(['toque', 'entrar', 'mutar']);

	await ana.page.evaluate(() => (window as any).__harmonyDirect?.hangup?.());
	await expect.poll(() => sounds(beto), { timeout: 15_000, message: 'beto did not hear ana hang up' })
		.toEqual(['toque', 'entrar', 'mutar', 'sair']);
});
