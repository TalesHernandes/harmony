import { test, expect } from '@playwright/test';
import { Person } from './fixtures';

// A linha de base das imagens completas, 20/09/2026.
//
// O log do dono mostra o telefone dele decodificando uma imagem completa a
// cada 2,5 segundos numa janela em que a camada encaminhada nao mudou e o
// contador de PLIs do remetente nao subiu nada. Ou seja: ninguem pediu. Antes
// de pedir outro teste a ele, a pergunta e se isso reproduz aqui, num
// Chromium que eu controlo.
test('quantas imagens completas o codificador emite sem ninguem pedir', async ({
	browser,
}) => {
	test.setTimeout(180_000);
	const quemMostra = await Person.create(browser, 'kfmostra');
	const quemAssiste = await Person.create(browser, 'kfassiste');

	await quemMostra.joinVoice();
	await quemAssiste.joinVoice();
	await quemMostra.shareScreen();
	await quemAssiste.watchScreen();

	// Le direto do getStats do remetente, por camada, sem passar pelo diag.
	const amostra = async () =>
		quemMostra.page.evaluate(async () => {
			// `pc` e privado no TypeScript, o que e uma regra de compilacao e
			// nao existe em tempo de execucao. Este e um instrumento, nao
			// codigo de produto.
			const w = window as unknown as {
				__harmonyVoice?: { pc?: RTCPeerConnection | null };
			};
			const pc = w.__harmonyVoice?.pc;
			if (!pc) return null;
			const r = await pc.getStats();
			const out: Record<string, { kf: number; pli: number; res: string }> = {};
			r.forEach((s: any) => {
				if (s.type === 'outbound-rtp' && s.kind === 'video') {
					out[String(s.rid ?? s.ssrc)] = {
						kf: s.keyFramesEncoded ?? 0,
						pli: s.pliCount ?? 0,
						res: `${s.frameWidth ?? 0}x${s.frameHeight ?? 0}`,
					};
				}
			});
			return out;
		});

	// Espera estabilizar antes de comecar a contar.
	await quemMostra.page.waitForTimeout(15_000);
	const inicio = await amostra();
	expect(inicio, 'nao consegui ler o getStats do remetente').not.toBeNull();

	await quemMostra.page.waitForTimeout(30_000);
	const fim = await amostra();

	const linhas: string[] = [];
	let sozinhasMax = 0;
	for (const rid of Object.keys(fim ?? {})) {
		const a = inicio?.[rid];
		const b = fim?.[rid];
		if (!a || !b) continue;
		const sozinhas = b.kf - a.kf - (b.pli - a.pli);
		if (sozinhas > sozinhasMax) sozinhasMax = sozinhas;
		linhas.push(
			`camada ${rid} (${b.res}): chaves +${b.kf - a.kf} em 30s, ` +
				`pedidos +${b.pli - a.pli}, sozinhas ${sozinhas}`,
		);
	}
	console.log('MEDIDO EM 30 SEGUNDOS:\n  ' + linhas.join('\n  '));
	console.log('DIAG MOSTRA: ' + (await quemMostra.diag()).split('\n').slice(-2).join(' | '));

	await quemMostra.close();
	await quemAssiste.close();
});
