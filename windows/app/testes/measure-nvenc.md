# Como repetir a medição do NVENC

O relatório desta rodada está em `win-codificador-relatorio.md`. As medições por
quadro, metadados e logs ficam no arquivo `win-codificador-dados.zip`; ele contém
somente números e descrições, sem quadros de vídeo ou bitstreams.

Na raiz deste worktree, usando apenas as dependências já instaladas:

```powershell
cmd /c windows\native\build-gpu.cmd
python -B windows/app/testes/measure-nvenc-tests.py
python -B windows/app/testes/measure-nvenc.py --videos <pasta-de-trabalho>/trechos -o windows/app/testes/win-codificador-relatorio.md
```

O script lê `jogo-1.mp4` e `jogo-2.mp4`, sem selecionar áudio. Os arquivos de entrada
não são alterados. Não abre janela nem injeta mouse/teclado. Não executa o programa
instalado. O modo novo do `gpu_selftest` usa o `NVEncoder` e o decodificador reais.
Os argumentos de teste são opcionais; os chamadores de produção continuam com os
mesmos padrões.

Para recalcular o relatório sem acessar os vídeos nem executar o codificador,
extraia `win-codificador-dados.zip` nesta pasta (ele contém `nvenc-results/`) e rode:

```powershell
python -B windows/app/testes/measure-nvenc.py --report-only -o windows/app/testes/win-codificador-relatorio.md
```

As tabelas principais reúnem os quadros de todas as rodadas e recalculam os
percentis. O apêndice preserva as medidas de cada rodada separadamente.

Os resultados temporários ficam em `nvenc-results/` (ignorado pelo git). O script
reaproveita rodadas completas que encontrar ali. Para outra campanha de medições,
use uma cópia limpa dessa pasta, sem misturar arquivos de outras versões do chicote,
da placa, do driver ou dos vídeos. A cópia parcial é atualizada em
`<pasta-de-trabalho>/win-codificador-parcial.md` ao terminar cada trecho.

Uma execução curta para conferir o caminho, sem produzir a campanha completa:

```powershell
windows/native/build/gpu_selftest.exe --measure-encoder <pasta-de-trabalho>/trechos/jogo-1.mp4 "windows/app/testes/nvenc-results/conferencia-{config}" hoje,vbv2,vbv4,p3,p4,aq,low 15 120
```

Até sete sessões ficam residentes, mas só uma codifica de cada vez. A ordem de
codificação alterna a cada quadro. O leitor do vídeo e a referência são compartilhados;
o histórico de compressão de cada configuração é independente. A decodificação
mantém esse histórico e ocorre em blocos de cinco segundos. Há oito texturas de
leitura por decodificador para evitar esperar a GPU a cada callback. Nada disso muda
os ajustes do aplicativo. O tempo medido vai de `Submit` até `Collect`, sem incluir
leitura do arquivo, cálculo de nitidez ou decodificação.

A comparação de configurações mede a oferta de bytes na cadência de 60 fps, sem
simular uma conexão. O maior bloco de seis quadros é a janela de 100 ms, sem
cabeçalhos de transporte e sem pacer. A taxa real usa o número de quadros dividido
por 60, mesmo quando há repetições da fonte.

A combinada é escolhida separadamente para cada jogo, depois das sete alternativas:
VBV com maior soma de PSNR no IDR e nos cinco seguintes; preset com maior PSNR médio;
AQ quando melhora o intervalo escuro; low latency quando melhora a média. Só entram
ganhos maiores que 0,1 dB na soma de IDR+pós, ou 0,05 dB nas outras escolhas. Esses
limiares são uma regra do chicote para escolher um candidato, não limiares perceptuais.
A combinada precisa ser medida; os ganhos isolados não são somados como previsão.
Se a média escolher o tuning low latency, o chicote mede também a mesma combinada
em ultra low latency. Esse controle verifica diretamente se o ganho de média vem
com perda nos piores quadros, na recuperação após IDR ou no envio. As duas ficam
no relatório; a média sozinha não decide a recomendação.

Duas rodadas são obrigatórias. Se a diferença do PSNR médio passar de 0,3 dB ou a
diferença do tempo médio passar de 10% do menor tempo, o grupo roda uma terceira vez.
O relatório preserva todas as rodadas. O próprio benchmark confere o cálculo de erro
quadrático contra uma conta escalar; o teste Python confere as estatísticas contra
uma série conhecida, incluindo recuperação pós-IDR e rejeição de quadros faltantes.
