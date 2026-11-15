// The lost offer (PROTOCOL-HARMONY §4.4). The server sends renegotiations on
// the live socket only, keeps nothing for a socket that is closed, and waits
// for the answer to an offer forever — so an offer sent while the socket was
// down left the call deaf to everybody who came in after it, with nothing on
// the screen to say so. Found while building the Android A3 (25/09/2026); the
// web had the same hole.

import type { WebSocketRoute } from '@playwright/test';
import { test, expect, Person } from './fixtures';

test('quem entra com o canal ao vivo fechado é ouvido quando ele volta', async ({ browser }) => {
	const ana = await Person.create(browser, 'o1ana');
	const bruno = await Person.create(browser, 'o1bruno');
	const carla = await Person.create(browser, 'o1carla');

	// Ana's live socket goes through the test, which can cut it and keep it
	// cut. The call itself does not use the socket, so it stays up.
	let cut = false;
	const open = new Set<WebSocketRoute>();
	await ana.context.routeWebSocket(/\/ws$/, (ws) => {
		if (cut) {
			void ws.close({ code: 1011, reason: 'teste: canal cortado' });
			return;
		}
		ws.connectToServer();
		open.add(ws);
	});
	await ana.page.reload();

	await ana.joinVoice();
	await bruno.joinVoice();
	await expect.poll(() => ana.diag(), { timeout: 15_000 }).toMatch(/recebendo mic de o1bruno/);

	cut = true;
	for (const ws of open) await ws.close({ code: 1011, reason: 'teste: canal cortado' });
	open.clear();
	await carla.joinVoice();
	// Carla's sound reaches the server; the offer that would bring it to Ana
	// has nowhere to go.
	await ana.page.waitForTimeout(3000);
	expect(await ana.diag()).not.toMatch(/recebendo mic de o1carla/);

	cut = false;
	// The outcome first: Ana hears Carla. Then why.
	await expect.poll(() => ana.diag(), { timeout: 40_000 }).toMatch(/recebendo mic de o1carla/);
	expect(await ana.diag()).toMatch(/a sala mudou com o canal ao vivo fechado/);
	await ana.page.locator('[data-call="live"]').waitFor({ timeout: 30_000 });

	await ana.close();
	await bruno.close();
	await carla.close();
});
