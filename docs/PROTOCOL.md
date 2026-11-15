# Protocolo de signaling — contrato entre servidor e clientes

Este arquivo é a **fonte única da verdade** do protocolo. As structs Go em
`internal/protocol` e os types TypeScript do front são escritos a partir dele.
O app original foi aposentado em 03/11/2026; seu código e o teste de contrato
estão na etiqueta `app-1a1-final`. O harmony continua seguindo este contrato
na sinalização das chamadas diretas; sua integração, autenticação e transporte
estão descritos em `docs/PROTOCOL-HARMONY.md`.

As seções abaixo descrevem o transporte e os endpoints do app original:
mensagens JSON, uma por frame de texto, sobre WSS em
`wss://call.taleshernandes.com/ws` (desativado). Toda mensagem tem `{ "t": <tipo>, ... }`.

## 1. Autenticação — `POST /auth`

Antes de abrir o WebSocket o cliente troca a passphrase compartilhada por um
token:

```
POST /auth
{ "passphrase": "..." }

200 → { "token": "<token>" }
401 → { "error": "unauthorized" }        passphrase errada
429 → { "error": "rate_limited" }        mais de 5 tentativas/min por IP
```

Token: `base64url(exp) + "." + base64url(HMAC-SHA256(secret, exp))`, onde
`exp` é o instante de expiração em segundos Unix (12 h após a emissão).
Não é JWT. O cliente trata o token como opaco — com uma exceção: ele decodifica
`exp` para saber se o token guardado ainda vale antes de tentar usá-lo. A
assinatura só o servidor verifica.

## 1.1. Campainha com o app fechado — `/push/*`

Quatro endpoints, todos JSON, todos atrás do mesmo `reverse_proxy /push/*` no
Caddy. O contrato completo do mecanismo está na §11.

```
GET /push/key

200 → { "enabled": true, "key": "<chave pública VAPID base64url>" }
200 → { "enabled": false }               servidor sem VAPID configurado
```

Sem token: uma chave pública é pública por definição, e a página precisa dela
antes de conseguir se inscrever.

```
POST /push/subscribe          Authorization: Bearer <token>
{ "endpoint": "https://...", "keys": { "p256dh": "...", "auth": "..." } }

200 → { "subscribed": true, "count": <dispositivos registrados> }
400 → { "error": "bad_endpoint" }        endpoint que não é https
401 → { "error": "unauthorized" }
503 → { "error": "push_disabled" }       servidor sem VAPID configurado
```

`p256dh` e `auth` são guardados e não usados: nada aqui cifra payload (§11).
Ficam no lugar para que implementar a RFC 8291 depois seja mudança só de
servidor, e não algo que exija todo mundo se inscrever de novo.

O corpo aceita um campo `replaces` com o endpoint que está sendo rotacionado.
Ele é a única forma de se inscrever **sem token**: o evento
`pushsubscriptionchange` acontece no service worker, onde não existe página e
portanto não existe token, e conhecer um endpoint vivo — uma URL de
capacidade que mais ninguém tem — é o que faz as vezes de credencial. Uma
requisição assim só consegue **trocar** um registro por outro, nunca somar um
dispositivo novo.

```
POST /push/unsubscribe        Authorization: Bearer <token>
{ "endpoint": "https://..." }

200 → { "subscribed": false, "count": <n> }
```

```
GET /push/status?endpoint=<url>   Authorization: Bearer <token>

200 → { "enabled": bool, "subscribed": bool, "count": <n> }
```

`subscribed` é sobre **este** dispositivo; `count` é sobre o servidor. Este
endpoint existe por um motivo específico: uma campainha desligada é
indistinguível de uma ligada até alguém não ser atendido. O cliente mostra
esse estado, para que "o servidor não tem o meu registro" seja algo que se lê,
e não algo que se descobre.

## 2. Cliente → servidor

```
{ t: "hello", room: "main", token: "<token>", client: "web|android", session: "<id>" }
{ t: "ring" }                                  // convidar / tocar campainha
{ t: "accept" } | { t: "reject" } | { t: "hangup" }
{ t: "offer",  sdp: "..." }
{ t: "answer", sdp: "..." }
{ t: "ice",    candidate: {...} | null }       // null = fim dos candidatos
{ t: "state",  mic: bool, cam: bool, screen: bool }
{ t: "ping" }
```

`session` é **opcional** e opaco ao servidor: no máximo 128 bytes, comparado
só por igualdade. Ele identifica uma *instância* de cliente através de
reconexões do WebSocket — é o que permite reivindicar o próprio slot depois de
uma troca de rede (§8). Um cliente que não mande `session` se comporta
exatamente como antes de ele existir; dois clientes sem `session` são sempre
dois participantes, nunca um reconectando, porque não há no que casá-los.

O cliente gera um por carga de página, na memória. Não vai para localStorage:
um reload é genuinamente uma instância nova e deve pegar um slot novo.

## 3. Servidor → cliente

```
{ t: "welcome", self: "<id>", peer: "<id>|null", polite: bool, iceServers: [...] }
{ t: "push-sent" }                                  // ring sem peer, mas com push (§11)
{ t: "peer-joined", id }  |  { t: "peer-left", id }
{ t: "ring" } | { t: "accept" } | { t: "reject" } | { t: "hangup" }
{ t: "offer" } | { t: "answer" } | { t: "ice" }     // repassados 1:1
{ t: "state", ... }
{ t: "error", code, message }
{ t: "pong" }
```

`iceServers` segue o formato de `RTCIceServer` (`{ urls, username?, credential? }`).
O cliente repassa a lista ao `RTCPeerConnection` como veio e não tem nada
hardcoded. Desde o Sprint 4 ela traz duas entradas:

1. STUN (`STUN_URLS`), sem credenciais, igual pra todo mundo.
2. TURN (`TURN_URLS`), com credencial **efêmera, gerada por conexão** no
   esquema REST do coturn (`use-auth-secret`):
   `username = "<exp>:<self>"` (expiração em segundos Unix, 2 h após o
   `welcome`, seguida do id do cliente) e
   `credential = base64(HMAC-SHA1(TURN_SECRET, username))`.
   Cada `welcome` gera um par novo; nada é reaproveitado entre conexões. O
   cliente trata os dois campos como opacos.

Sem `TURN_URLS` no servidor (dev e e2e) só a entrada STUN é enviada.

## 4. Handshake

1. O cliente abre o WebSocket e envia `hello` como **primeira** mensagem, em
   até 5 s. Qualquer outra coisa antes, ou silêncio, resulta em `error` + close.
2. O servidor valida token e sala. `room` só pode ser `"main"`.
3. Se o `session` do `hello` for igual ao de um cliente já sentado, é a mesma
   instância voltando: ela **reivindica o próprio slot** (§8). Isso é checado
   *antes* da lotação.
4. Sala com 2 clientes de sessões diferentes → `error { code: "room_full" }` +
   close. **Sem fila, sem lobby.** A conexão é rejeitada e ponto.
5. Sucesso → `welcome`. Se já havia alguém, esse alguém recebe `peer-joined`
   — **exceto** numa reivindicação, que não emite evento nenhum ao peer.

## 5. Polite / impolite — perfect negotiation

O servidor decide os papéis; o cliente não adivinha.

- `welcome.polite === false` → você entrou em sala vazia. Você é o **impolite**.
- `welcome.polite === true` → já havia um peer. Você é o **polite**.
- Receber `peer-joined` implica que você estava antes: você é o **impolite**
  daquela dupla, independentemente do que era numa dupla anterior.

Regra geral: **o primeiro a entrar é impolite.** Como a `RTCPeerConnection` é
criada por chamada, o papel é fixado no momento em que a dupla se forma e não
muda até um dos dois sair.

Uma reivindicação de slot (§8) **preserva o papel**, em vez de recalculá-lo.
Recalcular seria um bug silencioso: o cliente que era impolite voltaria com
`polite: true` (porque há um peer presente), enquanto o peer — que não é
notificado de nada — continuaria se achando impolite. Dois polites significa
que ninguém cede no glare, que é exatamente o travamento que o perfect
negotiation existe para evitar.

O cliente **deve** implementar o padrão do W3C (perfect negotiation) com
`onnegotiationneeded` e as flags `makingOffer`, `ignoreOffer` e
`isSettingRemoteAnswerPending`. O polite faz rollback quando recebe offer em
`have-local-offer`; o impolite ignora a offer e descarta os `ice` que chegarem
enquanto `ignoreOffer` estiver ativo. Sem isso, glare trava a negociação.

Trickle ICE é obrigatório: cada `onicecandidate` vira um `ice` na hora.
Nunca esperar `iceGatheringState === 'complete'` para enviar a offer.

## 6. Relay

`ring`, `accept`, `reject`, `hangup`, `offer`, `answer`, `ice` e `state` são
encaminhados ao outro cliente **byte a byte**, sem re-serialização. Campos
extras que um cliente colocar chegam intactos ao outro. O servidor não
interpreta SDP nem candidatos.

Sem peer presente → `error { code: "no_peer" }`; a conexão continua aberta.

**Exceção: `ring`.** Um `ring` sem ninguém na sala é o caso normal deste app,
não um erro — quer dizer que o app do outro lado está fechado. Se houver
dispositivo registrado para push, o servidor dispara a campainha e responde
`push-sent` em vez de `no_peer`. Sem dispositivo registrado (ou sem VAPID
configurado), o `no_peer` de sempre. Detalhes na §11.

## 7. Heartbeat

O cliente envia `ping` a cada **20 s**; o servidor responde `pong`. O servidor
fecha a conexão se ficar **60 s** sem receber nenhuma mensagem (três pings
perdidos). Quando isso acontece, o peer restante recebe `peer-left`.

A queda do WebSocket **não** derruba a chamada: o WebRTC já negociado segue
sozinho. A reconexão do WebSocket com backoff (1 s → 5 s, com jitter) é
responsabilidade do cliente.

O teto do backoff é **5 s**, não 30 s. Backoff longo é controle de carga, e
com dois usuários e um servidor não há carga a controlar — o que ele comprava
era uma janela em que um ICE restart não pode ser entregue, já que o signaling
é o único caminho de uma offer até o peer. Um celular cujo rádio demorou
alguns segundos para voltar depois de trocar de rede ficava 15-30 s atrás de
um backoff crescido, tempo suficiente para o outro lado desistir de uma
chamada que ia se recuperar. O cliente também reconecta na hora no evento
`online` do browser, em vez de esperar o timer.

## 8. Saída

Desconexão limpa ou por timeout → o slot é liberado imediatamente e o outro
cliente recebe `peer-left { id }`. O próximo `hello` ocupa o slot e recebe
`polite: true` (porque já há alguém).

Se o WebSocket cair **sem FIN** (cabo puxado, troca de rede), o servidor não
vê close nenhum e o slot fica ocupado por um fantasma até os 60 s de silêncio.
A chamada em si nunca é afetada — o WebRTC é independente do signaling (§7) —
mas a reconexão do cliente, que acontece em ~1 s, chegaria antes disso.

**Reivindicação de slot.** Por isso o `hello` traz `session` (§2): se ele
casar com o de um cliente já sentado, a conexão nova **assume aquele slot**,
mantendo o mesmo `id` e o mesmo papel polite/impolite, e o servidor derruba a
conexão fantasma. O peer não recebe `peer-left` nem `peer-joined`: do ponto de
vista dele é o mesmo participante, no mesmo id, num transporte novo.

Sem isso — e era assim até 07/09/2026 — a reconexão depois de uma troca de
rede era respondida com `room_full` pelo próprio cadáver do cliente, o que
apareceu como uma faixa de erro permanente em teste de aparelho real.

Duas armadilhas de implementação, ambas registradas porque custaram teste:

- o `leave` do fantasma tem que comparar **identidade do objeto**, não só o
  `id`: os dois têm o mesmo `id`, e comparar só por `id` faria a morte do
  fantasma despejar a conexão viva que acabou de ocupar o slot;
- do lado do cliente, só o socket **atual** pode disparar reconexão. O socket
  abandonado é fechado pelo servidor logo depois da reivindicação, e se esse
  close cair no mesmo handler ele reconecta de novo, abandonando outro socket
  — cascata observada como três reivindicações para uma única troca de rede.

## 9. Erros

| `code`          | Quando                                              | Fecha? |
|-----------------|-----------------------------------------------------|--------|
| `bad_hello`     | primeira mensagem não é `hello` válido, ou timeout  | sim    |
| `unauthorized`  | token inválido ou expirado                          | sim    |
| `bad_room`      | `room` diferente de `"main"`                        | sim    |
| `room_full`     | já há 2 clientes de sessões **diferentes** (§8)      | sim    |
| `no_peer`       | mensagem de relay sem peer presente                 | não    |
| `bad_type`      | `t` desconhecido                                    | não    |
| `bad_message`   | JSON inválido                                       | não    |

## 10. Limites

- Mensagem máxima: 64 KiB (uma SDP com vídeo + tela fica em ~10 KB).
- Fila de escrita por cliente: 32 mensagens. Se encher (cliente não lê), a
  conexão é fechada para não travar o outro lado.
- Origem: o servidor aceita `Origin` na lista `ALLOWED_ORIGINS` ou ausência do
  header (clientes não-browser).

## 11. Campainha com o app fechado (Web Push)

O celular com a PWA fechada não tem WebSocket, então não tem como receber
`ring`. O único caminho até ele é a Push API do browser: o servidor fala com
o serviço de push do fabricante (FCM, no Chrome do Android), que acorda o
service worker, que mostra a notificação.

**Só o celular.** O PC roda Firefox, que não instala PWA nem recebe push, e
não precisa: lá o app fica aberto numa aba fixada. Nada no protocolo exige que
os dois lados tenham push, e um servidor sem VAPID configurado se comporta
exatamente como antes deste sprint.

**Sem payload.** O push vai com corpo vazio. Um push só pode significar uma
coisa aqui — a outra pessoa está ligando —, então o texto da notificação é
constante e não há o que carregar. Isso dispensa a RFC 8291 inteira (ECDH,
HKDF, AES-GCM) e deixa só o VAPID (RFC 8292) do lado do servidor.

**Frescor pelo `TTL`, não por timestamp.** O rádio de um Android que estava
dormindo entrega a mensagem enfileirada quando acorda, o que pode ser minutos
depois. O servidor manda `TTL: 45`, e o serviço de push descarta a mensagem em
vez de acordar o celular para uma chamada que já acabou. É garantia mais forte
que um timestamp dentro do payload, que só permitiria ficar quieto **depois**
de o aparelho já ter sido acordado. Junto vão `Urgency: high` (acorda o
aparelho em doze) e `Topic: ring` (um segundo ring substitui o primeiro ainda
na fila, em vez de virar uma rajada de notificações).

**Cadência.** No máximo um push a cada 10 s. O `push-sent` continua saindo em
todo `ring` — ele é sobre alcançabilidade, não sobre entrega.

**O que o servidor guarda.** Os endpoints de subscription, em memória e
espelhados num arquivo JSON (`PUSH_STORE`). Isso não é estado de signaling nem
banco: é registro de dispositivo, e sem persistir ele morre a cada deploy —
com a campainha parando de funcionar **em silêncio** até o celular abrir o app
em primeiro plano de novo, que é exatamente o caso que este sprint existe para
cobrir. Um `404` ou `410` do serviço de push é autoritativo e derruba o
registro; qualquer outra falha não.

**Fluxo completo, do toque do botão até a chamada:**

1. PC manda `ring`. Não há peer; o servidor manda o push e responde
   `push-sent`. O PC continua em `ringing-out`.
2. O service worker do celular acorda e mostra a notificação com a ação
   **Atender**.
3. O toque abre o app. Se já houver uma aba, ela é focada e recebe
   `postMessage({ t: "answer" })`; se não, abre `/?answer=1`.
4. O app carrega, faz login automático com o token guardado e conecta o
   WebSocket.
5. O servidor manda `peer-joined` ao PC. **O PC, ainda em `ringing-out`,
   reenvia `ring`** — o primeiro morreu junto com a ausência de peer, e sem
   este reenvio o celular abriria sem nada tocando.
6. O celular recebe `ring` e, por ter vindo de uma notificação, atende
   sozinho: manda `accept` e a chamada começa pelo caminho normal.

A janela do passo 6 é de 60 s a partir da carga da página. Passado isso o
`ring` toca normalmente e espera o toque no botão — um deep link velho não
pode fazer o aparelho entrar em chamada sozinho.

**Login automático.** O passo 4 exige que o app tenha credencial sem passar
pela tela de passphrase, senão "Atender" cai no formulário de login e não na
chamada. O token de 12 h fica no `localStorage`. A regra do `CLAUDE.md` proíbe
`localStorage` **para estado de chamada**; um token de acesso não é estado de
chamada, e nada da chamada é gravado. Um `unauthorized` no `hello` apaga o
token guardado e devolve a tela de login.
