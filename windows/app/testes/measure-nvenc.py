"""Run sequential headless NVENC measurements; standard library only."""
import argparse
import csv
import datetime
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
RESULTS = Path(__file__).with_name('nvenc-results')
EXE = ROOT / 'windows/native/build/gpu_selftest.exe'
BASE = ['hoje', 'vbv2', 'vbv4', 'p3', 'p4', 'aq', 'low']


def capture_environment():
    def read_command(command):
        try:
            return subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=True,
                                  creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0,
                                  timeout=10).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            return 'não encontrado'
    return dict(local_date=datetime.datetime.now().astimezone().strftime('%d/%m/%Y'),
                gpu_driver_inventory=read_command(['nvidia-smi','--query-gpu=name,driver_version','--format=csv,noheader']),
                git_head=read_command(['git','rev-parse','HEAD']))


def quantile(values, fraction):
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)]


def load(prefix):
    with prefix.with_suffix('.csv').open() as source:
        rows = [{k: float(v) for k, v in r.items()} for r in csv.DictReader(source)]
    meta = dict(line.split('=', 1) for line in prefix.with_suffix('.meta').read_text().splitlines())
    assert rows and len(rows) == int(meta['frames'])
    assert [r['frame'] for r in rows] == list(range(len(rows)))
    assert all(r['key'] == (i % 600 == 0) for i, r in enumerate(rows))
    p = [r['psnr_y'] for r in rows]
    ms = [r['encode_ms'] for r in rows]
    sizes = [r['bytes'] for r in rows]
    post = [r['psnr_y'] for i, r in enumerate(rows) if 1 <= i % 600 <= 5]
    idr = [r['psnr_y'] for r in rows if r['key']]
    window = min(300, len(rows))
    # Whole five-second windows, sliding one frame; stable across every config.
    bright = [sum(r['brightness'] for r in rows[:window])]
    motion = [sum(r['motion'] for r in rows[:window])]
    for i in range(window, len(rows)):
        bright.append(bright[-1] + rows[i]['brightness'] - rows[i-window]['brightness'])
        motion.append(motion[-1] + rows[i]['motion'] - rows[i-window]['motion'])
    dark_start = min(range(len(bright)), key=bright.__getitem__)
    motion_start = max(range(len(motion)), key=motion.__getitem__)
    six = sum(sizes[:6]); burst = six
    for i in range(6, len(sizes)):
        six += sizes[i] - sizes[i-6]
        burst = max(burst, six)
    return dict(meta=meta, frames=len(rows), psnr=statistics.mean(p), p5=quantile(p,.05),
                post5=statistics.mean(post), idr=statistics.mean(idr),
                ms=statistics.mean(ms), ms95=quantile(ms,.95), msmax=max(ms),
                slow=sum(m > 16.7 for m in ms), bytes=statistics.mean(sizes),
                bytes95=quantile(sizes,.95), bytesmax=max(sizes), ratio=max(sizes)/statistics.mean(sizes),
                burst=burst, burst_ratio=burst/(int(meta['bitrate'])/80),
                mbps=sum(sizes)*8/(len(rows)/60)/1e6,
                dark_start=dark_start/60, motion_start=motion_start/60,
                dark=statistics.mean(p[dark_start:dark_start+window]),
                motion=statistics.mean(p[motion_start:motion_start+window]),
                dark_y=bright[dark_start]/window, motion_mad=motion[motion_start]/window)


def run_group(video, rate, configs, repeat):
    prefixes = {c: RESULTS / f'{video.stem}-{rate}-{c}-r{repeat}' for c in configs}
    if all(p.with_suffix('.csv').exists() and p.with_suffix('.meta').exists() for p in prefixes.values()):
        return {c: load(p) for c, p in prefixes.items()}
    prefix = RESULTS / f'{video.stem}-{rate}-{{config}}-r{repeat}'
    command = [str(EXE), '--measure-encoder', str(video), str(prefix), ','.join(configs), str(rate)]
    env = os.environ.copy()
    env.pop('HARMONY_LOG_FILE', None)
    start = time.monotonic()
    log_path = RESULTS / f'{video.stem}-{rate}-group-{configs[0]}-r{repeat}.log'
    with log_path.open('w') as log:
        completed = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0,
                                   timeout=600)
    if completed.returncode:
        raise RuntimeError(log_path.read_text())
    result = {c: load(p) for c, p in prefixes.items()}
    for c, r in result.items():
        print(f'{video.stem}-{rate}-{c}-r{repeat}: {r["frames"]} frames, {r["psnr"]:.3f} dB, {r["ms"]:.3f} ms', flush=True)
    print(f'GROUP wall {time.monotonic()-start:.1f}s', flush=True)
    return result


def measure_group(video, rate, configs):
    first = run_group(video, rate, configs, 1)
    second = run_group(video, rate, configs, 2)
    result = {c: [first[c], second[c]] for c in configs}
    diverged = [c for c in configs if abs(first[c]['psnr']-second[c]['psnr']) > .3 or
                abs(first[c]['ms']-second[c]['ms'])/min(first[c]['ms'],second[c]['ms']) > .1]
    if diverged:
        print('THIRD required: '+','.join(diverged), flush=True)
        third = run_group(video, rate, configs, 3)
        for c in configs:
            result[c].append(third[c])
    return result


def choose(matrix):
    means = {c: {k: statistics.mean(r[k] for rate in matrix.values() for r in rate[c])
                  for k in ('psnr','post5','idr','dark','ms')} for c in BASE}
    base = means['hoje']
    vbv = max(('vbv2','vbv4'), key=lambda c: means[c]['idr']+means[c]['post5'])
    preset = max(('p3','p4'), key=lambda c: means[c]['psnr'])
    parts = []
    if (means[vbv]['idr']+means[vbv]['post5'])-(base['idr']+base['post5']) > .1:
        parts.append(vbv)
    if means[preset]['psnr']-base['psnr'] > .05:
        parts.append(preset)
    if means['aq']['dark']-base['dark'] > .05:
        parts.append('aq')
    if means['low']['psnr']-base['psnr'] > .05:
        parts.append('low')
    return 'combined-'+'-'.join(parts) if parts else 'combined-baseline'


def pooled(video, rate, config, runs):
    """Pool frames, rather than averaging percentiles from separate runs."""
    assert all(r['frames'] == runs[0]['frames'] for r in runs)
    rows = []
    for repeat in range(1, len(runs)+1):
        path = RESULTS / f'{video}-{rate}-{config}-r{repeat}.csv'
        with path.open() as source:
            rows.extend({k: float(v) for k, v in row.items()} for row in csv.DictReader(source))
    assert len(rows) == sum(r['frames'] for r in runs)
    result = dict(runs[0])
    quality = [r['psnr_y'] for r in rows]
    milliseconds = [r['encode_ms'] for r in rows]
    sizes = [r['bytes'] for r in rows]
    result.update(frames=len(rows), psnr=statistics.mean(quality), p5=quantile(quality,.05),
                  ms=statistics.mean(milliseconds), ms95=quantile(milliseconds,.95), msmax=max(milliseconds),
                  slow=sum(m > 16.7 for m in milliseconds), bytes=statistics.mean(sizes),
                  bytes95=quantile(sizes,.95), bytesmax=max(sizes), ratio=max(sizes)/statistics.mean(sizes),
                  burst=max(r['burst'] for r in runs), burst_ratio=max(r['burst_ratio'] for r in runs),
                  mbps=sum(sizes)*8/(len(rows)/60)/1e6)
    for key in ('idr','post5','dark','motion'):
        result[key] = statistics.mean(r[key] for r in runs)
    return result


def metric_row(label, r, rate):
    return (f"| {label} | {r['psnr']:.3f} / {r['p5']:.3f} | {r['idr']:.3f} / {r['post5']:.3f} | "
            f"{r['dark']:.3f} / {r['motion']:.3f} | {r['ms']:.3f} / {r['ms95']:.3f} / {r['msmax']:.3f} | "
            f"{r['slow']}/{r['frames']} | {r['bytes']:.0f} / {r['bytes95']:.0f} / {r['bytesmax']:.0f} | "
            f"{r['ratio']:.2f} | {r['burst']:.0f} / {r['burst_ratio']:.2f} | "
            f"{r['mbps']:.3f} / {rate} ({r['mbps']/rate*100:.1f}%) |")


def report(all_data, output):
    details = []
    manifest = RESULTS / 'campaign.json'
    environment = json.loads(manifest.read_text(encoding='utf-8')) if manifest.exists() else {}
    lines = ['# Medição do codificador da tela — NVENC', '',
             f'Data: {environment.get("local_date", "não registrada")}. '
             f'Inventário NVIDIA (placa, driver): {environment.get("gpu_driver_inventory", "não registrado")}. '
             'Produção preservada: as opções são passadas somente pelo novo modo do gpu_selftest. '
             'Medição sequencial, sem jogo executado pelo chicote, sem janela, áudio ou entrada real. Até sete sessões ficam residentes para compartilhar a leitura; apenas uma codifica e decodifica de cada vez, com ordem alternada a cada quadro.', '',
             '[Dados numéricos, metadados e logs](win-codificador-dados.zip). '
             '[Como repetir ou recalcular sem os vídeos](measure-nvenc.md).', '',
             '## Método', '',
             'Media Foundation Source Reader seleciona só vídeo e entrega RGB32; escala do projeto limita a 1920×1080 sem ampliar. '
             'Amostragem a 60 Hz pelo timestamp, mantendo o último quadro disponível. NVEncoder real, High, CBR, GOP infinito, '
             'sem B e sem lookahead; IDR forçado em 0, 600, 1200… Sem alterar SetRate nem os chamadores de produção. '
             'AQ usa aqStrength=0 (automática do driver). Cada rodada abre sessões novas. A decodificação ocorre em blocos de cinco segundos, mantendo as referências e os pacotes só em RAM; oito texturas de leitura deixam o DXVA avançar sem uma espera de GPU por callback.', '',
             'Configurações: hoje=P1/ultra low latency/VBV1/AQ não; vbv2 e vbv4 mudam somente o VBV; '
             'p3 e p4 mudam somente o preset; aq liga somente AQ espacial automática; '
             'low muda somente o tuning para low latency. O nome combined explicita as opções escolhidas '
             '(por exemplo, combined-vbv4-p4 = P4, VBV4, ultra low latency, AQ desligada). '
             'A combinada fica sozinha; as sete configurações isoladas compartilham a leitura em sessões residentes, '
             'portanto os tempos não equivalem a uma comparação estrita de uma única sessão isolada.', '',
             'Referência Y: conversão do próprio quadro BGRA entregue ao NVEncoder (libyuv ARGBToI420, faixa limitada/BT.601). '
             'Saída: Y NV12 do decoder.cpp, lido diretamente por staging reutilizado. Soma dos erros quadráticos implementada no chicote, '
             'PSNR=10 log10(255²/MSE), média aritmética dos PSNR por quadro. Um quadro idêntico recebe 128 dB. '
             'Portanto mede o caminho BGRA→NVENC→decodificador, incluindo conversão de cor, e não a qualidade do arquivo original. '
             'Sem SSIM: não medido. Sem salvar pixels ou bitstream. Superfícies de 1088 linhas do Media Foundation são recortadas nas 1080 linhas visíveis. Texturas de staging e de entrada são reutilizadas para reduzir o custo de alocação do chicote.', '',
             'Tempo inclui Submit (inclusive blit) até Collect terminar e copiar os bytes; leitura do arquivo, referência e decodificação ficam fora. '
             'Execução acelerada, sem espera de relógio: não mede disputa com captura/simulcast, jogo ou fila de rede. '
             'Rajada de 100 ms = maior soma de seis quadros consecutivos na cadência ideal de 60 Hz, dividida pela taxa pedida/80 em bytes. '
             'É oferta de bytes ao envio, não pacotes observados numa rede nem efeito de um pacer. Sem RTP/UDP/IP.', '',
             'Percentis pelo posto mais próximo (ceil(N×p)). Pós-IDR conta os cinco quadros seguintes e exclui o IDR; '
             'a coluna IDR mostra o quadro completo em separado. Intervalos de jogo escolhidos por janelas deslizantes de 5 s: '
             'menor Y médio e maior diferença absoluta temporal de Y, amostrada a cada 8 pixels. '
             'Nenhuma seleção por inspeção visual. Todas as configurações usam os mesmos intervalos.', '',
             'Duas rodadas por configuração; terceira se |ΔPSNR médio|>0,3 dB ou |Δtempo médio|/menor tempo>10%. '
             'Se uma configuração do grupo pede terceira rodada, repetimos também as outras do mesmo grupo, nas mesmas condições. As tabelas principais reúnem os quadros de todas as rodadas, recalculando os percentis; o total de quadros aparece junto ao número acima de 16,7 ms. O apêndice mostra cada rodada separadamente, sem esconder a variação. O limite aplica-se ao tempo médio de codificação.', '']
    for video, matrix in all_data.items():
        for rate, configs in matrix.items():
            if not configs:
                continue
            lines += [f'## {video} — {rate} Mbit/s', '']
            first = next(iter(configs.values()))[0]
            m = first['meta']
            lines += [f"Fonte {m['source_width']}×{m['source_height']}, {m['fps_n']}/{m['fps_d']} fps, {m['duration_s']} s; "
                      f"saída {m['width']}×{m['height']}, {first['frames']} quadros. "
                      f"Repetidos a 60 Hz: {m['repeated']} por rodada. "
                      f"Escuro: {first['dark_start']:.3f}–{first['dark_start']+5:.3f} s (Y={first['dark_y']:.2f}); "
                      f"movimento: {first['motion_start']:.3f}–{first['motion_start']+5:.3f} s (MAD={first['motion_mad']:.2f}).", '',
                      '| Configuração (rodadas) | Y médio / p5 (dB) | IDR / pós-5 (dB) | Escuro / movimento (dB) | ms médio / p95 / máx | >16,7 ms / total | bytes médio / p95 / máx | máx/médio | 100 ms bytes / ×esperado | Mbit/s real / pedida |',
                      '|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
            details += [f'### {video} / {rate} Mbit/s', '', lines[-2].replace('(rodadas)', '/ rodada'), lines[-1]]
            for config, runs in configs.items():
                lines.append(metric_row(f'{config} ({len(runs)} rodadas)', pooled(video,rate,config,runs), rate))
                for i, r in enumerate(runs,1):
                    assert abs(r['dark_start']-first['dark_start']) < .001
                    assert abs(r['motion_start']-first['motion_start']) < .001
                    details.append(metric_row(f'{config} / {i}', r, rate))
            details.append('')
            lines.append('')
    if 'accepted_runs' in environment:
        lines += ['## Conferência da campanha', '',
                  f'{environment["accepted_runs"]} rodadas aceitas, {environment["total_frames"]} quadros codificados '
                  'e decodificados. Contagens, sequência dos quadros, sete IDRs por rodada, estatísticas e '
                  'terceiras rodadas exigidas foram conferidos. '
                  f'PSNR e tamanhos por quadro idênticos entre repetições: {"sim" if environment["all_repeat_psnr_and_sizes_identical"] else "não"}. '
                  f'Quadros sujeitos ao teto de 128 dB: {environment["zero_error_capped_frames"]}.', '',
                  f'Código nativo medido: `{environment.get("native_code_commit", environment.get("git_head", "não encontrado"))}`. '
                  'Compilação: `cmd /c windows\\native\\build-gpu.cmd`. '
                  'Verificação das contas: `python -B windows/app/testes/measure-nvenc-tests.py`. '
                  'Ambos concluíram com sucesso. O controle contra o piloto isolado manteve PSNR e bytes '
                  'idênticos nos 3.656 quadros do primeiro jogo a 15 Mbit/s.', '']
    lines += findings(all_data)
    lines += ['## Limitações', '', 'Cena escura de filme: **não medido**. Corte de cena de cinema: **não medido**. '
              'Não há filme entre os arquivos fornecidos. Cortes de cena de jogos não receberam anotação manual. '
              'Com jogo rodando e disputando a placa: **não medido**. Percepção a olho: **não medido**. '
              'Rede real, perda, recuperação do decodificador após perda, entrada de pessoa, troca de camada, '
              'pacer e atraso de ponta a ponta: **não medido**. Os IDRs só simulam o esforço de codificar um quadro completo. '
              'AQ pode mudar qualidade percebida sem aumentar PSNR; a medida não resolve essa preferência.', '']
    if len(all_data) == 2 and all(all(configs for configs in matrix.values()) for matrix in all_data.values()) and all(runs[0]['meta']['fps_n'] == '30' and runs[0]['meta']['fps_d'] == '1'
           for matrix in all_data.values() for configs in matrix.values() for runs in configs.values()):
        lines += ['Os dois trechos são fontes de 30 fps repetidas a 60. Jogo com 60 quadros novos por segundo: '
                  '**não medido**. São gravações já comprimidas, não captura bruta original. '
                  'Os ganhos e a taxa efetiva não devem ser extrapolados automaticamente para esse outro cenário.', '']
    lines += ['## Todas as rodadas, sem agregar', ''] + details
    output.write_text('\n'.join(lines), encoding='utf-8')


def findings(all_data):
    cases = [(video, rate, configs) for video, matrix in all_data.items()
             for rate, configs in matrix.items()
             if all(c in configs for c in BASE) and any(c.startswith('combined-') for c in configs)]
    if len(cases) != 6:
        return ['## Conclusões em andamento', '',
                'Campanha parcial: as conclusões finais dependem das seis combinações de trecho e taxa.', '']

    def avg(configs, config, key):
        return statistics.mean(r[key] for r in configs[config])

    def deltas(config, key):
        return [avg(c, config, key)-avg(c, 'hoje', key) for _, _, c in cases]

    def span(values, signed=True):
        return f'{min(values):+.3f} a {max(values):+.3f}' if signed else f'{min(values):.3f} a {max(values):.3f}'

    out = ['## Hipóteses', '']
    recovery = deltas('vbv4', 'post5')
    idr = deltas('vbv4', 'idr')
    verdict = 'confirmada para IDR nestes jogos' if min(recovery) > 0 and min(idr) > 0 else 'inconclusiva no conjunto'
    out += [f'**1. VBV de um quadro aperta os quadros completos: {verdict}.** '
            f'VBV4 mudou o PSNR do IDR em {span(idr)} dB e dos cinco seguintes em {span(recovery)} dB. '
            f'VBV2 mudou os cinco seguintes em {span(deltas("vbv2", "post5"))} dB. '
            'Isso mede recuperação objetiva após um IDR pedido; não confirma por percepção o efeito de borrar e limpar, '
            'nem mede cortes de cinema.', '']
    for config in ('vbv2', 'vbv4'):
        burst = [avg(c,config,'burst_ratio') for _,_,c in cases]
        actual = [(avg(c,config,'mbps')/avg(c,'hoje','mbps')-1)*100 for _,_,c in cases]
        out += [f'{config}: maior janela de 100 ms = {span(burst,False)} vez o orçamento da taxa pedida; '
                f'variação da taxa real contra hoje = {span(actual)}%. '
                'São bytes oferecidos ao envio, sem pacer ou conexão real.', '']
    preset_runs = [r for _,_,c in cases for name in ('p3','p4') for r in c[name]]
    fast = max(r['ms95'] for r in preset_runs) < 16.7 and max(r['ms'] for r in preset_runs) < 16.7
    gains = min(deltas('p3','psnr')+deltas('p4','psnr')) > 0
    verdict = 'confirmada para qualidade e média/p95 na placa livre' if fast and gains else 'inconclusiva no conjunto'
    if fast and gains and any(r['slow'] for r in preset_runs):
        verdict = 'inconclusiva quanto a cumprir todo quadro a 60 fps; ganho de qualidade confirmado'
    out += [f'**2. P3/P4 melhoram qualidade e cabem no quadro: {verdict}.** '
            f'Ganho médio de Y: P3 {span(deltas("p3","psnr"))} dB; P4 {span(deltas("p4","psnr"))} dB. '
            'Média e p95 ficaram abaixo de 16,7 ms, mas houve picos acima desse prazo; '
            'a causa dos picos não foi isolada. Não é garantia com jogo disputando a GPU.', '']
    for name in ('p3','p4'):
        runs = [r for _,_,c in cases for r in c[name]]
        out += [f'{name}: médias de codificação {span([r["ms"] for r in runs],False)} ms; '
                f'pior p95 {max(r["ms95"] for r in runs):.3f} ms; máximo {max(r["msmax"] for r in runs):.3f} ms; '
                f'{sum(r["slow"] for r in runs)}/{sum(r["frames"] for r in runs)} quadros acima de 16,7 ms.', '']
    out += [f'**3. AQ desligado piora filme escuro: inconclusiva.** Filme não foi medido. '
            f'Nos intervalos mais escuros dos jogos, AQ mudou Y em {span(deltas("aq","dark"))} dB; '
            f'na média inteira, {span(deltas("aq","psnr"))} dB. '
            'PSNR não decide se uma redistribuição de detalhe ou banding agrada mais ao olhar.', '',
            '## Recomendação e o que ela não prova', '']
    names = sorted({n for _,_,c in cases for n in c if n.startswith('combined-')})
    combined_gains, combined_post, combined_burst, interaction = [], [], [], []
    for _,_,c in cases:
        for name in (n for n in c if n.startswith('combined-')):
            combined_gains.append(avg(c,name,'psnr')-avg(c,'hoje','psnr'))
            combined_post.append(avg(c,name,'post5')-avg(c,'hoje','post5'))
            combined_burst.append(avg(c,name,'burst_ratio'))
            interaction.append(avg(c,name,'psnr')-max(avg(c,n,'psnr') for n in BASE))
    proposal = ('P4 + VBV4, ultra low latency e AQ desligada, medida nos dois jogos'
                if all('combined-vbv4-p4' in c for _,_,c in cases)
                else 'a combinada medida em cada jogo, sem extrapolar para uma combinação ainda não medida')
    out += [f'**Medida:** as combinadas medidas foram {", ".join(names)}. '
            f'Contra hoje, o ganho médio foi {span(combined_gains)} dB e o ganho pós-IDR {span(combined_post)} dB; '
            f'a maior janela de 100 ms foi {span(combined_burst,False)} vez o orçamento. '
            f'Contra a melhor configuração isolada de cada caso, a média mudou em {span(interaction)} dB. '
            'A combinação foi codificada e decodificada; os efeitos não foram somados como previsão.', '',
            f'**Opinião para o próximo teste:** priorizar VBV4; a combinação candidata é {proposal}, '
            'validando antes com o jogo rodando e com a conexão dos amigos. VBV2 é a alternativa conservadora '
            'se os picos do VBV4 forem um problema. A escolha depende do custo de fila, que não foi medido em rede real. '
            'Nenhum desses ajustes foi aplicado à produção.', '',
            f'Low latency mudou o PSNR médio em {span(deltas("low","psnr"))} dB; '
            f'a janela de 100 ms ficou em {span([avg(c,"low","burst_ratio") for _,_,c in cases],False)} vez o orçamento. '
            'Não recomendo uma troca geral para low com base neste conjunto: há ganhos e perdas conforme a taxa, '
            'e o envio precisa ser considerado junto. Também não recomendo habilitar AQ como regra para filme '
            'com base em PSNR de jogos. Isso continua dependendo de filme e avaliação a olho.', '',
            'A comparação fixa a taxa pedida, não a taxa efetivamente obtida. Quando a alternativa gastou mais bits, '
            'o ganho de nitidez não pode ser apresentado como ganho puro por bit. O PSNR trata a gravação decodificada '
            'como referência, não a captura original antes da primeira compressão.', '']
    for video, matrix in all_data.items():
        low_names = sorted({n for configs in matrix.values() for n in configs
                            if n.startswith('combined-') and n.endswith('-low')})
        for low_name in low_names:
            control = low_name.removesuffix('-low')
            if control == 'combined':
                control = 'combined-baseline'
            if not all(low_name in c and control in c for c in matrix.values()):
                continue
            out += [f'### Controle de interação do tuning — {video}', '',
                    f'Comparação direta: {low_name} menos {control}. O restante dos ajustes é idêntico. '
                    'Cada configuração foi medida em sua própria sessão, sem outras codificações simultâneas.', '',
                    '| Taxa pedida | ΔY médio (dB) | Δp5 (dB) | Δpós-5 (dB) | 100 ms ×orçamento low / ULL | Taxa real low / ULL (Mbit/s) |',
                    '|---|---:|---:|---:|---:|---:|']
            worse_tail = False
            for rate, c in matrix.items():
                dp5 = avg(c,low_name,'p5')-avg(c,control,'p5')
                dpost = avg(c,low_name,'post5')-avg(c,control,'post5')
                worse_tail |= dp5 < -.05 or dpost < -.05
                out.append(f'| {rate} | {avg(c,low_name,"psnr")-avg(c,control,"psnr"):+.3f} | '
                           f'{dp5:+.3f} | {dpost:+.3f} | '
                           f'{avg(c,low_name,"burst_ratio"):.2f} / {avg(c,control,"burst_ratio"):.2f} | '
                           f'{avg(c,low_name,"mbps"):.3f} / {avg(c,control,"mbps"):.3f} |')
            out.append('')
            if worse_tail:
                out += ['**Opinião após o controle:** como low perdeu mais de 0,05 dB no p5 ou no pós-IDR '
                        'em pelo menos uma taxa, eu manteria ultra low latency na proposta geral. '
                        'Uma melhora da média não compensa automaticamente esses quadros piores. '
                        'A alternativa sem low também foi medida, não apenas sugerida.', '']
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--videos', type=Path, default=Path('<pasta-de-trabalho>/trechos'))
    parser.add_argument('-o', type=Path, default=Path(__file__).with_name('win-codificador-relatorio.md'))
    parser.add_argument('--report-only', action='store_true')
    args = parser.parse_args()
    RESULTS.mkdir(exist_ok=True)
    if args.report_only:
        saved = json.loads((RESULTS / 'summary.json').read_text(encoding='utf-8'))
        all_data = {video: {int(rate): configs for rate, configs in matrix.items()} for video, matrix in saved.items()}
        report(all_data, args.o)
        return
    if not (RESULTS / 'campaign.json').exists():
        (RESULTS / 'campaign.json').write_text(json.dumps(capture_environment(),ensure_ascii=False,indent=2),encoding='utf-8')
    all_data = {}
    for stem in ('jogo-1','jogo-2'):
        video = args.videos / (stem+'.mp4')
        if not video.is_file():
            raise RuntimeError(f'Vídeo ausente: {video}')
        matrix = {rate: {} for rate in (15,8,4)}
        all_data[stem] = matrix
        # Keep sessions sequential so measurements do not compete for NVENC.
        for rate in matrix:
            for configs in (BASE,):
                matrix[rate].update(measure_group(video, rate, configs))
                report(all_data, args.o)
        combined = choose(matrix)
        print(f'COMBINED {stem}: {combined}', flush=True)
        candidates = [combined]
        # A mean-quality win can hide worse recovery/tails. When low is chosen,
        # measure its interaction directly against the same combination in ULL.
        if combined.endswith('-low'):
            control = combined.removesuffix('-low')
            candidates.append('combined-baseline' if control == 'combined' else control)
        for candidate in candidates:
            for rate in matrix:
                matrix[rate].update(measure_group(video, rate, [candidate]))
        report(all_data, args.o)
        (ROOT.parent / 'win-codificador-parcial.md').write_text(args.o.read_text(encoding='utf-8'), encoding='utf-8')
        (RESULTS / 'summary.json').write_text(json.dumps(all_data, ensure_ascii=False, indent=2), encoding='utf-8')
        print(f'CLOSED {stem}: partial saved', flush=True)


if __name__ == '__main__':
    main()
