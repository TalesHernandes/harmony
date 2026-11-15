# Recursos reutilizados

Os nove pacotes de compilação e runtime são os mesmos de
`../native/telas/THIRD-PARTY.md`, com as versões e SHA-256 do W1.
Não há pacote novo. WebView2 continua apenas como referência exigida pela
compilação do WinUI: nenhuma tela deste app contém um navegador.

A fonte Inter vem do arquivo já versionado no cliente web,
`web/harmony/src/fonts/inter-latin.woff2`. O DirectWrite do Windows descompacta
o arquivo em `assets/Inter.ttf` durante o build, sem download ou ferramenta
adicional. Inter é distribuída sob a SIL Open Font License 1.1.
O ícone é o próprio `windows/res/harmony.ico` do harmony.
O desenho do fone em `assets/headphones.xaml` copia o caminho e a posição
de `web/harmony/src/Icon.tsx`, do kit que o web já utiliza.
O aviso e a licença da Inter acompanham a pasta em `assets/Inter-LICENSE.txt`.

O W3 liga com o mesmo libwebrtc Shiguredo m155.8059.4.1 já aprovado e
usado pelo W1, em `%USERPROFILE%\harmony-deps`. O adaptador de mixer e o
resampler usam essa biblioteca, sem pacote adicional. As licenças do
libwebrtc e de seus componentes estão no `assets/webrtc-NOTICE.txt`,
copiado sem alteração do bundle aprovado; W3 não assina nem publica.
Os cinco ícones adicionais também copiam os caminhos do `Icon.tsx` do web.
DeepFilterNet **0.5.6**, libDF/C API, aprovado pelo dono em 06/10/2026:
`df.dll` e `DeepFilterNet3_onnx.tar.gz` são distribuídos junto ao executável,
dentro de cada pasta de versão. O código é MIT **ou** Apache-2.0; as três
licenças originais acompanham `licenses/DeepFilterNet-0.5.6`. O modelo é o
arquivo oficial do mesmo repositório usado pelo web; licença específica dos
pesos, distinta da licença geral do projeto: **não encontrado** nos fontes
locais. A aprovação do dono inclui esse arquivo sem alterar a DLL.

| Arquivo | Bytes | SHA-256 |
| --- | ---: | --- |
| `df.dll` | 16.678.912 | `4040ce3da1dd1c26e15d2615f49fc38a92566a0384a0c4f1d149b8fe852f320f` |
| `DeepFilterNet3_onnx.tar.gz` | 7.983.136 | `c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616` |

Build e instalador conferem tamanho e SHA-256. O programa também confere
esses artefatos antes de carregar o modelo, com caminho absoluto em UTF-8.
As gravações `clean_freesound_33711.wav` (CC BY-NC 3.0) e
`noise_freesound_2530.wav` (CC BY 3.0), descritas no `assets/README.md` dos
fontes oficiais locais, são apenas provas de teste. **Não entram no pacote.**
