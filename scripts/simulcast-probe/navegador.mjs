// O navegador da sonda: manda tres camadas de H.264 para o servidor em
// scripts/simulcast-probe/main.go e imprime o que saiu de cada uma.
//
// Roda da RAIZ do repositorio, porque o Playwright vem do node_modules daqui:
//   PLAYWRIGHT_BROWSERS_PATH="$USERPROFILE/.pw-browsers" node scripts/simulcast-probe/navegador.mjs
import { chromium } from '@playwright/test';

const ENDERECO = process.env.SONDA ?? 'http://127.0.0.1:8123';

const browser = await chromium.launch({ args: ['--mute-audio'] });
const page = await browser.newPage();
// A pagina tem de vir do mesmo lugar do servidor: assim o fetch e da mesma
// origem e nao ha CORS no meio de uma medicao.
await page.goto(ENDERECO + '/vazio').catch(() => {});

const saida = await page.evaluate(async () => {
	const pc = new RTCPeerConnection();

	// Conteudo que MUDA. Uma tela parada nao produz quadro — e a mesma regra
	// que faz `captura` cair para 1 fps no diag quando ninguem mexe em nada.
	const canvas = document.createElement('canvas');
	canvas.width = 1920;
	canvas.height = 1080;
	const ctx = canvas.getContext('2d');
	let i = 0;
	setInterval(() => {
		i++;
		ctx.fillStyle = `hsl(${i % 360},80%,50%)`;
		ctx.fillRect(0, 0, 1920, 1080);
		ctx.fillStyle = '#000';
		ctx.fillRect(i % 1800, 100, 120, 120);
	}, 33);
	const track = canvas.captureStream(30).getVideoTracks()[0];

	const tr = pc.addTransceiver(track, {
		direction: 'sendonly',
		sendEncodings: [
			{ rid: 'alta', scaleResolutionDownBy: 1, maxBitrate: 3_000_000 },
			{ rid: 'media', scaleResolutionDownBy: 2, maxBitrate: 1_000_000 },
			{ rid: 'baixa', scaleResolutionDownBy: 4, maxBitrate: 300_000 },
		],
	});

	// H.264 fixado, porque e o que o projeto usa e porque o suporte a simulcast
	// nele e exatamente o que esta sendo verificado.
	const h264 = RTCRtpSender.getCapabilities('video').codecs.filter((c) => /H264/i.test(c.mimeType));
	if (h264.length === 0) return { erro: 'este navegador nao tem H.264' };
	tr.setCodecPreferences(h264);

	await pc.setLocalDescription(await pc.createOffer());
	await new Promise((pronto) => {
		if (pc.iceGatheringState === 'complete') return pronto();
		pc.addEventListener(
			'icegatheringstatechange',
			() => pc.iceGatheringState === 'complete' && pronto(),
		);
		setTimeout(pronto, 4000);
	});

	const res = await fetch('/offer', {
		method: 'POST',
		body: JSON.stringify({ type: 'offer', sdp: pc.localDescription.sdp }),
	});
	await pc.setRemoteDescription(await res.json());

	// Tempo para a estimativa de banda subir e as tres camadas acordarem.
	await new Promise((pronto) => setTimeout(pronto, 14_000));

	const camadas = [];
	(await pc.getStats()).forEach((s) => {
		if (s.type === 'outbound-rtp') {
			camadas.push({
				rid: s.rid,
				kb: Math.round((s.bytesSent ?? 0) / 1024),
				largura: s.frameWidth,
				altura: s.frameHeight,
				fps: s.framesPerSecond,
			});
		}
	});
	return { estado: pc.connectionState, camadas };
});

console.log(JSON.stringify(saida, null, 2));
await browser.close();
