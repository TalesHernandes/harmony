// O medidor: quanto uma chamada custa nesta máquina.
//
// Não é um teste — não afirma nada, não fica vermelho. É uma régua, e está
// aqui porque a mesma medição foi improvisada e jogada fora duas vezes em
// 18/09/2026, com uma conclusão errada em cada uma das duas primeiras vezes.
//
// Roda de propósito, nunca junto com a suíte:
//
//     MEDIR=1 npx playwright test --config playwright.harmony.config.ts --grep @medir
//
// O que ele responde, e que o Gerenciador de Tarefas não responde:
//
//   - onde mora a memória — heap de JavaScript (o nosso código) contra o
//     processo inteiro (o navegador, incluindo captura e codificador). Foi
//     essa separação que mostrou que ~155 MB não eram nossos;
//   - se cresce sozinha com o tempo, que é vazamento, ou se estabiliza, que
//     é preço;
//   - se ligar e desligar empilha, que é recurso criado por tentativa em vez
//     de por chamada;
//   - e qual codec está REALMENTE em uso, que é a única forma honesta de
//     conferir uma decisão de codec — a que está escrita no plano e a que o
//     navegador negociou não são a mesma coisa por definição.

import { test, expect, Person } from './fixtures';
import { execFileSync } from 'node:child_process';

test.setTimeout(300_000);

// O trabalho é de mídia real: ele espera, e esperar é o ponto.
const SEGURAR = 12_000;

type Leitura = {
	fase: string;
	heapMB: number;
	nos: number;
	ouvintes: number;
	processoMB: number;
	emissores: number;
	video: number;
	envio: string;
	codec: string;
};

// A memória dos processos do navegador, que é o número que bate com o
// Gerenciador de Tarefas e que o heap sozinho não mostra.
//
// Filtra por CAMINHO e não por nome: em modo headless o Playwright roda
// `headless_shell.exe` e não `chrome.exe`, então procurar pelo nome devolve
// zero e a coluna inteira some sem dizer por quê. Só Windows, que é onde este
// projeto se desenvolve.
let porQueNao = '';
function memoriaDosProcessos(): number {
	if (process.platform !== 'win32') return 0;
	try {
		const out = execFileSync(
			'powershell',
			[
				'-NoProfile',
				'-Command',
				"(Get-Process | Where-Object { $_.Path -like '*.pw-browsers*' } |" +
					' Measure-Object -Property WorkingSet64 -Sum).Sum',
			],
			{ encoding: 'utf8', timeout: 15_000 },
		).trim();
		if (!out) {
			porQueNao = 'nenhum processo do Playwright encontrado';
			return 0;
		}
		return Math.round((Number(out) / 1048576) * 10) / 10;
	} catch (e) {
		// Contado e não engolido: uma régua que devolve zero calada mente.
		porQueNao = String(e).slice(0, 120);
		return 0;
	}
}

test('@medir quanto custa uma chamada', async ({ browser }) => {
	// A porta é uma variável de ambiente e não um filtro no config: --grep na
	// linha de comando e grepInvert no config se cruzam, e a interseção é
	// vazia — o Playwright responde "No tests found" e parece defeito.
	test.skip(!process.env.MEDIR, 'régua, não teste: rode com MEDIR=1 (veja o topo do arquivo)');

	const eu = await Person.create(browser, 'medidor');
	await eu.page.getByPlaceholder(/Escrever em/).waitFor({ timeout: 15_000 });

	const cdp = await eu.page.context().newCDPSession(eu.page);
	const leituras: Leitura[] = [];

	const ler = async (fase: string): Promise<Leitura> => {
		const mb = (n: number) => Math.round((n / 1048576) * 10) / 10;

		// getHeapUsage pelo CDP, e não performance.memory: aquele devolve um
		// valor quantizado que não se mexe. Deu 9,5 MB em onze leituras
		// seguidas, o que parece um app estável e é a API se protegendo.
		const uso = (await cdp.send('Runtime.getHeapUsage')) as { usedSize: number };
		const dom = (await cdp.send('Memory.getDOMCounters')) as {
			nodes: number;
			jsEventListeners: number;
		};

		// O codec que o navegador de fato negociou, não o que foi pedido.
		const midia = await eu.page.evaluate(async () => {
			const pc = (window as unknown as { __harmonyPC?: RTCPeerConnection }).__harmonyPC;
			if (!pc) return { codec: '—', envio: '—', emissores: 0, video: 0 };
			const stats = await pc.getStats();
			const codecs = new Map<string, string>();
			const ativos: string[] = [];
			const nomes: string[] = [];
			stats.forEach((s: Record<string, unknown>) => {
				if (s.type === 'codec') codecs.set(s.id as string, String(s.mimeType ?? ''));
			});
			stats.forEach((s: Record<string, unknown>) => {
				if (s.type !== 'outbound-rtp' || s.kind !== 'video') return;
				const w = s.frameWidth ?? 0;
				const h = s.frameHeight ?? 0;
				if (Number(w) > 0) ativos.push(`${w}x${h}@${Math.round(Number(s.framesPerSecond ?? 0))}`);
				const m = codecs.get(String(s.codecId ?? ''));
				if (m) nomes.push(m.replace('video/', ''));
			});
			// emissores conta os transceptores de VÍDEO ainda vivos — por
			// kind da faixa e não por "tem encodings", que é verdade também
			// para áudio e teria misturado as duas coisas.
			//
			// É essa contagem que denuncia recurso criado por tentativa: uma
			// câmera ligada e desligada três vezes deve deixar o mesmo número
			// de transceptores que deixou na primeira. Um transceptor é uma
			// vaga de codificador, e vaga de codificador é memória.
			return {
				codec: nomes.length ? [...new Set(nomes)].join('+') : '—',
				envio: ativos.length ? ativos.join(' ') : '—',
				emissores: pc
					.getTransceivers()
					.filter((t) => !t.stopped && (t.sender.track?.kind === 'video' || t.receiver.track?.kind === 'video'))
					.length,
				video: document.querySelectorAll('video').length,
			};
		});

		const l: Leitura = {
			fase,
			heapMB: mb(uso.usedSize),
			nos: dom.nodes,
			ouvintes: dom.jsEventListeners,
			processoMB: memoriaDosProcessos(),
			emissores: midia.emissores,
			video: midia.video,
			envio: midia.envio,
			codec: midia.codec,
		};
		leituras.push(l);
		return l;
	};

	await ler('parado');
	await eu.joinVoice();
	await eu.page.waitForTimeout(3000);
	await ler('na sala, sem vídeo');

	await eu.startCamera();
	await eu.page.waitForTimeout(SEGURAR);
	await ler('câmera');
	await eu.page.waitForTimeout(SEGURAR);
	await ler('câmera, +12s');

	await eu.stopCamera();
	await eu.page.waitForTimeout(3000);

	await eu.shareScreen();
	await eu.page.waitForTimeout(SEGURAR);
	await ler('tela');
	await eu.page.waitForTimeout(SEGURAR);
	await ler('tela, +12s');

	await eu.startCamera();
	await eu.page.waitForTimeout(SEGURAR);
	await ler('tela + câmera');

	await eu.stopCamera();
	await eu.stopScreen();
	await eu.page.waitForTimeout(3000);
	await ler('tudo desligado');

	// Três ciclos de ligar e desligar. Se a memória subir em degrau a cada
	// ciclo, alguma coisa nasce por tentativa e não por chamada; se voltar ao
	// mesmo lugar, é preço e não vazamento.
	for (let i = 1; i <= 3; i++) {
		await eu.startCamera();
		await eu.page.waitForTimeout(3000);
		await eu.stopCamera();
		await eu.page.waitForTimeout(2000);
		await ler(`ciclo ${i}`);
	}

	// E o que a coleta devolve quando é pedida de verdade. A diferença entre
	// esta linha e a de cima é lixo pendente; o que sobra é retenção.
	await cdp.send('HeapProfiler.collectGarbage');
	await eu.page.waitForTimeout(2000);
	await ler('depois do GC');

	const col = (s: string | number, n: number) => String(s).padStart(n);
	console.log('\n  MEDIDOR — uma chamada, do começo ao fim\n');
	console.log(
		'  fase               heap JS   processo    nós  ouvint  emiss  vídeo  codec      saindo',
	);
	for (const l of leituras) {
		console.log(
			`  ${l.fase.padEnd(17)}${col(l.heapMB, 6)} MB${col(l.processoMB || '—', 8)} MB` +
				`${col(l.nos, 7)}${col(l.ouvintes, 8)}${col(l.emissores, 7)}${col(l.video, 7)}  ` +
				`${l.codec.padEnd(11)}${l.envio}`,
		);
	}
	const pico = leituras.reduce((a, b) => (b.heapMB > a.heapMB ? b : a));
	const picoProc = leituras.reduce((a, b) => (b.processoMB > a.processoMB ? b : a));
	console.log(
		`\n  pico de heap: ${pico.heapMB} MB (${pico.fase})` +
			`   |   pico de processo: ${picoProc.processoMB} MB (${picoProc.fase})`,
	);
	console.log(
		'  o que estiver no processo e não no heap é do navegador: captura,' +
			' codificador, compositor.\n',
	);

	// A única afirmação: a medição aconteceu. Uma régua que falha sozinha
	// deixa de ser usada.
	expect(leituras.length).toBeGreaterThan(0);

	await eu.close();
});
