# Prova W1: instalação real — passou dentro do worktree

**Confirmação do dono na terceira passada (06/10/2026):** a instalação real
em `%LOCALAPPDATA%` também passou, fora do sandbox: 110 MB, 276 arquivos, sem
administrador. Os tempos desse teste externo: **não encontrado**. O relatório
abaixo conserva a execução anterior no worktree; a pasta atual das telas já
inclui a fonte Twemoji e não foi reinstalada nesta terceira passada.

Em 06/10/2026, instalei a saída compilada de `telas.exe` junto com a cópia real
de `harmony-w1.exe` (17.856.000 bytes). **Não executei harmony-w1.exe**.
Usei `-Destino` no worktree após tentar o destino padrão, que foi bloqueado.

## Números medidos

MB significa bytes / 1.048.576 (MiB); inclui todos os arquivos, inclusive
símbolos de depuração presentes na saída do build.

| Etapa | Tempo | Tamanho total | Arquivos |
|---|---:|---:|---:|
| Instalar versão 1 | 4,250 s | 109,599 MiB | 276 |
| Atualizar para 2 com a 1 aberta | 3,495 s | 219,195 MiB | 547 |
| Após fechar a 1 e limpar | — | 109,599 MiB | 276 |

Cada versão tem 109,597 MiB e 271 arquivos (saída do telas, harmony-w1 e
manifesto). Os cinco arquivos restantes pertencem ao instalador/lançador.
Os tempos incluem cópia, SHA-256, manifesto, confirmação do apontador e
tentativa de limpeza. Não houve solicitação de elevação; a identidade de
execução **não era administradora**. Os scripts não escrevem no registro,
HKCU ou HKLM. Não monitorei todas as escritas internas feitas pelo Windows.

A prova real confirmou, em `prova-instalacao.json`:

- versão 1 com janela Harmony aberta durante a instalação da 2;
- hashes conferidos antes da troca do apontador; app-1 retida enquanto aberta;
- lançador abrindo efetivamente `app-2\telas.exe`, com janela Harmony;
- fechamento da 1 e remoção da pasta antiga somente depois de sua saída;
- medição da versão 2 instalada;
- remoção completa: o destino deixou de existir e nenhum telas ficou aberto.

Na versão instalada: abertura **423,011 ms**; memória no fim **114,395 / 121,391
MiB** (working set / privada); após rolar tudo **116,602 / 136,152 MiB**;
**3.334** intervalos, **26,988** notificações de Rendering/s, pior intervalo
**111,348 ms**, **986** acima de 33 ms, em **123,535 s**. São notificações antes
do desenho, não quadros apresentados. `medidas-instalado.json` contém o resultado.
A medição não inclui o tempo de validar hashes no lançador: começa no processo
`telas.exe`. O protótipo ainda não demonstrou rolagem fluida.

## Bloqueio exato do destino padrão

Tentei `%LOCALAPPDATA%\Programs\harmony-prova`, resolvido para
`%USERPROFILE%\AppData\Local\Programs\harmony-prova`. Falhou em
`instalar.ps1`, no `New-Item -ItemType Directory -Path $root`, com:

```
New-Item : O acesso ao caminho 'harmony-prova' foi negado.
FullyQualifiedErrorId : CreateDirectoryUnauthorizedAccessError,Microsoft.PowerShell.Commands.NewItemCommand
```

`prova-localappdata.json` preserva o erro completo. Não contornei o sandbox.
A prova real foi feita em
`<pasta-de-trabalho>\w1-telas\windows\native\instalar\prova-real`,
que já foi removido pelo próprio teste. A instalação no LOCALAPPDATA do dono
fica pronta para repetir, mas não foi validada neste ambiente.

## Comandos (na raiz do worktree)

Este comando foi executado e passou; inclui instalar 1, abrir, atualizar 2,
abrir pelo lançador, limpar a 1, medir a 2 e remover:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File windows\native\instalar\provar-instalacao.ps1 -Destino "$PWD\windows\native\instalar\prova-real"
```

Para repetir no destino padrão, numa sessão normal do dono:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File windows\native\instalar\provar-instalacao.ps1
```

Use destino novo: a prova completa recusa sobrescrever uma instalação anterior.
Os JSONs são `instalacao-1.json`, `atualizacao-2.json`, `medidas-instalado.json`
e `prova-instalacao.json`. A prova auxiliar usa uma janela identificada pelo
PID e título Harmony, e não considera um processo sem janela como abertura.

Uso separado:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File windows\native\instalar\instalar.ps1 -Versao 1
& "$env:LOCALAPPDATA\Programs\harmony-prova\harmony.cmd"
powershell -NoProfile -ExecutionPolicy Bypass -File windows\native\instalar\instalar.ps1 -Versao 2
powershell -NoProfile -ExecutionPolicy Bypass -File windows\native\instalar\instalar.ps1 -Limpar
powershell -NoProfile -ExecutionPolicy Bypass -File windows\native\instalar\instalar.ps1 -Remover
```

`-Origem`, `-Harmony` e `-Destino` permitem caminhos explícitos. `-Relatorio`
grava tamanho, contagem, duração, hashes e versões cuja limpeza foi adiada.
A versão é um inteiro crescente. Não há administrador, serviço, tarefa
agendada, MSIX, assinatura, publicação ou execução do programa de voz nesta prova.

## Atualização interrompida e arquivos em uso

A cópia ocorre em `.staging-*`. Só depois de conferir tamanho e SHA-256 de
cada arquivo, gravar o manifesto e renomear a pasta no mesmo volume, o script
substitui `current.json` atomicamente. O lançador usa essa versão confirmada
e valida novamente os arquivos; não abre staging.

Uma queda durante a cópia deixa staging e conserva o apontador antigo.
Uma queda após o rename e antes do apontador deixa uma versão ainda não
selecionada, que a próxima tentativa pode recopiar se não estiver em uso.
Depois da troca, a nova versão já está completa. Versão antiga aberta ou com
arquivo travado fica para a próxima instalação ou `-Limpar`, sem erro.
`-Remover` exige fechar os programas; não força o término do programa do dono.

O teste de lógica foi repetido e passou (`teste-logica.json`): arquivo travado,
manifesto, apontador, recuperação de interrupção simulada, recusa de corrupção
e remoção. A interrupção foi **simulada com arquivos não executáveis**;
não desliguei a máquina durante uma instalação real.

A prova das telas e as pendências de teclado/leitor de tela estão em
`../telas/README.md`. O autoteste completo das telas continua vermelho por
Enter não acrescentar o item no ambiente de execução; a instalação passou.
