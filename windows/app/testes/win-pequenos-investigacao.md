# Windows: captura sem espectador e cópia do NVENC

Investigação de 13/11/2026. Nenhuma decisão de captura, taxa de produção ou caminho do NVENC foi alterado. O acréscimo é um modo de medição do `gpu_selftest`: `--preview-capture-probe`, em `windows/native/gpu/selftest/main.cpp:241`.

## Captura com a transmissão sem espectadores

O custo existe: `windows/native/src/voice.cpp:833` mantém o limite da prévia local em `min(taxa pedida, frequência do monitor, 30)`. Havendo alguma camada ativa, restaura a taxa pedida **antes** de ativar o encoder. A fonte continua aberta. Isso não autoriza desligar a captura por um estado transitório.

O probe usa a captura WGC real, padrão sintético 1920×1080 desenhado a 60 Hz, consumidor local que conta quadros, nenhum encoder e nenhum espectador remoto. Aguarda 1,5 s e mede 6 s por fase, sem fechar/reabrir a fonte. O executável final foi recompilado de todas as fontes GPU com `windows/native/build-gpu.cmd`, em `windows/app/test-output/pequenos-gpu-current`. Os dois snapshots registraram `foreground_owned=0`.

| Limite solicitado | Quadros em aproximadamente 6 s | CPU da thread de captura, % de um núcleo | Tempo acumulado da etapa `capture`, ms |
|---|---:|---:|---:|
| 30 Hz | 140 | 1,30177 | 114,918 |
| 5 Hz | 28 | 3,38416 | 33,8732 |

O limite de 30 Hz não produziu 30 quadros por segundo nesta janela: foram cerca de 23,3. A etapa contabiliza tempo de parede dos quadros concluídos, não tempo de GPU medido por timestamps. A fonte ainda recebe callbacks WGC quando descarta quadros. Não há economia de CPU comprovada ao baixar para 5 Hz: o contador subiu nesta corrida e variou nas explorações anteriores. O padrão desenhado, driver e WinRT também consomem recursos; não se deve atribuir a CPU total do processo à captura.

A memória privada do processo variou de 86,258 MiB para 281,816 MiB entre os snapshots; a residente ficou aproximadamente em 58,531/58,812 MiB. Não foi isolada a origem dessa variação. Portanto também não há ganho de RAM demonstrado. Contadores completos: `pequenos-medidas-captura.json`. Jogo, filme, prévia renderizada e transmissão real: não conferidos neste probe.

### Por que ficou em 30

O commit `3b190c9e` (25/10/2026, “Pace preview-only capture without limiting viewers' requested frame rate”) introduziu `SetCaptureRate`: alinha a captura à prévia local, que desenha até 30 Hz. Separou as restrições da fonte dos limites dos sinks porque estes agregam pelo mínimo e poderiam limitar também os espectadores. A retomada da taxa pedida antecede `SetParameters`.

O commit `6013ee40` (20/09/2026) explica a travada: o usuário voltou após 1,3 s, uma guarda retornou antes de cancelar um temporizador antigo de 5 s, e esse temporizador desativou a camada alta ainda necessária. O SFU considerava viva uma camada sem pacotes e não recuperava o espectador já estabelecido. Não foi simplesmente um número de quadros baixo.

Não recomendo reduzir a taxa em produção com a economia ainda não demonstrada. Se a hipótese voltar a ser testada, a condição proposta é ausência **confirmada e contínua** de espectadores por 30 s, com prévia local e PiP também invisíveis. Presença desconhecida, reconexão e negociação não contam como zero. Uma geração invalida a espera em qualquer retorno de espectador, inclusive no caminho em que a configuração desejada já parece igual à atual. A tarefa atrasada revalida tudo ao vencer; a volta restaura imediatamente a taxa pedida antes de ativar camadas. Manter a mesma fonte/track/PeerConnection. Essa histerese é proposta, não segurança comprovada: exige testes de retorno rápido, temporizador obsoleto, desconexão e retomada a 60 Hz antes de qualquer mudança.

## Registrar diretamente a textura na camada alta

O custo existe: `windows/native/gpu/nvenc.cpp:154` chama `Blit` em todo `Submit`, mesmo com tamanho/formato compatíveis. O encoder registra sua textura particular uma vez, no construtor, e a reutiliza.

A API aceita recursos DirectX externos, registrados e mapeados como entrada; exige desmapear e remover o registro antes de destruir o recurso. Isso torna o caminho direto possível em princípio. [Documentação oficial NVIDIA, SDK 13.0, §4.1.2](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html#input-buffers-allocated-externally).

Pelo código, seria preciso conferir dispositivo, tamanho, formato e subresource, manter a textura viva até `Collect`/`Abort` e impedir escrita/reuso durante a codificação. A captura cria uma textura destacada por quadro; registrar/desregistrar cada uma troca o custo da cópia por chamadas ao driver. Um cache sem limite reteria texturas. A prévia e as outras camadas compartilham a entrada, e `encoder.cpp:262` envia todas antes de coletar: leituras concorrentes e sincronização precisam de teste específico. Também há o histórico `21e17725`, que retirou esperas de dentro do driver D3D para evitar travadas.

Tempo de registro/desregistro, economia de GPU e comportamento direto com esse driver: **não medidos**. Nenhum caminho direto foi implementado ou testado. Um experimento isolado precisa comparar registro por quadro com um pool pequeno, incluindo falha/aborto e fallback, antes de justificar a alteração no aplicativo.
