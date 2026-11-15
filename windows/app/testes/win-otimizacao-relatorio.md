# Windows nativo: onde ficam RAM, CPU e GPU

Medições em 13/11/2026, no worktree `w3b`, branch `codex/otimizacao`, base `de743e04` (código 51). Nenhuma otimização de comportamento foi implementada: o commit acrescenta instrumentos e evidências para a próxima mudança. A instalação do dono foi apenas consultada por contadores; não foi encerrada, depurada nem usada para entrar em produção.

Máquina: Intel Core i5-10400F, 6 núcleos/12 processadores lógicos; NVIDIA RTX 2060, driver 32.0.16.1714; adaptador virtual Parsec 0.45.0.0. MiB = 1.048.576 bytes. CPU em % de **um núcleo**, sem dividir por 12. GPU é uso do mecanismo indicado, não % de um núcleo.

## 1. Abertura: atribuição e limites

A referência do dono foi 128 MB privados, 146 MB residentes, 78 MB dedicados, 44 threads e 1317 handles. A consulta externa inicial desta sessão encontrou 128,402 MiB privados, 146,578 MiB residentes, 41 threads e 1303 handles. Em outra consulta, GPU dedicada = 77,941 MiB; compartilhada = 1,828 MiB. São instantes diferentes, não uma decomposição exata da conta carregada.

O executável próprio usa `--resource-audit`: WinUI real, conta/lista sintéticas, sem restauração de sessão, sem rede, sem bandeja e sem registro de notificações. Nas execuções 1–3, a janela estava fora da tela, mostrada sem ativação (`Show(false)`); esses resultados não equivalem a `Hide()`. A execução 4 usa `Hide()` explícito para representar a bandeja. Os deltas abaixo localizam **quando** o processo compromete memória; não são uma atribuição de heap a cada biblioteca. Não se deve somar deltas de execuções distintas.

| Ponto | Privada (MiB) | Residente (MiB) | Threads | Handles |
|---|---:|---:|---:|---:|
| Entrada nativa | 5.047 | 13.711 | 4 | 274 |
| Antes do WinUI | 5.148 | 14.570 | 5 | 276 |
| Runtime WinUI iniciado | 10.105 | 30.805 | 14 | 516 |
| Tema e recursos carregados | 13.789 | 38.980 | 14 | 518 |
| Janela criada | 18.895 | 55.941 | 20 | 670 |
| Janela configurada | 83.938 | 88.484 | 41 | 914 |
| Serviço criado | 84.141 | 89.641 | 46 | 967 |
| Primeira tela | 84.445 | 93.270 | 46 | 1003 |
| Primeira lista sintética | 88.926 | 103.488 | 47 | 1094 |
| Lista estabilizada | 89.809 | 104.523 | 47 | 1109 |
| Repouso final | 89.723 | 104.180 | 43 | 1105 |

Fonte da tabela: `winui4-stages.jsonl`.

Threads no repouso, por nome ou módulo da rotina inicial:

| Nome/módulo | Quantidade |
|---|---:|
| Composition Token Thread | 1 |
| CoreMessagingXP.dll | 1 |
| DManip Delegate Thread | 1 |
| DWM Compositor Thread | 1 |
| DWM Manipulation Thread | 1 |
| DWM Master Input Thread | 1 |
| Lifted DMIT | 1 |
| Microsoft.UI.Xaml.dll | 1 |
| combase.dll | 1 |
| harmony-account | 1 |
| harmony-diagnostics | 1 |
| harmony-nativo.exe | 1 |
| harmony-pictures | 4 |
| harmony-ui | 1 |
| ntdll.dll | 8 |
| nvwgf2umx.dll | 18 |

CPU no intervalo final: 140.625 ms em 60.015 s = 0.234% de um núcleo. Inclui os instrumentos de medição e o histórico de diagnóstico.

Com Hide() explícito, GPU dedicada estabilizou em 17.684–17.684 MiB e compartilhada em 2.758–3.113 MiB. Todas as amostras externas tiveram `main_window_handle=0`; todos os snapshots com o campo `foreground_owned` registraram 0. As execuções fora da tela mantinham um handle de janela principal e ~23,574 MiB dedicados. Essa diferença é uma correção do chicote de medição; não é economia implementada no aplicativo do dono, cujo fluxo normal já oculta a janela.

O refinamento da execução `winui2` localizou o salto principal na **configuração da janela**, antes do serviço: 18,547 → 84,242 MiB (+65,695 MiB). Surgiram 18 threads `nvwgf2umx.dll`. Criar o Service acrescentou 0,203 MiB naquele instante e cinco workers; a primeira Render levou a 84,750 MiB. Os 4 workers de imagens e o de conta existem na abertura; não medimos um ganho ao adiá-los.

O contador dos donos de mídia no próprio modo oculto registrou: `media_device_loaded=0`, `media_device_users=0`, `cached_texture_bytes=0`, `voice_loaded=0`, `dfn_loaded=0`. Portanto fábrica de voz, DFN e dispositivo D3D **da mídia** não explicam o repouso. Eles já são criados por demanda de sala/chamada. O D3D usado pelo WinUI é separado.

| Dono de vídeo, parado | Evidência | Vídeo dedicado |
|---|---|---:|
| Processo WinUI fora da tela, sem ativação | Contador externo GPU Process Memory, execuções 1/2 | 23,578 MiB |
| Dispositivo compartilhado da mídia | Dono descarregado, 0 leases | Sem dispositivo ativo |
| Cache de texturas da mídia | Snapshot do cache | 0 bytes lógicos |
| Swap chains, fontes e caches internos do WinUI/driver, separados | Não há inventário de alocações por dono | **não medido** |

Atribuição exata dos 128 MB/78 MB da instalação carregada, item a item: **não medido**. Faltam um estado sintético com o mesmo histórico/imagens e inventário do alocador/recursos internos do WinUI. Não se atribui a diferença de dois processos diferentes a um suposto vazamento. `ntdll.dll` é módulo de início, não prova de quem solicitou cada thread.

## 2. O que volta ao sair

Motor: fábrica, PeerConnection, APM e DFN reais; transporte HTTP falso e dispositivo de áudio em memória já existente. Compilado com `HARMONY_ENGINE_TEST` e `HARMONY_GPU=0`. Não há SFU, negociação de SDP, Opus remoto ou pessoas reais. Cada ciclo cria o motor, inicia callbacks simulados, chama `Leave()` e destrói a Session, como o `Service::DropVoice` faz. Primeira permanência: 3 s; demais: 100 ms; pausa de 100 ms entre ciclos e 3 s no final.

| Ponto | Privada (MiB) | Residente (MiB) | Threads | Handles |
|---|---:|---:|---:|---:|
| Antes de qualquer sala | 2.656 | 9.934 | 8 | 210 |
| Após ciclo 1 | 5.227 | 16.500 | 8 | 259 |
| Após ciclo 10 | 6.695 | 17.781 | 8 | 259 |
| Após ciclo 25 | 7.629 | 18.105 | 8 | 259 |
| Após ciclo 50 | 29.441 | 41.539 | 8 | 257 |
| Após ciclo 75 | 28.664 | 40.426 | 8 | 257 |
| Ciclo 100, ativo | 31.004 | 48.449 | 13 | 279 |
| Ciclo 100, Leave sem destruição | 30.945 | 48.418 | 11 | 273 |
| Após destruir ciclo 100 | 6.488 | 18.602 | 7 | 257 |
| Final estabilizado | 6.547 | 18.660 | 7 | 257 |

Após 100 ciclos ficam +3,891 MiB e +47 handles sobre o início; threads caem de 8 para 7. A memória oscila bastante e chegou a 29,441 MiB imediatamente após a destruição no ciclo 50, voltando a 6,547 MiB no final. Não foi observado crescimento contínuo de memória/threads/handles nesse motor. `Leave()` sozinho retém fábrica/DFN; é a destruição subsequente que interessa ao aplicativo. A primeira carga DFN isolada na execução `engine20` acrescentou 11,961 MiB; em outras passagens o delta varia com o alocador.

GPU: 20 recriações de dispositivo D3D e três escalas NV12, primeiro na mesma thread e depois com thread nova a cada ciclo. Política de liberar o dispositivo ocioso igual à do app. Durante cada ciclo há 3 processadores, 3 views e **4.082.400 bytes lógicos** de texturas retidas no cache (descritivos das texturas, não VRAM física). Após cada ciclo: dispositivo descarregado, zero leases, processadores, views e bytes de cache.

| Ponto | Privada (MiB) | Residente (MiB) | Threads | Handles |
|---|---:|---:|---:|---:|
| GPU, antes de tudo | 2.191 | 9.043 | 4 | 166 |
| GPU após ciclo 1 | 25.781 | 22.430 | 5 | 282 |
| GPU após ciclo 10 | 48.957 | 30.305 | 5 | 300 |
| GPU após ciclo 20 | 51.590 | 34.992 | 5 | 320 |
| GPU final +3 s | 41.637 | 34.680 | 5 | 320 |

Há retenção reproduzível de **2 handles por recriação D3D**, mesmo com thread nova: um `Mutant` (mutex) e uma `Section`. Entre as saídas 1 e 20: +19 de cada. Os outros tipos ficaram estáveis após aquecimento. GPU Process Memory volta a 0 nas amostras de repouso entre ciclos; a RAM privada do processo não volta ao começo. A medição por tipo não revela a pilha que criou o handle: atribuir o defeito especificamente ao app, runtime ou driver é **não medido**. Nenhum handle foi fechado à força.

Ciclo completo de codecs (NVENC + DXVA, 5 passagens no mesmo processo, 3 s de espera após cada):

| Ponto | Privada (MiB) | Residente (MiB) | Threads | Handles |
|---|---:|---:|---:|---:|
| Antes de qualquer codec | 2.156 | 8.652 | 4 | 161 |
| Após codec ciclo 1 | 219.809 | 136.539 | 6 | 336 |
| Após codec ciclo 2 | 240.152 | 191.855 | 7 | 338 |
| Após codec ciclo 3 | 238.562 | 222.422 | 9 | 340 |
| Após codec ciclo 4 | 243.551 | 217.672 | 9 | 342 |
| Após codec ciclo 5 | 241.574 | 216.277 | 6 | 345 |
| Codec final +3 s | 13.656 | 27.809 | 6 | 345 |


Execução final dos codecs: **PASS**, cinco ciclos completos, zero falha de cadência. Após a saída 1 ficaram 219,809 MiB privados; após a saída 5 (3 s) ainda 241,574 MiB; três segundos adicionais de repouso reduziram para 13,656 MiB. O resíduo final é +11,500 MiB, +184 handles e +2 threads sobre o começo; grande parte dos handles surge na primeira passagem. Entre ciclos 1 e 5 reaparece a assinatura de +4 Mutant e +4 Section. Donos do dispositivo/caches zerados em todas as cinco saídas. A queda de 241,574 para 13,656 MiB sem fechar o processo mostra liberação tardia, não crescimento contínuo nessa amostra. A atribuição dessa memória transitória a alocador/runtime/driver é não medido.

## 3. CPU/GPU por etapa

WGC real capturando apenas a janela sintética do teste existente. Janelas em x=1920, `WS_EX_NOACTIVATE` e `SW_SHOWNOACTIVATE`; sem áudio, mouse/teclado, TOPMOST ou pedido de frente. Transmissão com três camadas 1080p60/540p30/270p15, H.264 NVENC; depois decodificação DXVA e apresentação. As duas fases são **sequenciais**, não uma sala simultânea. O padrão é simples; custo de jogo/filme/câmera real: **não medido**.

A tabela usa a execução final `capture-present2` quando disponível (sem compilação concorrente). A execução 1 está documentada abaixo para comparação.

Tempos de `perf::Stage`: **parede**, incluindo esperas e submissão ao driver. Não são tempo de execução da GPU nem necessariamente tempo de CPU. `completion_wait` já está incluído nas etapas de codificação; não somar.

| Etapa | Operações/quadros | Média de parede (ms) | Máximo (ms) | CPU isolada, % núcleo | Tempo GPU isolado |
|---|---:|---:|---:|---|---|
| Captura WGC | 600 | 0.469474 | 4.421800 | não medido | não medido |
| Escala combinada transmissão/apresentação | 1651 | 0.112425 | 3.545400 | não medido | não medido |
| NVENC alta | 600 | 4.683320 | 12.119700 | não medido | não medido |
| NVENC média | 300 | 2.578670 | 7.304800 | não medido | não medido |
| NVENC baixa | 151 | 2.478240 | 12.147600 | não medido | não medido |
| Espera NVENC (incluída acima) | 1051 | 2.422130 | 9.195900 | não medido | não medido |
| Decodificação DXVA | 600 | 0.450313 | 15.385700 | não medido | não medido |
| Apresentação | 600 | 0.166320 | 3.178800 | não medido | não medido |

Execução `capture-present1`: PASS; 593 quadros na alta, 301 na média, 151 na baixa; 593 decodificados e apresentados; zero fallback e zero cópia GPU→CPU. Captura: 9 drops, intervalo médio de saída 16,8985 ms (59,18 quadros/s observados para pedido de 60). Os skips das camadas 15/30 são normais e registrados separadamente. Processo todo: captura+geração do padrão+codificação = 2218,75 ms CPU em 10,328 s (**21,483% de um núcleo**); decodificação+apresentação = 765,62 ms CPU em 10,547 s (**7,259%**). Não é CPU isolada de cada etapa.

Amostras externas daquele processo: mecanismo VideoEncode 14,50–18,04% durante transmissão; VideoDecode 9,30–14,78% durante reprodução (sem a amostra da transição). Pico observado: 215,313 MiB dedicados e 32,578 MiB compartilhados. São amostras, não um máximo instrumentado de toda alocação nem utilização total da placa. O processo do dono e outros programas continuam abertos.

Teste sem WGC (`codec1`), útil para separar a geração: PASS, 600 quadros na alta e 600 decodificados. Escala: 0,059713 ms/operação; alta 7,70342 ms; completion_wait 4,1282 ms; decode 1,70789 ms. A decodificação aqui corre em rajada, sem apresentação: não comparar diretamente com assistir a 60 fps. As compilações/testes concorrentes afetam o tempo de parede; repetição sem compilar identificada nos artefatos finais, quando disponível.

Áudio offline, nenhum dispositivo de reprodução aberto: tom sintético 440 Hz, 48 kHz mono; 100 blocos de aquecimento, 3000 blocos de 10 ms. CPU da thread medida com `GetThreadTimes`; percentual abaixo normalizado para 100 blocos/s (não é utilização instantânea do teste acelerado).

| Caminho | CPU por bloco de 10 ms | % de um núcleo em tempo real |
|---|---:|---:|
| DFN isolado, tom | 0,270833 ms | 2,70833% |
| APM + transporte, DFN desligado | 0,052083–0,057292 ms | 0,520833–0,572917% |
| APM + transporte, DFN habilitado, sem audiência | 0,057292 ms | 0,572917% |
| APM + transporte + DFN, com audiência | 0,281250 ms | 2,81250% |

DFN com audiência ficou ativo, sem fallback. Silêncio exato foi abaixo da resolução do contador de CPU, não custo literalmente zero; parede 0,000263 ms/bloco. Os custos de receber/misturar 4–8 pessoas, Opus, referência de eco real, fala/câmera e som do filme são **não medido**.


Repetição final sem compilação concorrente: **PASS**, 600 quadros alta, 300 média, 151 baixa; 600 decodificados/apresentados, captura drop=1, intervalo de saída 16,6801 ms (59,95 fps). Zero fallback e zero cópia GPU→CPU.
Captura + padrão + codificação: 1390.625 ms CPU em 10.172 s = **13.671% de um núcleo**.
Decodificação + apresentação: 734.370 ms CPU em 10.609 s = **6.922% de um núcleo**.
Pico dedicado observado nessa repetição: 206.750 MiB.

Áudio repetido em `audio2`, sem compilação: DFN isolado 0,270833 ms/bloco (2,70833% de um núcleo); pipeline DFN com audiência 0,307292 ms/bloco (3,07292%); sem audiência 0,057292 ms/bloco (0,572917%). Sem fallback. O silêncio ficou próximo do limite de quantização de CPU (um tick de 15,625 ms no conjunto de 10.000 blocos), insuficiente para distinguir seu custo real de overhead.

CPU por worker nas amostras publicadas de `capture-present2`: `harmony-capture` 1,6–7,8% de um núcleo, `harmony-encode` 1,6–7,8%. São custos parciais dos workers, não de todos os trabalhos da etapa: escala e driver participam de caminhos compartilhados. O formato do perf omite amostras abaixo de 0,5%; essas faixas não são médias ponderadas nem incluem zeros omitidos. O driver NVIDIA chegou a 10,9% em uma amostra durante a transmissão.

## 4. Achados, por impacto observado

1. **Janela antecipada ao iniciar na bandeja.** A configuração acrescentou 65,695 MiB privados e 18 threads NVIDIA antes do Service. Candidato: criar a janela e a composição quando o usuário abrir o app. O ganho de um protótipo desse adiamento é **não medido**; exige separar a bandeja do ciclo de vida WinUI. Só descrito, sem conserto/commit de otimização.
2. **Mídia usa muito mais memória durante operação que a fábrica ociosa.** `codec1`: inicializar três encoders levou de 2,199 a 157,309 MiB privados; o processo chegou a 288,621 MiB ao decodificar em rajada. O inventário por superfície interna NVENC/DXVA é **não medido**. Candidato: primeiro rastrear superfícies/filas, sem reduzir 60 fps nem mudar motion. Só descrito; não prometer economia com base em tamanhos nominais.
3. **DFN custa CPU e modelo, mas já fica fora do repouso.** +11,961 MiB na primeira carga; 0,270833 ms CPU/10 ms no tom; pipeline completo 2,8125% de um núcleo. Sem audiência, a pausa existente derruba o custo a ~0,573%. Não há ganho de repouso a obter adiando um DFN que já não nasceu. Nenhum conserto feito.
4. **Dois handles por nova instância D3D.** +38 handles em 19 recriações após a primeira, reproduzido mesmo terminando a thread. Candidato: rastrear pilhas de criação no runtime/driver antes de mexer na política de liberar o dispositivo (que de fato devolve VRAM/threads). Custo de RAM por handle e conserto: **não medido**. Só descrito.
5. **Saída do motor não mostrou vazamento contínuo em 100 ciclos.** Resíduo final +3,891 MiB/+47 handles; threads retornam. A oscilação até 29,441 MiB após destruição recomenda medir janelas longas, não declarar vazamento por uma única saída. Nenhum conserto feito.

Nenhum limite de cache foi alterado, nenhuma dependência adicionada, nenhuma reestruturação. Diaglog/histórico seguem existindo; regras de motion, 60 fps e SFU sem decodificar permanecem.

## 5. Verificação e reprodução

`test.cmd` inteiro passou com código 0 registrado em `windows/app/test-output/optimization-test-final.exit.txt`. A linha `FAIL: speaking lights quiet audio above -60 dBFS` é a mutação esperada. Últimas linhas literais:

```text
installer-text-tests.cpp
actual glyph family=Inter
actual glyph weight=500 expected=500
actual glyph family=Inter
actual glyph weight=600 expected=600
PASS: installer uses actual Inter variable glyph weights 500/600 without synthetic bold; weights below 500 refused.
```

`build.cmd` padrão: PASS, código 0 observado também na compilação final às 19:20, além dos builds -OptimizedTests.

```text
  log.cpp
  perf.cpp
  UndockedRegFreeWinRT-AutoInitializer.cpp
  WindowsAppRuntimeAutoInitializer.cpp
  Generating Code...
  app.vcxproj -> <pasta-de-trabalho>\w3b\windows\app\out\harmony-nativo.exe
```

Comandos da medição (na raiz; nomes de saída novos para não acrescentar a uma execução anterior):

```powershell
cmd /c windows\app\build.cmd -OptimizedTests
powershell -NoProfile -File windows/app/testes/measure-resources.ps1 -Exe <pasta-de-trabalho>\w3b\windows\app\out\harmony-nativo.exe -Arguments --resource-audit -Name winui-new -TimeoutSeconds 180
cmd /c windows\app\testes\measure-engine-build.cmd
powershell -NoProfile -File windows/app/testes/dfn-stage.ps1 -Destination windows/app/test-output/resource-engine
powershell -NoProfile -File windows/app/testes/measure-resources.ps1 -Exe <pasta-de-trabalho>\w3b\windows\app\test-output\resource-engine\engine-tests.exe -Arguments 100 -Name engine-new -TimeoutSeconds 180
cmd /c windows\native\build-gpu.cmd
# O relink rápido exige os objetos recém-gerados pelo build completo acima.
cmd /c windows\app\testes\measure-gpu-build.cmd
powershell -NoProfile -File windows/app/testes/measure-resources.ps1 -Exe <pasta-de-trabalho>\w3b\windows\native\build\gpu_selftest.exe -Arguments --resource-worker-lifecycle -Name gpu-new -TimeoutSeconds 180
# Outros argumentos do mesmo exe: --codec-probe, --present, --resource-media-cycles.
# Para --present, HARMONY_TEST_MONITOR_X=1920, como nesta medição.
cmd /c windows\app\test.cmd
cmd /c windows\app\build.cmd
```

Áudio: `measure-engine-build.cmd` aceita como primeiro argumento o caminho absoluto de `resource-audio.cpp` e como segundo o diretório de saída; depois `dfn-stage.ps1` prepara esse diretório com os arquivos já aprovados. `measure-resources.ps1` roda o exe ali. Nenhum download. Os modos são instrumentos de teste; `--resource-audit` não entra no build publicado.

## 6. Estado do Git

Os instrumentos e as medições pertencem somente a `codex/otimizacao`. Os comandos literais de entrega `git log --oneline master..HEAD`, `git log --oneline HEAD..master` e `git status --short` são anexados à cópia final fora do repositório e à resposta após o commit, para não tentar gravar o hash de um commit dentro dele mesmo.

## 7. Não medido e cuidados de leitura

- **não medido:** conta do dono reproduzida, histórico real de imagens/chat, decomposição exata dos 128 MB e dos 78 MB por alocador/swap chain/cache interno.
- **não medido:** sala completa com 4–8 pessoas, jogo/filme real a 60 fps, câmera e transporte de rede/SFU/Opus/áudio do sistema.
- **não medido:** ms de execução GPU por etapa e CPU isolada de cada etapa de vídeo; timers perf são parede e as amostras GPU são por mecanismo.
- **não medido:** pilhas/origem de criação dos Mutant/Section que não voltam e seu custo privado individual.
- **não medido:** ganho antes/depois de qualquer conserto; não houve otimização de comportamento.
- O autoteste completo de sala/bancada foi apenas lido: exige trazer o navegador à frente e grava na pasta de outra sessão. Em vez dele foi usado o selftest GPU que só captura sua própria janela sintética, com apresentação sem ativação.
- Contadores externos de GPU são coletados durante alguns segundos: `gpu_seconds` marca o fim da consulta nas execuções mais novas; os campos de processo são colhidos antes dela. Não combinar os dois como um único instante. Zeros ao sair do processo não provam liberação durante a vida dele; por isso há ciclos no mesmo processo com pausa.
- Os snapshots são dados de processo, não um despejo de memória; não contêm e-mail, token, senha ou conteúdo de conversa. Nomes de objetos dos handles nunca são lidos; apenas seus tipos.
- A primeira versão do coletor não registrou o código de saída em `engine20-exit.txt`. Essa lacuna foi preservada no dado bruto; a aprovação do motor usa `engine100`, cujo código 0 foi registrado. A execução exploratória `media5` falhou a cadência; a execução `media5-final` passou os cinco ciclos com código 0.
