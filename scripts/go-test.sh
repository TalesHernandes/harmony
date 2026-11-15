#!/usr/bin/env bash
#
# `go test ./... -race`, sem o Windows perguntar do firewall a cada corrida.
#
# O problema, medido em 18/09/2026 na máquina do dono. O Windows guarda a
# resposta daquela caixa de "permitir este aplicativo?" **pelo caminho do
# arquivo**, e `go test -race` linka o binário numa pasta temporária nova toda
# vez — então a caixa volta a cada corrida, e a máquina dele tinha acumulado
# mais de quatrocentas regras de três binários nossos. Sem `-race` isso não
# acontece: aí o Go roda o binário direto do cache, que é um caminho estável.
#
# Não é o endereço do socket. Foi a primeira suspeita e estava errada: mesmo com
# tudo preso em 127.0.0.1 (o servidor de mídia faz isso desde o D4n) a caixa
# continuava aparecendo, e a medição — contar as regras, rodar, contar de novo —
# mostrou que só as corridas com `-race` criavam regra.
#
# O conserto é dar um caminho fixo a cada binário de teste, com `-o`. Depois da
# primeira vez que você permitir, nenhum deles pergunta de novo.
#
#   bash scripts/go-test.sh              # todos os pacotes, com -race
#   bash scripts/go-test.sh ./internal/harmony/sfu/
#
# Os binários ficam em .gotestbin/, que está no .gitignore. Apagar a pasta só
# custa uma pergunta do firewall por binário na corrida seguinte.
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p .gotestbin

# Só os pacotes que têm teste: `go test -o` num pacote sem teste ainda linka um
# binário, e são dez segundos por nada.
if [ $# -gt 0 ]; then
	pacotes=$(go list -f '{{if or .TestGoFiles .XTestGoFiles}}{{.ImportPath}}{{end}}' "$@")
else
	pacotes=$(go list -f '{{if or .TestGoFiles .XTestGoFiles}}{{.ImportPath}}{{end}}' ./...)
fi

falhou=0
for p in $pacotes; do
	nome=$(basename "$p")
	if ! go test -race -count=1 -o ".gotestbin/$nome.test.exe" "$p"; then
		falhou=1
	fi
done
exit $falhou
