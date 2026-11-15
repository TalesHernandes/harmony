# Captura do microfone e saída de mídia

O AAR original continua fixado em Shiguredo **155.8059.1.0**, SHA-256
`65ebb4faf70d32b606d2132ca2f327aa92c365ac8928278cfc7b51b8a9407fc4`.
O [VERSION do mantenedor](https://raw.githubusercontent.com/shiguredo-webrtc-build/webrtc-build/master/VERSION)
informa WebRTC **M155.8059@{#1}**, commit
`c4f21b1f91386bae6d710540976dff5aae67b3c5`.

`upstream/WebRtcAudioRecord.java` é o conteúdo Java consultado no
[Gitiles, refs/heads/main](https://webrtc.googlesource.com/src/+/refs/heads/main/sdk/android/src/java/org/webrtc/audio/WebRtcAudioRecord.java),
blob `2a59e364d71f3131625a252b5aff5a6a9332fcc3`.
A consulta ao arquivo pelo commit fixo não funcionou nesta sessão. Não se
afirma que esse download veio do commit fixo. A base funcional foi confirmada
contra **o binário fixo**, conforme abaixo; a extração textual ajustou a indentação.

`baseline/WebRtcAudioRecord.java` conserva essa implementação e recupera do
`javap -p -c` do AAR o estado de gravação e os métodos de pausa/retomada do
Shiguredo (ausentes no arquivo upstream consultado). Sua recompilação confere
com **todas as classes `WebRtcAudioRecord*` originais**: declarações, descritores,
instruções, destinos de salto, tabelas de exceção e receitas de `invokedynamic`.
A comparação ignora apenas os índices do constant pool. Assim, o script recusa
uma base incompatível, em vez de presumir que o fonte upstream serve ao Shiguredo.

`src/org/webrtc/audio/WebRtcAudioRecord.java` adiciona somente o diff de
`capture.patch`: visibilidade pública, interface/gancho estático, uma referência
por captura e chamadas de início, processamento e liberação. As duas chamadas
de ciclo de vida permitem preparar/liberar o modelo fora do laço de áudio.
O `process` vem antes do mute, do JNI e da cópia para o callback. O mute
continua zerando o quadro **depois** do filtro, inclusive seu histórico.
O segundo ponto, `processAfterMute` (método padrão vazio para manter os
processadores existentes compatíveis), vem **depois de zerar o mute** e antes
do JNI. O app usa esse ponto para somar a reprodução do celular: o jogo não
passa pelo DeepFilterNet nem é apagado pelo mudo. Como o mudo da sala antes
desabilitava a faixa inteira, enquanto há som da tela o app mantém a faixa
ligada e zera apenas a voz nesse segundo ponto. Ao parar o som, restaura o
mudo da faixa antes de remover esse ponto de mistura.

Execute na raiz do projeto:

```powershell
$env:JAVA_HOME = 'C:\Program Files\Android\Android Studio\jbr'
bash android/scripts/fetch-webrtc.sh
```

Python (somente biblioteca padrão) e JDK 25 são necessários. O original tem
major version 69; `javac -source 25 -target 25 -proc:none` preserva esse alvo.
O SDK vem de `android/local.properties`, campo `sdk.dir`; a plataforma segue
`compileSdk`. Duas declarações de anotações servem apenas à compilação e não
entram no AAR. As anotações JNI existentes vêm do `classes.jar` original.

O script verifica o SHA antes de qualquer remendo, recompila e compara a base,
confere os descritores originais no remendo e a ordem **no bytecode compilado**
(`process` → leitura do mute → `put(emptyBytes)` → `processAfterMute` → JNI →
callback), troca apenas
`WebRtcAudioRecord*.class`, `WebRtcAudioTrack*.class` e as classes novas de
`PlaybackRoute`, e confirma que o restante do JAR, o manifesto e o
`.so` ficaram iguais byte a byte. Só publica o arquivo depois dessas verificações.
Rodar de novo gera exatamente os mesmos bytes. O app usa
`app/libs/libwebrtc-155.8059.1.0-capture.aar`; o original fica guardado para a
verificação. Nenhum desses AARs entra no git.

## Reprodução como mídia (06/10/2026)

`baseline/WebRtcAudioTrack.java` veio do
[fonte Java do WebRTC](https://webrtc.googlesource.com/src/+/refs/heads/main/sdk/android/src/java/org/webrtc/audio/WebRtcAudioTrack.java),
blob `58f2743d16c9863ce47c8a5d5abca445bc229400`, extraído do Gitiles sem a
indentação. Sua recompilação também é comparada com o bytecode do AAR fixado;
o script recusa qualquer diferença funcional na base. Não se presume que o
fonte atual seja igual ao binário: a comparação foi executada e passou.

O remendo inicial de `playback.patch` tratava da escolha da saída de mídia
(`AudioTrack.setPreferredDevice`) e dos métodos de volume/taxa de saída, que
usam `STREAM_MUSIC`; a medição acrescentada está descrita abaixo.
`PlaybackRoute.setPreferredDevice` publica somente o
dispositivo; a thread de reprodução aplica a preferência entre escritas,
sem trocar atributos ou recriar o `AudioTrack`. Os atributos são fixados
uma vez, no `JavaAudioDeviceModule` (`USAGE_MEDIA` / `CONTENT_TYPE_MOVIE`).
O padrão da classe remendada também é mídia. A API pública do módulo não
expõe a escolha da saída: retirar esse gancho impediria o seletor de pedir
alto-falante com fone conectado. A preferência aceita ainda depende da
política de áudio do fabricante; recusas entram no callback de diagnóstico.

Na segunda passada (06/10/2026), saíram a reconstrução do dispositivo de
reprodução e toda a troca entre mídia/comunicação. `AudioRoute` usa sempre
`MODE_NORMAL`, lista saídas comuns e seleciona A2DP, sem solicitar SCO.
Não há remendo de captura novo: DeepFilterNet e mistura continuam intactos.
`Engine` usa `VOICE_RECOGNITION`, desliga AEC/NS por hardware e mantém o eco
habilitado nas opções das fontes para AudioProcessing/AEC3 em software.
A2DP não fornece o microfone do headset clássico; a entrada fica local.
O volume, eco e roteamento reais precisam ser testados no Redmi.

## Medição da saída para sincronia (16/10/2026)

`PlaybackRoute.output()` entrega uma amostra imutável com tipo numérico da saída
real (`AudioTrack.getRoutedDevice`), atraso em ms e instante monotônico.
`outputLatencyMs()` retorna `-1` enquanto não há leitura válida/estável;
`routedDeviceType()` retorna `0` quando a saída é desconhecida. Não expõe nome,
endereço ou identificador do fone. Não usa reflexão nem API oculta.

A thread soma somente os bytes efetivamente aceitos por `AudioTrack.write`.
Para PCM16, quadros escritos = bytes / (2 × canais). Com o timestamp público:

`atraso_ms = (quadros_escritos − posição_timestamp) × 1000 / sampleRate + (timestamp.nanoTime − System.nanoTime()) / 1e6`.

O cálculo estima o tempo até a apresentação da cauda já escrita; não confunde
tamanho do buffer com latência medida. Compensa a volta dos 32 bits da posição
usando a contagem acumulada. Na troca de rota, underrun, falha de escrita,
timestamp ausente ou ao terminar a thread, invalida a amostra. Há verificação de
rota a cada 250 ms; o timestamp é consultado a cada 250 ms durante estabilização
e a cada 10 s depois de estável. Três intervalos consecutivos precisam avançar
com taxa a até 2% da nominal e variação de atraso a até 20 ms. Rejeita posição
inválida, timestamp distante mais de 2 s e atraso fora de 0..2000 ms. O getter
expira amostras após 12 s sem atualização. Esses limiares são escolhas de
validação do remendo, **não medições do Redmi**.

A [documentação de getTimestamp](https://developer.android.com/reference/android/media/AudioTrack#getTimestamp(android.media.AudioTimestamp))
prevê indisponibilidade durante mudanças de rota e recomenda reduzir consultas
quando o relógio estabilizar. Garantia de que todo o atraso acústico A2DP está
incluído, ou afirmação de que está excluído: **não encontrado** nessa fonte.
O teste físico com/sem Bluetooth continua necessário.

`build.py` conserva a comparação integral da base com o AAR fixado, os
descritores JNI e a igualdade dos demais arquivos. Também verifica no bytecode
`write → Meter.written → rewind`, a invalidação ao sair, os getters e as chamadas
públicas de timestamp/rota. `playback.patch` usa diff sem linhas de contexto para
evitar espaços finais em linhas vazias; o script confere o diff exato.

O fonte upstream permanece sob a licença BSD do WebRTC, com os avisos do próprio
arquivo: [LICENSE](https://webrtc.googlesource.com/src/+/main/LICENSE),
[PATENTS](https://webrtc.googlesource.com/src/+/main/PATENTS),
[AUTHORS](https://webrtc.googlesource.com/src/+/main/AUTHORS).
O comportamento de pausa/retomada do Shiguredo provém do seu AAR e dos seus
[remendos Apache-2.0](https://github.com/shiguredo-webrtc-build/webrtc-build/tree/master/patches).
