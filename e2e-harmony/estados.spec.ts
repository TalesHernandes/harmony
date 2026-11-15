// Rodada de 24/09/2026: dizer o que está acontecendo de verdade. A coluna
// dizia "Conectando…" do toque na sala até a voz ligar, e um anexo cujo
// arquivo se perdeu aparecia como um nome solto, como se ainda carregasse.
import { test, expect, Person } from './fixtures';

test('entrar na sala diz a etapa em que está, e não só "Conectando…"', async ({ browser }) => {
	const ana = await Person.create(browser, 'estana' + test.info().repeatEachIndex);
	// The server's answer is held back, so the one wait worth seeing lasts
	// long enough to be read.
	let release = () => {};
	const held = new Promise<void>((r) => (release = r));
	await ana.page.route('**/api/voice/*/join', async (route) => {
		await held;
		await route.continue();
	});
	await ana.openChannels();
	await ana.page.locator('button[title*="entrar na sala" i]:visible').first().click();

	const bar = ana.page.locator('nav');
	await expect(bar.getByText('Entrando na sala…')).toBeVisible({ timeout: 10_000 });
	await expect(bar.getByText('Conectando…')).toHaveCount(0);

	release();
	await ana.page.locator('[data-call="live"]').waitFor({ timeout: 30_000 });
	await expect(bar.getByText('Voz conectada')).toBeVisible();

	await ana.close();
});

test('um anexo cujo arquivo se perdeu diz que se perdeu', async ({ browser }) => {
	const ana = await Person.create(browser, 'estgone' + test.info().repeatEachIndex);
	await ana.openChannels();
	// The message list as the server now sends it for the pictures lost in
	// the move: the row is there, and the bytes are not.
	await ana.page.route('**/api/channels/*/messages*', async (route) => {
		const res = await route.fetch();
		const body = await res.json();
		body.messages = [
			...(body.messages ?? []),
			{
				id: 999999,
				channel_id: body.messages?.[0]?.channel_id ?? '',
				author: { username: ana.username, display_name: ana.username },
				body: 'harmony…',
				created_at: new Date().toISOString(),
				attachments: [
					{
						id: 'perdidoAAAAAAAAA',
						name: 'imagem_2026-09-11_111236810.png',
						mime: 'image/png',
						size: 74153,
						width: 0,
						height: 0,
						url: '/api/files/perdidoAAAAAAAAA',
						missing: true,
					},
				],
				mentions: [],
				reactions: [],
			},
		];
		await route.fulfill({ response: res, json: body });
	});
	await ana.page.reload();

	const gone = ana.page.locator('[data-gone="perdidoAAAAAAAAA"]');
	await expect(gone).toBeVisible({ timeout: 15_000 });
	await expect(gone).toContainText('não está mais no servidor');
	// Not drawn as a picture that is still coming.
	await expect(ana.page.locator('[data-image="perdidoAAAAAAAAA"]')).toHaveCount(0);

	await ana.close();
});
