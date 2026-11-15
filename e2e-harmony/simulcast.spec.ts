// D4r: a tela sobe em três camadas, e é o navegador quem oferta.
//
// Este é o único lugar do repositório capaz de responder "as três camadas
// chegaram mesmo?". A sonda de `scripts/simulcast-probe` provou que o caminho
// existe, com um Pion nu e um navegador nu; aqui é o servidor de verdade, a
// sessão de voz de verdade e a tela de verdade — que é onde ainda dá para
// errar: uma oferta recusada, uma vaga adotada na ordem trocada, três
// publicações no lugar de uma.

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

// As linhas "voice track" que o servidor escreve, com a camada que veio em
// cada uma.
function camadas(): string[] {
	return serverLog()
		.split('\n')
		.filter((l) => l.includes('voice track') && l.includes('source=screen'))
		.map((l) => /camada=("?)([a-z]*)\1/.exec(l)?.[2] ?? '')
		.filter((c) => c !== '');
}

test('a tela sobe em três camadas, e a sala continua vendo uma só', async ({ browser }) => {
	const quemMostra = await Person.create(browser, 'camadas');
	const quemAssiste = await Person.create(browser, 'assiste');

	await quemMostra.joinVoice();
	await quemAssiste.joinVoice();
	await quemMostra.shareScreen();

	try {
		// 1. O navegador ofertou, e ofertou em camadas. Se a oferta tivesse
		//    falhado, media.ts cai para o caminho antigo — de uma camada só —
		//    e a linha abaixo não existe. É por isso que ela é a primeira
		//    coisa verificada: sem ela o resto do teste passaria enganado.
		await expect
			.poll(async () => await quemMostra.diag(), { timeout: 30_000 })
			.toContain('voz tela ofertada em 3 camadas');

		// 2. As três chegaram ao servidor.
		await expect
			.poll(() => new Set(camadas()).size, { timeout: 30_000 })
			.toBe(3);
		expect(new Set(camadas())).toEqual(new Set(['alta', 'media', 'baixa']));

		// 3. Quem assiste pede — entrar no canal não começa a assistir — e
		//    então vê UMA tela, não três. Isto é o agrupamento: três
		//    OnTrack, uma publicação. Se falhar, ele vê a mesma tela
		//    desenhada três vezes lado a lado.
		await quemAssiste.watchScreen();
		await expect
			.poll(async () => (await quemAssiste.videos()).filter((v) => v.w > 0 && v.live).length, {
				timeout: 30_000,
			})
			.toBe(1);

		// 4. E o que ele vê tem quadros de verdade dentro, não é uma caixa
		//    preta com tamanho.
		const vistas = (await quemAssiste.videos()).filter((v) => v.w > 0 && v.live);
		expect(vistas.length).toBe(1);
		expect(vistas[0].w).toBeGreaterThan(0);
	} finally {
		console.log('camadas no servidor: ' + JSON.stringify(camadas()));
		console.log('quem assiste vê: ' + JSON.stringify(await quemAssiste.videos()));
		console.log(
			serverLog()
				.split('\n')
				.filter((l) => l.includes('voice track') || l.includes('client offer'))
				.join('\n'),
		);
	}
});

test('uma tela pequena não faz o outro subir a camada grande', async ({ browser }) => {
	// A economia dos dois lados de uma vez, que foi o pedido do dono em
	// 19/09/2026. Quem assiste mede o tamanho em que a imagem está sendo
	// desenhada e pede a camada que cabe; o servidor soma o que foi pedido e
	// avisa quem mostra, que desliga as outras. Ninguém codifica para ninguém.
	const quemMostra = await Person.create(browser, 'sobetudo');
	const quemAssiste = await Person.create(browser, 'janelinha');

	await quemMostra.joinVoice();
	await quemAssiste.joinVoice();
	await quemMostra.shareScreen();

	await expect
		.poll(async () => await quemMostra.diag(), { timeout: 30_000 })
		.toContain('voz tela ofertada em 3 camadas');

	// Antes de alguém pedir, nada sobe.
	await expect
		.poll(async () => await quemMostra.diag(), { timeout: 30_000 })
		.toContain('voz tela subindo: nenhuma');

	// A janela de quem assiste é estreita, então a imagem é desenhada pequena.
	// Estreita mas ainda de computador: abaixo de 768 o layout vira o de
	// celular e a gaveta de canais cobre o botão — o que é o desenho certo,
	// e não o que este teste está medindo.
	await quemAssiste.page.setViewportSize({ width: 900, height: 600 });
	await quemAssiste.watchScreen();

	// Ele pede a camada pequena — e o diag dele diz o tamanho que mediu.
	await expect
		.poll(async () => await quemAssiste.diag(), { timeout: 30_000 })
		.toMatch(/pedindo a camada (baixa|media) da tela de sobetudo/);

	// E quem mostra para de subir a camada grande, que ninguém está olhando.
	//
	// O diag é um histórico, então `alta` aparece nele — ela subiu de verdade
	// no instante entre o clique em assistir e a medida do tamanho. O que vale
	// é a ÚLTIMA vez que ele disse o que está subindo.
	const subindo = async () => {
		const linhas = [...(await quemMostra.diag()).matchAll(/voz tela subindo: (.*)$/gm)];
		return linhas.length > 0 ? linhas[linhas.length - 1][1].trim() : '';
	};
	await expect.poll(subindo, { timeout: 30_000 }).not.toContain('alta');
	expect(await subindo(), 'parou de subir tudo, em vez de encolher').not.toBe('nenhuma');

	// E ele continua vendo a tela: menor, mas vendo.
	await expect
		.poll(async () => (await quemAssiste.videos()).filter((v) => v.w > 0 && v.live).length, {
			timeout: 30_000,
		})
		.toBe(1);

	await quemMostra.close();
	await quemAssiste.close();
});

test('o diag de quem assiste conta os quadros que o aparelho não deu conta', async ({
	browser,
}) => {
	// O instrumento que separa os dois motivos de uma imagem travar.
	//
	// Depois que a oscilação do D4s foi consertada, o log do dono mostrou o
	// telefone dele com `perda=0%` em TODAS as amostras e mesmo assim 76
	// travadas em dez minutos. Perda zero quer dizer que a linha entregou
	// tudo; o que travou foi o aparelho, que recebeu e não desenhou. Nenhum
	// número do log respondia isso, e sem o número a próxima decisão seria um
	// palpite.
	//
	// O que este teste prende é a fiação: que a contagem chega ao diag de quem
	// assiste, com nome. Quanto ela vale num aparelho que está sofrendo, só um
	// aparelho que esteja sofrendo diz — e isso não se encomenda num navegador
	// de teste.
	const quemMostra = await Person.create(browser, 'mostrador');
	const quemAssiste = await Person.create(browser, 'contador');

	await quemMostra.joinVoice();
	await quemAssiste.joinVoice();
	await quemMostra.shareScreen();
	await quemAssiste.watchScreen();

	await expect
		.poll(async () => await quemAssiste.diag(), { timeout: 30_000 })
		.toMatch(/descartadas=\d+/);

	// E a DURACAO das travadas, e nao so a contagem. Oitenta travadas que somam
	// dois segundos em onze minutos sao solucos; oitenta que somam um minuto sao
	// outra coisa. Sem este numero eu vinha chamando a contagem de ruim sem
	// saber o que ela media.
	expect(await quemAssiste.diag()).toMatch(/travadas=\d+\(\d+(\.\d+)?s\)/);

	// Do lado de quem mostra, o PISO da captura nos ultimos dez segundos: a
	// media esconde o buraco, que e justamente o que vira travada do outro lado.
	await expect
		.poll(async () => await quemMostra.diag(), { timeout: 30_000 })
		.toMatch(/captura=\d+fps piso=\d+fps/);

	// E é da imagem que CHEGA, não da que sai: quem mostra não tem o que
	// descartar, e um número desses na linha dele seria uma acusação falsa
	// contra a máquina de quem transmite — o mesmo erro do painel de camadas
	// em 19/09/2026.
	await expect
		.poll(async () => await quemMostra.diag(), { timeout: 30_000 })
		.toContain('video eu:screen:');
	expect(await quemMostra.diag()).not.toMatch(/video eu:screen:.*descartadas=/);

	// As imagens completas e os pedidos delas, somados por camada e para
	// sempre. Ate 20/09/2026 os dois vinham SO da camada de cima, entao
	// rebaseavam quando o conjunto de camadas mudava — e um numero que
	// rebaseia nao pode ser comparado com outro. E a comparacao e a pergunta
	// inteira: chave que ninguem pediu foi decisao do codificador.
	expect(await quemMostra.diag()).toMatch(/chaves=\d+ pedidos=\d+/);

	await quemMostra.close();
	await quemAssiste.close();
});

test('duas partes da tela pedindo os números não estragam a medida uma da outra', async ({
	browser,
}) => {
	// Tudo no painel de números é DIFERENÇA contra a leitura anterior, e mais de
	// uma parte da tela pergunta: a grade tem o seu contador, a imagem em tela
	// cheia tem outro, e quem transmite ainda tem o aviso de limite. Duas
	// perguntas separadas por 20 ms davam à segunda uma janela de 20 ms para
	// dividir — e era ela que escrevia `0 kbit/s` logo depois de 9000 no log do
	// dono, e que fez o juiz de decodificação ler "50% dos quadros" com dois
	// quadros na mão. Um estado de diferença com dois donos não tem dono.
	const quemMostra = await Person.create(browser, 'medido');
	const quemAssiste = await Person.create(browser, 'doisleitores');

	await quemMostra.joinVoice();
	await quemAssiste.joinVoice();
	await quemMostra.shareScreen();
	await quemAssiste.watchScreen();

	await expect
		.poll(async () => (await quemAssiste.videos()).filter((v) => v.w > 0 && v.live).length, {
			timeout: 30_000,
		})
		.toBe(1);

	const par = await quemAssiste.page.evaluate(async () => {
		const w = window as unknown as {
			__harmonyVoice: { stats(): Promise<{ mine: boolean; kbps: number }[]> };
		};
		const pega = (l: { mine: boolean; kbps: number }[]) =>
			l.filter((x) => !x.mine).map((x) => x.kbps);
		// Espera uma medida de verdade aparecer. Cada volta é mais longa que a
		// janela, então cada uma é uma medida nova.
		let a: number[] = [];
		for (let i = 0; i < 20; i++) {
			await new Promise((r) => setTimeout(r, 600));
			a = pega(await w.__harmonyVoice.stats());
			if (a.some((k) => k > 0)) break;
		}
		// E esta vem logo em seguida, que é o caso do defeito.
		const b = pega(await w.__harmonyVoice.stats());
		return { a, b };
	});

	expect(par.a.some((k) => k > 0), 'nenhuma medida de verdade apareceu').toBe(true);
	expect(par.b, 'a segunda pergunta zerou a resposta da primeira').toEqual(par.a);

	await quemMostra.close();
	await quemAssiste.close();
});

// A tela morta do dono, 20/09/2026: quarenta segundos de `0 fps` no Firefox
// dele, e so voltou porque parou e recomecou na mao.
//
// O caminho e estreito de proposito, porque o defeito e estreito: a sala pede
// MENOS (a espera de cinco segundos e armada) e volta a pedir EXATAMENTE o que
// ja esta sendo enviado. Nesse caso `screenWatchers` saia na primeira linha —
// "nao ha nada a fazer" — sem cancelar o desligamento que ja ia acontecer.
//
// Chamado direto em vez de encenado com uma aba escondendo: o tamanho do
// quadradinho no e2e faz a sala pedir outra coisa na volta, que e um caso
// diferente e legitimo. O defeito e a guarda, entao e a guarda que se testa.
test('voltar a pedir o que ja esta no ar cancela o desligamento agendado', async ({
	browser,
}) => {
	test.setTimeout(120_000);
	const quemMostra = await Person.create(browser, 'voltamostra');
	const quemAssiste = await Person.create(browser, 'voltaassiste');

	await quemMostra.joinVoice();
	await quemAssiste.joinVoice();
	await quemMostra.shareScreen();
	await quemAssiste.watchScreen();
	// The watcher's own layer request must reach the sharer before the scene
	// below. Arriving in the middle of it (the PC busy, 16/10/2026: it came
	// 400 ms after the scene began and the server relays it ~2 s later), it
	// legitimately arms a new shrink, and the test failed a correct client.
	await expect
		.poll(async () => await quemAssiste.diag(), { timeout: 30_000 })
		.toMatch(/pedindo a camada/);
	await quemMostra.page.waitForTimeout(3_000);

	const tresCamadas = async () =>
		quemMostra.page.evaluate(async () => {
			const w = window as unknown as {
				__harmonyVoice?: { screenWatchers: (d: unknown) => Promise<void> };
			};
			await w.__harmonyVoice?.screenWatchers({
				screen: true,
				layers: ['alta', 'media', 'baixa'],
			});
		});

	// Assentado nas tres.
	await tresCamadas();
	await expect
		.poll(async () => await quemMostra.diag(), { timeout: 30_000 })
		.toMatch(/voz tela subindo: alta,baixa,media/);
	const antes = (await quemMostra.diag()).length;

	// A sala pede menos: a espera de cinco segundos e armada.
	await quemMostra.page.evaluate(async () => {
		const w = window as unknown as {
			__harmonyVoice?: { screenWatchers: (d: unknown) => Promise<void> };
		};
		await w.__harmonyVoice?.screenWatchers({
			screen: true,
			layers: ['media', 'baixa'],
		});
	});
	expect(await quemMostra.diag()).toMatch(/voz tela vai encolher para baixa,media/);

	// E logo volta a pedir o que ja esta no ar.
	await quemMostra.page.waitForTimeout(500);
	await tresCamadas();

	// Passada a espera inteira, o desligamento nao pode ter acontecido.
	await quemMostra.page.waitForTimeout(8_000);
	const depois = (await quemMostra.diag()).slice(antes);
	expect(
		depois,
		`o desligamento agendado disparou depois de a sala voltar atras:
${depois}`,
	).not.toMatch(/voz tela subindo: baixa,media/);

	await quemMostra.close();
	await quemAssiste.close();
});
