#pragma once
#include <vector>

// What each distribution code brought, in the owner's words, newest first.
// Shown by clicking "Versão N" (the owner, 25/10/2026: "um log de atualizações
// clicável ... retroativo também"). Written from docs/DEPLOY.md at each
// release; codes that never reached anyone (2, 3, 6) are folded into the next.
struct ChangelogEntry { int version; wchar_t const* date; std::vector<wchar_t const*> items; };

inline std::vector<ChangelogEntry> const& Changelog() {
  static std::vector<ChangelogEntry> const entries{
    {54, L"13/11", {
      L"Ao transmitir a tela, a imagem chega bem mais limpa para quem acabou de entrar, quando troca de qualidade e depois de uma falha na conexão; antes ela chegava borrada e ia limpando."}},
    {53, L"13/11", {
      L"Na bandeja ou minimizado, o harmony acorda muito menos: a conexão, os relógios internos e o diagnóstico só trabalham quando há algo a fazer.",
      L"Passar o mouse sobre um botão só troca a cor do ícone, em vez de desenhá-lo de novo.",
      L"Alguém ficar online, ausente ou em chamada atualiza só a linha dessa pessoa na coluna Pessoas.",
      L"Na sala, a luz de quem está falando e os medidores dão menos trabalho ao programa, e a câmera gasta menos processador.",
      L"Arrastar um volume grava o ajuste ao soltar, não a cada passo; fotos de perfil pequenas ocupam menos memória."}},
    {52, L"13/11", {
      L"A tela de alguém para de chegar depois de 30 segundos sem você olhar (lendo um canal de texto, janela minimizada ou na bandeja) e volta na hora quando você olha.",
      L"No mudo, o filtro de ruído para de trabalhar; volta em dois décimos de segundo ao abrir o microfone.",
      L"Transmitir a tela ocupa menos memória de vídeo.",
      L"Imagens nas mensagens ocupam cerca de um décimo da memória; ao abrir a imagem ela continua inteira."}},
    {51, L"13/11", {
      L"Atualizar, reparar ou desinstalar não esbarra mais na conferência em segundo plano em contas do Windows com letra maiúscula acentuada no nome (Álvaro, Érica).",
      L"Sair do harmony com um aviso do Windows ainda na fila fecha na hora, sem mostrar o aviso depois."}},
    {50, L"13/11", {
      L"Chamada direta: o mudo corta o microfone no instante do clique, como já era na sala.",
      L"Chamada direta: se a conexão com o servidor cair bem na hora em que a outra pessoa atende ou desliga, a chamada se acerta quando a conexão volta.",
      L"Sair do harmony durante o envio de um arquivo cancela o envio e fecha na hora.",
      L"Menos trabalho para transmitir a tela em computadores sem placa NVIDIA, e para o filtro de ruído e as câmeras."}},
    {49, L"13/11", {
      L"O harmony abre mais rápido: antes de abrir ele confere só o tamanho e a data dos arquivos, e a conferência completa roda depois, em segundo plano.",
      L"Depois de uma atualização, a primeira abertura ainda confere tudo antes de abrir."}},
    {48, L"12/11", {
      L"A tela de alguém, dentro da sala e da chamada direta, acompanha o monitor até 60 quadros; antes parava em 30.",
      L"Chamada direta: escolher outra câmera com a câmera ligada troca de verdade.",
      L"Fixadas: quem pode gerenciar mensagens é quem pode desafixar pelo painel.",
      L"Um aviso do Windows não trava mais a janela enquanto é confirmado.",
      L"Menos memória presa ao rolar conversas, e uma resposta do servidor cortada no meio não apaga mais o que você escreveu."}},
    {47, L"12/11", {
      L"Mudo e \"não ouvir\" na sala de voz respondem na hora, mesmo com a internet lenta.",
      L"Com a rede oscilando e a tela de alguém aberta, a voz volta mais rápido: o pedido do tamanho da imagem espera entre as tentativas.",
      L"Fechar o programa não corre mais o risco de deixá-lo preso sem abrir de novo."}},
    {46, L"03/11", {
      L"Perfil: silenciar alguém cala só o microfone para você; o som da tela e seu volume continuam separados.",
      L"Bloquear recolhe as mensagens, cala voz e tela e impede novas mensagens privadas e chamadas entre vocês. A pessoa não é avisada.",
      L"Ajustes → Conta: pessoas silenciadas e bloqueadas, para desfazer. Sua escolha acompanha os seus aparelhos ao vivo."}},
    {45, L"03/11", {
      L"Perfil: quem gerencia cargos dá e tira cargo ali mesmo, pelo \"+ Dar cargo\" e pelo × de cada cargo, sem abrir os Ajustes.",
      L"Ajustes → Cargos: a lista de \"Dar o cargo a…\" ficou maior e mais espaçada, com a foto de cada pessoa.",
      L"Arrastar um cargo mostra só uma barra entre as linhas, onde ele vai cair, no lugar da borda."}},
    {44, L"03/11", {
      L"Cargos: o nome de cada pessoa aparece na cor do cargo dela, nas mensagens, na lista de pessoas e no perfil, e o perfil mostra os cargos (\"Dono\" no dono).",
      L"Cargo marcado \"mostrar separado\" ganha um grupo próprio na lista de pessoas, com quem está online.",
      L"Ajustes → Cargos, para quem gerencia cargos: criar, nome, cor, permissões, quem tem, arrastar a ordem e apagar.",
      L"Apagar a mensagem dos outros e fixar seguem a permissão Gerenciar mensagens; organizar canais segue Gerenciar canais. Tudo muda na hora, sem reabrir o programa."}},
    {43, L"03/11", {
      L"O programa passa a rodar sempre da mesma pasta: o Firewall do Windows pergunta uma última vez e depois não pergunta mais a cada versão.",
      L"O relógio da barrinha da chamada 1:1 não muda mais de largura a cada segundo."}},
    {42, L"02/11", {
      L"Fixar mensagem volta a funcionar no Windows: o pedido saía como uma leitura e o servidor respondia que a mensagem não existia."}},
    {41, L"02/11", {
      L"Os botões na beira direita de um painel (o x do perfil, as pessoas, os canais) pegam o clique até a borda: uma barra de rolagem invisível ficava por cima deles."}},
    {40, L"02/11", {
      L"A sua câmera na chamada aparece inteira e nítida: um celular usado como câmera (Camo) manda a imagem em pé, e ela era encolhida e cortada no meio.",
      L"Sem borda cinza em volta das câmeras: só a verde, quando a pessoa fala.",
      L"A janelinha da chamada direta não abre mais sozinha: ela abre pelo botão dela, no topo da chamada ou na faixa \"Ver chamada\", e abre de novo depois de fechada.",
      L"A faixa \"Ver chamada\" ficou do tamanho do que mostra, com outra cor e sombra, e dá para arrastar com o mouse para onde quiser.",
      L"O ícone, o nome e o tempo no topo da chamada ficam na mesma linha.",
      L"A barra de ações da mensagem mostra todos os botões, o de apagar inclusive; o alfinete agora é desenhado em traço, como os outros.",
      L"Clique direito abre o menu onde está o mouse. Na imagem, ele oferece copiar ou baixar.",
      L"A menção (@nome) tem o fundo arredondado e mais visível.",
      L"Dá para procurar dentro das mensagens fixadas.",
      L"No topo da janela aparece onde você está (harmony · #geral)."}},
    {39, L"30/10", {
      L"O + ao lado de CANAIS e de VOZ cria um canal (só o dono); \"Apagar canal\" no clique direito.",
      L"Abrir um canal para na linha NÃO LIDAS, ou no fim quando não há."}},
    {38, L"30/10", {
      L"Pastas de canais: abrir e fechar, e (só o dono) criar, renomear, apagar e arrastar canais e pastas com o mouse.",
      L"Fixar mensagens, e o alfinete no topo da conversa com a lista das fixadas."}},
    {37, L"30/10", {
      L"Quem fixa uma mensagem aparece numa linha da conversa, com o alfinete."}},
    {36, L"30/10", {
      L"Mandar um link na hora já mostra o cartão da prévia: antes ele chegava meio segundo depois e ficava escondido embaixo da conversa, e parecia que só aparecia esperando com o link na caixa."}},
    {35, L"30/10", {
      L"Prévia de links, como no Discord: um link na mensagem ganha um cartão com o site, o título, a descrição e a imagem; vídeo do YouTube aparece com a capa. Clicar abre no navegador. Quem busca a página é o servidor, então o site não fica sabendo do endereço de ninguém.",
      L"Links no texto da mensagem agora são azuis e clicáveis."}},
    {34, L"29/10", {
      L"Ouvir minha voz agora funciona dentro da sala e da chamada direta, como retorno de voz: continua ligado com os Ajustes fechados, some quando você fica mudo e acaba quando você sai da chamada.",
      L"Ouvir minha voz com um pouco menos de atraso ainda.",
      L"O destaque azul de uma menção (@nome) fica em cima do nome, não duas letras à frente."}},
    {33, L"29/10", {
      L"Ouvir minha voz responde mais rápido: a sua voz volta para o fone com menos atraso.",
      L"Enviar diagnóstico agora leva a chamada inteira (até 6 horas), não só os últimos 10 minutos."}},
    {32, L"29/10", {
      L"Sons: o de entrar toca no clique; a sua própria live toca ao começar e acabar; o fim de qualquer live agora toca para a sala toda.",
      L"Ajustes → Voz e vídeo → Ouvir minha voz: com fone de ouvido, você ouve a sua própria voz com os filtros ligados, como os outros ouvem, e escuta na hora o efeito de mexer na força do DFN e nos outros filtros."}},
    {31, L"29/10", {
      L"Transmitindo um programa que fecha a janela e abre outra (o cliente do LoL ao entrar na partida), a live segue sozinha para a janela nova do mesmo programa, com o som, sem parar.",
      L"O filtro de ruído (DFN) não abaixa mais as sílabas fracas da sua voz: enquanto você fala ele pega leve, e nas pausas continua tirando teclado e mouse.",
      L"O harmony ganhou sons próprios: um \"pop\" quando alguém entra ou sai da sala, ao mutar e desmutar, e quando uma tela começa ou acaba.",
      L"A chamada direta toca o toque novo, e quem liga ouve o mesmo toque mais baixo. Atender, desligar e chamada recusada também têm som.",
      L"A janelinha da chamada direta é opcional (Ajustes → Aplicativo), tem um x para fechar nesta chamada, mostra os botões só com o mouse em cima e muda de tamanho puxando os cantos, com a imagem nítida no tamanho novo.",
      L"As câmeras que flutuam sobre a imagem ficam nítidas ao aumentar, com a borda verde fina de sempre, e preenchem o bloco arredondado, sem faixas pretas.",
      L"A sua câmera na chamada direta preenche o bloco, como no web."}},
    {30, L"29/10", {
      L"Na chamada direta, a câmera na miniatura fica com os cantos arredondados, como o realce de quem fala.",
      L"Minimizar o harmony ou voltar ao app durante uma chamada direta com a câmera ou a tela do outro ligada deixa a imagem numa janelinha por cima de tudo, com microfone e desligar. Clique nela para voltar à chamada; arraste para mudar de lugar.",
      L"Ajustes → Diagnóstico → \"Preparar para enviar\": junta a última gravação da chamada num arquivo só, em Downloads, para mandar a quem pediu."}},
    {29, L"26/10", {
      L"Ajustes → Diagnóstico → \"Gravar o áudio da próxima chamada\": grava 1 minuto da chamada (sua voz antes e depois dos filtros, o que você ouve e a conexão) para entender eco e voz ruim. Fica só no seu PC, com um botão para abrir a pasta."}},
    {28, L"25/10", {
      L"A chamada direta tem a mesma tela da chamada da sala: clicar numa tela deixa grande, a faixa de pessoas se esconde com as câmeras flutuando por cima, e tem tela cheia.",
      L"Na chamada direta, o volume da voz e o do som da tela de cada pessoa valem como na sala (botão direito na pessoa, controle na tela).",
      L"A câmera de um amigo no celular aparece em pé, e não mais deitada."}},
    {27, L"25/10", {
      L"O harmony abre mais rápido: de cerca de 3,9 para 2,3 segundos até a lista aparecer, com o computador já ligado há um tempo.",
      L"Depois de sair de uma chamada com tela ou câmera, o programa devolve a memória da placa de vídeo em vez de guardá-la até fechar."}},
    {26, L"25/10", {
      L"Clicar em \"Versão\" mostra este histórico."}},
    {25, L"25/10", {
      L"Com a faixa de pessoas escondida, as câmeras flutuam sobre a live: arraste para mover, puxe as bordas para mudar o tamanho, clique para pôr no grande.",
      L"Na tela cheia, o mouse some junto com os botões quando fica parado.",
      L"O clique do meio para rolar mostra o cursor de rolagem.",
      L"O instalador mostra o número certo da versão."}},
    {24, L"25/10", {
      L"Depois que o servidor reinicia, a sua voz volta sozinha, sem precisar sair e entrar na sala.",
      L"Minimizar ou fechar a janela que você transmite não deixa a tela preta: quem assiste vê a última imagem com um aviso.",
      L"Jogos em tela cheia exclusiva continuam aparecendo para quem assiste."}},
    {23, L"25/10", {
      L"Assistir a live como no web: clicar deixa grande, com os outros numa faixa que se esconde, e tela cheia dentro do próprio programa.",
      L"Volume da voz e do som da live separados, direto no perfil da pessoa, e mais forte acima de 100%.",
      L"Com o filtro de ruído ligado, a voz não fica mais mudando de volume.",
      L"O ícone no canto de cima e o nome \"harmony\" de volta; \"(nativo)\" saiu."}},
    {22, L"25/10", {
      L"Os outros sons do computador (jogo, filme, música) não abaixam mais durante a chamada."}},
    {21, L"25/10", {
      L"Menos processador: o filtro de ruído pausa quando você está sozinho na sala, e transmitir sem ninguém assistindo gasta menos.",
      L"Trocar de conversa ficou mais rápido e não volta mais sozinha para a anterior.",
      L"As pessoas da chamada em ordem alfabética."}},
    {20, L"24/10", {
      L"Desligar a câmera devolve a memória que ela usava.",
      L"Quando a atualização falha, o canto diz \"Não atualizou\" em vez de \"Sem conexão\"."}},
    {19, L"23/10", {
      L"A tela sai com boa qualidade em toda transmissão, não só na primeira.",
      L"Parar de transmitir libera a placa de vídeo.",
      L"Depois de muitas transmissões seguidas, o programa avisa que é preciso sair e entrar na sala."}},
    {18, L"23/10", {
      L"Parar de transmitir solta tudo o que a transmissão usava."}},
    {17, L"23/10", {
      L"Parado numa sala, o programa não acumula mais recursos do Windows.",
      L"A lista de fones e microfones acompanha quando você pluga ou troca um aparelho."}},
    {16, L"23/10", {
      L"Diagnóstico novo, com memória, processador, travadas e conexão, e o botão \"Enviar diagnóstico\".",
      L"Seletor de tela reorganizado.",
      L"Baixar imagem salva com a extensão certa.",
      L"A mãozinha do mouse não pisca mais quando aparece uma dica."}},
    {15, L"22/10", {
      L"Todas as telas acertadas com o web: lista, perfil, seletor de tela, Ajustes de voz, cadastro e \"Esqueci a senha\".",
      L"Qualidade da conexão na barra da chamada, com os números ao clicar.",
      L"A bandeja mostra mutar, ensurdecer e sair da chamada só durante uma chamada."}},
    {14, L"22/10", {
      L"Bandeja e menu da câmera redesenhados.",
      L"As barrinhas de conexão ao lado de \"Voz conectada\".",
      L"A ilha da versão numa linha só."}},
    {13, L"22/10", {
      L"Qualquer microfone em qualidade cheia, sem precisar mexer nas configurações do Windows.",
      L"Bandeja com ícones; gráficos do Diagnóstico sempre coletando; \"Para onde vai a memória\"."}},
    {12, L"22/10", {
      L"O programa fica sabendo na hora quando sai uma versão nova."}},
    {11, L"22/10", {
      L"Microfones configurados em formatos altos voltam a funcionar."}},
    {10, L"22/10", {
      L"Escolher a câmera pelo próprio botão da câmera.",
      L"Esta ilha da versão, com o progresso do download e o botão Atualizar.",
      L"Abas Diagnóstico e Internet nos Ajustes, e mensagens de cada passo ao entrar na sala.",
      L"Tela de entrar nova, e o programa abre mais rápido."}},
    {9, L"22/10", {
      L"Voz mais clara: o Windows entregava o microfone com metade da qualidade.",
      L"A voz de quem sai e entra de novo na sala não fica muda.",
      L"A janela lembra tamanho, posição e se estava maximizada.",
      L"Bandeja com mutar, ensurdecer e sair da chamada."}},
    {8, L"21/10", {
      L"Download de 113 MB para 35 MB.",
      L"Procura atualização logo depois de entrar, e tenta de novo em 15 minutos se falhar.",
      L"Tela \"Atualizando o harmony\" ao abrir uma versão nova."}},
    {7, L"21/10", {
      L"Instalador com a cara do harmony: escolher a pasta, atalho na área de trabalho, abrir com o Windows e cada passo com contagem.",
      L"Telas de já instalado, atualizar, erro e desinstalar."}},
    {5, L"21/10", {
      L"O programa antigo sai de vez.",
      L"A janela vem para a frente depois de instalar ou atualizar.",
      L"Aplicativos instalados mostra a versão do harmony."}},
    {4, L"21/10", {
      L"O harmony nativo: um programa feito para o Windows, com voz, tela e câmera direto nele, sem um navegador por dentro.",
      L"A troca do programa antigo é automática e, se der errado, volta sozinha para o anterior."}},
    {1, L"03/10", {
      L"O primeiro programa para Windows: o harmony numa janela própria, com ícone na bandeja e abrir com o Windows."}},
  };
  return entries;
}
