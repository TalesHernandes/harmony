# Retransmissor das chamadas diretas do harmony

O coturn retransmite áudio, câmera e tela das chamadas 1:1 do harmony quando
a rede impede a conexão direta entre os participantes.

Roda na máquina `usuario@maquina-do-backup`, na pasta `~/call-app`. Ali, só o
serviço `coturn` deve estar de pé: o site e a sinalização do app antigo foram
aposentados em 03/11/2026. A máquina também continua recebendo os backups do harmony.

Este compose e `turnserver.conf` ficam juntos na pasta de operação. O arquivo
local `.env`, não versionado, pode ser criado a partir de
[`.env.example`](.env.example), substituindo os valores de exemplo, e fornece
`TURN_SECRET`, `TURN_PRIVATE_IP` e
`TURN_PUBLIC_IP`. O harmony aponta para esse retransmissor pelas variáveis
`HARMONY_TURN_URLS` e `HARMONY_TURN_SECRET`.

Esta mudança só reorganiza os arquivos do repositório; a atualização da
máquina é responsabilidade do Claude Code.
