# Prova W1: telas — terceira passada, 06/10/2026

**Twemoji em fonte colorida embutida funcionou, com seleção e cópia Unicode.**
O build passou. A rolagem e o Enter por entrada real continuam sem comprovação:
esta sessão usa um desktop isolado do sandbox, diferente do desktop de entrada
do Windows. O autoteste continua vermelho. Sem pacote NuGet novo, servidor,
commit, push ou publicação.

## Números novos

MB significa bytes / 1.048.576 (MiB). Uma execução, sem extrapolação.

| Medida | Resultado |
|---|---:|
| Processo até primeiro Rendering com corpo disposto | 419,116 ms |
| Modelo inicial | 5.000 mensagens |
| Frequência nominal do monitor da janela | 240 Hz |
| Memória no fim: working set / privada | 110,590 / 93,039 MiB |
| FPS na animação e na roda | não consegui medir em primeiro plano |
| Pior intervalo; quantidade acima de 33 ms | não encontrado |
| Memória após percurso completo | não encontrado |
| Pasta `out` completa | 94,375 MiB; 98.959.849 bytes; 271 arquivos |
| Fonte Twemoji | 1.474.284 bytes; 1,406 MiB |

A abertura é anterior à apresentação na tela, não uma latência de apresentação
confirmada. A frequência nominal veio de MonitorFromWindow, GetMonitorInfoW e
EnumDisplaySettingsW. O tamanho inclui PDB, ILK, runtime e todos os recursos.
`saida.json` também lista as dez maiores DLLs.

`medidas-passada3.json` registra a falha com FPS/intervalos nulos. `medidas.json`
é histórico da segunda passada, com passos por itens: **não é a medida nova e
não serve para concluir fluidez**. Não repeti a instalação, já confirmada pelo
dono fora do sandbox (110 MB, 276 arquivos, sem administrador; saída anterior).

## Primeiro plano: bloqueio real

Diagnóstico gravado pelo próprio executável:

```
Thread desktop: CodexSandboxDesktop-4116161f0dd59b134cedbd1fdb7bfaa9
input desktop: Default
foreground HWND: 0
```

SetForegroundWindow retornou falso. Não havia thread de janela em primeiro
plano à qual ligar AttachThreadInput. Uma tentativa intermediária de clique
por SendInput também não trouxe a janela ao primeiro plano. A versão final
verifica o desktop de entrada antes do clique; registra o bloqueio quando é
diferente. Não troquei de desktop nem contornei o sandbox.

Numa sessão normal, tenta SetForegroundWindow, depois AttachThreadInput +
BringWindowToTop + SetFocus e, finalmente, clique no título da própria janela.
Registra qual funcionou. A medição interrompe se perder primeiro plano.

## Rolagem preparada

Removi o percurso antigo de três itens. Um timer conduz o ScrollViewer da
ListView com ChangeView e animação habilitada, pelo tempo decorrido: cinco
segundos topo → fim e cinco fim → topo, mais meio segundo para assentar cada
ponta. Cerca de onze segundos, sem esperar a realização de cada item.

Depois, uma thread independente envia 600 cliques por SendInput: 300 para
baixo e 300 para cima, delta 120, cadência solicitada de 60 cliques/s. A roda
percorre o que essa entrada produzir; não prometo que 300 cliques cheguem ao
item 5.000. Ambos gravam offsets inicial, de inversão e final e altura rolável.

Conta somente RenderingEventArgs.RenderingTime **distinto**. Calcula FPS pelos
intervalos desse relógio, pior intervalo e quantidade acima de 33 ms. É a
alternativa de relógio de Rendering solicitada pelo dono; não confirma cada
apresentação pela GPU. Não conta callbacks repetidos do mesmo RenderingTime.
**Compilado, mas não executado em primeiro plano nesta sessão.**

## Autoteste: passou parcialmente

`autoteste.json` contém `passed: false`. Passaram de verdade:

- ListView e modelo com 5.000 itens; nomes acessíveis do item e da caixa.
- Seleção parcial atravessando o Run de emoji da fonte Twemoji embutida.
- Cópia nativa, lida de CF_UNICODETEXT: exatamente `ação 🔥 ç ã ê`.
- Cópia da mensagem só de emoji: `🔥😀\r\n` (CRLF nativo do RichTextBlock).
- **UIA SetFocus** na caixa, por cliente nativo numa thread MTA.
- **UIA ValuePattern** e preservação de `ação é ç ã ê`.

O foco UIA passou, mas a janela não está no desktop de entrada. Antes de
SendInput VK_RETURN, o teste verifica primeiro plano. O item 5.001 **não foi
comprovado**; não substituí Enter por chamada direta ao método de acrescentar.

**ABNT2 foi encontrado carregado**, KLID `00010416`. O teste preparado envia
KEYEVENTF_SCANCODE para `´ + a`, `~ + a`, `Shift + ~ + e` e `ç`, verifica
`á ã ê ç` e restaura o layout. Nesta execução, `dead_keys_tested: false`:
o desktop isolado impediu a entrada. Preservar Unicode via ValuePattern não
comprova teclas mortas.

A seleção/cópia foi exercitada por RichTextBlock.Select e
CopySelectionToClipboard. Arrasto físico, Ctrl+C físico e Narrador/NVDA não
foram testados. Os nomes acessíveis foram consultados, não presumidos.

## Fonte Twemoji: encontrada e funcionando

Fontes oficiais consultadas:
[projeto COLR/CPAL](https://github.com/mozilla/twemoji-colr),
[release 0.7.0](https://github.com/mozilla/twemoji-colr/releases/tag/v0.7.0),
[licenciamento](https://github.com/mozilla/twemoji-colr/blob/v0.7.0/LICENSE.md).
**Apache 2.0 para o código; CC BY 4.0 para a arte**, com atribuição a Twitter e
contribuidores. Família Twemoji Mozilla, versão interna 0.7.0, COLR versão 0
e CPAL. Repertório Twemoji 14; cobertura de emoji posteriores não comprovada.

O download pelo shell falhou: Invoke-RestMethod retornou
`Impossível conectar-se ao servidor remoto`. Essa mesma fonte estava na
instalação local do Firefox, em
`C:/Program Files/Mozilla Firefox/fonts/TwemojiMozilla.ttf`. Copiei somente
esse TTF, sem modificar, para `emoji/TwemojiMozilla.ttf`. Não instalei fonte
no Windows. Origem, tamanho, licenças e SHA estão em `emoji/FONT-LICENSE.md`,
também distribuído em `out`.

SHA-256:
`6D90152EE0D29E82FE2A87793AF5AA4B7AD13E6538360889E141E81ED299EE8E`.
`validar-fonte.py` passou, usando só a biblioteca padrão: hash, nome, versão,
COLR/CPAL e glifos com camadas coloridas para os **30 emoji** (`fonte.json`).

O programa usa em cada Run de emoji:

```cpp
FontFamily(L"ms-appx:///emoji/TwemojiMozilla.ttf#Twemoji Mozilla")
```

**Funcionou no WinUI 3 sem MSIX.** Inspecionei `janela-twemoji.png` e
`janela-segoe.png`: fogo e rostos em cor e com desenhos diferentes. A fonte
embutida mostra o desenho plano Twemoji; a comparação Segoe mostra o desenho
da Microsoft. Isso descarta fallback para Segoe nesses exemplos visíveis.
O autoteste de cópia foi executado com o padrão Twemoji.

## Comandos exatos na raiz do worktree

```powershell
cmd /c windows\native\telas\build.cmd
python windows\native\telas\validar-fonte.py
powershell -NoProfile -ExecutionPolicy Bypass -File windows\native\telas\autoteste.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File windows\native\telas\medir.ps1 -Arquivo windows\native\telas\medidas-passada3.json
powershell -NoProfile -ExecutionPolicy Bypass -File windows\native\telas\abrir-prova.ps1 -Arquivo janela-twemoji.png
powershell -NoProfile -ExecutionPolicy Bypass -File windows\native\telas\abrir-prova.ps1 -Segoe -Arquivo janela-segoe.png
```

As provas pendentes precisam rodar na sessão normal do dono, sem administrador,
com desktop de entrada ativo. Não mexa em outras janelas durante a medição.
Os scripts retornam erro quando a prova falha. Build: v145, SDK 10.0.26100.0,
x64 Release, runtime autocontido, os mesmos nove NuGet aprovados, exclusivamente
locais e conferidos por SHA-256. Nenhuma dependência adicional.

Conferi `git log --oneline HEAD..master`: há `5380027` (aprovação dos três
pacotes de build) no master e ainda não neste branch. Sem merge ou commit.
