# harmony nativo — W3

Programa Windows x64 em C++/WinUI 3, sem pacote, com runtime autocontido.
Login, dois passos, sessão guardada com DPAPI, salas de voz ao vivo,
bandeja e Ajustes. O W3 integra a voz do W1: entrar, sair, trocar de sala,
mudo, não ouvir, quem fala, volume por pessoa, dispositivos e reconexão.

## Compilar

Na raiz do worktree, com o VS 18 BuildTools v145 e SDK 10.0.26100.0 do W1:

```powershell
cmd /c windows\app\build.cmd
cmd /c windows\native\build.cmd
cmd /c windows\app\test.cmd
```

Saídas: `windows/app/out/harmony-nativo.exe` e
`windows/native/build/harmony-w1.exe`. Distribua a pasta `out` inteira.
Os fontes de rede, diagnóstico e voz entram por referência de `windows/native/src`.
O aplicativo liga com o mesmo libwebrtc Shiguredo **m155.8059.4.1** do W1.
O programa W1 continua compilando com captura e codificação NVIDIA; no W3,
a interface só ativa voz. Nenhuma biblioteca nova foi adicionada.

O build restaura exclusivamente os mesmos nove arquivos NuGet locais do W1,
com versões e SHA-256 conferidos em `testes/restore-offline.ps1`, de
`$(USERPROFILE)\harmony-deps\nuget`. Não baixa nem resolve dependências novas.
A fonte Inter é descompactada do WOFF2 do web pelo próprio DirectWrite do
Windows, em `assets/Inter.ttf`. Não instala fontes no computador.

## Rodar

```powershell
& .\windows\app\out\harmony-nativo.exe
```

O padrão é `https://harmony.taleshernandes.com`. Para um servidor local já
aberto:

```powershell
$env:HARMONY_URL = 'http://127.0.0.1:8099'
& .\windows\app\out\harmony-nativo.exe
```

HTTP sem cadeado só é aceito para localhost/loopback. O servidor Go deve ter
`HARMONY_BASE_URL` igual ao endereço usado: é a origem enviada no gateway.
A sessão fica em `%LOCALAPPDATA%\harmony-nativo\session.dpapi`, cifrada para
o usuário do Windows e vinculada ao endereço do servidor. Trocar `HARMONY_URL`
não reaproveita o token em outro servidor. Reabrir valida `/api/me` antes de
entrar. Falha de rede preserva o arquivo; sessão revogada pede login de novo.

Fechar a janela esconde na bandeja e mantém a chamada; Abrir traz de volta.
Sair pelo menu encerra a chamada e o programa e mantém a sessão da conta. **Sair da conta**, nos Ajustes, revoga a sessão no
servidor e apaga o arquivo. Se o servidor não responder, mostra erro e permite
repetir a saída. Não afirma que revogou uma sessão sem resposta.

As salas vêm de `/api/channels` e `/api/voice/rooms`; `voice_room` substitui a
lista inteira de uma sala. Presença offline não inventa saída da voz. Em cada
reconexão há nova busca, antes de consumir os eventos já recebidos. Atraso de
reconexão: 500 ms a 5 s, como o web. A lista antiga fica com indicação
“Reconectando…” durante a interrupção. Criar/remover sala também atualiza a
lista. Não há polling da lista.

O registro guarda até 4.000 linhas em memória desde o início. Copiar registro
nos Ajustes usa a área de transferência Unicode. Os chamadores não passam
respostas, credenciais, e-mails ou mensagens para o registro; o filtro descarta
linhas sensíveis antes de qualquer destino. O painel Desempenho nos Ajustes
lê as medidas compartilhadas de HTTP, desenho e tarefas uma vez por segundo,
somente enquanto os Ajustes estão abertos. Não é medida de quadros apresentados
pela placa. O registro do W1 mantém a opção de arquivo `HARMONY_LOG_FILE` para
a bancada; ela não é configurada pelo app.

## Provas automáticas

```powershell
cmd /c windows\app\test.cmd
cmd /c windows\app\arquivo\autoteste.cmd
```

O primeiro roda o executável de testes, sem biblioteca adicional. O segundo
usa `e2e-harmony/server.mjs`: cria um servidor Go e banco temporários, contas
por convite e a senha já definida em `e2e-harmony/fixtures.ts`, entregando-a ao
filho só por variável de ambiente. Nada é escrito no banco das contas reais.

O script invoca `harmony-nativo.exe --autoteste <saida.json>`, reconecta o
gateway de verdade, lista as salas, e um segundo cliente Node entra e sai
pela API existente. O cliente de teste não precisa negociar áudio: o teste é
da ocupação da sala. O resultado registra tempo desde o início de cada POST
até o modelo nativo receber o evento. Exige ambos abaixo de 1.000 ms.
Tokens, senhas, códigos e e-mails não vão para o JSON.
O observador entra por e-mail com dois passos: a fixture confirma o e-mail
pelo arquivo do remetente de teste, liga TOTP e fornece um código de recuperação
gerado pelo servidor somente por variável de ambiente. O executável usa o mesmo
serviço de login, leitura de salas e saída da interface, desativando apenas
a persistência da sessão temporária. Não modifica a sessão real do usuário.

Usa porta 8099 e voz 40003; recusa um servidor que já estiver ali. Resultados
em `windows/app/autoteste.json`; caches e servidor de teste em `test-output`.
`GOFLAGS=-buildvcs=false` evita o problema de propriedade do Git no sandbox;
não altera a configuração global do Git. Uma falha tem `passed:false`, tempos
nulos e fase identificada; não substitui falha por estimativa.

Teste adicional do armazenamento, na sessão normal do Windows:

```powershell
cmd /c windows\app\test.cmd --dpapi
```

Também verifica restauração para a mesma origem, recusa outra origem e apaga
o arquivo. Usa um valor sintético, nunca uma sessão real, e um LOCALAPPDATA
de teste dentro do worktree. A suíte obrigatória e esta integração são
separadas: uma falha de DPAPI continua sendo falha de integração.

## Instalar por usuário

Depois do build, na sessão normal do dono, sem administrador:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File windows\app\instalar\instalar.ps1
```

Pasta `%LOCALAPPDATA%\Programs\harmony-nativo`, atalho “harmony (nativo)” no
Iniciar, AppUserModelID `Harmony.Native.Windows`, entrada própria em Aplicativos
instalados. Só HKCU, com nome próprio `harmony-nativo` na chave Run. A primeira
instalação liga abrir com o Windows, com `--bandeja`; os Ajustes permitem
desligar. Atualizar preserva a preferência. Rodar o build avulso não altera
a chave Run. A casca `harmony.exe` mantém todos os seus arquivos e chaves.

O instalador adapta o W1: cópia em staging, conferência SHA-256, versão imutável
`app-N`, troca atômica de `current.json`, limpeza adiada enquanto a versão
antiga estiver em uso. O atalho e a chave Run apontam para o executável da
versão completa. `harmony.cmd` usa o lançador que confere o manifesto.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File windows\app\instalar\instalar.ps1 -Versao 2
powershell -NoProfile -ExecutionPolicy Bypass -File windows\app\instalar\instalar.ps1 -Limpar
powershell -NoProfile -ExecutionPolicy Bypass -File windows\app\instalar\instalar.ps1 -Remover
```

Feche o programa antes de desinstalar. A desinstalação remove somente o
atalho, a chave Run, o cadastro e a pasta de instalação do nativo; conserva
o arquivo DPAPI. Use Sair da conta antes para revogar e apagar essa sessão.

`-Destino <pasta>` e `-SomenteArquivos` permitem provar cópia, hashes e troca de
versão no worktree, sem gravar atalho ou registro. Esse teste não comprova
integração com Iniciar, bandeja ou Aplicativos instalados.

## Conferência com o dono

O teste real com a conta do dono e o celular, login com dois passos, bandeja,
segunda instância, abertura no próximo login do Windows, Narrador, aparência
das três telas e instalação em LOCALAPPDATA precisam da sessão normal do dono.
O relatório do W2 distingue o que foi medido do que ainda não foi testado.

## Regressões da auditoria

`test.cmd` inclui servidor TCP local mudo (handshake e WebSocket), retorno
do login de dois passos, presença com falha/reconexão, aviso de sincronização,
linha real de crash, lançador estável com registro simulado e estado da voz.
`arquivo/autoteste.cmd` também cria/libera 20 interruptores WinUI e verifica referências fracas.

```powershell
python windows\app\arquivo\mutation-tests.py
powershell -NoProfile -ExecutionPolicy Bypass -File windows\app\arquivo\toggle-mutation.ps1
```

As mutações geram cópias em `test-output/mutations`; não alteram os fontes.
Não rode a prova WinUI junto de `build.cmd`: usam o mesmo cache local de pacotes.

## Usar a voz

Clique no nome de uma sala. Clicar em outra sai da anterior e entra na escolhida.
A barra inferior tem Sair, microfone, não ouvir e a engrenagem de Ajustes de voz.
Não ouvir também desliga o envio do microfone, mantendo sua escolha anterior de mudo.
O anel verde usa RMS dos desvios em 512 amostras, na escala de bytes do web
(>2,5), consulta a cada 25 ms
e segura 320 ms. O medidor remoto vem antes do ganho: volume zero não esconde quem fala.

Os participantes também aparecem na área central, em quadros 16:9 com o anel
de fala do web. Os quadros abrem o mesmo controle de volume da lista.
Clique ou use o botão direito em uma pessoa para alterar 0–200%. O arquivo local
`voice-volumes.json` mantém o volume por usuário entre sessões, sem credenciais.
Um adaptador do mixer do próprio libwebrtc aplica o ganho por SSRC depois
da decodificação e do medidor, antes da soma dos participantes. O volume do
RemoteAudioSource fica em unidade; o adaptador é o ganho efetivo, garantindo
um único ajuste verificável por fonte antes da soma. A saída segue intacta para o fone. A referência do AEC recebe esse
mesmo mix já ajustado, somado ao loopback do W1 que exclui o próprio processo:
o som de outros programas serve apenas para cancelar eco; não é publicado como microfone.
O APM do microfone é separado do APM de saída para analisar uma única referência combinada.
A disponibilidade do loopback é registrada; se Windows o recusar, resta a referência da sala.
O loopback conserva até oito blocos de 10 ms, consumidos uma única vez, em ordem.
Blocos com 80 ms de idade são descartados; falta de referência gera silêncio.
Faltas, descartes do mais antigo e idade aparecem em Desempenho e no registro.
A eficácia acústica precisa do ensaio com alto-falante e live; a prova sintética não mede eco.

Ajustes de voz mostra primeiro Padrão do Windows, seguido de endpoints ativos.
Trocar dispositivo para/reinicia só captura ou reprodução, conservando a PeerConnection.
Durante a chamada a lista é revista a cada segundo; remoção ou mudança do padrão volta
para o padrão e fica no registro. Fora da chamada a lista é atualizada ao abrir os Ajustes.
A intenção da chamada sobrevive à falha do dispositivo. Abertura e reinício têm até
cinco tentativas, com espera crescente de 500 ms até 4 s. A recuperação só é anunciada
quando os callbacks daquele sentido crescem. A barra avisa quando o dispositivo
está indisponível ou aguardando áudio; uma nova escolha ou mudança na lista abre
outra janela de tentativas.

Rede, gateway e retomada automática do Windows pedem ICE restart da mesma sessão.
Uma oferta pendente é respondida antes do restart novo; ICE conectado sozinho não basta:
a indicação some quando os bytes RTP crescem. Se a sessão não existir após reiniciar o
servidor, entra novamente na mesma sala. Enquanto a rede está fora, mantém a sala
sem gastar tentativas. Ao voltar a rede ou retomar do sono, começa uma janela nova
e invalida os timers antigos, conservando o tempo total da queda. O prazo por
tentativa é 8 s, com até três novas entradas por ciclo; falhas persistentes esperam
10 s antes de outro ciclo, sem desistência permanente. A contagem e os tempos
aparecem no registro, sem identificadores secretos.

DFN3 está integrado pela libDF/C API **0.5.6**, aprovada pelo dono. O modelo
oficial vai como arquivo junto à DLL na pasta de cada versão instalada. O programa
confere tamanho e SHA-256 dos dois antes de usar; caminho absoluto em UTF-8,
inclusive com acentos e espaços. Modelo ausente, truncado ou trocado nunca chega
a `df_create`: conserva o bloco com NS do libwebrtc e conta a reserva.

Ajustes de voz permite ligar/desligar DFN e escolher Força **10–100 dB**, padrão
**50**. As escolhas ficam em `voice-noise.json`, sem credenciais. O processamento
é síncrono em blocos mono de 480 amostras a 48 kHz; estéreo desce pela média e
recebe o mesmo mono nos dois canais, como o web. Não há fila de voz antiga. Falha,
exceção, formato inválido ou chamada com 10 ms ou mais conserva o bloco original
com NS; o painel e o registro mostram **DFN3**, **libwebrtc NS** ou **raw** conforme
o caminho efetivo. Uma chamada atrasada síncrona só pode ser rejeitada depois que
retorna; esse limite não é um cancelamento da DLL.

DLL, modelo e licenças entram no build e no instalador com hashes fixos.
As gravações reais da bancada são somente testes locais, fora do pacote.
Na conferência de 07/10, a bancada de 6.000 blocos mediu p99 de **2,309 ms**
e CPU de **6,77% de um núcleo**. A mistura de fala real com ruído a SNR de
5 dB mostrou **20,096 dB** de redução nas pausas, correlação de fala **0,862**
e energia de **46,25%** da fala limpa. A redução nas pausas não representa
preservação integral da fala nem aprovação de qualidade por escuta.
Decisão e resultados: [DFN-PROPOSTA.md](DFN-PROPOSTA.md) e
[w3-dfn.md](../../w3-dfn.md). Repita `python windows/app/arquivo/dfn-benchmark.py` para os
6.000 blocos e `cmd /c windows\app\testes\dfn-test.cmd` para o adaptador e fala real.
Conferência final de 07/10: os dois builds, `test.cmd`, **52 mutações** e
`arquivo/autoteste-voz.cmd` passaram; o autoteste exigiu DFN ativo, com Força 50.
Cada mutação conserva seus próprios resultados, sem substituir o ensaio válido.

## Provar W3 com um comando

Depois dos dois builds:

```powershell
cmd /c windows\app\arquivo\autoteste-voz.cmd
cmd /c windows\app\queixas\w3\medir.cmd
```

O autoteste usa Go local descartável, três contas de teste e Chromium com áudio falso,
sem alterar Go nem o servidor em produção. Porta 8103; `network_ignore_mask=0` só para
loopback. O executável é chamado com `--autoteste-voz <saida.json>` dentro do worktree.
Verifica níveis nos dois sentidos, mudo e não ouvir com áudio recebido silencioso,
ganho zero estável na saída, outra pessoa ainda ouvida, dispositivo ausente e
rede/retomada simulados sem trocar sessão; mata e religa
apenas o Go que criou e mede a volta à mesma sala. O tom sintético do nativo exige
servidor HTTP local e variável de teste; não é ativado no uso normal.
O dispositivo é um ADM controlado, habilitado somente para a prova HTTP local:
abre A por ID, retira A, abre o padrão B e exige novos callbacks de captura e saída
pelo Transport real, mantendo a PeerConnection e a sessão do servidor. Isso não
substitui tirar um USB/Bluetooth físico. Falha de abertura ou fluxo parado reprova.

`test.cmd` roda também `testes/engine-test.cmd`, que compila os próprios `voice.cpp` e
`audio.cpp` com ADM, transporte HTTP e relógio controlados. As mutações atingem
`Session::SetMute`, `ChooseDevice`, `RestartAttempt`, o medidor e a fila da live.
Rede longa fora, sessão perdida no servidor e retomada com timers vencidos têm
caminhos distintos. Resultados e limites atualizados: [w3-conserto.md](../../w3-conserto.md).

A auditoria usa Go local na porta 8105 e WinUI com dados visuais fictícios. Fotos e
retângulos ficam em `queixas/w3`, com resultado separado por ferramenta. Não execute
as duas provas simultaneamente nem junto do build. Credenciais transitórias passam
em variáveis de ambiente; JSON, fotos e diagnóstico não incluem senha/token/e-mail.

Os ensaios físicos de eco, microfone/fone USB ou Bluetooth, Wi-Fi e suspensão real
ficam descritos em [w3-saida.md](../../w3-saida.md), junto das limitações e resultados.

## Revisão W3b — filtros e comparação com o web

O microfone é normalizado pelo resampler já incluído para mono, 48 kHz e 480
quadros antes do APM/DFN. Eco, ruído, ganho automático e volume do microfone
são ajustáveis e guardados entre sessões; Ajustes mostra o filtro efetivo e
seu motivo de reserva. O ADM controlado cobre seis taxas e quatro quantidades
de canais, com a DLL real e publicação de PCM pelo motor real.

As medidas, estados e fotos ficam em `queixas/w3b`. Execute
`cmd /c windows\app\queixas\w3b\medir.cmd` na raiz do worktree, em sequência
com os demais testes. A comparação declara as diferenças restantes.
`arquivo/verificar-iniciar.ps1` lê o atalho e `Get-StartApps`; a instalação de teste
exige uma conta Windows separada, sem o nativo instalado.
Resultados, limites e comandos completos: [w3b-saida.md](../../w3b-saida.md).

## W4 — tela e câmera na sala

O seletor próprio lista janelas e monitores, qualidade, quadros e som. A captura
reaproveita WGC, escala D3D11 e H.264 do W1. Tela, câmera e som da tela usam as
vagas existentes do protocolo; trocar a fonte mantém a PeerConnection. O aviso
externo usa uma barra Win32 sempre visível, excluída da captura antes de começar.
As miniaturas atualizam uma fonte por vez, com pausa de dois segundos, enquanto
o seletor está aberto. A câmera escolhida em Ajustes é guardada localmente.

Som de janela inclui a árvore do processo escolhido; som de monitor exclui a
árvore do harmony. A faixa é estéreo a 48 kHz, separada do filtro de microfone.
Em navegadores, o processo da janela pode não ser ancestral do processo que toca
o som. Nesse caso, escolha o monitor; isso inclui os outros programas, exceto
o harmony. Não existe injeção em jogos.

Na raiz deste worktree, execute em sequência, sem compilações concorrentes:

```bat
cmd /c windows\app\arquivo\media-inventory.cmd
cmd /c windows\app\arquivo\camera-formats.cmd
cmd /c windows\app\build.cmd
cmd /c windows\native\build.cmd
cmd /c windows\native\build-gpu.cmd
cmd /c windows\app\arquivo\system-audio-test.cmd
cmd /c windows\app\test.cmd
python windows\app\arquivo\mutation-tests.py
cmd /c windows\app\queixas\w4\medir.cmd
cmd /c windows\app\testes\autoteste-tela.cmd --minutos 1 --camera
cmd /c windows\app\testes\autoteste-tela.cmd --minutos 30 --camera
```

O autoteste cria um Go local descartável e uma janela-jogo própria; mede no
Chromium os quadros RTP recebidos/decodificados e os apresentados separadamente,
intervalos sem quadro, mudanças de resolução e piscar/bipe. Cada janela de um
minuto em alta precisa cumprir ≥57 q/s; o modo de 30 minutos mantém alta durante
todo o período. Cada janela também exige sincronia ≤100 ms. Jogo e chamada de
teste usam CABLE Input; sem ele, o teste falha. Chromium é headless e mudo.
A voz de outro cliente precisa aparecer no medidor de reprodução
do nativo para a prova de exclusão poder passar. `--camera` solicita câmera real.
Sem desktop interativo, o teste reprova e registra o impedimento. As fontes
sintéticas usadas na auditoria de desenho não contam como prova de mídia.

O modo de bancada é `bash scripts/bancada.sh --modo w4 --minutos 30`.
Saídas ficam em `queixas/w4`, com CPU, memória e etapas no formato do W1.
Este sprint ainda tem lacunas de implementação e aceite; veja
[w4-conserto.md](../../w4-conserto.md) e [w4-saida.md](../../w4-saida.md) antes de
juntar ou publicar.
