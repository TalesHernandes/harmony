# Onde encontrar as coisas

- `src/`: código e recursos compilados no programa, incluindo os headers da interface.
- `testes/`: testes em uso e auxiliares de compilação e auditoria.
- `arquivo/`: scripts, análises e resultados de sprints encerrados; guardados como história.
- `instalar/`: criação do pacote, instalador e suas verificações.
- `assets/`: fontes, imagens e outros recursos usados pelo programa.
- `queixas/`: medições e evidências das verificações de uso real.
- `test-output/`: saídas geradas pelos testes e pacotes locais; não é código do programa.

Os comandos atuais continuam na raiz desta pasta. Execute a partir da raiz do repositório:

```bat
cmd /c windows\app\build.cmd
cmd /c windows\app\test.cmd
```

As auditorias que abrem a interface são executadas separadamente pelo dono:

```bat
cmd /c windows\app\chat-audit.cmd
cmd /c windows\app\call-audit.cmd
```

O projeto e as configurações de compilação ficam na raiz, junto de `THIRD-PARTY.md`.
O inventário e os resultados desta organização estão em `ORGANIZACAO.md`.
