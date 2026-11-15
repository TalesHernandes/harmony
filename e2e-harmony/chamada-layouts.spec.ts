// A sala como a do Discord (24/09/2026, segunda volta).
//
// A primeira volta deu três layouts num menu; o dono testou e disse "ficou bom
// mas não era o que eu queria": a live é um bloco a mais ao lado de quem
// transmite, clicar num bloco põe ele em evidência com os outros numa tira
// embaixo, a tira se esconde e aí as câmeras flutuam por cima da imagem, para
// qualquer lugar, sem nome e sem bolinha. O botão direito abre o menu da
// pessoa. E a janela flutuante (↗), por cima do jogo.
//
// O que pode quebrar sem ninguém ver é a economia do D4s: quem assiste pede a
// camada da tela pelo tamanho em que a desenha. Cada troca aqui muda esse
// tamanho, e o pedido tem de acompanhar — medido no diaglog de quem assiste.
import { mkdirSync } from 'node:fs';
import { join } from 'node:path';
import type { Page } from '@playwright/test';
import { test, expect, Person } from './fixtures';

// Onde guardar os prints de antes e depois, quando alguém pede.
function shot(name: string): string | undefined {
	const dir = process.env.HARMONY_SHOTS;
	if (!dir) return undefined;
	mkdirSync(dir, { recursive: true });
	return join(dir, name);
}

const LAYERS = ['baixa', 'media', 'alta'];

// A última camada que quem assiste pediu da tela de alguém, e o tamanho que
// ele mediu para pedir.
async function lastAsk(viewer: Person, of: string): Promise<{ layer: number; px: number }> {
	const lines = [
		...(await viewer.diag()).matchAll(new RegExp(`pedindo a camada (\\w+) da tela de ${of} \\((\\d+)px\\)`, 'g')),
	];
	const last = lines[lines.length - 1];
	return last ? { layer: LAYERS.indexOf(last[1]), px: Number(last[2]) } : { layer: -1, px: 0 };
}

// O tamanho dos blocos da chamada, na ordem da página.
async function blocks(page: Page) {
	return page.locator('[data-call] [data-tile]').evaluateAll((els) =>
		els
			.filter((e) => !e.closest('[data-flutuantes]'))
			.map((e) => {
				const r = e.getBoundingClientRect();
				return { tile: (e as HTMLElement).dataset.tile ?? '', w: r.width, h: r.height, x: r.x, y: r.y };
			}),
	);
}

test('o botão de assistir é do tamanho de um botão e fica debaixo do círculo da pessoa', async ({ browser }) => {
	const n = test.info().repeatEachIndex;
	const ana = await Person.create(browser, 'layolha' + n);
	const bea = await Person.create(browser, 'laymostra' + n);
	await ana.joinVoice();
	await bea.joinVoice();
	await bea.shareScreen();

	const watch = ana.page.locator('[data-call] button[aria-label="assistir à tela"]').first();
	await expect(watch).toBeVisible({ timeout: 20_000 });
	const seat = ana.page.locator(`[data-call] [data-person="${bea.username}"]`).first();
	const out = shot('assistir.png');
	if (out) await ana.page.locator('[data-call]').screenshot({ path: out });

	const b = (await watch.boundingBox())!;
	const c = (await seat.locator('[data-initials]').first().boundingBox())!;
	expect(b.height, 'do tamanho dos botões da barra').toBeGreaterThanOrEqual(40);
	expect(Math.abs(b.x + b.width / 2 - (c.x + c.width / 2)), 'centrado sob o círculo').toBeLessThanOrEqual(2);
	expect(b.y, 'abaixo do círculo').toBeGreaterThanOrEqual(c.y + c.height);
	expect(b.y - (c.y + c.height), 'logo abaixo, não no pé do quadro').toBeLessThanOrEqual(20);

	await watch.click();
	await expect
		.poll(async () => (await ana.videos()).filter((v) => v.w > 0 && v.live).length, { timeout: 30_000 })
		.toBe(1);
	await bea.close();
	await ana.close();
});

test('a live é um bloco ao lado de quem transmite, e clicar põe em evidência com a tira embaixo', async ({
	browser,
}) => {
	const n = test.info().repeatEachIndex;
	const ana = await Person.create(browser, 'laygrade' + n, { viewport: { width: 1600, height: 900 } });
	const bea = await Person.create(browser, 'laybloco' + n);
	await ana.joinVoice();
	await bea.joinVoice();
	await ana.shareScreen();

	// Quem transmite vê três blocos iguais, 16:9: ele, a bea e a própria tela.
	const call = ana.page.locator('[data-call]');
	await expect(call).toHaveAttribute('data-layout', 'grade');
	await expect(call.locator('[data-grande]')).toHaveCount(0);
	await expect.poll(async () => (await blocks(ana.page)).length, { timeout: 20_000 }).toBe(3);
	const grade = await blocks(ana.page);
	for (const b of grade) {
		expect(Math.abs(b.w - grade[0].w), 'todos do mesmo tamanho').toBeLessThanOrEqual(1);
		expect(Math.abs(b.w / b.h - 16 / 9), '16:9').toBeLessThan(0.02);
	}
	const out = shot('grade.png');
	if (out) await call.screenshot({ path: out });

	// Clicar na tela: ela vira o bloco grande, e os outros dois vão para a
	// tira embaixo dela.
	const tela = grade.find((b) => b.tile.startsWith('eu:'))!;
	await ana.page.locator(`[data-call] [data-tile="${tela.tile}"] > div > button`).first().click();
	await expect(call).toHaveAttribute('data-layout', 'foco');
	await expect(call.locator('[data-grande] [data-big]')).toHaveAttribute('data-tile', tela.tile);
	await expect(call.locator('[data-tira] [data-tile]')).toHaveCount(2);
	const grande = (await call.locator('[data-grande] [data-big]').boundingBox())!;
	const tira = (await call.locator('[data-tira]').boundingBox())!;
	expect(tira.y, 'a tira fica embaixo').toBeGreaterThanOrEqual(grande.y + grande.height);
	expect(grande.width, 'bem maior que um bloco da grade').toBeGreaterThan(tela.w * 1.3);
	const foco = shot('foco.png');
	if (foco) await call.screenshot({ path: foco });

	// Clicar no grande volta para a grade.
	await ana.page.locator('[data-call] [data-big] > div > button').first().click();
	await expect(call).toHaveAttribute('data-layout', 'grade');

	// Uma pessoa sem câmera também entra em evidência, como no Discord.
	await ana.page.locator(`[data-call] [data-person="${bea.username}"] > div > button`).first().click();
	await expect(call).toHaveAttribute('data-layout', 'foco');
	await expect(call.locator('[data-grande] [data-big] [data-initials]')).toBeVisible();
	await bea.close();
	await ana.close();
});

test('entrar em evidência e esconder a tira pedem camadas maiores, e voltar devolve', async ({ browser }) => {
	const n = test.info().repeatEachIndex;
	const mostra = await Person.create(browser, 'laycam' + n);
	const olha = await Person.create(browser, 'laymede' + n, { viewport: { width: 1920, height: 1000 } });
	await mostra.joinVoice();
	await olha.joinVoice();
	await mostra.startCamera();
	await mostra.shareScreen();
	await olha.watchScreen();

	// Assistir põe a tela em evidência.
	const call = olha.page.locator('[data-call]');
	await expect(call).toHaveAttribute('data-layout', 'foco', { timeout: 20_000 });
	await expect.poll(async () => (await lastAsk(olha, mostra.username)).layer, { timeout: 30_000 }).toBeGreaterThan(0);
	const foco = await lastAsk(olha, mostra.username);
	const tela = await call.locator('[data-big]').getAttribute('data-tile');

	// De volta à grade a tela é um bloco entre quatro: menor, e a camada desce.
	await olha.page.locator('[data-call] [data-big] > div > button').first().click();
	await expect(call).toHaveAttribute('data-layout', 'grade');
	await expect.poll(async () => (await lastAsk(olha, mostra.username)).px, { timeout: 30_000 }).toBeLessThan(foco.px);
	const grade = await lastAsk(olha, mostra.username);
	expect(grade.layer, 'na grade, camada menor que em evidência').toBeLessThan(foco.layer);

	// Em evidência de novo, e com a tira escondida a imagem cresce mais.
	await olha.page.locator(`[data-call] [data-tile="${tela}"] > div > button`).first().click();
	await expect(call).toHaveAttribute('data-layout', 'foco');
	await expect.poll(async () => (await lastAsk(olha, mostra.username)).layer, { timeout: 30_000 }).toBe(foco.layer);
	// Sem a tira, a imagem grande cresce. Em evidência ela já pede a camada
	// mais alta, e o app só escreve um pedido quando a camada muda — então o
	// que se mede aqui é o tamanho desenhado, e que a camada não caiu.
	const bigWidth = () => call.locator('[data-big]').evaluate((e) => e.getBoundingClientRect().width);
	const comTira = await bigWidth();
	await olha.page.locator('[data-esconder-tira]').click();
	await expect(call.locator('[data-tira]')).toHaveCount(0);
	await expect.poll(bigWidth).toBeGreaterThan(comTira + 50);
	await olha.page.waitForTimeout(1000);
	expect((await lastAsk(olha, mostra.username)).layer, 'a camada não caiu').toBeGreaterThanOrEqual(foco.layer);
	console.log(
		`grade ${JSON.stringify(grade)} · foco ${JSON.stringify(foco)} · largura com tira ${Math.round(comTira)} sem ${Math.round(await bigWidth())}`,
	);
	expect((await olha.videos()).filter((v) => v.w > 0 && v.live).length, 'nenhuma imagem parou').toBe(2);
	await mostra.close();
	await olha.close();
});

test('sem a tira as câmeras flutuam sem nome, vão para qualquer lugar, e os botões saem da frente', async ({
	browser,
}) => {
	const n = test.info().repeatEachIndex;
	const mostra = await Person.create(browser, 'layrosto' + n);
	const olha = await Person.create(browser, 'laysolta' + n, { viewport: { width: 1400, height: 860 } });
	await mostra.joinVoice();
	await olha.joinVoice();
	await mostra.startCamera();
	await mostra.shareScreen();
	await olha.watchScreen();
	const call = olha.page.locator('[data-call]');
	await expect(call).toHaveAttribute('data-layout', 'foco', { timeout: 20_000 });
	await olha.page.locator('[data-esconder-tira]').click();

	const big = call.locator('[data-big]');
	const float = big.locator('[data-flutuantes]');
	await expect(float).toHaveAttribute('data-flutuantes', '1', { timeout: 20_000 });
	// Só a imagem: nem o nome, nem a bolinha de quem está sem câmera.
	await expect(float.locator('[data-nome]')).toHaveCount(0);
	await expect(float.locator('[data-initials]')).toHaveCount(0);
	await expect(float.locator('video')).toHaveCount(1);
	// E as duas imagens tocando — a grande não pode ficar preta na troca.
	await expect
		.poll(async () => (await olha.videos()).filter((v) => v.w > 0 && v.live).length, { timeout: 20_000 })
		.toBe(2);
	await olha.page.waitForTimeout(1500);
	const out = shot('flutuante.png');
	if (out) await call.screenshot({ path: out });

	// Arrastar até o meio da imagem: fica onde foi solto, não num canto.
	const r = (await big.boundingBox())!;
	const drag = async (x: number, y: number) => {
		const s = (await float.boundingBox())!;
		await olha.page.mouse.move(s.x + s.width / 2, s.y + s.height / 2);
		await olha.page.mouse.down();
		await olha.page.mouse.move(x, y, { steps: 10 });
		await olha.page.mouse.up();
	};
	const grande = await big.getAttribute('data-tile');
	await drag(r.x + r.width * 0.45, r.y + r.height * 0.5);
	const m = (await float.boundingBox())!;
	expect(Math.abs(m.x + m.width / 2 - (r.x + r.width * 0.45)), 'solto no meio, na horizontal').toBeLessThanOrEqual(4);
	expect(Math.abs(m.y + m.height / 2 - (r.y + r.height * 0.5)), 'solto no meio, na vertical').toBeLessThanOrEqual(4);
	// Arrastar não é clicar: a imagem grande continua a mesma.
	await olha.page.waitForTimeout(400);
	expect(await big.getAttribute('data-tile')).toBe(grande);

	// No canto de cima à direita ela ficaria sobre os botões — que vão para a
	// esquerda.
	const topo = big.locator('[data-botoes-topo]');
	await expect(topo).toHaveAttribute('data-botoes-topo', 'direita');
	await drag(r.x + r.width - 20, r.y + 20);
	await expect(topo).toHaveAttribute('data-botoes-topo', 'esquerda');
	const t = (await topo.boundingBox())!;
	const f = (await float.boundingBox())!;
	expect(t.x + t.width, 'os botões não passam por baixo da câmera').toBeLessThan(f.x);
	// E o lugar fica guardado no aparelho.
	const saved = await olha.page.evaluate(() => JSON.parse(localStorage.getItem('harmony.call.flutuante') ?? 'null'));
	expect(saved.x).toBeGreaterThan(0.95);
	expect(saved.y).toBeLessThan(0.05);
	await mostra.close();
	await olha.close();
});

test('botão direito num bloco abre o menu da pessoa ali, e o Ver perfil abre o perfil', async ({ browser }) => {
	const n = test.info().repeatEachIndex;
	const ana = await Person.create(browser, 'laymenu' + n);
	const bea = await Person.create(browser, 'layalvo' + n);
	await ana.joinVoice();
	await bea.joinVoice();
	const block = ana.page.locator(`[data-call] [data-person="${bea.username}"]`).first();
	await expect(block).toBeVisible({ timeout: 20_000 });
	await block.click({ button: 'right' });
	const menus = ana.page.locator('[data-menu]');
	await expect(menus).toHaveCount(1);
	const m = (await menus.boundingBox())!;
	const b = (await block.boundingBox())!;
	expect(m.x >= b.x && m.y >= b.y && m.y < b.y + b.height, 'dentro do bloco clicado').toBe(true);
	await menus.getByRole('menuitem', { name: 'Ver perfil' }).click();
	await expect(ana.page.getByRole('dialog', { name: /perfil de/ })).toBeVisible();
	// E o clique esquerdo não abre perfil nenhum: põe em evidência.
	await ana.page.keyboard.press('Escape');
	await expect(ana.page.getByRole('dialog', { name: /perfil de/ })).toHaveCount(0);
	await block.locator('> div > button').first().click();
	await expect(ana.page.locator('[data-call]')).toHaveAttribute('data-layout', 'foco');
	await expect(ana.page.getByRole('dialog', { name: /perfil de/ })).toHaveCount(0);
	await bea.close();
	await ana.close();
});

test('o menu de uma pessoa na sala abre uma vez só, e o Ver perfil funciona, até no próprio', async ({
	browser,
}) => {
	const n = test.info().repeatEachIndex;
	const ana = await Person.create(browser, 'laylista' + n, { viewport: { width: 1600, height: 900 } });
	const bea = await Person.create(browser, 'layperfil' + n);
	await bea.joinVoice();
	// A coluna de pessoas aberta, como no dia do defeito: a bea aparece nela e
	// debaixo da sala, na coluna da esquerda.
	const people = ana.page.locator('[data-island="pessoas"]');
	if (!(await people.isVisible())) await ana.page.locator('button[aria-label="pessoas"]').click();
	await expect(ana.page.locator(`nav button[data-person="${bea.username}"]`)).toBeVisible({ timeout: 20_000 });

	for (const who of [bea.username, ana.username]) {
		if (who === ana.username) {
			await ana.joinVoice();
			if (!(await people.isVisible())) await ana.page.locator('button[aria-label="pessoas"]').click();
		}
		await ana.page.locator(`nav button[data-person="${who}"]`).click({ button: 'right' });
		await expect(ana.page.locator('[data-menu]'), `um menu só (${who})`).toHaveCount(1);
		await ana.page.locator('[data-menu]').getByRole('menuitem', { name: 'Ver perfil' }).click();
		await expect(ana.page.getByRole('dialog', { name: /perfil de/ }), `o perfil de ${who} abriu`).toBeVisible();
		await ana.page.keyboard.press('Escape');
		await expect(ana.page.getByRole('dialog', { name: /perfil de/ })).toHaveCount(0);
	}
	await bea.close();
	await ana.close();
});

test('entrar na chamada fecha a coluna de pessoas, e as letras dela são as da esquerda', async ({ browser }) => {
	const n = test.info().repeatEachIndex;
	const ana = await Person.create(browser, 'laycoluna' + n, { viewport: { width: 1600, height: 900 } });
	const people = ana.page.locator('[data-island="pessoas"]');
	if (!(await people.isVisible())) await ana.page.locator('button[aria-label="pessoas"]').click();
	await expect(people).toBeVisible();

	const size = (sel: string) =>
		ana.page.locator(sel).first().evaluate((el) => getComputedStyle(el).fontSize);
	expect(await size('[data-island="pessoas"] button[data-person]')).toBe(await size('nav button[aria-label^="#"]'));
	expect(await size('[data-island="pessoas"] .text-label')).toBe(await size('nav .text-label'));

	await ana.joinVoice();
	await expect(people).toBeHidden();
	// Abrir de novo durante a chamada fica aberto.
	await ana.page.locator('button[aria-label="pessoas"]').click();
	await expect(people).toBeVisible();
	await ana.page.waitForTimeout(500);
	await expect(people).toBeVisible();
	await ana.close();
});

test('a janela flutuante leva a imagem para fora e traz de volta', async ({ browser }) => {
	const n = test.info().repeatEachIndex;
	const mostra = await Person.create(browser, 'laypip' + n);
	const olha = await Person.create(browser, 'layjanela' + n);
	await mostra.joinVoice();
	await olha.joinVoice();
	await mostra.shareScreen();
	await olha.watchScreen();
	const call = olha.page.locator('[data-call]');
	await expect(call).toHaveAttribute('data-layout', 'foco', { timeout: 20_000 });

	const supported = await olha.page.evaluate(() => 'documentPictureInPicture' in window);
	test.skip(!supported, 'este Chromium não tem Document Picture-in-Picture');
	const inPage = () => olha.page.evaluate(() => document.querySelectorAll('[data-call] video').length);
	await expect.poll(inPage, { timeout: 20_000 }).toBe(1);
	await call.locator('[data-janela="dentro"]').click();
	await expect(call.locator('[data-janela="fora"]')).toBeVisible({ timeout: 10_000 });
	await expect(call.getByText('Na janela flutuante')).toBeVisible();
	expect(await inPage(), 'a imagem saiu da página').toBe(0);
	const outside = await olha.page.evaluate(() => {
		const w = (window as any).documentPictureInPicture.window as Window | null;
		const v = w?.document.querySelector('video');
		return { open: !!w, playing: !!v && !v.paused && !!v.srcObject };
	});
	expect(outside).toEqual({ open: true, playing: true });

	await call.locator('[data-janela="fora"]').click();
	await expect(call.locator('[data-janela="dentro"]')).toBeVisible();
	await expect.poll(inPage).toBe(1);
	await expect
		.poll(async () => (await olha.videos()).filter((v) => v.w > 0 && v.live).length, { timeout: 20_000 })
		.toBe(1);
	await mostra.close();
	await olha.close();
});

test('no celular a barra cabe inteira, e o assistir continua sob o círculo', async ({ browser }) => {
	const n = test.info().repeatEachIndex;
	const ana = await Person.create(browser, 'layfone' + n, { viewport: { width: 360, height: 740 } });
	const bea = await Person.create(browser, 'laytela' + n);
	await ana.joinVoice();
	await bea.joinVoice();
	await bea.shareScreen();

	const watch = ana.page.locator('[data-call] button[aria-label="assistir à tela"]').first();
	await expect(watch).toBeVisible({ timeout: 20_000 });
	const out = shot('celular.png');
	if (out) await ana.page.screenshot({ path: out });
	const b = (await watch.boundingBox())!;
	const c = (await ana.page.locator(`[data-call] [data-person="${bea.username}"] [data-initials]`).first().boundingBox())!;
	expect(Math.abs(b.x + b.width / 2 - (c.x + c.width / 2))).toBeLessThanOrEqual(2);
	expect(b.y).toBeGreaterThanOrEqual(c.y + c.height);

	// A própria barra não transborda — a borda dos botões dentro da janela não
	// bastava: a 320 px a barra já rolou por dentro (24/09/2026).
	for (const w of [360, 320]) {
		await ana.page.setViewportSize({ width: w, height: 740 });
		await expect(async () => {
			const cabe = await ana.page
				.locator('[data-call-controls]')
				.evaluate((bar) => bar.scrollWidth <= bar.clientWidth + 1);
			expect(cabe, `a barra transborda em ${w}px`).toBe(true);
		}).toPass({ timeout: 5000 });
	}
	await bea.close();
	await ana.close();
});

// Terceira volta (24/09/2026): o dono testou a sala do Discord e pediu cinco
// acertos — nenhuma dica sobre as imagens, um ✕ para tirar uma webcam de cima
// da live, clicar em tela cheia não sair dela, o quadrado preto vazio, e os
// botões do bloco em evidência só enquanto o mouse está em cima.

async function sharerInFocus(browser: import('@playwright/test').Browser, tag: string) {
	const n = test.info().repeatEachIndex;
	const ana = await Person.create(browser, tag + n, { viewport: { width: 1400, height: 860 } });
	await ana.joinVoice();
	await ana.shareScreen();
	await ana.focusMyScreen();
	return ana;
}

test('nenhuma dica aparece sobre as imagens da chamada', async ({ browser }) => {
	const n = test.info().repeatEachIndex;
	const mostra = await Person.create(browser, 'laydica' + n);
	const olha = await Person.create(browser, 'laysemdica' + n);
	await mostra.joinVoice();
	await olha.joinVoice();
	await mostra.startCamera();
	await mostra.shareScreen();
	await olha.watchScreen();
	const call = olha.page.locator('[data-call]');
	await expect(call).toHaveAttribute('data-layout', 'foco', { timeout: 20_000 });
	await expect(call.locator('[data-tira] [data-tile]')).toHaveCount(2);
	await expect(call.locator('[data-tile][title], [data-tile] [title]')).toHaveCount(0);
	await olha.page.locator('[data-esconder-tira]').click();
	await expect(call.locator('[data-flutuantes]')).toBeVisible();
	await expect(call.locator('[data-tile][title], [data-tile] [title]')).toHaveCount(0);
	await mostra.close();
	await olha.close();
});

test('o ✕ tira a webcam de cima da live e ela fica na tira', async ({ browser }) => {
	const n = test.info().repeatEachIndex;
	const mostra = await Person.create(browser, 'layxis' + n);
	const olha = await Person.create(browser, 'layrecolhe' + n, { viewport: { width: 1400, height: 860 } });
	await mostra.joinVoice();
	await olha.joinVoice();
	await mostra.startCamera();
	await mostra.shareScreen();
	await olha.watchScreen();
	const call = olha.page.locator('[data-call]');
	await expect(call).toHaveAttribute('data-layout', 'foco', { timeout: 20_000 });
	const cam = await call.locator('[data-tira] [data-tile]:not([data-tile=""])').first().getAttribute('data-person');

	await olha.page.locator('[data-esconder-tira]').click();
	const float = call.locator('[data-flutuantes]');
	await expect(float).toHaveAttribute('data-flutuantes', '1', { timeout: 20_000 });
	await float.hover();
	await float.locator('[data-recolher]').click();
	// Fora de cima da imagem, e com a tira escondida, em lugar nenhum.
	await expect(float).toHaveCount(0);
	await expect(call.locator('[data-tira]')).toHaveCount(0);
	// Abrir a tira: ela está lá.
	await olha.page.locator('[data-esconder-tira]').click();
	await expect(call.locator(`[data-tira] [data-person="${cam}"]`)).toHaveCount(1);
	// E esconder de novo traz todo mundo de volta para cima da imagem.
	await olha.page.locator('[data-esconder-tira]').click();
	await expect(call.locator('[data-flutuantes]')).toHaveAttribute('data-flutuantes', '1');
	await mostra.close();
	await olha.close();
});

test('em tela cheia clicar na imagem não sai dela', async ({ browser }) => {
	const ana = await sharerInFocus(browser, 'laycheia');
	const big = ana.page.locator('[data-call] [data-big]');
	await big.hover();
	await ana.page.locator('button[aria-label="tela cheia"]').first().click();
	await expect.poll(() => ana.page.evaluate(() => !!document.fullscreenElement), { timeout: 10_000 }).toBe(true);
	const box = (await big.boundingBox())!;
	await ana.page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
	await ana.page.waitForTimeout(800);
	expect(await ana.page.evaluate(() => !!document.fullscreenElement), 'continua em tela cheia').toBe(true);
	await expect(ana.page.locator('[data-call]')).toHaveAttribute('data-layout', 'foco');
	// O botão continua sendo a saída.
	await ana.page.locator('button[aria-label="sair da tela cheia"]').first().click();
	await expect.poll(() => ana.page.evaluate(() => !!document.fullscreenElement), { timeout: 10_000 }).toBe(false);
	await ana.close();
});

test('nenhum quadrado vazio em cima da imagem em evidência', async ({ browser }) => {
	const ana = await sharerInFocus(browser, 'layvazio');
	await ana.page.locator('[data-call] [data-big]').hover();
	// Durante três segundos, desde o primeiro instante: todo pedaço na fileira
	// de cima tem alguma coisa dentro.
	const empties: number[] = [];
	for (let i = 0; i < 30; i++) {
		empties.push(
			await ana.page
				.locator('[data-call] [data-big] [data-botoes-topo] > *')
				.evaluateAll((els) => els.filter((e) => !(e as HTMLElement).innerText.trim() && !e.querySelector('svg')).length),
		);
		await ana.page.waitForTimeout(100);
	}
	expect(Math.max(...empties), 'um pedaço vazio na fileira de cima').toBe(0);
	await ana.close();
});

test('os botões e o nome do bloco em evidência só aparecem com o mouse em cima', async ({ browser }) => {
	const ana = await sharerInFocus(browser, 'laymouse');
	const bar = ana.page.locator('[data-call] [data-big] [data-tilebar]');
	const opacity = () => bar.evaluate((e) => getComputedStyle(e).opacity);
	await ana.page.mouse.move(5, 5);
	await expect.poll(opacity).toBe('0');
	await ana.page.locator('[data-call] [data-big]').hover();
	await expect.poll(opacity).toBe('1');
	await ana.page.mouse.move(5, 5);
	await expect.poll(opacity).toBe('0');
	// Na grade os nomes ficam: é neles que se sabe quem é quem.
	await ana.page.locator('[data-call] [data-big] > div > button').first().click({ force: true });
	await expect(ana.page.locator('[data-call]')).toHaveAttribute('data-layout', 'grade');
	await ana.page.mouse.move(5, 5);
	expect(await ana.page.locator('[data-call] [data-tilebar]').first().evaluate((e) => getComputedStyle(e).opacity)).toBe('1');
	await ana.close();
});

test('a barra de entrar na chamada tem a altura da ilha da conta, ao lado dela', async ({ browser }) => {
	// O dono, 24/09/2026: "esse menu de entrar na chamada não está alinhado
	// com o da esquerda". Fora da sala, a ilha da conta é uma linha só.
	const n = test.info().repeatEachIndex;
	const ana = await Person.create(browser, 'layalinha' + n, { viewport: { width: 1600, height: 900 } });
	await ana.joinVoice();
	const edges = () =>
		ana.page.evaluate(() => {
			const r = (sel: string) => {
				const el = [...document.querySelectorAll(sel)].find((e) => (e as HTMLElement).offsetParent);
				const b = el!.getBoundingClientRect();
				return [Math.round(b.top), Math.round(b.bottom)];
			};
			return {
				conta: r('[data-island="voce"]'),
				barra: r('[data-call] > :last-child'),
			};
		});
	console.log('na sala: ' + JSON.stringify(await edges()));
	await ana.page.locator('[data-call-controls] button[aria-label="sair da chamada"]').click();
	await expect(ana.page.locator('[data-call="fora"]')).toBeVisible({ timeout: 10_000 });
	await expect(ana.page.getByRole('button', { name: 'Entrar na chamada' })).toBeVisible();
	const fora = await edges();
	console.log('fora da sala: ' + JSON.stringify(fora));
	expect(fora.barra, 'as mesmas bordas de cima e de baixo').toEqual(fora.conta);
	await ana.close();
});

// Quarta volta (24/09/2026).

test('sozinho na chamada, clicar no próprio bloco não muda nada', async ({ browser }) => {
	const n = test.info().repeatEachIndex;
	const ana = await Person.create(browser, 'laysozinho' + n, { viewport: { width: 1600, height: 900 } });
	await ana.joinVoice();
	const call = ana.page.locator('[data-call]');
	const block = call.locator(`[data-person="${ana.username}"]`).first();
	await expect(block).toBeVisible();
	const before = (await block.boundingBox())!;
	const circle = (await block.locator('[data-initials]').boundingBox())!;
	await block.locator('> div > button').first().click();
	await ana.page.waitForTimeout(500);
	await expect(call).toHaveAttribute('data-layout', 'grade');
	const after = (await call.locator(`[data-person="${ana.username}"]`).first().boundingBox())!;
	expect(after).toEqual(before);
	expect((await call.locator('[data-initials]').first().boundingBox())!.width).toBe(circle.width);
	await ana.close();
});

test('puxar as bordas das webcams flutuantes aumenta e diminui o tamanho delas', async ({ browser }) => {
	const n = test.info().repeatEachIndex;
	const mostra = await Person.create(browser, 'layalca' + n);
	const olha = await Person.create(browser, 'laytamanho' + n, { viewport: { width: 1400, height: 860 } });
	await mostra.joinVoice();
	await olha.joinVoice();
	await mostra.startCamera();
	await mostra.shareScreen();
	await olha.watchScreen();
	const call = olha.page.locator('[data-call]');
	await expect(call).toHaveAttribute('data-layout', 'foco', { timeout: 20_000 });
	await olha.page.locator('[data-esconder-tira]').click();
	const float = call.locator('[data-flutuantes]');
	await expect(float).toHaveAttribute('data-flutuantes', '1', { timeout: 20_000 });
	const big = (await call.locator('[data-big]').boundingBox())!;
	// Pelas bordas, não por um botão (o dono, 24/09/2026).
	await expect(float.locator('[data-redimensionar]')).toHaveCount(0);

	// The cursor tells an edge is there: pointing at it is the only way to find it.
	await expect(float.locator('[data-borda="se"]')).toHaveCSS('cursor', 'nwse-resize');
	await expect(float.locator('[data-borda="w"]')).toHaveCSS('cursor', 'ew-resize');

	const pull = async (edge: string, dx: number, dy: number) => {
		const g = (await float.locator(`[data-borda="${edge}"]`).boundingBox())!;
		await olha.page.mouse.move(g.x + g.width / 2, g.y + g.height / 2);
		await olha.page.mouse.down();
		await olha.page.mouse.move(g.x + g.width / 2 + dx, g.y + g.height / 2 + dy, { steps: 8 });
		await olha.page.mouse.up();
	};
	const b0 = (await float.boundingBox())!;
	// The corner in: smaller, and the top left corner stays put.
	await pull('se', -120, -67);
	const b1 = (await float.boundingBox())!;
	expect(b1.width, 'diminuiu').toBeLessThan(b0.width - 40);
	expect(Math.abs(b1.x - b0.x), 'o canto oposto ficou').toBeLessThan(2);
	expect(Math.abs(b1.y - b0.y)).toBeLessThan(2);
	// The left edge out: bigger, and it is the right edge that stays put.
	await pull('w', -200, 0);
	const b = (await float.boundingBox())!;
	expect(b.width, 'aumentou').toBeGreaterThan(b1.width + 120);
	expect(Math.abs(b.x + b.width - (b1.x + b1.width)), 'a borda direita ficou').toBeLessThan(2);
	// Nunca sai de dentro da imagem, nem crescendo.
	expect(b.x).toBeGreaterThanOrEqual(big.x);
	expect(b.y).toBeGreaterThanOrEqual(big.y);
	expect(b.x + b.width).toBeLessThanOrEqual(big.x + big.width + 1);
	expect(b.y + b.height).toBeLessThanOrEqual(big.y + big.height + 1);
	// Redimensionar não é clicar: a imagem grande continua a mesma, e o
	// tamanho fica guardado no aparelho.
	await expect(call).toHaveAttribute('data-layout', 'foco');
	const saved = await olha.page.evaluate(() => Number(localStorage.getItem('harmony.call.flutuante.tamanho')));
	expect(saved).toBeGreaterThan(1);
	await mostra.close();
	await olha.close();
});
