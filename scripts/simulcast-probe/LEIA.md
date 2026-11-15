# A sonda que provou o simulcast (18/09/2026)

Antes de escrever uma linha do D5 foi preciso saber se o caminho existe, porque
três coisas podiam matá-lo e nenhuma estava documentada para a nossa pilha. Esta
pasta é o instrumento que respondeu, e fica porque a pergunta volta a cada
atualização do Pion ou do navegador.

**O que ela responde:** o navegador manda simulcast H.264, e o Pion recebe as
camadas separadas, com RID?

## Como rodar

```bash
cd scripts/simulcast-probe && go run .          # sobe em 127.0.0.1:8123
```

E, de outro terminal, na raiz do repositório:

```bash
PLAYWRIGHT_BROWSERS_PATH="$USERPROFILE/.pw-browsers" node scripts/simulcast-probe/navegador.mjs
```

## O que ela achou, em 18/09/2026

| pergunta | resposta |
|---|---|
| O Pion oferta simulcast (`a=simulcast:recv`)? | **Não.** Só escreve isso ao **responder** — `getRids` lê a descrição remota. |
| Dá para editar a oferta do Pion antes de aplicá-la? | **Não.** `InvalidModificationError: new sdp does not match previous offer`. |
| O navegador responde simulcast a uma oferta que pede `recv`? | **Sim** — `a=simulcast:send` e três encodings. |
| O Pion entrega uma faixa por camada? | **Sim** — três `OnTrack`, com RID e SSRC próprios. |
| Funciona com **H.264**, que é o codec fixado pelo dono? | **Sim** — as três camadas em `video/H264`. |

**A decisão que sai daí:** quem oferta a tela passa a ser o **cliente**, uma vez,
no momento em que começa a compartilhar. É o caminho que o Pion suporta de
verdade, e é o que o ion-sfu, o Galène e o LiveKit fazem — três implementações
independentes, a mesma escolha. O servidor continua ofertando todo o resto, e
segura as próprias ofertas enquanto a da tela acontece, senão as duas se cruzam
(*glare*).

> **Uma armadilha do próprio instrumento**, custou uma rodada: a porta estava
> ocupada por um processo esquecido de outra medição e o `ListenAndServe` tinha
> o erro engolido com `_ =`. O servidor nunca subiu e o navegador conversou com
> a coisa errada. Quem abre porta grita quando não consegue.
