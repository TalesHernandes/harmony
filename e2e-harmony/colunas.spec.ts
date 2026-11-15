// As duas colunas, medidas.
//
// O mesmo motivo da `lista.spec.ts`: estes números somem quando erram. Um fio
// de 1px que volta, uma linha que passa de 32 para 34 pixels, um raio de 6 no
// meio de uma coluna de 12 — nada disso aparece num print, e tudo isso é a
// diferença entre uma tela desenhada e uma tela remendada. É exatamente a
// queixa do dono em 19/09/2026: "metade antigo e metade novo, ta ruim".
//
// Os valores vêm de "harmony - Etapa 2", seções 02 e 03.

import { test, expect, Person } from './fixtures';

// A coluna das pessoas começa **aberta** num monitor (App.tsx: largura >= 768),
// então apertar "pessoas" sem olhar antes é fechá-la. Pergunta-se ao painel, e
// não à linha — é a mesma lição do `openDMWith` nas fixtures.
async function abrirPessoas(who: Person) {
	const painel = who.page.locator('aside');
	if (!(await painel.isVisible())) {
		await who.page.locator('button[aria-label="pessoas"]').first().click();
	}
	await expect(painel).toBeVisible({ timeout: 10_000 });
	return painel;
}

// O que o navegador calculou de verdade — uma classe pode estar no elemento e
// perder para outra.
async function drawn(who: Person, selector: string, props: string[]) {
	return who.page.evaluate(
		({ selector, props }) => {
			const el = document.querySelector(selector);
			if (!el) return null;
			const s = getComputedStyle(el);
			const out: Record<string, string> = {};
			for (const p of props) out[p] = s.getPropertyValue(p);
			return out;
		},
		{ selector, props },
	);
}

test('nenhuma das três fronteiras do topo tem fio, e o degrau de superfície é o que separa', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'fiozero');
	await ana.page.locator('nav.channel-column').first().waitFor({ timeout: 15_000 });
	await abrirPessoas(ana);

	const lados = ['border-top-width', 'border-right-width', 'border-bottom-width', 'border-left-width'];

	// A coluna dos canais, a coluna das pessoas e o cabeçalho da conversa: as
	// três peças do topo, e nenhuma das três se separa das outras com um fio.
	// The channel column is not among them since the islands (23/09/2026): its
	// own ground is the page's black, and its surface is in the islands below.
	for (const seletor of ['aside', 'main > header']) {
		const caixa = await drawn(ana, seletor, [...lados, 'background-color', 'height']);
		expect(caixa, `não achei ${seletor}`).not.toBeNull();
		for (const lado of lados) {
			expect(caixa![lado], `${seletor} ainda tem ${lado}`).toBe('0px');
		}
		// #18181b — a superfície. É ela que separa, e é por isso que o fio pode
		// ir embora.
		expect(caixa!['background-color'], `${seletor} mudou de superfície`).toBe('rgb(24, 24, 27)');
	}

	// E o topo do app é uma linha só: o "harmony" da coluna mora numa faixa de
	// 64 (desde que o dono disse que estava tudo pequeno, 20/09/2026), e o
	// cabeçalho da conversa, desde que virou ilha (23/09/2026), tem 48 a 8 do
	// topo — o mesmo centro, 32. O que se mede é o centro dos dois.
	const centros = await ana.page.evaluate(() => {
		const meio = (el: Element) => {
			const r = el.getBoundingClientRect();
			return Math.round((r.top + r.bottom) / 2);
		};
		const marca = [...document.querySelectorAll('nav.channel-column span')].find(
			(e) => e.textContent === 'harmony',
		)!;
		return [meio(document.querySelector('main > header')!), meio(marca.parentElement!)];
	});
	expect(centros).toEqual([32, 32]);

	// E **nada dentro da coluna** tem borda, em nenhum lado.
	//
	// Não é excesso de zelo: em 20/09/2026 eu pus quatro fios de 1px dentro
	// dela para separar os blocos e o dono respondeu "não quero esses
	// risquinhos dividindo nada NUNCA". A separação é degrau de superfície
	// (#060607 na moldura, #18181b na lista) e distância. O teste acima só
	// olhava a própria `nav`, e os quatro fios que ele recusou estavam todos
	// em filhos dela — passou verdinho o tempo todo.
	const comBorda = await ana.page.evaluate(() => {
		const col = document.querySelector('nav.channel-column')!;
		const lados = [
			'border-top-width',
			'border-right-width',
			'border-bottom-width',
			'border-left-width',
		];
		const achados: string[] = [];
		for (const el of col.querySelectorAll('*')) {
			const s = getComputedStyle(el);
			for (const lado of lados) {
				if (parseFloat(s.getPropertyValue(lado)) > 0) {
					achados.push(`${el.tagName.toLowerCase()} "${(el.textContent ?? '').trim().slice(0, 16)}" ${lado}`);
					break;
				}
			}
		}
		return achados;
	});
	expect(comBorda, `voltou fio na coluna: ${comBorda.join(' | ')}`).toEqual([]);

	// E a coluna é o preto da página com **três ilhas** em cima dele: as abas,
	// a lista, e você lá embaixo. O dono pediu assim em 23/09/2026, riscando
	// de preto por cima de um print ("tipo ilhas"), depois de recusar fios, um
	// degrau de superfície e "nenhuma divisão". Três e só três pintam fundo;
	// cada uma com raio 16, a 8px da borda esquerda da janela, e 8px entre uma
	// e a seguinte — o rascunho dele tinha a de baixo bem mais longe, e ele
	// mesmo disse que estava fora de escala. À direita, 4: o puxador de
	// redimensionar (4px) completa os 8 até a ilha do cabeçalho.
	const ilhas = await ana.page.evaluate(() => {
		const col = document.querySelector('nav.channel-column')!;
		const c = col.getBoundingClientRect();
		const pintados: string[] = [];
		for (const el of col.querySelectorAll('*')) {
			if (el.closest('[data-island]') !== el && el.closest('[data-island]')) continue;
			if (el.hasAttribute('data-island')) continue;
			const bg = getComputedStyle(el).backgroundColor;
			if (bg !== 'rgba(0, 0, 0, 0)' && bg !== 'transparent') pintados.push(el.tagName);
		}
		const lista = [...col.querySelectorAll('[data-island]')].map((d) => {
			const r = d.getBoundingClientRect();
			const s = getComputedStyle(d);
			return {
				nome: d.getAttribute('data-island'),
				fundo: s.backgroundColor,
				raio: s.borderTopLeftRadius,
				esquerda: Math.round(r.left - c.left),
				direita: Math.round(c.right - r.right),
				topo: r.top,
				base: r.bottom,
			};
		});
		return { coluna: getComputedStyle(col).backgroundColor, pintados, lista };
	});
	expect(ilhas.coluna).toBe('rgb(6, 6, 7)');
	expect(ilhas.pintados, 'algo fora das ilhas pinta o próprio fundo').toEqual([]);
	expect(ilhas.lista.map((i) => i.nome)).toEqual(['abas', 'lista', 'voce']);
	for (const i of ilhas.lista) {
		expect([i.fundo, i.raio, i.esquerda, i.direita], `ilha ${i.nome}`).toEqual([
			'rgb(24, 24, 27)',
			'8px', // one corner since 24/09/2026 (cantos.spec.ts)
			8,
			4,
		]);
	}
	for (let k = 1; k < ilhas.lista.length; k++) {
		expect(Math.round(ilhas.lista[k].topo - ilhas.lista[k - 1].base), 'o vão entre duas ilhas').toBe(8);
	}

	// E o resto da janela, pedido na mesma noite: o cabeçalho é uma ilha, a
	// coluna de pessoas é uma ilha grande da altura da janela, e a conversa fica
	// no preto, sem a faixa cinza que sobrava embaixo dela. Todo vão entre duas
	// ilhas vizinhas é 8, e a conversa começa na altura da ilha das abas.
	const pessoas = ana.page.locator('button[aria-label="pessoas"]').first();
	if ((await pessoas.getAttribute('aria-pressed')) !== 'true') await pessoas.click();
	const janela = await ana.page.evaluate(() => {
		const caixa = (sel: string) => {
			const el = document.querySelector(sel)!;
			const r = el.getBoundingClientRect();
			const s = getComputedStyle(el);
			return { l: r.left, r: r.right, t: r.top, b: r.bottom, fundo: s.backgroundColor, raio: s.borderTopLeftRadius };
		};
		return {
			w: innerWidth,
			h: innerHeight,
			main: getComputedStyle(document.querySelector('main')!).backgroundColor,
			lista: caixa('nav.channel-column [data-island="lista"]'),
			abas: caixa('nav.channel-column [data-island="abas"]'),
			topo: caixa('[data-island="topo"]'),
			pessoas: caixa('[data-island="pessoas"]'),
			conversa: caixa('main > header + div'),
		};
	});
	expect(janela.main).toBe('rgb(6, 6, 7)');
	for (const i of [janela.topo, janela.pessoas]) {
		expect([i.fundo, i.raio]).toEqual(['rgb(24, 24, 27)', '8px']);
	}
	expect(Math.round(janela.topo.l - janela.lista.r), 'da coluna ao cabeçalho').toBe(8);
	expect(Math.round(janela.pessoas.l - janela.topo.r), 'do cabeçalho às pessoas').toBe(8);
	expect(Math.round(janela.topo.t), 'o cabeçalho, do topo').toBe(8);
	expect(Math.round(janela.pessoas.t), 'as pessoas, do topo').toBe(8);
	expect(Math.round(janela.h - janela.pessoas.b), 'as pessoas, de baixo').toBe(8);
	expect(Math.round(janela.w - janela.pessoas.r), 'as pessoas, da direita').toBe(8);
	expect(Math.round(janela.conversa.t), 'a conversa começa com as abas').toBe(Math.round(janela.abas.t));
	expect(Math.round(janela.conversa.b), 'nada embaixo da conversa').toBe(janela.h);

	await ana.close();
});

test('uma linha da coluna tem 36 pixels e 8 de raio, e a que você está vendo ganha a barra de destaque', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'linha32');
	// Pelo nome acessível da linha, e não por "o botão com bg-default": a aba
	// aberta também tem essa superfície, e a primeira vez que este teste rodou
	// ele mediu o "Servidor" achando que media o "#geral".
	const linha = ana.page.locator('nav.channel-column button[aria-label="#geral"]').first();
	await linha.waitFor({ timeout: 15_000 });
	await linha.click();

	// 32 no D4t, 40 quando a letra cresceu, 36 quando o dono disse que as
	// coisas estavam muito separadas. A letra ficou em 15 nas três vezes: o que
	// muda aqui é o ar em volta dela, e é por isso que as duas medidas abaixo
	// andam juntas neste teste.
	const caixa = await linha.boundingBox();
	expect(caixa!.height).toBe(36);
	// O texto da linha é o que puxou a altura: 16px desde 24/09/2026, um degrau acima do resto (style.css .channel-column).
	// Medido aqui e não só na folha de estilo porque é a linha que tem que
	// crescer junto — uma altura de 40 com texto de 13 é ar, não legibilidade.
	expect(
		await linha.evaluate((el) => getComputedStyle(el).fontSize),
		'a linha ficou alta mas o texto ficou pequeno',
	).toBe('16px');
	const estilo = await ana.page.evaluate(() => {
		const el = document.querySelector('nav.channel-column button[aria-label="#geral"]');
		if (!el) return null;
		const s = getComputedStyle(el);
		const bar = el.querySelector('span.absolute');
		const b = bar ? getComputedStyle(bar) : null;
		return {
			radius: s.borderTopLeftRadius,
			fundo: s.backgroundColor,
			barraLargura: b?.width ?? '',
			barraCor: b?.backgroundColor ?? '',
		};
	});
	expect(estilo, 'nenhuma linha está marcada como a aberta').not.toBeNull();
	// 8 since the two-corner rule (23/09/2026): a row sits 8 inside a 16px
	// island, so its corner is 16 - 8.
	expect(estilo!.radius).toBe('8px');
	// #27272a, o --color-default do kit.
	expect(estilo!.fundo).toBe('rgb(39, 39, 42)');
	// Três pixels de accent, encostados na borda da coluna.
	expect(estilo!.barraLargura).toBe('3px');
	expect(estilo!.barraCor).toBe('rgb(4, 133, 247)');

	await ana.close();
});

test('a coluna das pessoas é separada por estado, e a conta do título bate com as linhas', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'agrupada');
	const painel = await abrirPessoas(ana);
	// Esperar a lista chegar do servidor: ela é desenhada vazia e preenchida
	// depois, e um título de grupo antes disso conta zero pessoas.
	await painel.locator('button[data-person]').first().waitFor({ timeout: 15_000 });

	const titulos = await painel.locator('p').allInnerTexts();
	// O primeiro é o total; os outros são os grupos que existem agora.
	// `allInnerTexts` devolve o texto como a tela o pinta, e o rótulo é
	// `uppercase` no CSS - o `textContent` lá embaixo não é.
	expect(titulos[0]).toMatch(/^Pessoas · \d+$/i);
	expect(titulos.length, 'nenhum grupo foi desenhado').toBeGreaterThan(1);

	// Cada grupo diz quantos tem, e tem quantos diz — contado no DOM em vez de
	// lido do texto, que é a única forma de a conta ser uma medida.
	//
	// E, o que importa mais: **cada pessoa está no grupo do estado dela**. Só a
	// conta não basta, e isso ficou provado — com todo mundo empilhado num
	// grupo só o número continua batendo, porque ele sai da mesma lista. O
	// `data-state` de cada linha vem de outro lugar, e é por isso que ele
	// consegue discordar.
	const conferido = await painel.evaluate((el) => {
		const out: Array<[string, number, number, string[]]> = [];
		for (const grupo of el.querySelectorAll(':scope > div')) {
			const titulo = grupo.querySelector('p')?.textContent ?? '';
			const [nome, n] = titulo.split(' · ');
			const linhas = [...grupo.querySelectorAll('button[data-person]')];
			out.push([
				nome,
				Number(n),
				linhas.length,
				linhas.map((b) => b.getAttribute('data-state') ?? '?'),
			]);
		}
		return out;
	});
	expect(conferido.length).toBeGreaterThan(0);
	const cabe: Record<string, string[]> = {
		// Quem está em chamada está online, e o fone verde ao lado do nome diz o
		// resto — um quarto grupo o tiraria da lista onde procuram por ele.
		Online: ['online', 'in_call'],
		Ausente: ['away'],
		Offline: ['offline'],
	};
	for (const [nome, diz, tem, estados] of conferido) {
		expect(Object.keys(cabe)).toContain(nome);
		expect(tem, `o grupo ${nome} diz ${diz} e tem ${tem}`).toBe(diz);
		for (const estado of estados) {
			expect(cabe[nome], `alguém "${estado}" foi parar no grupo ${nome}`).toContain(estado);
		}
	}

	// E uma pessoa é uma linha de 48 pixels com um rosto de 32.
	//
	// `avatar-hole-lg` e não `avatar-hole`: são duas regras porque são dois
	// rostos, e o buraco é desenhado em pixels absolutos — 7 para o de 32, 6
	// para o de 28. Um seletor de classe não casa com o outro nome, e é bom que
	// não case: se o rosto grande voltar a usar o buraco pequeno, este teste
	// não acha a linha em vez de medir a coisa errada.
	const pessoa = painel.locator('button[data-person]').first();
	expect((await pessoa.boundingBox())!.height).toBe(48);
	expect((await pessoa.locator('span.avatar-hole-lg').first().boundingBox())!.width).toBe(32);

	await ana.close();
});

test('a coluna da esquerda tem uma linha para as marcas, uma para os nomes e uma para os controles', async ({
	browser,
}) => {
	// Este teste existe por uma frase do dono em 20/09/2026, olhando a coluna
	// pronta: *"tenho a sensação de que tá tudo meio jogado, mas não sei como
	// resolver isso não"*. Ele não conseguia apontar, e com razão — não havia
	// **um** erro. Havia seis arestas verticais diferentes numa coluna de 240
	// pixels: o rótulo em 16, o `#` em ~21, o alto-falante em 16, o nome de um
	// canal em 46, o de uma sala em 44 e o rosto do rodapé em 8.
	//
	// Nenhuma dessas diferenças aparece num print e nenhuma delas tem nome. É
	// exatamente o tipo de coisa que só uma régua encontra, e exatamente o tipo
	// de coisa que volta na próxima edição se ninguém estiver medindo.
	//
	// Como `dono`, para que os botões de `+` existam: eles são a linha da
	// direita, e sem eles o teste mediria metade.
	const ana = await Person.signIn(browser, 'dono');
	await ana.page.locator('nav.channel-column').first().waitFor({ timeout: 15_000 });
	// A sala de voz vem do servidor depois da pintura, e é ela que dá ao teste
	// a segunda marca para comparar com a primeira. Sem esperar, ele compara um
	// item com ele mesmo e passa sempre.
	await ana.page
		.locator('nav.channel-column [data-icon="headphones"]')
		.first()
		.waitFor({ timeout: 15_000 });

	const medida = await ana.page.evaluate(() => {
		const col = document.querySelector('nav.channel-column')!;
		// Measured from the island each thing sits in, since the column became
		// islands (23/09/2026): the lines are 16 from the island's edge, the
		// way they were 16 from the column's.
		const ilha = (el: Element) => (el.closest('[data-island]') ?? col).getBoundingClientRect().left;
		const esq = (el: Element) => Math.round(el.getBoundingClientRect().left - ilha(el));
		const dir = (el: Element) => Math.round(el.getBoundingClientRect().right - ilha(el));
		// Por atributo e não por classe: o cabeçalho da coluna também tem
		// px-4, e a primeira versão deste teste mediu ele.
		const rodape = col.querySelector('[data-account]');
		// Pelo nome e pelo irmão anterior dele, e não pela classe da caixa
		// da marca. Se alguém puser o alto-falante numa caixa de outro
		// tamanho, um seletor de classe simplesmente não acha a marca e o
		// teste morre contando em vez de medindo — foi o que a primeira
		// versão deste teste fez quando eu recoloquei o defeito de propósito.
		// Assim ele sempre acha as duas e sempre diz **onde** elas estão.
		const nomes = [...col.querySelectorAll('.scroller span.min-w-0.flex-1.truncate')];
		return {
			marcas: nomes.map((n) => esq(n.previousElementSibling!)),
			larguraDasMarcas: nomes.map((n) =>
				Math.round(n.previousElementSibling!.getBoundingClientRect().width),
			),
			nomes: nomes.map(esq),
            textNames: nomes.filter(n => n.previousElementSibling!.querySelector('[data-icon="hashtag"]')).map(esq),
            voiceNames: nomes.filter(n => n.previousElementSibling!.querySelector('[data-icon="headphones"]')).map(esq),
			rotulos: [...col.querySelectorAll('.scroller .text-label > span')].map(esq),
			maisDireita: [...col.querySelectorAll('.scroller .text-label > button')].map(dir),
			rostoDoRodape: rodape ? esq(rodape.querySelector('span')!) : -1,
			engrenagemDireita: rodape ? dir(rodape.querySelector('button[aria-label="ajustes"]')!) : -1,
			largura: Math.round(col.querySelector('[data-island="lista"]')!.getBoundingClientRect().width),
		};
	});

	// Duas marcas pelo menos — um canal de texto e uma sala de voz — e as duas
	// na mesma aresta. É a comparação que não existia: cada lista desenhava a
	// sua própria caixa.
	expect(medida.marcas.length, 'não achei as marcas das linhas').toBeGreaterThan(1);
	expect(new Set(medida.marcas), `as marcas estão em ${medida.marcas.join(', ')}`).toEqual(
		new Set([16]),
	);
	// E na mesma caixa, não só na mesma aresta: duas caixas de larguras
	// diferentes começando no mesmo ponto põem os nomes em pontos diferentes,
	// que é literalmente o defeito que este teste nasceu para pegar.
	expect(
		new Set(medida.larguraDasMarcas),
		`as caixas das marcas medem ${medida.larguraDasMarcas.join(', ')}`,
	).toEqual(new Set([18]));

	// Text channels halve the hash/name gap: 16 + 18 + 3 = 37.
	// Voice rooms keep their existing gap: 16 + 18 + 6 = 40.
	expect(medida.nomes.length).toBeGreaterThan(1);
	expect(new Set(medida.textNames)).toEqual(new Set([37]));
	expect(new Set(medida.voiceNames)).toEqual(new Set([40]));

	// Os títulos dos grupos na mesma aresta das marcas, que é a aresta do
	// conteúdo da coluna inteira — a mesma de "harmony", lá em cima.
	expect(medida.rotulos.length).toBeGreaterThan(1);
	expect(new Set(medida.rotulos)).toEqual(new Set([16]));

	// O rodapé não é uma linha de lista e não tem a pastilha atrás; ele estava
	// alinhado em 8, que é a borda dessa pastilha — uma linha que não se
	// desenha em lugar nenhum. Agora começa onde tudo o que significa alguma
	// coisa nesta coluna começa.
	expect(medida.rostoDoRodape, 'o rodapé voltou para a borda da pastilha').toBe(16);

	// E a direita também é uma linha só: o + de cada grupo e a engrenagem do
	// rodapé terminam no mesmo pixel. Medido contra a largura da coluna, e não contra um número:
	// este teste dizia 192, que é 208 − 16, porque um navegador novo abria a
	// coluna no degrau mais estreito em vez do padrão de 256 (o `Number(null)`
	// da auditoria de 23/09/2026). O número fixo tinha fotografado o defeito.
	// Desde 24/09/2026 é 8, não 16: o dono viu o quadrado do + terminando 8
	// antes da linha de canal embaixo dele. A linha da direita agora é a das
	// pastilhas das linhas, e o + e a engrenagem terminam nela.
	const borda = medida.largura - 8;
	expect(medida.maisDireita.length, 'os botões de + não apareceram').toBeGreaterThan(1);
	for (const x of medida.maisDireita) expect(x).toBe(borda);
	expect(medida.engrenagemDireita).toBe(borda);

	await ana.close();
});

test('o nome de uma linha está na mesma altura que a marca dela', async ({ browser }) => {
	// Três rodadas de 20/09/2026 moram neste teste, e as duas primeiras estavam
	// erradas do mesmo jeito.
	//
	// A queixa: *"os textos dos canais nao estao alinhados na horizontal
	// corretamente com seus # e icones de volume, eles estao mais para baixo"*.
	// Eu tratei como deslocamento e subi o nome, duas vezes, com dois números
	// diferentes — e das duas vezes uma palavra ficou boa e outra ficou pior,
	// porque a tinta de uma palavra depende da palavra (`geral` tem perna de
	// `g`, `links` não tem nada abaixo da linha).
	//
	// **Não era deslocamento, era tamanho.** O `#` estava desenhado a 18px ao
	// lado de uma palavra de 15: 13,2px de tinta contra 11,0. Os topos se
	// encontravam e as linhas de base ficavam 2,1px separadas — foi onde o dono
	// finalmente pôs uma régua. Escorregar uma das duas só troca qual ponta
	// fica errada; duas letras de tamanhos diferentes não se alinham.
	//
	// Agora o `#` tem o tamanho da linha, então os dois são uma linha de texto
	// de 15px numa caixa de 22 e **o navegador põe os dois na mesma base
	// sozinho**. É isto que o teste exige, e é uma igualdade e não uma
	// tolerância de gosto: base com base, topo com topo.
	const ana = await Person.signIn(browser, 'dono');
	await ana.page.locator('nav.channel-column').first().waitFor({ timeout: 15_000 });
	await ana.page
		.locator('nav.channel-column [data-icon="headphones"]')
		.first()
		.waitFor({ timeout: 15_000 });
	// A Inter é servida daqui de dentro; se ela não tiver chegado, o navegador
	// mede a fonte do sistema e a conta inteira fala de outra letra.
	await ana.page.evaluate(() => document.fonts.ready);

	const linhas = await ana.page.evaluate(() => {
		const ctx = document.createElement('canvas').getContext('2d')!;
		const S = 400;
		// Medidas da fonte tiradas grandes e divididas: o Chrome arredonda as
		// de tamanho pequeno para inteiro, e o erro procurado aqui é de um
		// pixel e pouco.
		const em = (peso: string, texto: string) => {
			ctx.font = `${peso} ${S}px Inter, sans-serif`;
			const m = ctx.measureText(texto);
			return {
				subida: m.fontBoundingBoxAscent / S,
				descida: m.fontBoundingBoxDescent / S,
				tinta: m.actualBoundingBoxAscent / S,
			};
		};
		const base = (el: Element, m: { subida: number; descida: number }) => {
			const fs = parseFloat(getComputedStyle(el).fontSize);
			const r = el.getBoundingClientRect();
			return r.top + (r.height - (m.subida + m.descida) * fs) / 2 + m.subida * fs;
		};

		const col = document.querySelector('nav.channel-column')!;
		const nomes = [...col.querySelectorAll('.scroller span.min-w-0.flex-1.truncate')];
		return nomes.map((n) => {
			const marca = n.previousElementSibling!;
			const texto = n.textContent!.trim();
			const fsN = parseFloat(getComputedStyle(n).fontSize);
			// A ascendente do `l` é a referência: é o que uma palavra minúscula
			// tem de mais alto, e é o que a tinta do `#` tem de medir.
			const mN = em(getComputedStyle(n).fontWeight, 'l');
			const linhaDeBase = base(n, mN);

			const svg = marca.querySelector('svg');
			if (svg) {
				const r = svg.getBoundingClientRect();
				return {
					texto,
					tipo: 'desenho' as const,
					// Onde o meio do desenho cai acima da base do nome.
					acimaDaBase: Math.round((linhaDeBase - (r.top + r.height / 2)) * 100) / 100,
					rodapes: 0,
					topos: 0,
				};
			}
			const fsM = parseFloat(getComputedStyle(marca).fontSize);
			const mM = em(getComputedStyle(marca).fontWeight, marca.textContent!.trim());
			const baseDaMarca = base(marca, mM);
			return {
				texto,
				tipo: 'caractere' as const,
				acimaDaBase: Math.round((linhaDeBase - (baseDaMarca - (mM.tinta * fsM) / 2)) * 100) / 100,
				rodapes: Math.round((baseDaMarca - linhaDeBase) * 100) / 100,
				topos:
					Math.round(
						(baseDaMarca - mM.tinta * fsM - (linhaDeBase - mN.tinta * fsN)) * 100,
					) / 100,
			};
		});
	});

	expect(linhas.length, 'não achei as linhas da coluna').toBeGreaterThan(1);
	// Since 24/09/2026 the `#` is the kit's glyph too, so every mark is a
	// drawing and the last check below - every drawing at the same height
	// against its name - is the whole of it. A character mark, if one comes
	// back, still has to meet the two checks in between.
	expect(
		linhas.filter((l) => l.tipo === 'desenho').length,
		'nenhuma linha tem marca desenhada',
	).toBeGreaterThan(1);

	// Uma marca que é letra divide a linha de base com o nome, e por ser do
	// mesmo tamanho acaba na mesma altura em cima também. Meio pixel de folga
	// para o arredondamento, e não para o gosto: o valor certo é zero.
	for (const l of linhas.filter((x) => x.tipo === 'caractere')) {
		expect(
			Math.abs(l.rodapes),
			`em "${l.texto}" a marca e o nome não pisam na mesma linha: ${l.rodapes}px`,
		).toBeLessThanOrEqual(0.5);
		expect(
			Math.abs(l.topos),
			`em "${l.texto}" a marca é de outro tamanho que o nome: o topo dela está ` +
				`${l.topos}px fora`,
		).toBeLessThanOrEqual(0.5);
	}

	// E as duas espécies de marca — a letra e o desenho — caem no mesmo lugar
	// em relação ao nome, que é o que faz as duas listas lerem como uma só.
	const alturas = linhas.map((l) => l.acimaDaBase);
	const fora = Math.max(...alturas) - Math.min(...alturas);
	expect(
		fora,
		`as marcas não concordam entre si: ${linhas.map((l) => `${l.texto} ${l.acimaDaBase}`).join(', ')}`,
	).toBeLessThanOrEqual(0.5);

	await ana.close();
});

test('o botão de atualizar aparece quando o servidor troca de versão, e só então', async ({
	browser,
}) => {
	// O pedido do dono em 20/09/2026, com o motivo: *"no aplicativo do edge
	// nunca sei qual a versao atual e se ta desatualizado e tenho que abrir e
	// fechar"*. A versão é o nome com hash do pacote, e o `index.html` que o
	// servidor devolve sempre nomeia o atual (src/version.ts).
	const ana = await Person.create(browser, 'versaonova');
	await ana.page.locator('nav.channel-column').first().waitFor({ timeout: 15_000 });
	await expect(ana.page.locator('[data-update]')).toHaveCount(0);

	// O que esta página está rodando de verdade. Daqui para a frente o `/` é
	// respondido por nós — e só daqui para a frente, porque antes disso a
	// página precisou carregar de verdade.
	// O que esta página está rodando de verdade — o script **e** a folha de
	// estilo, que é como o `version.ts` mede. Daqui para a frente o `/` é
	// respondido por nós, e só daqui para a frente, porque antes disso a
	// página precisou carregar de verdade.
	const meu = await ana.page.evaluate(() => ({
		js: document.querySelector('script[type="module"][src]')!.getAttribute('src')!,
		css: document.querySelector('link[rel="stylesheet"]')!.getAttribute('href')!,
	}));
	let sirvo = { ...meu };
	let pedidos = 0;
	await ana.page.route(
		(url) => url.pathname === '/',
		async (route) => {
			if (route.request().method() !== 'GET') return route.continue();
			pedidos++;
			await route.fulfill({
				status: 200,
				contentType: 'text/html',
				body:
					`<!doctype html><html><head>` +
					`<script type="module" crossorigin src="${sirvo.js}"></script>` +
					`<link rel="stylesheet" crossorigin href="${sirvo.css}">` +
					`</head><body><div id="app"></div></body></html>`,
			});
		},
	);

	const perguntar = () =>
		ana.page.evaluate(() => document.dispatchEvent(new Event('visibilitychange')));
	// O piso entre duas perguntas, em `version.ts`. Esperar por ele custa dez
	// segundos deste teste e é o preço de o `focus` não virar um pedido por
	// clique — ver a asserção sobre `pedidos`, mais abaixo.
	const PISO = 10_000;

	// **Primeiro a metade que tem de ficar calada.** O servidor responde, e
	// responde com a mesma versão: nada pode aparecer. Sem esta metade o teste
	// passaria com um botão que aparece a qualquer resposta, que é o defeito
	// mais provável deste código.
	await ana.page.waitForTimeout(PISO + 500);
	await perguntar();
	await ana.page.waitForTimeout(1_500);
	expect(pedidos, 'ninguém chegou a perguntar ao servidor').toBeGreaterThan(0);
	await expect(
		ana.page.locator('[data-update]'),
		'o botão apareceu com a versão que já está na tela',
	).toHaveCount(0);

	// E perguntar três vezes seguidas não são três pedidos. O `focus` dispara
	// a cada clique de volta na janela, e sem piso uma tarde de trabalho seria
	// um pedido por segundo.
	const antes = pedidos;
	await perguntar();
	await perguntar();
	await perguntar();
	await ana.page.waitForTimeout(1_000);
	expect(pedidos, 'o piso não segurou as perguntas seguidas').toBe(antes);

	// Agora o servidor passa a anunciar outro pacote. Só a folha de estilo,
	// de propósito: uma rodada que mexe só no visual deixa o JavaScript
	// idêntico, e uma verificação que olhasse só o script ficaria calada
	// exatamente na rodada em que a tela inteira mudou.
	sirvo = { js: meu.js, css: '/static/assets/index-OutroEstilo.css' };
	await ana.page.waitForTimeout(PISO + 500);
	await perguntar();
	const botao = ana.page.locator('[data-update]');
	await expect(botao).toBeVisible({ timeout: 10_000 });
	// The kit's arrow since 24/09/2026, not the system font's ↻.
	await expect(botao.locator('[data-icon="arrows-rotate"]')).toBeVisible();

	// E ele fica no canto direito do cabeçalho da coluna, que é onde o dono
	// pediu: "ao lado de harmony... mas no canto direito do menu esquerdo".
	//
	// A medida é a **borda direita**, que é a que ele descreveu, e não onde o
	// botão começa: onde ele começa depende da largura dele. Foi essa asserção
	// que pegou a primeira versão, uma pílula escrita "Atualizar" com 94px de
	// largura, espremendo o "harmony" numa coluna de 208. Daí os 28 redondos.
	const cabecalho = ana.page.locator('nav.channel-column > div').first();
	const c = (await cabecalho.boundingBox())!;
	const b = (await botao.boundingBox())!;
	// 24 and not 16 since the islands: the header stands on the black, and its
	// content lines up with the islands' content — 8 of gap plus the 16 inside.
	expect(Math.round(c.x + c.width - (b.x + b.width))).toBe(24);
	expect(Math.round(b.width), 'o botão cresceu e vai espremer a marca').toBe(28);

	// E a marca continua inteira ao lado dele.
	const marca = (await ana.page.locator('nav.channel-column .text-title').first().boundingBox())!;
	expect(marca.x + marca.width, 'o botão encostou no "harmony"').toBeLessThan(b.x);

	await ana.close();
});

test('a barra de ações da mensagem são quatro glifos do kit, e nenhuma palavra', async ({
	browser,
}) => {
	// Os dois últimos — pencil e trash-bin — chegaram em 20/09/2026 e são o
	// motivo de a barra ter ficado escrita por dois dias (PLANO-DESIGN §11.1).
	const ana = await Person.create(browser, 'quatroglifos');
	await ana.inChannel('geral').waitFor({ timeout: 15_000 });
	await ana.inChannel('geral').fill('a barra desta mensagem tem que ser de ícones');
	await ana.page.keyboard.press('Enter');

	const minha = ana.page
		.locator('[data-message]')
		.filter({ hasText: 'a barra desta mensagem tem que ser de ícones' })
		.first();
	await minha.waitFor({ timeout: 15_000 });
	await minha.hover();

	const barra = minha.locator('[data-actions]');
	await expect(barra).toBeVisible({ timeout: 10_000 });
	for (const glifo of ['face-smile', 'arrow-shape-turn-up-left', 'pencil', 'trash-bin']) {
		await expect(barra.locator(`[data-icon="${glifo}"]`), `falta o ${glifo}`).toBeVisible();
	}
	// E nenhuma das quatro palavras que estavam ali sobrou.
	await expect(barra).toHaveText('');

	await ana.close();
});

// Microphone and hearing remain on the account row. The voice row now also
// carries the shared screen selector requested in parity 7c; camera stays on
// the call bar. Outside a call only face, name and settings remain.
test('na ilha de baixo, microfone e fone moram na linha da conta', async ({ browser }) => {
	const ana = await Person.create(browser, 'ilhac');
	await ana.page.setViewportSize({ width: 1440, height: 900 });
	const conta = ana.page.locator('nav.channel-column [data-account]').first();
	await expect(conta.locator('[data-toggle]')).toHaveCount(0);

	await ana.joinVoice();
	await expect(conta.locator('[data-toggle]')).toHaveCount(2, { timeout: 15_000 });
	const coluna = ana.page.locator('nav.channel-column').first();
	await expect(coluna.locator('[data-open="screen"]')).toHaveCount(1);
	await expect(coluna.locator('[data-open="camera"]')).toHaveCount(0);
	const fundos = await conta
		.locator('[data-toggle]')
		.evaluateAll((els) => els.map((e) => getComputedStyle(e).backgroundColor));
	expect(fundos).toEqual(['rgb(35, 35, 37)', 'rgb(35, 35, 37)']);

	// E a ilha inteira numa aresta só dos dois lados: o texto da voz e o rosto
	// a 16 da borda da ilha, o botão de sair e a engrenagem terminando a 8
	// (a linha das pastilhas e dos +, desde 24/09/2026).
	const arestas = await ana.page.evaluate(() => {
		const ilha = document.querySelector('nav.channel-column [data-island="voce"]')!.getBoundingClientRect();
		const r = (sel: string) => document.querySelector(`nav.channel-column [data-island="voce"] ${sel}`)!.getBoundingClientRect();
		return {
			voz: Math.round(r('button[title="voltar para a chamada" i]').left - ilha.left),
			rosto: Math.round(r('[data-account] > button[aria-label="Meu perfil"]').left - ilha.left),
			sair: Math.round(ilha.right - r('button[title="sair da sala" i]').right),
			engrenagem: Math.round(ilha.right - r('button[aria-label="ajustes"]').right),
		};
	});
	expect(arestas).toEqual({ voz: 16, rosto: 16, sair: 8, engrenagem: 8 });

	// E na coluna mais estreita o nome sai da frente em vez de virar "D..":
	// escondido, mas ocupando o lugar, para os botões ficarem na aresta.
	const nome = conta.locator('.account-name');
	await expect(nome).toBeVisible();
	await ana.page.evaluate(() => localStorage.setItem('harmony.column', '0'));
	await ana.page.reload();
	await ana.joinVoice();
	await expect(conta.locator('[data-toggle]')).toHaveCount(2, { timeout: 15_000 });
	await expect(nome).toBeHidden();
	await ana.close();
});
