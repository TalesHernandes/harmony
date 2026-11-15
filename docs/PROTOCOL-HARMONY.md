# Protocolo do harmony — o que um cliente fala com o servidor

Contrato para quem não é o navegador — o app Android primeiro. O cliente web
(`web/harmony/src`) continua sendo a referência: se este arquivo e o código
discordarem, **o código vence** e este arquivo é que está errado.

Escrito em 24/09/2026, no A1, lido direto do código (os `arquivo:linha` são
daquele dia). Está dividido pelo sprint que precisa de cada parte:

| Parte | Sprint | Estado |
|---|---|---|
| §1 Login e sessão | A1 | escrita e conferida |
| §2 O canal de tempo real (`/ws`) | A1 | escrita e conferida |
| §3 A lista de salas e quem está dentro | A1 | escrita e conferida |
| §4 A sala de voz: entrar, oferta, resposta, candidatos, mudo, sair | A2 | escrita e conferida |
| §4.6 Publicar câmera e tela, assistir e pedir camada | A4 | escrita no A4 (25/09/2026) |
| §5 O chat | A5 | escrita e conferida em 25/09/2026 |

Respostas com corpo são JSON, exceto o download de arquivo (§5.4). Erro é sempre `{"error": "<motivo>"}` com o
código HTTP certo (`api.go:370`). Pedido com corpo precisa de
`Content-Type: application/json`, senão `415 expected_json` (`api.go:374`),
exceto o upload multipart (§5.4).

---

## 1. Login e sessão

### Entrar

`POST /api/login` (`api.go:224`, `:580`)

```json
{"username": "tales", "password": "..."}
```

| Resposta | Quando |
|---|---|
| `200 {"user": {...}}` + cookie | deu certo |
| `401 {"error":"bad_credentials"}` | usuário não existe, senha errada ou conta desativada — **a mesma resposta nos três casos**, de propósito |
| `429 {"error":"rate_limited"}` | tentativas demais por conta ou por endereço (a tentativa é gasta *antes* de conferir a senha, `api.go:598`) |
| `403 {"error":"bad_origin"}` | veio um `Origin` que não é o do harmony (ver abaixo) |

O `user` é `{"username", "display_name", "is_admin", "created_at"}`
(`api.go:719`). O nome de usuário é normalizado no servidor
(`store.NormalizeUsername`), então o app pode mandar como a pessoa digitou.

### O cookie é a sessão

- Nome **`harmony_session`** (`api.go:34`), `Path=/`, `HttpOnly`,
  `SameSite=Lax`, `Secure` em produção (`api.go:472`).
- Vale **30 dias** (`store/accounts.go:26`); o `Expires` do cookie diz quando.
- É um token aleatório; o app o guarda **cifrado** e o devolve em todo pedido
  como `Cookie: harmony_session=<valor>`. Não há outro cabeçalho de
  autenticação.
- Cookie que não vale mais: qualquer rota protegida responde
  `401 {"error":"unauthenticated"}` e manda apagar o cookie (`api.go:456`).
  Para o app, 401 = voltar para a tela de entrar.

### Quem sou eu

`GET /api/me` (`api.go:728`) →
`{"user": {...}, "session_id": "...", "push_enabled": bool}`. É o jeito de
conferir, ao abrir o app, se o cookie guardado ainda vale.

### Sair

`POST /api/logout` (`api.go:703`) revoga **esta** sessão, tira o aparelho de
qualquer sala de voz e fecha os sockets dela. Responde `200 {"ok": true}` mesmo
sem sessão.

### A origem (`Origin`), e por que o app passa

- Pedidos que **alteram** algo (`POST`, `PATCH`, `DELETE`, e o login) conferem
  o cabeçalho `Origin` **só quando ele existe** (`api.go:391`). Um app não
  manda `Origin` — é o mesmo caminho do `curl` — e passa. Se mandar, tem de ser
  a origem do harmony (a comparação ignora maiúsculas e uma `/` final,
  `api.go:391`).
- `GET` não confere origem.
- O **`/ws` é o contrário**: exige a origem (§2).

---

## 2. O canal de tempo real — `/ws`

`GET /ws` com upgrade para WebSocket (`api.go:287`, `gateway.go:351`).

- **Autenticação:** o mesmo cookie `harmony_session`. Sem ele, `401` antes do
  upgrade.
- **Origem obrigatória:** o cabeçalho `Origin` tem de ser igual à origem do
  harmony (`https://harmony.taleshernandes.com` em produção), sem diferenciar
  maiúsculas e ignorando `/` no fim (`gateway.go:355`). Motivo: o `SameSite`
  do cookie **não** impede um handshake de WebSocket vindo de outro site, e sem
  essa conferência qualquer página poderia ler as conversas de quem a visitasse.
  **O app manda `Origin` igual à URL base que a pessoa usa.** Em
  desenvolvimento (servidor sem `BaseURL`) qualquer origem passa.
- **Só recebe.** O servidor descarta tudo que o cliente mandar, com limite de
  1 KB por mensagem (`gateway.go:393`). Toda escrita é pela API JSON.
- **Ping:** o servidor manda ping a cada **25 s** e derruba o socket se não
  receber pong em **60 s** (`gateway.go:24`). O OkHttp responde pong sozinho.
  Em cada ping o servidor reconfere a sessão; sessão revogada ou vencida fecha
  o socket (`gateway.go:430`).
- **Fila:** um aparelho que atrasa mais de 64 eventos é desconectado
  (`gateway.go:31`); ele reconecta e busca de novo.
- **Presença:** abrir o socket é a pessoa chegar, fechar o último é sair
  (`gateway.go:376`, `:391`).

### Não há fotografia inicial — reconectar é buscar de novo

O servidor **não manda fotografia nenhuma ao abrir** — nem canais, nem salas,
nem histórico; no máximo um `presence`, que pode incluir o próprio socket
recém-aberto (`presence.go:97`). O que aconteceu enquanto o socket
estava fechado não chega por ele. A regra do web (`web/harmony/src/gateway.ts`,
comentário do topo), que o app segue igual: **a cada abertura do socket, a
primeira inclusive, buscar de novo** o que se está mostrando
(`GET /api/voice/rooms`, `GET /api/channels`). A tela só está em dia a partir
do momento em que o socket abriu. Reconexão com espera que dobra, de 0,5 s até
15 s (`gateway.ts:24`).

**Fechar o `/ws` não encerra a sessão de voz, e reabri-lo não a refaz**
(`gateway.go:386`): a mídia pode seguir de pé com o socket fechado. Mas os
eventos daquele intervalo — um `voice_offer` inclusive — **não são guardados
para ninguém** (`gateway.go:168`), e uma oferta perdida trava a sessão (§4.4).

### O envelope

Toda mensagem é um texto JSON (`gateway.go:34`):

```json
{"type": "<tipo>", "data": { ... }}
```

### Os eventos

Cada evento vai só para quem pode vê-lo: por membros do canal
(`notifyChannel`), por pessoa, ou por aparelho (`publishSession`).

| `type` | Para quem | `data` | De onde |
|---|---|---|---|
| `voice_room` | membros da sala | `{"channel_id", "peers": [Peer]}` — **a lista inteira** de quem está dentro, não a diferença | `voice.go:434` |
| `voice_offer` | só o aparelho que entrou | oferta do servidor de mídia — **§4, A2** | `sfu/sfu.go:1984` |
| `voice_watchers` | só o aparelho que transmite a tela | quem assiste — **§4, A4** | `sfu/sfu.go:2037` |
| `presence` | todos com o app aberto | `{"username", "state"}`, `state` ∈ `online`, `away`, `offline` | `presence.go:100` |
| `channel` | membros do canal | o canal (formato do §3) — criado, ou uma DM nova | `chat.go:309`, `:382` |
| `channel_removed` | quem era membro | `{"id"}` | `chat.go:344` |
| `message` | membros do canal | a mensagem nova — **A5** | `chat.go:537`, `call.go:508` |
| `message_changed` | membros do canal | editada, apagada ou reação — **A5** | `chat.go:560`, `:576`, `:610` |
| `read` | a própria pessoa | canal lido em outro aparelho — **A5** | `chat.go:635` |
| `typing` | os outros membros | alguém digitando — **A5** | `chat.go:197` |
| `ring` | a pessoa chamada | chamado para uma sala de voz — **A6** | `ring.go:133` |
| `call`, `call_accepted`, `call_taken`, `call_ended`, `call_signal`, `call_state` | os dois lados | a chamada direta 1:1 — **A7** | `call.go` |

Tipo desconhecido se ignora: o servidor pode ganhar eventos novos sem avisar o
app.

---

## 3. A lista de salas e quem está dentro

Duas buscas e um evento.

**Os canais**, `GET /api/channels` (`chat.go:259`) →
`{"channels": [Channel], "can_manage": bool}`. Cada canal
(`chat.go:35`):

```json
{"id": "...", "kind": "text|voice|dm", "name": "...", "topic": "...",
 "muted": false, "unread": 0, "mentions": 0,
 "last_message_id": 0, "last_read_id": 0,
 "peer": {"username": "...", "display_name": "..."}}
```

`peer` só existe em `dm`. As salas de voz são os de `kind == "voice"`, na
ordem em que vêm.

**Quem está dentro**, `GET /api/voice/rooms` (`voice.go:373`) →
`{"rooms": {"<channel_id>": [Peer], ...}}`, só das salas de que a pessoa é
membro. Sala vazia vem como lista vazia. Servidor sem mídia: todas vazias.

**`Peer`** (`sfu/sfu.go:96`):

```json
{"session_id": "...", "username": "...", "display_name": "...",
 "muted": false, "deafened": false, "joined_at": 0,
 "publishing": ["cam", "screen", "screen_audio"]}
```

`publishing` (valores em `sfu/sfu.go:147`) é o que está **chegando** de fato ao servidor, não o que a pessoa
disse que ia mandar — **menos o microfone, que nunca aparece**: "todo mundo
tem microfone; dizer isso não diz nada" (`sfu/sfu.go:1889`). Para saber se o
som de alguém chega, é preciso medir do lado de quem recebe. A mesma pessoa pode aparecer duas vezes, de dois
aparelhos: o `session_id` é que distingue.

**Ao vivo:** o evento `voice_room` traz a lista inteira de uma sala e
**substitui** a que o app tem. Um canal novo chega por `channel`, um apagado por
`channel_removed`.

---

## 4. A sala de voz

Toda a sinalização vai por pedidos JSON comuns; o `/ws` só traz as ofertas
de renegociação (`voice.go`, comentário do topo). **Quem oferta é sempre o
servidor** — a única exceção é a tela, no A4.

### 4.1 Entrar

`POST /api/voice/{channel_id}/join` com `{"publish": ["mic"]}` (`voice.go:77`).
O servidor abre as vagas de envio **antes** de escrever a oferta, porque o
cliente nunca oferta. Resposta (`voice.go:123`, `sfu/sfu.go:129`):

```json
{"session_id": "...", "type": "offer", "sdp": "v=0...",
 "send_mids": {"mic": "0"},
 "tracks": [{"mid": "1", "username": "sol", "display_name": "Sol",
             "kind": "audio", "source": "mic"}],
 "ice_servers": [{"urls": ["stun:..."]},
                 {"urls": ["turn:..."], "username": "...", "credential": "..."}]}
```

- `send_mids` diz **em qual vaga** o cliente manda cada coisa, pelo nome
  (`mic`, `cam`, `screen`, `screen_audio`). O cliente não conta linhas do SDP.
- `tracks` diz de quem é cada vaga que **chega**, por `mid`.
- `ice_servers` vem **sempre** que o servidor tiver STUN/TURN; nunca usar lista
  vazia de propósito (a lição de 18/09, `voice.go:103`).
- **Uma pessoa, um lugar:** entrar derruba qualquer outra sessão de voz da mesma
  conta, em qualquer aparelho (`sfu/sfu.go:790`). O PC e o celular testando
  juntos precisam de **duas contas**.
- Erros: `404 not_found` (sala que não existe ou não é sua — a mesma
  resposta), `409 room_full`, `503 voice_unavailable`. A sessão anterior da
  conta cai **antes** de conferir a lotação: um `409 room_full` pode deixar a
  pessoa fora de todas as salas (`sfu/sfu.go:790`, `:845`).
- O `voice_room` com a sessão nova sai pelo `/ws` **antes** da resposta HTTP
  do `join` (`sfu/sfu.go:868`): o cliente vê a si mesmo na lista antes de saber
  o próprio `session_id`.
- `publish` pode faltar ou vir vazio: vale `["mic"]`. Aceita `mic`, `cam`,
  `screen`, `screen_audio` e os apelidos `audio`→`mic`, `video`→`cam`;
  ignora o resto (`sfu/sfu.go:2135`).

### 4.2 Responder

1. `setRemoteDescription(oferta)`.
2. Para cada `send_mids[fonte] = mid`: achar o transceiver com esse `mid`,
   colocar a faixa local nele e **mudar a direção para `sendrecv`**. Uma vaga
   criada ao ler a oferta de outro nasce `recvonly`, e uma vaga `recvonly` com
   faixa dentro responde "não mando nada" e fica muda, sem erro
   (`media.ts:1561`). `sendrecv` e não `sendonly`, porque o servidor também
   põe o som dos outros na mesma vaga.
3. `createAnswer`, `setLocalDescription`, e
   `POST /api/voice/answer` com `{"session_id", "sdp"}` → `204`
   (`voice.go:133`).

O servidor junta todos os candidatos dele antes de ofertar (um endereço, uma
porta), então **não manda candidatos**; os que importam já estão no SDP
(`sfu/sfu.go:900`).

### 4.3 Candidatos do cliente

Cada candidato local: `POST /api/voice/candidate` com
`{"session_id", "candidate", "sdp_mid", "sdp_mline_index"}` → `204`
(`voice.go:188`). Os que chegam antes da resposta esperam no servidor, **até 64**;
os de sobra são descartados em silêncio, com `204` mesmo assim
(`sfu/sfu.go:979`). O fim da coleta não se manda.

### 4.4 Renegociar

Quando alguém entra, sai ou liga/desliga uma câmera, o servidor manda pelo
`/ws` o evento **`voice_offer`**, só para o aparelho daquela sessão
(`sfu/sfu.go:1984`), com o **mesmo formato** da resposta do `join`, sem
`ice_servers`. O cliente ignora se o `session_id` não for o dele e responde
exatamente como no 4.2, **uma oferta de cada vez, em fila** (`media.ts:1503`).
Se o servidor quiser renegociar enquanto espera uma resposta, ele não manda
duas: marca pendente e manda a mais nova depois que a resposta chega
(`sfu/sfu.go:1973`).

**Recuperar uma oferta perdida ou trocar de rede (03/10/2026).**
`POST /api/voice/restart {"session_id":"..."}` devolve uma oferta completa,
no mesmo formato do `join`, mantendo a sessão e as faixas. A sessão precisa
pertencer ao usuário autenticado; desconhecida ou de outro usuário dá o mesmo
`404 not_found`. Sem servidor de mídia, `503 voice_unavailable`.

Se existe oferta sem resposta, a rota devolve essa oferta exata. O cliente
responde pelo `answer` e pede novamente. Quando a conexão está pronta para
negociar, o servidor oferta com credenciais ICE novas e marca
`"ice_restart":true`. Uma resposta HTTP perdida pode ser buscada novamente,
sem sobrescrever a oferta que está no ar. Os eventos do gateway e os pedidos
de recuperação são aplicados na mesma fila do cliente. A tela continua
podendo ofertar para negociar simulcast; o restart continua sendo ofertado
pelo servidor.

Web e Android mostram reconexão e preservam áudio, câmera, tela e conexão.
Tentam recuperar a rota por até 8 segundos com rede disponível; só depois
usam a entrada na sala como último recurso. Online/offline e mudança de
interface disparam a tentativa; no Android, a troca da rede padrão independe
de ter recebido `onLost`. A volta da rede também antecipa o WebSocket.

### 4.5 Mudo, sem som, sair

- `POST /api/voice/state` com `{"session_id", "muted", "deafened"}` → `204`
  (`voice.go:348`). O servidor só **anuncia**: quem silencia o microfone é o
  cliente, desligando a faixa (`media.ts:3267`: sai som só se nem mudo nem sem
  som). Sem som = também não tocar o som dos outros. Todo mundo na sala recebe
  o `voice_room` novo com `muted`/`deafened`.
  O corpo aceita **só** esses três campos (outro dá `400 bad_request`), e
  não é atualização parcial: um booleano omitido vale `false` (`voice.go:349`).
- `POST /api/voice/leave` com `{"session_id"}` → `204`; repetir dá `404`.
- **Uma conexão desconectada tem 20 segundos para recuperar a rota**, contados
  do primeiro evento de perda no servidor. Pedidos repetidos não prorrogam
  essa janela; sem recuperação, a sessão sai da sala. Conectar cancela a
  perda, e um timer antigo não pode remover a sessão recuperada nem uma
  queda posterior. O ICE ainda detecta silêncio em cerca de 5 segundos;
  isso significa começar a recuperação, não remover a pessoa imediatamente.
  Uma sessão que nunca conectou tem o prazo separado de 30 segundos.
- **A mesma conta em outro aparelho** também tira a sessão da lista, e a
  lista sozinha não diz qual dos dois casos foi. O Android olha a lista
  inteira um segundo depois: se a própria conta está em alguma sala com outra
  sessão, foi outro aparelho e ele sai; se não está em lugar nenhum, caiu e ele
  entra de novo.
- **Quem está falando** não passa pelo servidor: cada cliente mede o volume do
  que recebe, por `mid`, e acende quem passou de um piso, mantendo aceso por
  320 ms depois do último som (`media.ts:308`). O próprio anel só acende se
  não estiver mudo.

### 4.6 Câmera, tela, assistir, camada — A4

Rotas (`api.go:262`–`276`). Todas respondem `204` e o que muda de verdade
chega depois, numa `voice_offer` pelo `/ws` — nunca no corpo da resposta.

**Saber quem mostra o quê.** `Peer.publishing` (§3) diz `screen`, `cam`,
`screen_audio`, montado pelo que o servidor **recebe**, não pelo que se
pediu (`sfu.go:1910`). Não é preciso assistir para saber que alguém
compartilha.

**A câmera dos outros chega sozinha.** O servidor assina toda publicação
nova para quem está na sala (`sfu.go:1946`); a oferta traz a faixa com
`source:"cam"`. Não se manda `watch` para câmera.

**A tela, só pedindo.** `POST /api/voice/watch {"session_id","username",
"watching":true}` assina `screen` **e** `screen_audio` daquela pessoa e
renegocia (`sfu.go:1101`); se ela ainda não compartilha, a escolha fica
guardada para quando começar. `"watching":false` tira as duas. Voz e câmera
não entram nessa escolha. A imagem de outra pessoa chega sempre numa vaga
**própria**, nunca na vaga que este aparelho abriu para mandar a sua câmera
(desde 25/09/2026: os codecs daquela vaga são os da câmera, e um celular sem
codificador H.264 não lia a tela ali). A voz, sim, pode chegar na vaga do
microfone.

**A camada.** `POST /api/voice/layer {"session_id","username","layer"}`,
`alta`/`media`/`baixa`, ou `""` para tirar o teto. É um **teto** só para
quem pediu: o servidor ainda desce por perda, pede um quadro completo ao
trocar e avisa quem transmite (`simulcast.go:644`). Não renegocia. O web e o
Android escolhem pela largura desenhada em pixels físicos — **≥1200 alta,
≥600 media, abaixo baixa**; app fora da tela pede baixa — e pela capacidade
do decodificador, vencendo a menor. Depois de cada oferta aplicada, o pedido
é refeito (a vaga pode ter nascido de novo); se o HTTP falhar, esquece-se o
pedido para tentar na próxima medida.

**Ligar a própria câmera.** Primeiro a faixa (1280×720 a 30, ideal), depois
`POST /api/voice/publish {"session_id","sources":["cam"]}`. A oferta que
responde traz `send_mids.cam`: coloca-se a faixa naquela vaga e a direção
passa a `sendrecv` — **a vaga criada por oferta remota nasce `recvonly`**, e
pôr a faixa não muda isso. Teto de 900 kbit/s, degradação `balanced`. O
servidor prefere VP8, depois H.264, depois VP9 (`codecs.go:199`).

**Desligar.** `POST /api/voice/unpublish {"session_id","sources":["cam"]}`;
o servidor para a vaga e renegocia. Religar abre **vaga nova** — não guarde
o `mid` antigo. Há um limite de **32 vagas por sessão** (`sfu.go:153`);
passado dele, a saída é sair e entrar de novo.

**`voice_watchers`** vai só para quem transmite tela:
`{"screen":bool,"layers":["alta",…]|null}` — quais camadas manter ligadas.
Quem transmite desliga as outras, esperando 5 s antes de desligar (religar
custa um quadro completo). O Android não transmite tela, então ignora.

**Volume por pessoa** é do cliente, não do servidor: chave `username` para
a voz e `username:tela` para o som da tela, 0 a 2 (1 = 100 %), guardado por
conta. "Sem som" zera tudo por cima do volume.

---

## 5. O chat

Conferido em 25/09/2026. Nesta seção, `api/chat.go`, `api/files.go` e
`store/chat.go` ficam em `internal/harmony/`; os arquivos `.tsx` ficam em
`web/harmony/src/`. As rotas estão em `api/api.go:235`–`252`.

### 5.1 Canais de texto e conversas privadas

Os kinds reais são `text`, `dm` e `voice` (`store/chat.go:55`). O formato de
canal do §3 vale inteiro: `id`, `kind`, `name`, `topic`, `muted`, `unread`,
`mentions`, `last_message_id`, `last_read_id`; só DM tem `peer`, com
`username` e `display_name` da outra pessoa (`api/chat.go:35`).

No web, os canais de texto conservam a ordem recebida; DMs são ordenadas
pelo maior `last_message_id`, com as vazias (`0`) primeiro
(`App.tsx:1329`–`1343`). O Android usa a mesma ordem. Os IDs das mensagens
crescem para o servidor inteiro, não são números consecutivos por canal
(`App.tsx:1336`).

`POST /api/dms {"username":"sol"}` abre ou reabre a mesma conversa,
respondendo `200` com o canal visto por quem pediu. Os dois lados recebem
`channel`, cada um com o seu `peer`. Usuário inexistente, desativado ou a
própria conta: `404 not_found` (`api/chat.go:361`–`405`).

`GET /api/members` responde `{"members":[{"username","display_name",
"is_admin","nickname"}]}`. Contas desativadas ficam fora; `nickname` só
vem quando existe e é o apelido privado escolhido por quem pediu
(`api/chat.go:442`–`469`).

### 5.2 Mensagens, histórico e reconexão

`GET /api/channels/{id}/messages?before=42&limit=50` responde
`{"messages":[Message],"has_more":true}`. `before` é exclusivo: pede IDs
menores que ele; ausente ou não positivo pede a página mais recente.
O padrão é 50; no store, limite fora de 1–200 volta a 50. Embora a consulta
interna seja decrescente, **o servidor devolve em ordem crescente de ID**
(`store/chat.go:612`–`643`). O Android também ordena ao ler o JSON.

`has_more` significa apenas que a página veio cheia; pode haver uma última
busca vazia. Há uma inconsistência para `limit > 200`: o store usa 50, mas
o handler compara o tamanho com o limite pedido, então pode devolver
`false` prematuramente (`api/chat.go:474`–`491`, `store/chat.go:619`).

O formato real da mensagem (`api/chat.go:81`–`134`):

```json
{"id":42,"channel_id":"c","author":{"username":"sol","display_name":"Sol"},
 "body":"oi @tales","created_at":"2026-09-25T12:00:00Z","deleted":false,
 "attachments":[],"mentions":["tales"],"reactions":[{"emoji":"👍","users":["tales"]}],
 "edited_at":"2026-09-25T12:01:00Z",
 "reply_to":{"id":40,"author":{"username":"tales","display_name":"Tales"},
             "body":"pergunta","deleted":false}}
```

`edited_at` e `reply_to` só aparecem quando existem. `kind` só aparece
quando não vazio (há `call`, `store/chat.go:1073`); o Android lê ausência
como `""`. Datas são RFC3339; no Android viram epoch em milissegundos usando
`OffsetDateTime`, disponível no minSdk 26 (`android/app/build.gradle.kts:15`).
Apagada conserva o ID, vem com `deleted:true`, corpo vazio, `deleted_at` e
`deleted_by`; o Android usa o booleano para desenhar a remoção. A referência
da resposta traz o texto atual da mensagem citada e informa se foi apagada
(`api/chat.go:113`–`134`, `store/chat.go:1252`–`1278`).

Para carregar para cima, o web passa o menor ID atual em `before`, junta
antes e ignora IDs já presentes, preservando as versões que chegaram ao
vivo (`App.tsx:1028`–`1043`; rolagem em `Messages.tsx:584`). `message`
insere só se o ID ainda não existe; `message_changed` substitui só um ID
que a página já tem (`App.tsx:593`–`598`, `:656`–`668`). A resposta do POST
e o evento podem chegar em qualquer ordem: `Timeline.withSent` insere ou
substitui pelo ID, sem repetir a linha.

**Diferença deliberada no A5:** o web recarrega substituindo a página e
reaplicando os eventos recebidos durante a busca, em ordem
(`App.tsx:921`–`942`). A `Timeline` Android segue o contrato pedido para A5:
se o menor ID da página nova é menor ou igual ao maior que já tinha, junta
e a página vence nos IDs repetidos; se houver buraco, substitui tudo.
Quando conserva mensagens mais antigas, conserva também o `hasMore`
daquela borda; quando a página determina a borda mais antiga, usa o dela.
Página vazia substitui por vazio. `withOlder` conserva a versão já em memória.

A `Timeline` não recebe ID de canal no construtor: **enquanto vazia, quem
a usa precisa encaminhar só os eventos do canal dela**. Depois da primeira
mensagem, rejeita eventos dos outros canais. Quem coordena as buscas deve
também cuidar dos eventos recebidos durante HTTP: o método não tem o buffer
`arrived` do web, e uma página pode vencer uma edição recebida nesse intervalo.

### 5.3 Mandar, responder, editar, apagar e reagir

Todas exigem sessão. Sem acesso ao canal ou mensagem, a resposta é
`404 not_found`, igual à de ID inexistente (`api/chat.go:235`,
`store/chat.go:615`, `:980`, `:821`).

| Pedido | Corpo | Resposta e regra |
|---|---|---|
| `POST /api/channels/{id}/messages` | `{"body":"texto","attachments":["id"],"reply_to":40}` | `201 Message`; publica `message` antes da resposta (`api/chat.go:494`–`540`) |
| `PATCH /api/messages/{id}` | `{"body":"corrigido"}` | `200 Message`; só o autor, nunca o dono em nome de outra pessoa; mensagem não apagada (`store/chat.go:1213`–`1249`) |
| `DELETE /api/messages/{id}` | sem corpo | `200 Message` apagada; autor ou dono, mas só em canal do qual participa (`store/chat.go:1252`–`1278`) |
| `POST /api/messages/{id}/reactions` | `{"emoji":"👍","on":true}` | `200 Message`; membro do canal liga/desliga a própria reação; repetir é idempotente (`store/chat.go:916`–`955`) |

Edição, remoção e reação publicam a mensagem inteira em `message_changed`
(`api/chat.go:543`–`614`). Reagir não gera notificação. Mensagem apagada
recusa reação; emoji inválido ou acima do limite por pessoa dá
`400 bad_reaction` (`store/chat.go:916`–`941`, `api/chat.go:243`).

O corpo é aparado nas pontas e aceita até **4.000 caracteres Unicode**.
Envio vazio só vale com anexo; edição vazia não vale, mesmo com anexo:
`400 bad_message` (`store/chat.go:984`–`992`, `:1216`). No máximo quatro
anexos; devem ser uploads próprios, no mesmo canal e ainda não usados:
`400 bad_attachments` (`store/chat.go:991`, `:1048`, `api/chat.go:241`).
`reply_to` ausente/zero não responde; referência positiva deve existir no
mesmo canal, senão `404 not_found` (`store/chat.go:1017`). O JSON inteiro
tem teto de 64 KiB (`api/api.go:45`–`48`).

Envio tem limite de **20 mensagens por minuto por pessoa**; exceder dá
`429 message_limit`, conferido novamente na transação que grava
(`api/chat.go:26`–`30`, `:494`–`527`). Limite equivalente para editar ou
apagar: **não encontrado** nos handlers (`api/chat.go:543`, `:564`).

### 5.4 Anexos

Primeiro `POST /api/channels/{id}/uploads`, **multipart/form-data**, campo
**`file`**. Responde `201 Attachment` diretamente, sem envelope
(`api/files.go:103`, `:144`, `:219`). Depois manda-se o ID no POST da
mensagem (§5.3). O tipo vem dos bytes, não do nome nem do MIME informado
pelo cliente (`api/files.go:165`).

O anexo tem `id`, `name`, `mime`, `size` em bytes, `width`, `height`,
`url:"/api/files/{id}"` e, se o arquivo sumiu do disco, `missing:true`.
Sem `missing` vale falso; `mime` começando em `image/` é imagem
(`api/chat.go:53`–`78`, `api/files.go:80`). Nome é limpo e limitado a 80
caracteres; nunca vira caminho no disco (`api/files.go:225`–`245`).

Limites: **32 MiB por arquivo, 60 uploads e 512 MiB por pessoa em 24 horas**
(`api/files.go:32`–`42`). Arquivo vazio/campo ausente: `400 bad_request`;
grande demais ou multipart que não pôde ser lido: `413 too_large`; tipo
recusado: `415 bad_file_type`; cota: `429 upload_limit` ou
`429 upload_bytes_limit` (`api/files.go:103`–`199`).

Tipos aceitos, decididos pelos bytes: PNG, JPEG, GIF, WebP, PDF, texto,
MP3 (`audio/mpeg`), WAV (`audio/wave`), AIFF, Ogg (`application/ogg`),
MP4 (`video/mp4`, inclusive certos áudios), WebM e ZIP
(`api/files.go:54`–`75`).

`GET /api/files/{id}` devolve **bytes com o mesmo cookie**, somente para
membro do canal; desconhecido, sem acesso, apagado ou ausente no disco dá
`404 not_found` (`api/files.go:251`–`274`, `store/chat.go:1405`). Envia MIME
armazenado, `nosniff`, CSP restritiva e cache privado; imagem/vídeo/áudio
usam disposição `inline`, os demais `attachment` (`api/files.go:276`–`291`).
O Android recusa endereço fora de `/api/files/` e caminhos que, normalizados,
não sejam um ID de arquivo. Upload usa o `MultipartBody` já disponível.

### 5.5 Lido e contadores

`POST /api/channels/{id}/read {"last_read_id":42}` responde `200 {"ok":true}`
e publica `read {"channel_id":"c","last_read_id":42}` **só para os aparelhos
da própria conta** (`api/chat.go:619`–`638`). O store grava o ID recebido
diretamente, sem exigir avanço nem comparar ao último do canal
(`store/chat.go:597`–`607`).

Regras que o Android porta do web:

- Mensagem alheia em canal que não está aberto e não está silenciado soma
  1 em `unread`. Mensagem própria não soma e **não avança `lastReadId`**
  (`App.tsx:628`–`654`). O Android faz `lastMessageId` subir sem retroceder;
  o web atribui o ID recebido diretamente (`App.tsx:645`).
- `mentions` soma se o username está em `msg.mentions`, fora do canal
  aberto e sem silêncio; DM sozinha não soma menção. O web não exclui aqui
  a menção explícita a si mesmo (`App.tsx:647`–`652`); o Android preserva isso.
- `read` **sempre zera** `unread` e `mentions`, mesmo com ID anterior à última
  mensagem (`App.tsx:772`–`781`). `RoomsState.markRead` aplica a mesma mudança
  localmente; deve ser chamado após sucesso HTTP. O web só confirma a leitura
  local depois do servidor aceitar (`App.tsx:995`–`1023`).
- `channel` atualiza os campos mas conserva os dois contadores que o cliente
  já tinha (`App.tsx:685`–`697`). Canal novo entra com os valores recebidos.
- O `open` passado ao Android é nulo se o app está em segundo plano. O web,
  no cálculo dos contadores, usa apenas o canal ativo, sem conferir foco ou
  visibilidade nessa condição (`App.tsx:639`–`650`).

### 5.6 Digitação e menções

`POST /api/channels/{id}/typing {}` responde `204`. Publica `typing`
com `channel_id`, `username`, `display_name` só para os outros membros,
sem gravar histórico. Canal sem acesso responde `404 not_found` — o comentário
que diz “204 qualquer que seja o resultado” não corresponde ao handler
(`api/chat.go:174`–`203`).

O web avisa ao digitar texto não vazio, quando passaram **mais de 3.000 ms**
desde a tentativa anterior (`Composer.tsx:144`–`152`). Recebido o evento,
o nome dura **5.000 ms**, não os quatro segundos citados no comentário do
servidor; nome vazio usa username (`App.tsx:676`–`683`). A limpeza visual
ocorre a cada segundo (`App.tsx:1131`–`1144`). Limpeza imediata ao receber
mensagem: **não encontrada** no handler do web (`App.tsx:574`–`655`).
No Android, conforme o contrato A5, o chamador usa `Typing.stopped` nessa chegada.
`TypingSender.shouldSend` já reserva a tentativa; `reset` libera a próxima
tecla após enviar mensagem, sem precisar de um método `sent` separado.

A menção é texto comum **`@username`**, seguido de espaço ao escolher a
sugestão (`Composer.tsx:160`–`165`). O servidor extrai com
`(?i)@([a-z0-9][a-z0-9._-]{1,30}[a-z0-9])`, normaliza, elimina repetições
e grava só nomes de membros daquele canal (`store/chat.go:172`,
`:1178`–`1208`). **`@todos`** expande para os membros, exceto o autor;
`mentions` devolve os usernames resultantes, não a palavra `todos`
(`store/chat.go:1173`–`1196`, `:710`–`723`). Tratamento especial de
`@everyone` ou `@here`: **não encontrado**. Editar recalcula as menções;
apagar remove-as (`store/chat.go:1240`, `:1275`).


## 6. Avisos com o app fechado

O Android recebe pelo FCM somente o tipo do aviso, identificadores e horário.
Texto de mensagem, nome de pessoa, título e nome de canal não passam pelo
Google. O app busca as informações no harmony com sua própria sessão. O Web
Push continua com POST vazio e consulta a `/api/notifications/pending`.

### Registro do aparelho

Ambas as rotas exigem sessão (401 sem ela):

- `POST /api/push/fcm`, corpo `{"token":"..."}`: registra na pessoa e sessão
  atuais. Resposta 200: `{"enabled":true,"registered":true,"devices":N}`.
- `POST /api/push/fcm/remove`, mesmo corpo: remove somente o token da pessoa
  autenticada. Resposta 200: `{"enabled":true,"registered":false,"devices":N}`.
  Token alheio responde exatamente como inexistente. Remover funciona mesmo
  com FCM desligado; nesse caso `enabled` é `false`.

`devices` conta tokens FCM de sessões válidas, não revogadas nem vencidas.
Registrar com FCM desligado responde 503 `push_disabled`. Token vazio, maior
que 4096 bytes ou fora de `[A-Za-z0-9_:.-]` responde 400 `bad_token` no registro.
A remoção é idempotente e não exige esse formato. JSON malformado segue o
tratamento comum das demais rotas.

O token da instalação Android pode sobreviver à troca de conta. Registrar
novamente transfere o token para a pessoa e a sessão atuais: o app tem uma
sessão por vez. Há até oito tokens por pessoa; saem os mais antigos, com
empates resolvidos pela ordem do token, como no Web Push. Logout e vencimento
impedem novos envios. O token é segredo e nunca vai ao log.

### Dados enviados

Corpo: `{"message":{"token":"...","data":{...},"android":{...}}}`.
Não existe campo `notification`. Todos os valores de `data` são strings:

| Ocorrência | Campos exatos de `data` | Validade |
|---|---|---|
| DM | `k:"dm"`, `c:<canal>`, `m:<mensagem decimal>`, `at:<milissegundos decimais>` | 12 horas |
| Menção | `k:"mention"`, `c:<canal>`, `m:<mensagem decimal>`, `at:<milissegundos decimais>` | 12 horas |
| Sala de voz | `k:"ring"`, `c:<canal>`, `at:<milissegundos decimais>` | 60 segundos |
| Chamada direta | `k:"call"`, `call:<id da chamada>`, `at:<milissegundos decimais>` | 60 segundos |

`android.priority` é `HIGH`; `android.ttl` é `43200s` ou `60s`;
`android.collapse_key` repete `k`. Na chamada direta, `c` fica ausente.
Os identificadores de canal e chamada são strings opacas, não necessariamente
números decimais; somente `m` e `at` são decimais. `at` é o instante do aviso.

A chamada direta continua sendo `ring` internamente, no Web Push e nos avisos
pendentes. O intervalo mínimo é compartilhado por pessoa e tipo interno entre
FCM e Web Push: 15 segundos para DM/menção e 10 para chamadas. Sem aparelhos,
não se consome o intervalo. Conexão visível impede envio; silenciamento e
acesso ao canal continuam valendo. Cada aparelho recebe seu prazo de 10
segundos, em paralelo. O Web Push conserva os 45 segundos de validade para
chamadas; os 60 segundos acima são do Android.

`GET /api/notifications/pending` inclui `message_id` (número JSON) em DM e
menção. `CallID` não aparece nesse JSON; a URL existente continua apontando
para a chamada. Os avisos continuam filtrados pela pessoa e pelo que está por ler.

### Configuração

`HARMONY_FCM_CREDENTIALS` é o JSON da conta de serviço em base64 padrão, com
ou sem padding. Campos obrigatórios: `type:"service_account"`, `project_id`,
`private_key` RSA PEM PKCS#8, `client_email`, `token_uri`. Ausente: FCM
desligado, uma linha no log, servidor sobe normalmente. Presente e inválida:
encerra com código 2. Válida: log `fcm push enabled` com `project_id`, nunca
a chave. FCM independe de VAPID; qualquer um ativa os avisos.

OAuth usa JWT RS256 e reaproveita o acesso até cinco minutos antes de vencer
(para validades menores, margem de 10%). 401 invalida o acesso e permite uma
única nova tentativa. 404, `UNREGISTERED` e 403 com `SENDER_ID_MISMATCH`
removem o token do aparelho. Outros erros registram o status, sem corpo remoto.

## 7. A chamada direta

A chamada é entre duas pessoas, com mídia P2P e TURN quando necessário.
O servidor só encaminha a sinalização; não interpreta SDP nem candidatos.
As seis rotas exigem a mesma sessão da §1 (`internal/harmony/api/call.go`):

| Pedido | Corpo | Resposta |
|---|---|---|
| `POST /api/calls` | `{"username":"bob"}` | 201: `id`, `from`, `to`, `ice_servers`, `polite:false` |
| `GET /api/calls/{id}` | — | 200: os mesmos campos, `ice_servers:null`, `answered` e `polite` conforme a pessoa |
| `POST /api/calls/{id}/accept` | `{}` | 200: os campos do início, com ICE e `polite:true` |
| `POST /api/calls/{id}/hangup` | `{"reason":"hangup"}` | 200: `{"ended":true}`; também aceita `rejected` ou `missed` |
| `POST /api/calls/{id}/signal` | `{"kind":"offer","payload":{"type":"offer","sdp":"..."}}` | 204; `kind` aceita `offer`, `answer` ou `ice` |
| `POST /api/calls/{id}/state` | `{"mic":true,"cam":false,"screen":false}` | 204; informa o que está ligado |

`from`, `to` e `peer` são `{username,display_name}`. ICE segue o formato
WebRTC: `urls` (o servidor manda lista), mais `username` e `credential`
temporários para TURN. O GET não fornece credenciais. IDs são segmentos de
caminho: o cliente deve escapá-los, sem concatenar texto cru na URL.

Chamada inexistente, expirada ou de outras pessoas dá `404 not_found`.
Só quem recebe pode atender. Iniciar com uma das pessoas ocupada dá
`409 busy`; insistir no toque dá `429 ring_limit`; outro aparelho que já
atendeu dá `409 taken`. Sinal ou estado antes de atender dá
`409 not_answered`; tipo de sinal desconhecido dá `400 bad_signal`.

### Eventos e vários aparelhos

O `/ws` entrega `{type,data}`; escritas continuam nos POSTs acima:

| `type` | `data` | Destino |
|---|---|---|
| `call` | `{id,from}` | Todos os aparelhos de quem recebe |
| `call_accepted` | `{id,peer}` | Sessão de quem ligou |
| `call_taken` | `{id,by}` | Todos os aparelhos de quem recebe; `by` é a sessão que atendeu |
| `call_ended` | `{id,reason}` | Todos os aparelhos das duas pessoas |
| `call_signal` | `{id,kind,payload}` | Sessão do outro lado da chamada atendida |
| `call_state` | `{id,mic,cam,screen}` | Sessão do outro lado da chamada atendida |

O primeiro aparelho a atender fica com a chamada; os demais param de tocar
ao receber `call_taken`. O vencedor ignora o próprio `by`. Ao abrir pelo
aviso, consultar o GET: `answered:true` significa que alguém já atendeu.
Firebase leva apenas `k:"call"`, `call:<id>` e `at` (§6), sem nome nem SDP;
os detalhes vêm do GET autenticado. Sem resposta, a chamada expira após
**45 segundos** (`ringFor`); a limpeza ocorre nas operações e na varredura
periódica. Essa expiração não publica `call_ended`: o cliente encerra o toque
pelo prazo. Os 60 segundos do FCM não prolongam a chamada.

### Negociação e formato dos sinais

Quem liga é **impolido** (`polite:false`); quem atende é **polido**
(`polite:true`). Usar perfect negotiation (`PROTOCOL.md` §5–6): em ofertas
simultâneas, o polido desfaz sua oferta e aceita a remota; o impolido ignora
a oferta em colisão e os candidatos associados. Só quem liga cria as
quatro vagas `sendrecv`, nesta ordem fixa: **microfone, câmera, tela, som
da tela**. Quem atende recebe as vagas pela oferta. Trocas usam
`replaceTrack`, preservando a conexão e a ordem (`web/harmony/src/direct.ts`).

Oferta e resposta levam `{type,sdp}`, como `pc.localDescription` serializado
por `JSON.stringify`. ICE leva `{candidate,sdpMid,sdpMLineIndex}`, em camelCase,
como `RTCIceCandidate.toJSON()`; pode trazer `usernameFragment` adicional.
É o objeto recebido por `pc.addIceCandidate(payload)`, sem envelope extra.
No Android, `sdpMid` ausente/nulo fica nulo; índice ausente/nulo vira 0.

Usar trickle ICE. Enquanto toca, quem liga guarda oferta e candidatos numa
fila de saída (**outbox**), pois ainda não há aparelho destinatário e o
servidor recusa sinais com `409 not_answered`. Ao receber `call_accepted`,
esvazia a fila em ordem, com a oferta antes dos candidatos. A queda do
WebSocket não encerra a mídia já negociada.

## Foto de perfil (web e Android)

A foto pertence à conta, não ao aparelho. Os três endpoints exigem sessão;
sem ela, respondem `401 {"error":"unauthenticated"}`. Escritas passam pela
mesma proteção de origem das outras operações da API.

`PUT /api/me/avatar` recebe **os bytes da imagem diretamente no corpo**,
sem JSON ou multipart. O servidor reconhece o tipo pelos bytes e ignora o
`Content-Type` enviado. Aceita JPEG, PNG e GIF; WebP e qualquer outro tipo
respondem `415 {"error":"avatar_type"}`. O limite é **5 MiB** (5.242.880
bytes); acima dele, `413 {"error":"avatar_too_large"}`.

Antes de decodificar, verifica as dimensões: largura e altura até 8000 pixels
e área até 40 milhões de pixels. Acima disso, responde
`413 {"error":"avatar_dimensions"}`. Imagem inválida ou incompleta responde
`400 {"error":"bad_image"}`. Recorta o quadrado central, redimensiona para
256×256 com interpolação bilinear, aplica fundo branco à transparência e
grava JPEG com qualidade 85. GIF usa o primeiro quadro. **O original não é
guardado**: somente o JPEG tratado e sua versão ficam no SQLite, junto do
backup já existente.

A resposta é `200 {"avatar":"<versão>"}`. A versão é uma string curta e
opaca (atualmente 12 caracteres hexadecimais aleatórios), nova a cada envio,
mesmo se a imagem for igual à anterior. Não interpretar nem ordenar versões.
Cada conta pode fazer dez mudanças por hora, contando envios e remoções;
acima disso, `429 {"error":"avatar_limit"}`. O limite permanece após
reiniciar o servidor. Envios inválidos não consomem mudanças.

`DELETE /api/me/avatar` remove o JPEG e limpa a versão; responde `204`, sem
corpo. PUT e DELETE sempre operam na conta da sessão: não há parâmetro para
escolher outra pessoa. Um pedido de remoção sem foto também conta no limite.

`GET /api/people/{username}/avatar` entrega o JPEG da pessoa, para qualquer
membro autenticado. Sem foto ou pessoa inexistente, responde `404
{"error":"not_found"}`. Cabeçalhos da imagem:

```http
Content-Type: image/jpeg
X-Content-Type-Options: nosniff
Cache-Control: private, max-age=31536000, immutable
```

O cliente sempre usa `/api/people/<username>/avatar?v=<versão>`, escapando
os componentes da URL. A versão identifica o cache; o servidor entrega a
foto atual. Não pedir imagem quando a versão estiver vazia. Se a imagem
falhar, mostrar as iniciais.

O campo `avatar` acompanha os dados de pessoas: conta (`me`, login e
cadastro), membros, interlocutor de DM, autor de mensagem e de resposta,
digitação, chamadas diretas, convite para sala, participantes de voz e
identificação de tracks nas ofertas. Sem foto, a string é vazia. O mapa
`presence` continua tendo estados como valores; a resposta de presença
também traz `avatars`, um mapa de username para versão das pessoas online.
O evento `presence` inclui `avatar` junto de username e estado.

Após gravar ou remover a foto, o gateway envia a todos os membros conectados:

```json
{"type":"profile","data":{"username":"tales","avatar":"<versão>"}}
```

Na remoção, `avatar` é `""`. Atualizar essa pessoa nas listas, DMs, salas e
perfil abertos, inclusive na própria conta, sem recarregar. Ao reconectar,
buscar novamente membros e salas, como já se faz para presença.
