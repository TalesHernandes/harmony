# Handoff: harmony — app Android (Kotlin + Jetpack Compose)

## Visão geral
harmony é um serviço de conversa privado para uns 20 amigos: um servidor só, canais de texto, salas de voz fixas (4 a 8 pessoas), mensagens diretas e chamada de voz/vídeo/tela. Já existe na web (Preact). Este pacote descreve o **app Android nativo**, que deve ter **a mesma cara do web aberto no celular**, mudando só onde o telefone obriga (barras do sistema, toque em vez de ponteiro, recursos que só o Android tem).

Português do Brasil. **Tema escuro e só escuro.** `dynamicColor = false`.

## Sobre os arquivos
Os arquivos em `telas/` são **referências de design feitas em HTML** (`.dc.html`), não código de produção. A tarefa é **recriar essas telas em Jetpack Compose (Material 3)**, com os padrões do projeto Android. Não embrulhe o HTML num WebView.

Cada `.dc.html` tem duas partes úteis para ler:
- o **template** (entre `<x-dc>` e `</x-dc>`): o layout com os estilos inline, em px = dp e px de letra = sp;
- a **classe `Component`** no `<script data-dc-script>`: os dados de exemplo e as listas de "o que mudou e por quê".

Nas páginas das telas 1 a 7, cada tela aparece duas vezes, desenhada pelo mesmo template: **"Web · Chrome no celular"** (referência) e **"Android"** (o que construir). As diferenças entre as duas são ligadas por chaves (`f.web`, `f.saida` etc.), então tudo que não está atrás de uma chave é igual.

Os HTML carregam o kit visual por caminhos relativos (`../../_ds/...`) que só existem no projeto de design. Fora dele a página pode não renderizar, mas o código-fonte continua completo e legível.

`telas/00-especificacao.dc.html` é a **fonte das medidas**: todas as seções em dp/sp estão no array `sections` da classe. Se este README e a especificação discordarem, vale a especificação.

## Fidelidade
**Alta fidelidade.** Cores, tipografia, espaçamento, raios e texto são finais. Recrie pixel a pixel, em dp.

## Regras globais (não negociáveis)
- **Ilhas:** as peças são blocos `surface #18181B` com raio 8, soltos sobre `background #060607`, com margem de 8 da borda da tela e 8 entre si. **Nenhuma linha divisória, borda, sombra, borrão ou gradiente.** O degrau de cor é o que separa.
- **Raio 8** em toda peça. 9999 (círculo/pílula) só em avatar, ponto de estado, chip, chave, trilho e alças. 4 só no `@nome` da menção.
- **Nada de sombra, borrão, filtro ou gradiente sobre vídeo.** Por cima de vídeo só vão peças opacas.
- **Alvo de toque mínimo 48dp.** O chip de reação aparece com 32, mas toca com 48 (`Modifier.minimumInteractiveComponentSize()`).
- **Movimento:** anime só alpha e translação, 180 ms, `FastOutSlowInEasing`. A cor troca sem animar.
- **Toque:** ripple da plataforma. Anel de foco (2 da cor do fundo + 2 accent) só com teclado físico ou D-pad.
- **Ícones:** só do kit, em `icones-do-kit/` (SVG 16×16, preenchimento). Converta para Vector Drawable e pinte com `tint`. Tamanhos: 12, 16, 20, 24. Nunca emoji como ícone. Emoji aparece só onde as pessoas o escrevem (texto e reações).
- **Edge-to-edge:** `enableEdgeToEdge()`, barras transparentes, ícones claros. O conteúdo respeita `WindowInsets.statusBars` e `navigationBars`. A última ilha termina 8 acima da barra de gestos. Com o teclado aberto: `imePadding()`.
- **Voltar do sistema:** `BackHandler` fecha, nessa ordem: folha, painel, teclado, subtela.

## Tokens

### Cores (as dez com significado)
Cada cor carrega uma frase e não pode ser usada para outra coisa.

| nome | valor | para que serve |
|---|---|---|
| background | `#060607` | fundo de tudo; a lista de mensagens fica direto nele |
| surface | `#18181B` | ilhas: cabeçalho, caixa de escrever, painel, folha, barra da chamada, faixas |
| surfaceSecondary | `#232325` | campo, botão secundário, tile de vídeo, chip de reação dos outros |
| surfaceTertiary | `#323235` | avatar, trilho de volume/progresso, alça da folha |
| selected | `#27272A` | canal aberto, aba ativa, botão da barra da chamada, saída escolhida |
| foreground | `#FCFCFC` | texto |
| muted | `#A1A1AA` | texto secundário, hora, ícone parado |
| accent | `#0485F7` | **"o que você escolheu"**: Entrar, enviar, Mensagem, Atualizar, chave ligada, parte cheia do volume |
| accentSoft / onAccentSoft | `rgba(4,133,247,0.12)` / `#61A8FC` | menção, chip de @, sua reação, caixa de ícone da faixa |
| success | `#17C964` | **"alguém está falando agora"** (anel inset 2) e ponto online. Nada mais. |
| warning | `#F7B750` | "funciona e está piorando": ponto ausente, imagem parada |
| danger | `#DB3B3E` | sair da sala, erro |
| dangerSoft / onDangerSoft | `rgba(219,59,62,0.15)` / `#F5645B` | etiqueta "não lidas", Sair desta conta, download parado |
| offline | `rgba(161,161,170,0.4)` | ponto offline |
| scrim | `rgba(0,0,0,0.30)` | véu atrás de painel e folha, **sem borrão** |

### Tipografia (Inter, sp)
| uso | tamanho/altura | peso | tracking |
|---|---|---|---|
| título de tela e folha | 20/28 | 600 | -0.01em |
| corpo da mensagem, campo | 16/24 | 400 | -0.005em |
| item de lista, linha de ajuste | 16/20 | 600 (não lido/aberto) · 500 | -0.02em |
| nome do autor | 15/22 | 600 | -0.02em |
| pessoa na sala, botão da faixa | 15/20 | 400 · 600 | -0.02em |
| meta (hora, contagem, linha de apoio) | 13/16 | 400 muted | -0.02em |
| rótulo de seção, etiqueta | 12/16 | 600, MAIÚSCULAS | +0.06em |

### Espaço
Escala única: 4, 8, 12, 16, 24, 32.

```kotlin
object H {
  val bg = Color(0xFF060607); val surface = Color(0xFF18181B)
  val surface2 = Color(0xFF232325); val surface3 = Color(0xFF323235)
  val selected = Color(0xFF27272A); val fg = Color(0xFFFCFCFC); val muted = Color(0xFFA1A1AA)
  val accent = Color(0xFF0485F7); val accentSoft = Color(0x1F0485F7); val onAccentSoft = Color(0xFF61A8FC)
  val success = Color(0xFF17C964); val warning = Color(0xFFF7B750)
  val danger = Color(0xFFDB3B3E); val dangerSoft = Color(0x26DB3B3E); val onDangerSoft = Color(0xFFF5645B)
  val scrim = Color(0x4D000000)
  val radius = RoundedCornerShape(8.dp); val gap = 8.dp; val hit = 48.dp
}
```

## Peças que se repetem
- **Cabeçalho:** ilha de 56 · padding horizontal 4 · botões de ícone 48×48 com ícone 20 · título 20/28 600.
- **Campo:** 48 de altura · padding 0 12 · surfaceSecondary · 16sp. O rótulo fica acima, com gap 6.
- **Botão principal:** 48 · accent · 16/24 600 (15/20 600 dentro das faixas). **Secundário:** surfaceSecondary.
- **Avatar:** círculo surfaceTertiary com as iniciais em 600. O ponto de estado fica no canto inferior direito, recortado do avatar (anel de 2 da cor de fundo). Tamanhos: 24 · 32 (ponto 9) · 36 · 64 (ponto 16).
- **Canal aberto / item selecionado:** fundo `selected` + **barra accent de 3 de largura, altura toda da linha, raio 0 9999 9999 0, colada na borda da ilha (8 à esquerda da linha)**.
- **Menção a você (igual ao web no PC):** à esquerda, a mesma barra do canal aberto, na borda da ilha. **5 de folga**, e depois a mensagem dentro de uma caixa `rgba(4,133,247,0.10)` com raio 8 e padding 10 8 12. O `@mel` no texto vai em accentSoft/onAccentSoft, raio 4, padding 1 4.
- **Folha (bottom sheet):** `ModalBottomSheet` com a forma trocada: **raio 8 nos quatro cantos**, 8 das laterais e 8 acima da barra de gestos. Alça: zona de 24, barra 36×4 surfaceTertiary. Fica sobre o véu de 30%.
- **Chave:** 52×32 · ligada: accent com polegar 24 foreground · desligada: surfaceTertiary com polegar muted.

## Telas

### 1 · Entrar — `telas/01-entrar.dc.html`
- Cartão-ilha centrado na vertical · padding 20 · gap 16.
- "harmony" 20/28 600 · "Entre para continuar." 13/20 muted.
- USUÁRIO / SENHA: campos de 48, gap 12 entre eles. A senha em foco mostra o anel accent.
- Botão "Entrar" (48, largura toda). Linha 13 muted: "Contas novas só por convite de quem já está dentro."
- **Só no Android:** abaixo do cartão, uma linha de 48 com o ícone `server` 16 muted, o endereço "harmony.taleshernandes.com" 13 muted e "Trocar" 13 600 (alvo 48). Tocar em Trocar abre uma segunda ilha (padding 16 20 20) com o rótulo "ENDEREÇO DO SERVIDOR", o campo em foco e o apoio "O mesmo endereço que você abre no navegador." O endereço vem preenchido com o último usado.
- Campos com `ContentType.Username` / `Password` (Autofill) e `ImeAction.Next`, depois `Done`.

### 2 · Lista (painel da esquerda) — `telas/02-lista.dc.html`
- Entra por cima da conversa: esquerda 8, topo e base 8, **largura 296**. Os 78 da conversa ficam à mostra sob o véu; tocar ali fecha o painel.
- Duas ilhas, gap 8: a **lista** (flex) e a **peça da pessoa** (64).
- Lista: padding 0 8 8. "harmony" 20/28 em linha de 56. **Abas** Servidor / Mensagens, 48 cada, gap 4: a ativa em `selected` 15/20 600, a outra muted 500. O contador fica **só na aba fechada**: pílula de 20 #FCFCFC com texto #18181B (ex.: "4"), ou chip @ accentSoft (ex.: "@2").
- Aba Servidor:
  - rótulo CANAIS;
  - canal de 48 · padding 0 12 · gap 12 · `hashtag` 16 · nome 16/20 (não lido: foreground 600; lido: muted 500) · chip de menção 20 (`at` 12 + número) · silenciado com `bell-slash` 16 muted;
  - rótulo VOZ; sala de 48 · `volume` 16 · nome · ocupação "4/8" 13 muted;
  - quem está na sala: linha de 44 · recuo 40 · avatar 24 · nome 15/20. Estados: **falando** = anel inset 2 success no avatar · **mudo** = `microphone-slash` 16 · **sem som** = `volume-xmark` 16 · **transmitindo** = `display-pulse` 16, **sem texto**.
- Aba Mensagens: conversa direta de 56 · avatar 32 com ponto de estado · nome · contador ou `bell-slash`.
- Peça da pessoa: avatar 32 com ponto · "Mel Sarmento" 15/20 600 + "@mel" 13 muted · `gear` 20 em alvo 48 (abre Ajustes).
- Abre pelo botão `bars` ou arrastando **a partir do meio** da conversa, nunca da borda, que é o gesto de voltar do sistema. Fecha arrastando para a esquerda, tocando na faixa ou com o voltar.

### 3 · Conversa — `telas/03-conversa.dc.html`
- Cabeçalho-ilha: `bars` 48 · `hashtag` 20 muted · "geral" 20/28 · `magnifier` 48 · `persons` 48.
- Lista direto no background, padding horizontal 8, ancorada embaixo (`LazyColumn(reverseLayout = true)`).
  - **Dia:** etiqueta de 24 · padding 0 10 · surface · centrada · sem linha.
  - **Não lidas:** etiqueta de 20 · dangerSoft · à esquerda · sem linha.
  - **Mensagem:** grade 48 + resto · avatar 36 · 12 de espaço acima do grupo · nome 15/22 600 + hora 13 muted · texto 16/24.
  - **Seguinte do grupo:** sem avatar · 2 acima · a hora vai ao fim da linha, só na última de cada minuto.
  - **Resposta citada:** linha acima da mensagem · `arrow-shape-turn-up-left` 12 · nome 13 600 · trecho 13 muted em uma linha.
  - **Imagens:** até 280 de largura · gap 4 · quadradas · raio 8.
  - **Reações:** chip de 32 · padding 0 10 · a sua em accentSoft, as dos outros em surface. Alvo 48.
  - **Menção a você:** ver "Peças que se repetem".
  - **Digitando:** "Tainá está digitando…" 13/20 muted, recuo 48.
- Caixa de escrever (ilha, padding 4): `paperclip` 48 · texto 16/24 com padding 12 0 · `face-smile` 48. Com texto, aparece o enviar: 48 accent com `paper-plane`.
- **Respondendo:** linha de 44 no topo da caixa · `arrow-shape-turn-up-left` 16 · "Respondendo a **Gabi Antunes**" 13 · `xmark` 16 em alvo 44.
- Toque longo na mensagem abre a folha de reagir e responder, com `HapticFeedbackType.LongPress`.
- Anexo: `PickVisualMedia` para foto e vídeo, `OpenDocument` para o resto.

### 4 · Sala de voz — `telas/04-sala-de-voz.dc.html`
- Cabeçalho: `chevron-left` 48 · "Mesa grande" 16/20 600 + "5 pessoas · 1:12:04" 13 muted · `persons` 48.
- **Palco com tela:** 16:9 na largura toda (374×210 em 390) · surfaceSecondary · raio 8. Depois, os assentos em grade 2×2, gap 8, ocupando o resto.
- **Palco com câmera:** ocupa a altura que sobra. Os assentos viram uma fileira de 4 × 96 (avatar 40, nome 12). Quem fala tem o anel success, no palco ou no assento.
- Sobre o vídeo, só peças opacas: nome em pílula de 28 background (padding 0 10, 13/16 500, com `display-pulse` 12 na tela); tela cheia 48×48 surface com `chevrons-expand-up-right` 20, a 8 do canto.
- Assento: surface · avatar 56 · nome em pílula no canto inferior esquerdo, com o ícone de estado 12.
- **Barra da chamada** (ilha, padding 8, gap 8, botões 48×48 raio 8): `microphone` · `volume` (sem som) · `camera` · **`headphones` (saída do som, só Android)** · [espaço] · `handset-arrow-out` em danger. Ligado: fundo `selected`, ícone foreground. Desligado: ícone muted.
- **Saída do som:** o ícone mostra a saída atual (`headphones` / `megaphone` / `smartphone`) e só aparece com 2 ou mais saídas (`AudioManager.getAvailableCommunicationDevices`). Toque abre a folha "Por onde sai o som": linhas de 56 (Fone Bluetooth, Alto-falante, Fone do aparelho), a escolhida em `selected` com `check` 20 accent.
- Fora do app: a sala continua via ForegroundService com notificação de chamada, e o palco vira PiP (tela 9).

### 5 · Painel da pessoa — `telas/05-painel-da-pessoa.dc.html`
- Folha (ver "Peças que se repetem") · padding 0 12 12 · gap 16.
- Cabeça: avatar 64 com ponto 16 · "Gabi Antunes" 20/28 · "@gabi · na Mesa grande" 13 muted.
- APELIDO · SÓ VOCÊ VÊ: campo de 48 ("gabi do estúdio").
- VOLUME PARA VOCÊ: duas linhas de 48 · ícone 20 (`volume`, `display`) · rótulo 15/20 500 com 92 de largura ("Voz", "Transmissão") · trilho 4 surfaceTertiary com a parte cheia em accent · polegar 20 foreground · valor 13 muted com 32 de largura. O volume anda de 5 em 5, com vibração leve a cada 10. O alvo é a linha inteira.
- "Mensagem": 48, largura toda, accent, `paper-plane`.
- Três ações de 72, gap 8, surfaceSecondary, ícone 20 + 13/16 500: **Ligar** (`handset`), **Entrar na sala** (`handset-arrow-in`), **Assistir** (`display-pulse`). Se a pessoa não está numa sala, as duas últimas somem e Ligar ocupa a largura toda.

### 6 · Ajustes — `telas/06-ajustes.dc.html`
- Uma coluna que rola · cabeçalho `chevron-left` + "Ajustes".
- Grupo: rótulo 12/16 com padding 8 12 4 · ilha com padding 4.
- Linha: mínimo 56 · padding 6 8 6 12 · ícone 20 muted · nome 16/20 500 + apoio 13/16 muted · `chevron-right` 20, chave ou nada.
- **VOZ E VÍDEO:** Microfone ("Do fone Bluetooth") · Câmera ("Frontal") · **Saída do som** ("Fone Bluetooth") · Reduzir ruído (chave) · Cancelar eco (chave).
- **AVISOS:** Notificações ("Ligadas · som e vibração nos Ajustes do Android", abre `Settings.ACTION_APP_NOTIFICATION_SETTINGS`) · Só menções e diretas (chave) · **Chamada com o celular bloqueado** ("Faltam 2 de 3 chaves da Xiaomi"; só quando `Build.MANUFACTURER` é Xiaomi; abre o guia da tela 10).
- **CONTA:** Aparelhos conectados · Convites · Sair desta conta (ícone e texto em onDangerSoft).
- **DIAGNÓSTICO:** Versão do app ("1.8.2 (182) · procurado hoje às 21:10") · botão "Procurar atualização" (48, surfaceSecondary, `arrow-rotate-right`) · Registro.

### 7 · Faixa de atualização — `telas/07-atualizacao.dc.html`
- Ilha **no layout, logo abaixo do cabeçalho**: empurra a lista, não flutua.
- Faixa: padding 8 8 8 12 · gap 12 · caixa do ícone 32 raio 8 · título 15/20 500 · linha 13/16 muted · botão de 48 com padding 0 16 e texto 15/20 600.
- Estados:
  1. **Disponível:** `arrow-down-to-line` em accentSoft · "Versão nova disponível" / "1.9.0 · 31 MB" · botão **Atualizar** (accent). Arrastar para o lado adia até o dia seguinte.
  2. **Baixando:** "Baixando a versão 1.9.0" / "12,4 de 31 MB" · botão **Cancelar** (surfaceSecondary) · trilho de progresso de 4 abaixo (a faixa passa de 64 para 76). O download continua fora do app, numa notificação com a mesma barra.
  3. **Pronta:** "Versão 1.9.0 baixada" / "O Android vai pedir para instalar" · botão **Instalar** (accent), que chama o `PackageInstaller`. Na primeira vez, antes da caixa do sistema, vem a folha de licença (tela 10, "3d").
  4. **Parou:** `circle-exclamation` em dangerSoft · "O download parou" / "Sem internet · 12,4 de 31 MB" · botão **Tentar de novo**. Quando a internet volta, o download retoma sozinho.

### 8 a 11 · Só do Android
- `08-tela-cheia.dc.html` — vídeo deitado 844×390, barras do sistema escondidas; os controles somem 3 s depois do último toque.
- `09-quadrinho-flutuante.dc.html` — PiP 16:9 ou 9:16, só o vídeo. Três `RemoteAction`: volume, microfone, sair.
- `10-permissoes.dc.html` — folha antes de cada pedido do sistema e depois de negado (microfone, câmera, notificações, instalar apps), mais o guia Xiaomi de três passos.
- `11-icone-do-app.dc.html` — ícone adaptativo (tela 108, visível 72, zona segura 66, fundo surface). **A marca ainda não foi entregue.**

## Estados que o app precisa ter
Canal sem mensagens · carregando histórico · reconectando · sem internet · mensagem apagada · pessoa ausente e offline · contador "9+" · nome comprido demais (sempre uma linha, com reticências) · imagem carregando · sala vazia · transmissão que ainda não chegou ("imagem parada · reconectando" em warning) · microfone ou câmera negados (tela 10).

## Imagens
`imagens/` tem uma captura de cada tela Android em 2x (780×1688 para 390×844). Use as imagens para conferir o resultado. As medidas continuam valendo pela especificação.

| arquivo | tela |
|---|---|
| `01-entrar.png` · `01-entrar-servidor.png` | Entrar, com o servidor fechado e depois de tocar em Trocar |
| `02-lista-servidor.png` · `02-lista-mensagens.png` | Painel da lista, nas duas abas |
| `03-conversa.png` · `03-conversa-respondendo.png` | Conversa, parada e respondendo com o teclado aberto |
| `04-sala-tela.png` · `04-sala-camera.png` · `04-folha-saida-do-som.png` | Sala de voz com tela, com câmera, e a folha de saída do som |
| `05-painel-da-pessoa.png` | Painel da pessoa |
| `06-ajustes.png` | Ajustes, com a rolagem inteira (390×1180) |
| `07-atualizacao-baixando.png` · `07a…07d-faixa-*.png` | Faixa de atualização na tela e os quatro estados |
| `08-tela-cheia-*.png` | Tela cheia deitada, com e sem controles |
| `09-pip-*.png` | Quadrinho flutuante com os botões e com a imagem parada |
| `10-permissao-*.png` · `10-guia-xiaomi.png` | Folha de permissão antes e negada, e o guia Xiaomi |

Os quadrados cinza com ícone de foto são lugares de imagem ou vídeo, não conteúdo.

## Assets
- `icones-do-kit/*.svg` — os 45 glifos usados nas telas, tirados do kit HeroUI 3 sem modificação (16×16, `fill` #FCFCFC; troque por `tint`).
- `icones-de-sistema/notif-*.svg` — ícones de notificação (glifo do kit, só alfa, 24dp).
- `icones-de-sistema/pip-*.svg` — ícones das ações do PiP.
- Fonte: **Inter** 400/500/600, hospedada junto com o app (`res/font`).

## Arquivos
```
design_handoff_harmony_android/
  README.md                      ← este arquivo
  telas/00-especificacao.dc.html ← todas as medidas em dp/sp (fonte da verdade)
  telas/01…07-*.dc.html          ← telas do app, web vs Android lado a lado
  telas/08…11-*.dc.html          ← telas que só existem no Android
  imagens/                       ← capturas das telas Android (2x)
  icones-do-kit/                 ← glifos usados nas telas
  icones-de-sistema/             ← notificação e PiP
```

## Pendências
- Confirmar no aparelho o caminho de "Outras permissões" nos passos 1 e 2 do guia Xiaomi (tela 10).
- Arquivo da marca para o ícone do app (frente, fundo e monocromático).
