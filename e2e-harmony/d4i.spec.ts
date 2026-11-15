// Sprint D4i — the six holes, in two real browsers.
//
// What is here is the half that cannot be answered in Go: a Go test can prove
// that a missed call writes a row and that the search only returns what the
// asker may read, and it does. What it cannot say is whether the person sees
// any of it — whether the line appears in the conversation they are looking at,
// whether the word they typed in the search box brings back the message they
// meant, whether the file they chose arrives as something they can open.

import { test, expect, Person } from './fixtures';

test('uma chamada que ninguém atendeu vira uma linha na conversa, dos dois lados', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'perdana');
	const bruno = await Person.create(browser, 'perdbru');

	await ana.openDMWith('perdbru');
	const callButton = ana.page.locator('[data-call-start]');
	await expect(callButton).toBeVisible({ timeout: 15_000 });
	await callButton.click();

	// It rings over there and he does not answer it: he refuses, which is the
	// case somebody chose, rather than the forty-five seconds a test cannot
	// afford to wait for.
	const box = bruno.page.locator('[data-incoming-call]');
	await expect(box).toBeVisible({ timeout: 20_000 });
	await box.locator('[data-call-reject]').click();

	// She is told, in the conversation, and the wording is hers: she is the one
	// who rang.
	const hers = ana.page.locator('[data-call-note]');
	await expect(hers).toBeVisible({ timeout: 15_000 });
	await expect(hers).toContainText(/recusou/i);

	// And it is in his conversation too — the whole point of the feature is the
	// person who was not there finding out afterwards.
	await bruno.openDMWith('perdana');
	await expect(bruno.page.locator('[data-call-note]')).toBeVisible({ timeout: 15_000 });

	await ana.close();
	await bruno.close();
});

test('procurar acha a mensagem e abre a conversa em que ela foi dita', async ({ browser }) => {
	const ana = await Person.create(browser, 'buscana');
	const bruno = await Person.create(browser, 'buscabru');

	// Something to find, in the conversation between the two of them.
	await ana.openDMWith('buscabru');
	await ana.writeTo('buscabru', 'a senha do wifi é jabuticaba42');

	// And somewhere else to be standing when she looks for it.
	await ana.openChannels();
	await ana.page.locator('nav button[data-space="server"]').first().click();
	await ana.page.getByRole('button', { name: '#geral' }).first().click();

	await ana.page.locator('[data-search-open]').click();
	await ana.page.locator('[data-search-box]').fill('jabuticaba');
	const hit = ana.page.locator('[data-hit]').first();
	await expect(hit).toBeVisible({ timeout: 15_000 });
	await expect(hit).toContainText('jabuticaba42');

	// Pressing it lands in the conversation it came from.
	await hit.click();
	await expect(ana.composerFor('buscabru')).toBeVisible({ timeout: 15_000 });

	// And the other one cannot find it from the server's channel, because the
	// only conversation it was in is one he is in too — so he *can*. The half
	// that is about somebody who is not in it is a Go test (chat_test.go): it
	// needs a third person, and two browsers is already the expensive part.
	await bruno.close();
	await ana.close();
});

test('o emoji entra na mensagem pelo painel, achado pelo nome', async ({ browser }) => {
	const ana = await Person.create(browser, 'emojana');

	await ana.page.locator('[data-emoji-open]').click();
	await ana.page.locator('[data-emoji-search]').fill('fogo');
	const fire = ana.page.locator('[data-emoji="🔥"]');
	await expect(fire).toBeVisible({ timeout: 10_000 });
	await fire.click();

	const box = ana.inChannel('geral');
	await expect(box).toHaveValue(/🔥/, { timeout: 10_000 });
	// The button rather than Enter: the panel puts the cursor back in the box a
	// frame later, and a test that raced it would be testing the race.
	await ana.page.getByRole('button', { name: /^enviar$/i }).click();
	// Drawn as Twemoji's picture since 24/09/2026, with the emoji as its alt.
	await expect(ana.page.locator('[data-message]').last().locator('img[data-twemoji="🔥"]')).toBeVisible({
		timeout: 15_000,
	});

	await ana.close();
});

test('um arquivo que não é imagem chega como arquivo, com nome e com jeito de baixar', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'arqana');

	// A real PDF as far as the server is concerned: the type is decided from
	// these bytes and from nothing else (api/files.go).
	await ana.page.locator('[data-composer] input[type="file"]').setInputFiles({
		name: 'contrato.pdf',
		mimeType: 'application/pdf',
		buffer: Buffer.from('%PDF-1.4\n1 0 obj\n<<>>\nendobj\ntrailer\n<<>>\n%%EOF\n'),
	});
	await expect(ana.page.locator('[data-composer]')).toContainText('contrato.pdf', {
		timeout: 15_000,
	});
	await ana.page.getByRole('button', { name: /^enviar$/i }).click();

	const card = ana.page.locator('[data-file]').first();
	await expect(card).toBeVisible({ timeout: 15_000 });
	await expect(card).toContainText('contrato.pdf');
	// The word went away on 20/09/2026 and the glyph took its place, which is
	// what the card had been waiting for since D4n. What is asserted is the
	// same thing either way: the card says it can be downloaded.
	await expect(card.locator('[data-icon="file-arrow-down"]')).toBeVisible();

	await ana.close();
});

test('@todos chama todo mundo do canal', async ({ browser }) => {
	const ana = await Person.create(browser, 'todosana');
	const bruno = await Person.create(browser, 'todosbru');

	const box = ana.inChannel('geral');
	await box.waitFor({ timeout: 15_000 });
	await box.fill('@todos alguém aí?');
	await ana.page.keyboard.press('Enter');

	// It arrives at the other one as something addressed to him, which is what
	// the highlight means.
	await expect(bruno.page.locator('[data-message].mentioned')).toContainText('@todos', {
		timeout: 20_000,
	});

	await ana.close();
	await bruno.close();
});
