// Two real browsers, a real media server, a real screen and a real camera.
//
// This is the only place in the repository that can answer the sentences the
// owner writes after using the thing — "não consigo ver a tela de quem está
// compartilhando", "fica muito ruim, cria um scroll", "a mesma conta pode
// entrar em dois lugares" — because everything else either stops at the server
// or has no camera at all.

import { test, expect, Person } from './fixtures';
import { readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

function serverLog(): string {
	try {
		return readFileSync(join(tmpdir(), 'harmony-e2e-server.log'), 'utf8');
	} catch {
		return '(no server log)';
	}
}

test('the other person sees a shared screen, and a camera', async ({ browser }) => {
	const ana = await Person.create(browser, 'ana');
	const bento = await Person.create(browser, 'bento');

	await ana.joinVoice();
	await bento.joinVoice();

	await ana.shareScreen();

	try {
		// Ana sees her own preview.
		await expect
			.poll(async () => (await ana.videos()).filter((v) => v.w > 0).length, { timeout: 30_000 })
			.toBeGreaterThan(0);

		// Bento pede a tela — entrar no canal não começa a assistir desde
		// 18/09/2026 — e então vê uma imagem chegando: um <video> com tamanho
		// de verdade e quadros dentro, não uma caixa preta. Esta é a asserção
		// de que o relato do dono falava.
		await bento.watchScreen();
		await expect
			.poll(async () => (await bento.videos()).filter((v) => v.w > 0 && v.live).length, {
				timeout: 30_000,
			})
			.toBeGreaterThan(0);

		// A camera on top of the screen, which is the acceptance criterion's
		// other half.
		await bento.startCamera();
		await expect
			.poll(async () => (await ana.videos()).filter((v) => v.w > 0 && v.live).length, {
				timeout: 30_000,
			})
			.toBeGreaterThan(0);
	} finally {
		console.log('ana   videos: ' + JSON.stringify(await ana.videos()));
		console.log('bento videos: ' + JSON.stringify(await bento.videos()));
		console.log(
			serverLog()
				.split('\n')
				.filter((l) => l.includes('voice track'))
				.join('\n'),
		);
	}

	await ana.close();
	await bento.close();
});

test('the call fills the window instead of pushing it taller', async ({ browser }) => {
	const ana = await Person.create(browser, 'ana2');
	const bento = await Person.create(browser, 'bento2');

	await ana.joinVoice();
	await bento.joinVoice();
	await ana.shareScreen();
	await bento.startCamera();

	await expect
		.poll(async () => (await bento.videos()).filter((v) => v.w > 0).length, { timeout: 30_000 })
		.toBeGreaterThan(0);

	// The owner's words: "fica muito ruim como na imagem, cria um scroll".
	for (const who of [ana, bento]) {
		const overflow = await who.page.evaluate(() => ({
			doc: document.documentElement.scrollHeight - document.documentElement.clientHeight,
			body: document.body.scrollHeight - document.body.clientHeight,
		}));
		expect(overflow.doc, 'the page grew taller than the window').toBeLessThanOrEqual(1);
		expect(overflow.body, 'the body grew taller than the window').toBeLessThanOrEqual(1);
	}

	await ana.close();
	await bento.close();
});

test('you can be in the call without receiving somebody screen', async ({ browser }) => {
	const ana = await Person.create(browser, 'mostra');
	const bento = await Person.create(browser, 'naovejo');
	await ana.joinVoice();
	await bento.joinVoice();
	await ana.shareScreen();

	// Ninguém pediu ainda, e por isso o navegador de quem mostra **não sobe** a
	// tela. Esta é a outra metade de "assistir é uma escolha": se o fluxo
	// continuasse subindo para um servidor que descarta tudo, a economia teria
	// só mudado de lugar — de quem recebe para quem transmite.
	await expect
		.poll(async () => await ana.diag(), { timeout: 30_000 })
		.toContain('voz tela subindo: nenhuma');

	await bento.watchScreen();
	await expect
		.poll(async () => await ana.diag(), { timeout: 30_000 })
		.toMatch(/voz tela subindo: (alta|media|baixa|todas)/);
	await expect
		.poll(async () => (await bento.videos()).filter((v) => v.w > 0 && v.live).length, {
			timeout: 30_000,
		})
		.toBeGreaterThan(0);

	// "Estar em uma chamada mas não assistir a live." The picture has to
	// actually stop arriving, not be hidden: the point is the megabits.
	await bento.page.locator('button[aria-label="parar de assistir"]').first().click();
	await expect
		.poll(async () => (await bento.videos()).filter((v) => v.live).length, { timeout: 20_000 })
		.toBe(0);

	// Still in the room, both of them, and still hearing each other: refusing a
	// screen is not leaving.
	await expect.poll(async () => bento.peopleInRoom(), { timeout: 20_000 }).toHaveLength(2);
	expect(await bento.page.locator('[data-call="live"]').count()).toBe(1);

	// Bento's own circle says "N", not "N(" — the initials come from the plain
	// name and not from the label, which carries a "(você)" on the end. That is
	// what drew the owner as "L(".
	await expect(
		bento.page.locator('[data-call] [data-person="naovejo"] [data-initials]').last(),
	).toHaveText('N');

	// And the way back is under the person, which is where somebody looks for
	// it.
	// Scoped to the call on purpose. Since D4e the people list is open by
	// default, so [data-person] now matches a row in that list as well — and
	// that row is later in the document than the call, so ".last()" quietly
	// stopped meaning "the seat".
	await bento.page
		.locator('[data-call] [data-person="mostra"]')
		.last()
		.getByRole('button', { name: 'assistir à tela' })
		.click();
	await expect
		.poll(async () => (await bento.videos()).filter((v) => v.w > 0 && v.live).length, {
			timeout: 30_000,
		})
		.toBeGreaterThan(0);

	await ana.close();
	await bento.close();
});

test('the call fits a telephone', async ({ browser }) => {
	// The owner's photograph: the row of faces was wider than the screen, so
	// the whole page slid sideways and the controls went off the edge, and the
	// page was taller than what could be seen.
	const ana = await Person.create(browser, 'fone1', { viewport: Person.PHONE });
	const bento = await Person.create(browser, 'fone2', { viewport: Person.PHONE });
	await ana.joinVoice();
	await bento.joinVoice();
	await ana.shareScreen();
	await bento.startCamera();

	await expect
		.poll(async () => (await bento.videos()).filter((v) => v.w > 0).length, { timeout: 30_000 })
		.toBeGreaterThan(0);

	for (const who of [ana, bento]) {
		const over = await who.page.evaluate(() => ({
			tall: document.documentElement.scrollHeight - document.documentElement.clientHeight,
			wide: document.documentElement.scrollWidth - document.documentElement.clientWidth,
		}));
		expect(over.tall, 'the page is taller than the telephone').toBeLessThanOrEqual(1);
		expect(over.wide, 'the page is wider than the telephone').toBeLessThanOrEqual(1);

		// And every control is on the screen, not past its edge.
		const box = await who.page.locator('button[aria-label="sair da chamada"]').boundingBox();
		expect(box, 'no hang-up button').not.toBeNull();
		expect(box!.x + box!.width, 'the controls run off the side').toBeLessThanOrEqual(
			Person.PHONE.width,
		);
		expect(box!.y + box!.height, 'the controls run off the bottom').toBeLessThanOrEqual(
			Person.PHONE.height,
		);
	}

	await ana.close();
	await bento.close();
});

test('one account is in one place', async ({ browser }) => {
	const ana = await Person.create(browser, 'ana3');
	const bento = await Person.create(browser, 'bento3');

	await ana.joinVoice();
	await bento.joinVoice();
	await expect.poll(async () => bento.peopleInRoom(), { timeout: 20_000 }).toHaveLength(2);

	// The same account arrives from a second browser. The first one has to go:
	// seeing the same name twice in a room is the bug that was reported.
	const anaAgain = await Person.signIn(browser, 'ana3');
	await anaAgain.joinVoice();

	await expect.poll(async () => bento.peopleInRoom(), { timeout: 20_000 }).toHaveLength(2);
	const names = await bento.peopleInRoom();
	expect(new Set(names).size, 'the same person is in the room twice: ' + names.join(', ')).toBe(
		names.length,
	);

	await ana.close();
	await anaAgain.close();
	await bento.close();
});

test('a person opens, and closes when you press outside', async ({ browser }) => {
	const ana = await Person.create(browser, 'ana4');
	const bento = await Person.create(browser, 'bento4');

	await ana.joinVoice();
	await bento.joinVoice();

	await ana.page.locator('button[data-person="bento4"]').first().click();
	const card = ana.page.getByRole('dialog');
	await expect(card).toBeVisible();
	await expect(card).toContainText('@bento4');
	// People in the same room retain their own voice level.
	await expect(card.locator('input[data-volume]')).toHaveCount(1);

	// Pressing outside closes it. That is the thing that was wrong before.
	//
	// Instrumentado em 20/09/2026 porque ele falhou **uma vez** numa suite cheia
	// e passou sozinho e em três repetições depois. Um teste instável que só
	// diz "continuou visível" custa uma tarde de palpite; estas duas linhas
	// fazem a próxima falha dizer **qual** das duas coisas quebrou — o
	// mousedown não chegou ao documento, ou chegou e o painel não fechou.
	await ana.page.evaluate(() => {
		(window as unknown as { __fora: string[] }).__fora = [];
		document.addEventListener(
			'mousedown',
			(ev) => {
				const el = ev.target as Element | null;
				(window as unknown as { __fora: string[] }).__fora.push(
					el ? el.tagName + '.' + (el.className || '') : 'sem alvo',
				);
			},
			true,
		);
	});
	await ana.page.mouse.click(5, 5);
	const visto = await ana.page.evaluate(() => {
		const em = document.elementFromPoint(5, 5);
		return {
			chegou: (window as unknown as { __fora: string[] }).__fora,
			em: em ? em.tagName + '.' + (em.className || '') : 'nada',
		};
	});
	await expect(
		card,
		`mousedown que chegou ao document: ${JSON.stringify(visto.chegou)}; ` +
			`elemento em (5,5) depois do clique: ${visto.em}`,
	).toBeHidden();

	await ana.close();
	await bento.close();
});

test('com o aplicativo atrás de outra coisa, nada é desenhado', async ({ browser }) => {
	// Pedido do dono em 19/09/2026: quem transmite está jogando, e o navegador
	// desenhando a sala inteira disputa a GPU com o jogo — que é o mesmo laço
	// que o D4q mediu não entregando 60 quadros. Parar de desenhar não para de
	// enviar e não cala ninguém: todo <video> daqui é mudo.
	const ana = await Person.create(browser, 'fundo1');
	const bento = await Person.create(browser, 'fundo2');

	await ana.joinVoice();
	await bento.joinVoice();
	await ana.shareScreen();
	await bento.watchScreen();

	await expect
		.poll(async () => (await bento.videos()).filter((v) => v.w > 0 && v.live).length, {
			timeout: 30_000,
		})
		.toBeGreaterThan(0);

	// Nada disso existe no navegador de verdade: é a única maneira de dizer
	// "o aplicativo saiu da frente" de fora dele.
	const esconde = (hidden: boolean) => (p: typeof bento.page) =>
		p.evaluate((h) => {
			Object.defineProperty(document, 'visibilityState', {
				configurable: true,
				get: () => (h ? 'hidden' : 'visible'),
			});
			document.dispatchEvent(new Event('visibilitychange'));
		}, hidden);

	const parados = async () =>
		await bento.page.evaluate(
			() => Array.from(document.querySelectorAll('video')).filter((v) => v.paused).length,
		);
	const quantos = async () =>
		await bento.page.evaluate(() => document.querySelectorAll('video').length);

	expect(await parados(), 'havia imagem parada antes de esconder').toBe(0);

	await esconde(true)(bento.page);
	await expect.poll(parados, { timeout: 10_000 }).toBe(await quantos());
	// E o áudio nunca esteve em jogo — quem escondeu continua na sala.
	await expect.poll(async () => bento.peopleInRoom(), { timeout: 10_000 }).toHaveLength(2);

	await esconde(false)(bento.page);
	await expect.poll(parados, { timeout: 10_000 }).toBe(0);
	await expect
		.poll(async () => (await bento.videos()).filter((v) => v.w > 0 && v.live).length, {
			timeout: 30_000,
		})
		.toBeGreaterThan(0);

	await ana.close();
	await bento.close();
});
