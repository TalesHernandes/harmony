# Organização do programa Windows

Separação por dependências atuais, sem refatorar o programa. Todos os arquivos rastreados movidos usaram `git mv`. Nenhum arquivo foi apagado.

## Árvore final

Contagem de arquivos rastreados, incluindo este relatório e o LEIAME; saídas locais ignoradas não entram. Os números de pastas abaixo incluem as subpastas.

```text
windows/app/ (13 arquivos na raiz)
├── src/ (84 arquivos)
├── testes/ (55 arquivos)
├── arquivo/ (176 arquivos)
│   ├── w12-results/ (15 arquivos)
│   └── w4-sfu-probe/ (1 arquivo)
├── instalar/ (24 arquivos)
├── assets/ (46 arquivos)
├── queixas/ (334 arquivos)
└── test-output/ (126 arquivos)
```

A raiz tinha 310 arquivos rastreados. Permaneceram os cinco comandos de entrada (`build.cmd`, `test.cmd`, `chat-audit.cmd`, `call-audit.cmd`, `camera-regressions.cmd`), o projeto, as configurações de pacotes, `.gitignore`, `README.md` e `THIRD-PARTY.md`. Foram acrescentados `LEIAME.md` e este relatório.

`src/` contém as unidades compiladas no projeto, recursos e o fechamento dos includes locais. Mesmo headers com nome de sprint/auditoria pertencem ao programa quando incluídos por ele. A ordem dos 54 includes locais de `main.cpp`, incluindo os headers de UI dentro de `struct App`, foi conferida e preservada.

`nuget.config` e `packages.config` ficam ao lado do projeto: são configurações da compilação/restauração, não fontes do programa. `THIRD-PARTY.md` continua na raiz porque é copiado para a distribuição.

## Grupo (b): em uso

Cada linha indica uma referência direta; quando ela vem de outro auxiliar, a ligação até um comando de entrada é indireta. Os caminhos abaixo já são os novos.

| Arquivo em testes/ | Por que está em uso |
|---|---|
| `autoteste-email.mjs` | Importado por `instalar/updater-e2e.mjs` e pelo autoteste de tela. |
| `autoteste-metrics.mjs` | Referenciado por `testes/w4-round5.py`. |
| `autoteste-monitor.mjs` | Referenciado por `testes/autoteste-tela.mjs`. |
| `autoteste-origin-sync.mjs` | Referenciado por `testes/autoteste-tela.mjs`. |
| `autoteste-processes.mjs` | Referenciado por `testes/w4-round5.py`. |
| `autoteste-surface.mjs` | Referenciado por `testes/w4-round5.py`. |
| `autoteste-tela.cmd` | Referenciado por `testes/w4-round23-compare.ps1`. |
| `autoteste-tela.mjs` | Referenciado por `testes/w4-regressions.py`. |
| `build.ps1` | Referenciado por `build.cmd`. |
| `call-audit.mjs` | Referenciado por `call-audit.cmd`. |
| `camera-regressions.cpp` | Referenciado por `camera-regressions.cmd`. |
| `chat-audit.mjs` | Referenciado por `chat-audit.cmd`. |
| `chat-tests.cpp` | Referenciado por `test.cmd`. |
| `dfn-speech-test.py` | Referenciado por `testes/dfn-test.cmd`. |
| `dfn-stage.ps1` | Referenciado por `testes/build.ps1`. |
| `dfn-test.cmd` | Referenciado por `test.cmd`. |
| `dfn-tests.cpp` | Referenciado por `testes/dfn-test.cmd`. |
| `emoji-from-web.py` | Referenciado por `test.cmd`. |
| `emoji-tests.cpp` | Referenciado por `test.cmd`. |
| `engine-regressions.cpp` | Referenciado por `testes/engine-test.cmd`. |
| `engine-test.cmd` | Referenciado por `test.cmd`. |
| `fake-adm.h` | Referenciado por `testes/engine-regressions.cpp`. |
| `installer-state-tests.ps1` | Referenciado por `test.cmd`. |
| `installer-tests.ps1` | Referenciado por `test.cmd`. |
| `media-game.cpp` | Referenciado por `testes/autoteste-tela.cmd`. |
| `media-inventory.cpp` | Referenciado por `testes/w4-platform-regressions.py`. |
| `media-tests.h` | Referenciado por `testes/voice-tests.cpp`. |
| `photo-crop-tests.cpp` | Referenciado por `test.cmd`. |
| `restore-offline.ps1` | Referenciado por `testes/build.ps1`. |
| `sound-rules-tests.cpp` | Referenciado por `test.cmd`. |
| `tests.cpp` | Referenciado por `test.cmd`. |
| `toast-tests.cpp` | Referenciado por `test.cmd`. |
| `unpack-font.cpp` | Referenciado por `build.cmd`. |
| `update-transport-tests.mjs` | Referenciado por `test.cmd`. |
| `updater-tests.cpp` | Referenciado por `test.cmd`. |
| `voice-test-engine.cpp` | Referenciado por `test.cmd`. |
| `voice-tests.cpp` | Referenciado por `test.cmd`. |
| `w4-bandwidth-tests.h` | Referenciado por `testes/engine-regressions.cpp`. |
| `w4-cadence-tests.cpp` | Referenciado por `testes/w4-regressions.py`. |
| `w4-guard-fixture.cpp` | Referenciado por `testes/w4-round5.py`. |
| `w4-guard-tests.mjs` | Referenciado por `testes/w4-round5.py`. |
| `w4-metrics-tests.mjs` | Referenciado por `testes/w4-round5.py`. |
| `w4-platform-regressions.py` | Referenciado por `test.cmd`. |
| `w4-probe-tests.mjs` | Referenciado por `testes/w4-regressions.py`. |
| `w4-process-tests.mjs` | Referenciado por `testes/w4-round5.py`. |
| `w4-regressions.py` | Referenciado por `test.cmd`. |
| `w4-round23-compare.ps1` | `testes/w4-process-tests.mjs` lê e verifica seu conteúdo; retenção conservadora. |
| `w4-round4.py` | Referenciado por `test.cmd`. |
| `w4-round5.py` | Referenciado por `test.cmd`. |
| `w4-surface-tests.mjs` | Referenciado por `testes/w4-round5.py`. |
| `w4-timestamp-tests.h` | Referenciado por `testes/w4-regressions.py`. |
| `w6-tests.cpp` | Referenciado por `test.cmd`. |
| `w8a-tests.cpp` | Referenciado por `test.cmd`. |
| `w8c-tests.cpp` | Referenciado por `test.cmd`. |
| `w8d-resource-tests.cpp` | Referenciado por `test.cmd`. |

Os comandos mantidos na raiz também são do grupo (b): `build.cmd` e `test.cmd` são os comandos do runbook; `call-audit.cmd` e `camera-regressions.cmd` são citados nele; `chat-audit.cmd` foi explicitamente preservado pela tarefa.

## Casos de dúvida

- `w4-round23-compare.ps1` parece uma rodada encerrada, mas um teste atual lê seu conteúdo: ficou em `testes/` e trouxe consigo `autoteste-tela.cmd` e `media-game.cpp`. Esses auxiliares continuam com os caminhos ajustados; a medição real não foi executada.
- `autoteste-tela.mjs` é lido pelos testes de regressão; seus módulos de métricas, superfície, processos, monitores, sincronização e e-mail foram mantidos em `testes/`, inclusive dependências usadas só em modos opcionais.
- `w4-round4.py`, `w4-round5.py`, os testes W8 e os headers de auditoria continuam em uso apesar dos nomes de sprint; os primeiros são alcançados por `test.cmd`, e os headers compilados ficam em `src/`.

## Referências preservadas por proibição

`CLAUDE.md`: nenhuma referência a caminho movido foi encontrada. O arquivo não foi editado.

`docs/DEPLOY-HISTORICO.md` não foi editado. Referências a arquivos que mudaram de lugar:

| Linhas do histórico | Referência antiga → novo caminho em windows/app/ |
|---|---|
| 13622 | `autoteste-voz.cmd` → `arquivo/autoteste-voz.cmd` |
| 15267 | `changelog.h` → `src/changelog.h` |
| 15961 | `chat.h` → `src/chat.h` |
| 15011, 15041, 15069, 15137, 15162, 15198, 15234, 15286, 15443, 15696, 15751 | `engine-test.cmd` → `testes/engine-test.cmd` |
| 15437 | `gravacao-teste.mjs` → `arquivo/gravacao-teste.mjs` |
| 15358 | `main.cpp` → `src/main.cpp` |
| 13810 | `medidas-web.md` → `arquivo/medidas-web.md` |
| 13532, 13621 | `mutation-tests.py` → `arquivo/mutation-tests.py` |
| 15673 | `panel-renderer.h` → `src/panel-renderer.h` |
| 15652, 15729 | `sound-rules-tests.cpp` → `testes/sound-rules-tests.cpp` |
| 15163 | `w12-duck-level.cpp` → `arquivo/w12-duck-level.cpp` |
| 15163 | `w12-duck-optout.cpp` → `arquivo/w12-duck-optout.cpp` |
| 15099, 15138 | `w4-round6.py` → `arquivo/w4-round6.py` |

A busca por caminhos antigos usou `git grep` pelos nomes movidos em `windows/`, `scripts/`, `docs/DEPLOY.md`, `AGENTS.md` e `.gitignore`. As referências editáveis foram corrigidas, inclusive `scripts/bancada/w4.mjs` e o README. Não houve referência a corrigir em `docs/DEPLOY.md`, `AGENTS.md` ou no `.gitignore`. Os scripts arquivados conservam referências históricas e não têm funcionamento garantido.

Também foram preservadas as citações antigas em `queixas/`, em `test-output/` e no comentário de `windows/native/src/call-media.h:5`, conforme as restrições da tarefa. Lista das ocorrências explícitas nessas áreas:

- `windows/app/queixas/RELATORIO.md:153: windows/app/autoteste.cmd`
- `windows/app/queixas/segue/RELATORIO.md:33: windows/app/direct-call.cpp`
- `windows/app/queixas/segue/RELATORIO.md:35: windows/app/media-tests.h`
- `windows/app/queixas/segue/RELATORIO.md:37: windows/app/engine-regressions.cpp`
- `windows/app/queixas/segue/RELATORIO.md:95: windows/app/engine-regressions.cpp`
- `windows/app/queixas/segue/TESTES.txt:4: windows/app/engine-test.cmd`
- `windows/app/queixas/w3/relatorios/w3-auditoria.md:61: windows/app/mutation-tests.py`
- `windows/app/queixas/w3/relatorios/w3-auditoria.md:61: windows/app/voice-test-engine.cpp`
- `windows/app/queixas/w3/relatorios/w3-auditoria.md:61: windows/app/voice-tests.cpp`
- `windows/app/queixas/w3/relatorios/w3-auditoria.md:73: windows/app/voice-selftest.cpp`
- `windows/app/queixas/w3/relatorios/w3-auditoria.md:88: windows/app/mutation-tests.py`
- `windows/app/queixas/w3/relatorios/w3-auditoria.md:91: windows/app/autoteste-voz.cmd`
- `windows/app/queixas/w3/relatorios/w3-conserto.md:11: windows/app/engine-regressions.cpp`
- `windows/app/queixas/w3/relatorios/w3-conserto.md:37: windows/app/engine-regressions.cpp`
- `windows/app/queixas/w3/relatorios/w3-conserto.md:41: windows/app/mutation-tests.py`
- `windows/app/queixas/w3/relatorios/w3-conserto.md:47: windows/app/device-proof.h`
- `windows/app/queixas/w3/relatorios/w3-conserto.md:63: windows/app/autoteste-voz.cmd`
- `windows/app/queixas/w3/relatorios/w3-conserto.md:63: windows/app/mutation-tests.py`
- `windows/app/queixas/w3/relatorios/w3-conserto.md:9: windows/app/main.cpp`
- `windows/app/queixas/w3/relatorios/w3-dfn.md:133: windows/app/dfn-speech-test.py`
- `windows/app/queixas/w3/relatorios/w3-dfn.md:144: windows/app/mutation-tests.py`
- `windows/app/queixas/w3/relatorios/w3-dfn.md:145: windows/app/autoteste-voz.cmd`
- `windows/app/queixas/w3/relatorios/w3-dfn.md:16: windows/app/dfn-benchmark.py`
- `windows/app/queixas/w3/relatorios/w3-dfn.md:17: windows/app/DFN-PROPOSTA.md`
- `windows/app/queixas/w3/relatorios/w3-dfn.md:65: windows/app/dfn-benchmark.py`
- `windows/app/queixas/w3/relatorios/w3-dfn.md:86: windows/app/main.cpp`
- `windows/app/queixas/w3/relatorios/w3-dfn.md:87: windows/app/dfn-stage.ps1`
- `windows/app/queixas/w3/relatorios/w3-dfn.md:92: windows/app/dfn-benchmark.py`
- `windows/app/queixas/w3/relatorios/w3-saida.md:38: windows/app/voice-state.h`
- `windows/app/queixas/w3/relatorios/w3-saida.md:39: windows/app/main.cpp`
- `windows/app/queixas/w3/relatorios/w3-saida.md:42: windows/app/voice-tests.cpp`
- `windows/app/queixas/w3/relatorios/w3-saida.md:43: windows/app/voice-selftest.cpp`
- `windows/app/queixas/w3/relatorios/w3-saida.md:78: windows/app/mutation-tests.py`
- `windows/app/queixas/w3/relatorios/w3-saida.md:79: windows/app/autoteste-voz.cmd`
- `windows/app/queixas/w3/relatorios/w3-saida.md:88: windows/app/voz-manual.cmd`
- `windows/app/queixas/w3/verificacao.json:107: windows/app/voice-engine.cpp`
- `windows/app/queixas/w3/verificacao.json:108: windows/app/voice-engine.h`
- `windows/app/queixas/w3/verificacao.json:109: windows/app/voice-selftest.cpp`
- `windows/app/queixas/w3/verificacao.json:110: windows/app/voice-state.h`
- `windows/app/queixas/w3/verificacao.json:111: windows/app/voice-test-engine.cpp`
- `windows/app/queixas/w3/verificacao.json:112: windows/app/voice-tests.cpp`
- `windows/app/queixas/w3/verificacao.json:113: windows/app/voz-manual.cmd`
- `windows/app/queixas/w3/verificacao.json:22: windows/app/main.cpp`
- `windows/app/queixas/w3/verificacao.json:23: windows/app/mutation-tests.py`
- `windows/app/queixas/w3/verificacao.json:24: windows/app/service.cpp`
- `windows/app/queixas/w3/verificacao.json:25: windows/app/service.h`
- `windows/app/queixas/w3/verificacao.json:27: windows/app/tests.cpp`
- `windows/app/queixas/w3/verificacao.json:37: windows/app/DFN-PROPOSTA.md`
- `windows/app/queixas/w3/verificacao.json:44: windows/app/autoteste-voz.cmd`
- `windows/app/queixas/w3/verificacao.json:45: windows/app/autoteste-voz.mjs`
- `windows/app/queixas/w3b/relatorios/w3b-saida.md:101: windows/app/verificar-iniciar.ps1`
- `windows/app/queixas/w3b/relatorios/w3b-saida.md:106: windows/app/voz-manual.cmd`
- `windows/app/queixas/w3b/relatorios/w3b-saida.md:120: windows/app/verificar-iniciar.ps1`
- `windows/app/queixas/w3b/relatorios/w3b-saida.md:37: windows/app/verificar-iniciar.ps1`
- `windows/app/queixas/w3b/relatorios/w3b-saida.md:72: windows/app/mutation-tests.py`
- `windows/app/queixas/w3b/relatorios/w3b-saida.md:73: windows/app/autoteste-voz.cmd`
- `windows/app/queixas/w3b/relatorios/w3b-saida.md:91: windows/app/mutation-tests.py`
- `windows/app/queixas/w3b/relatorios/w3b-saida.md:92: windows/app/autoteste-voz.cmd`
- `windows/app/queixas/w3b/relatorios/w3b-saida.md:94: windows/app/verificar-iniciar.ps1`
- `windows/app/test-output/w13/camera/RELATORIO.md:238: windows/app/engine-test.cmd`
- `windows/app/test-output/w13/camera/RELATORIO.md:251: windows/app/w13-camera-build.cmd`
- `windows/app/test-output/w13/camera/RELATORIO.md:256: windows/app/w13-camera-run.mjs`
- `windows/app/test-output/w13/camera/RELATORIO.md:257: windows/app/w13-camera-summary.py`
- `windows/app/test-output/w13/camera/RELATORIO.md:258: windows/app/w13-camera-run.mjs`
- `windows/app/test-output/w13/camera/RELATORIO.md:259: windows/app/w13-camera-summary.py`
- `windows/app/test-output/w13/camera/RELATORIO.md:260: windows/app/w13-camera-run.mjs`
- `windows/app/test-output/w13/camera/RELATORIO.md:261: windows/app/w13-camera-memory-summary.py`
- `windows/app/test-output/w13/camera/RELATORIO.md:262: windows/app/w13-camera-run.mjs`
- `windows/app/test-output/w13/camera/RELATORIO.md:263: windows/app/w13-camera-memory-summary.py`
- `windows/app/test-output/w13/camera/RELATORIO.md:463: windows/app/engine-test.cmd`
- `windows/app/test-output/w13/camera/RELATORIO.md:484: windows/app/w13-camera-firefox.mjs`
- `windows/app/test-output/w13/camera/RELATORIO.md:485: windows/app/w13-camera-firefox-summary.py`
- `windows/app/test-output/w13/camera/RELATORIO.md:487: windows/app/w13-camera-firefox.mjs`
- `windows/app/test-output/w13/camera/RELATORIO.md:488: windows/app/w13-camera-firefox-summary.py`
- `windows/app/test-output/w13/camera/RELATORIO.md:490: windows/app/w13-camera-verify.py`
- `windows/app/test-output/w13/camera/firefox-verification.json:47: windows/app/engine-test.cmd`
- `windows/app/test-output/w13/camera/firefox-verification.json:8: windows/app/w13-camera-sdp-test.mjs`
- `windows/app/test-output/w13/camera/verification.json:3: windows/app/engine-test.cmd`
- `windows/app/test-output/w13/captura/RELATORIO.md:273: windows/app/w13-captura-build.py`
- `windows/app/test-output/w13/captura/RELATORIO.md:274: windows/app/w13-captura-build.py`
- `windows/app/test-output/w13/captura/RELATORIO.md:275: windows/app/w13-captura-build.py`
- `windows/app/test-output/w13/captura/RELATORIO.md:277: windows/app/w13-captura-run.mjs`
- `windows/app/test-output/w13/captura/RELATORIO.md:278: windows/app/w13-captura-run.mjs`
- `windows/app/test-output/w13/captura/RELATORIO.md:279: windows/app/w13-captura-run.mjs`
- `windows/app/test-output/w13/captura/RELATORIO.md:280: windows/app/w13-captura-policy-proof.py`
- `windows/app/test-output/w13/captura/RELATORIO.md:281: windows/app/w13-captura-verify.py`
- `windows/app/test-output/w13/reconexao/RELATORIO.md:113: windows/app/engine-test.cmd`
- `windows/app/test-output/w13/reconexao/RELATORIO.md:134: windows/app/w13-reconexao-build.py`
- `windows/app/test-output/w13/reconexao/RELATORIO.md:135: windows/app/w13-reconexao-build.py`
- `windows/app/test-output/w13/reconexao/RELATORIO.md:136: windows/app/w13-reconexao-vite.config.mts`
- `windows/app/test-output/w13/reconexao/RELATORIO.md:138: windows/app/w13-reconexao-vite.config.mts`
- `windows/app/test-output/w13/reconexao/RELATORIO.md:140: windows/app/w13-reconexao-web-test.mjs`
- `windows/app/test-output/w13/reconexao/RELATORIO.md:143: windows/app/w13-reconexao-web-test.mjs`
- `windows/app/test-output/w13/reconexao/RELATORIO.md:150: windows/app/w13-reconexao-run.mjs`
- `windows/app/test-output/w13/reconexao/RELATORIO.md:154: windows/app/w13-reconexao-run.mjs`
- `windows/app/test-output/w13/reconexao/RELATORIO.md:158: windows/app/w13-reconexao-run.mjs`
- `windows/native/src/call-media.h:5: windows/app/direct-call.h`

## Verificações

| Verifica??o | Antes | Depois |
|---|---|---|
| `cmd /c windows\app\build.cmd` (raiz do worktree) | Passou, c?digo 0. | Passou, c?digo 0. |
| `cmd /c windows\app\test.cmd` (raiz do worktree) | Falhou, c?digo 1: falta `windows/native/build/gpu_selftest.exe`. | Mesma falha, c?digo 1, no mesmo ponto. |
| Etapas alcan?adas de `test.cmd` | 153 verifica??es do chat, 277 do app, DFN sem falhas, 2.396 do engine, W8C, regress?es W4 e c?mera passaram antes do bin?rio GPU ausente. | Mesmas verifica??es passaram antes do mesmo bin?rio ausente. |
| Trecho ap?s o GPU, executado por c?pia tempor?ria em `temp/` | Todos os testes emitiram PASS, incluindo W6, toast, muta??o esperada de speaking, atualiza??o, estado do instalador, cabinet e tipografia. A invoca??o do wrapper foi registrada com c?digo 1; n?o ? um passe agregado. | Passou, c?digo 0 (`TAIL_EXIT=0`); mesmo conjunto, sem pular verifica??es dentro desse trecho. |
| Empacotamento `-TestOnly -Release 126 -Source windows/app/out -Output windows/app/test-output/packages-org` | **n?o testado** | Passou, c?digo 0 (`PACKAGE_EXIT=0`); instalador de teste gerado, sem instala??o. |
| Execut?veis do conte?do do pacote | **n?o testado** | Manifesto validado cont?m exatamente `app/harmony-nativo.exe` e `app/RestartAgent.exe`, em 291 arquivos de execu??o. |
| Compila??o isolada dos recursos `src/app.rc` | **n?o testado** | Passou, c?digo 0. |
| Sintaxe Python/JavaScript/PowerShell dos auxiliares ativos | **n?o testado** | Passou, sem executar auditorias ou abrir a interface. |
| Includes/imports locais e ordem de includes de `main.cpp` | **n?o testado** | Nenhuma depend?ncia local ausente; mesmos 54 includes, na mesma ordem. |
| `git diff --check` | **n?o testado** | Passou. |
| `git log --oneline HEAD..master` | Nenhum commit pendente encontrado. | Nenhum commit pendente encontrado. |

O pedido de deixar os dois comandos verdes n?o p?de ser atendido neste worktree: a r?gua j? estava vermelha pela aus?ncia do execut?vel GPU. Ele n?o foi criado em `windows/native/`, pois a tarefa pro?be escrever nessa pasta. A execu??o separada do trecho posterior n?o substitui um passe de `test.cmd` inteiro.

Houve uma tentativa intermedi?ria de build/teste com caminhos incorretos de headers externos do WebRTC/libyuv. Esses caminhos foram corrigidos e as verifica??es finais acima foram executadas novamente. O programa n?o foi iniciado para conferir apar?ncia: **n?o testado**; n?o houve mudan?a de interface.

Logs locais (ignorados pelo git) em `windows/app/temp/`: `organizacao-build-antes.log`, `organizacao-test-antes.log`, `test-tail-antes.log`, `build-depois-final.log`, `test-depois-final.log`, `test-tail-depois.log` e `package-org.log`.

Não foram executados `chat-audit.cmd`, `call-audit.cmd`, medições com interface ou e2e do instalador: **não testado**, reservados ao dono. Nenhum pacote foi instalado nem o programa instalado do dono foi alterado.

Ajustes em `instalar/`: somente referências a fontes/headers movidos e ao módulo de e-mail movido. `queixas/`, arquivos rastreados de `test-output/`, `windows/native/`, `CLAUDE.md` e o histórico ficaram sem alterações.

## Commits

- `83e4d65`: somente movimentações, para tornar a separação revisável.
- O commit seguinte contém os ajustes de caminhos e a documentação.
