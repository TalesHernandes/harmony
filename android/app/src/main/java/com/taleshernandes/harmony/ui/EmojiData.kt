package com.taleshernandes.harmony.ui

// Same searchable vocabulary and order as web/harmony/src/emoji.ts; glyphs stay native.
// Store data as strings so initialization stays below the JVM method-size limit.
private fun emojiGroup(name: String, data: String) = EmojiGroup(name,
    data.trimIndent().lineSequence().filter { it.isNotBlank() }.map { line ->
        val parts = line.split('\t', limit = 2)
        parts[0] to parts[1]
    }.toList())

internal val EmojiGroups: List<EmojiGroup> by lazy {
    listOf(
        emojiGroup("Mais usados", """
            👍	joia positivo curtir like ok
            👎	negativo nao dislike
            ❤️	coracao amor vermelho
            🔥	fogo top incendio
            😂	rindo chorando risada engracado
            😮	surpreso uau espanto
            😢	triste chorando lagrima
            👏	palmas aplauso parabens
            🎉	festa parabens comemoracao
            💯	cem nota dez perfeito
            ✅	certo ok feito confirmado
            ❌	errado nao cancelado
        """),
        emojiGroup("Rostos", """
            😢	triste chorando lagrima
            😮	surpreso uau espanto
            😂	rindo chorando risada engracado
            😀	sorriso feliz rosto
            😃	sorriso alegre rosto
            😄	sorriso rindo rosto
            😁	sorriso dentes rosto
            😆	rindo muito rosto
            😅	rindo suor alivio
            🤣	rolando de rir
            🙂	leve sorriso rosto
            🙃	de cabeca para baixo ironia
            😉	piscada rosto olho
            😊	timido feliz corado
            😇	anjo santo rosto
            🥰	apaixonado coracoes rosto
            😍	apaixonado olhos coracao
            🤩	estrelas deslumbrado rosto
            😘	beijo rosto mandando
            😗	beijinho rosto beijando
            😚	beijo fechado rosto
            😋	delicia lambendo rosto
            😛	lingua rosto mostrando
            😜	lingua piscada brincadeira
            🤪	doido maluco rosto
            🤨	desconfiado sobrancelha rosto
            🧐	monoculo analisando rosto
            🤓	nerd oculos rosto
            😎	oculos escuros estiloso
            🥳	festa comemorando rosto
            😏	malicioso sorriso torto
            😒	sem gracao entediado
            😞	desapontado rosto decepcao
            😔	pensativo triste rosto
            😟	preocupado rosto decepcionado
            😕	confuso rosto indeciso
            🙁	carinha triste rosto
            ☹️	triste rosto descontente
            😣	perseverando aflito rosto
            😖	angustiado rosto perplexo
            😫	cansado exausto rosto
            😩	desanimado rosto desolado
            🥺	suplicante pidao olhinhos
            😤	bufando raiva vapor
            😠	bravo rosto zangado
            😡	furioso vermelho raiva
            🤬	xingando palavrao rosto
            😭	chorando muito rosto
            😳	envergonhado corado rosto
            🥵	calor derretendo rosto
            🥶	frio congelando rosto
            😱	gritando medo rosto
            😨	assustado rosto amedrontado
            😰	ansioso suor rosto
            😥	aliviado triste rosto
            😓	suor decepcionado rosto
            🤗	abraco rosto abracando
            🤔	pensando duvida rosto
            🤭	risinho tapando a boca
            🤫	silencio shhh rosto
            🤥	mentira pinoquio rosto
            😶	sem boca calado
            😐	neutro rosto putz
            😑	sem expressao rosto
            😬	careta constrangido rosto
            🙄	revirando os olhos
            😯	surpresa rosto surpreso
            😦	boca aberta rosto
            😧	angustia rosto angustiado
            😲	chocado rosto espantado
            🥱	bocejo sono rosto
            😴	dormindo rosto cama
            🤤	babando rosto babar
            😪	sono lagrima rosto
            😵	tonto rosto atordoado
            🤯	explodindo mente chocado
            🤠	cauboi chapeu rosto
            🥸	disfarce bigode rosto
            😷	mascara doente rosto
            🤒	febre termometro rosto
            🤕	machucado curativo rosto
            🤢	enjoado nojo rosto
            🤮	vomitando rosto doente
            🤧	espirro resfriado rosto
            😈	diabinho travesso rosto
            👿	diabo bravo rosto
            💀	caveira morto morrendo de rir
            ☠️	caveira perigo ossos
            👻	fantasma assombracao assombrado
            👽	alien et alienigena
            🤖	robo rosto monstro
            🎃	abobora halloween comemoracao
            😺	gato feliz rosto
            😻	gato apaixonado rosto
            🙈	macaco nao vejo
            🙉	macaco nao ouco
            🙊	macaco nao falo
            🫠	rosto derretendo calor derreter desaparecer dissolver
            ☺️	rosto sorridente corada corado relaxado sorriso
            😙	rosto beijando olhos sorridentes beijinho beijos
            🥲	rosto sorridente lagrima aliviada chorar contente
            😝	rosto olhos semicerrados lingua fora eca
            🤑	rosto cifroes avarenta caro cifrao dinheiro
            🫢	rosto boca surpresa choque espanto segredo
            🫣	rosto olho espiando cativada cativado envergonhada
            🤐	rosto boca ziper calada calado quieta
            🫥	rosto linha pontilhada deprimida desaparecer esconder
            😶‍🌫️	rosto nuvens distraido cabeca nevoeiro
            😮‍💨	rosto exalando alivio assobio assopro cansaco
            🫨	rosto tremendo choque susto terremoto abalado
            😌	rosto aliviado alivio zen paz
            🥴	rosto embriagado alcoolizado bebada bebado embriagada
            😵‍💫	rosto olhos espiral caramba confuso desnorteado
            🫤	rosto boca diagonal cetico confusao decepcao
            🥹	rosto segurando lagrimas admiracao alegria chorar
            👹	ogro assustador demoniaco japones malvado mascara
            👺	duende japones bravo conto fadas goblin
            👾	alien monstro extraterrestre videogame invasor marciano
            😸	rosto gato sorrindo olhos sorridentes rindo
            😹	rosto gato lagrimas alegria chorando choro
            😼	rosto gato sorriso ironico
            😽	rosto gato mandando beijo beijando fechado
            🙀	rosto gato desolado choque oh surpresa
            😿	rosto gato chorando choro lagrima triste
            😾	rosto gato mal humorado bico bravo
        """),
        emojiGroup("Gente e gestos", """
            👏	palmas aplauso parabens
            👎	negativo nao dislike
            👍	joia positivo curtir like ok
            🙏	obrigado por favor reza
            🤝	aperto de mao acordo
            💪	forca musculo biceps
            🫡	continencia respeito rosto
            ✌️	paz vitoria mao
            🤞	dedos cruzados sorte
            🤟	te amo rock
            🤘	rock chifres saudacao
            👌	ok perfeito sinal
            🤌	italiano gesto dedos
            👈	aponta esquerda dorso
            👉	aponta direita dorso
            👆	aponta cima dorso
            👇	aponta baixo dorso
            ☝️	dedo indicador apontando
            ✋	mao parada levantada
            🖐️	mao aberta dedos
            🖖	vulcano spock saudacao
            👋	tchau oi aceno
            🤙	me liga shaka
            💅	unha esmalte deboche
            👀	olhos olhando olho
            🧠	cerebro inteligencia inteligente
            🫀	coracao orgao humano
            👑	coroa rei rainha
            🙌	maos erguidas comemorando
            👐	maos abertas mao
            🤲	maos juntas pedindo
            🤷	sei la ombros
            🤦	mao na cara facepalm
            💁	atendente sei pessoa
            🙋	levantando a mao
            🙆	ok bracos pessoa
            🙅	nao bracos cruzados
            🕺	dancando homem danca festa
            💃	dancando mulher bailarina
            🧘	meditando ioga pessoa
            🏃	correndo pessoa correr
            🚶	andando pessoa andar
            👶	bebe bebezinho gravida
            👴	velho vovo homem
            👵	velha vovo idosa
            🦸	heroi super bem
            🥷	ninja assassino furtividade
            🤡	palhaco rosto cara
            💩	coco merda estrume
            🤚	dorso mao levantado levantada pare tudo
            🫱	mao direita apertar aperto pegar segurar
            🫲	mao esquerda apertar pegar segurar
            🫳	mao palma baixo cair derrubar descartar
            🫴	mao palma cima acenar levantar oferecer
            🫷	mao empurrando esquerda bloquear empurrar esperar
            🫸	mao empurrando direita bloquear empurrar esperar
            🤏	mao beliscando beliscar pequeno pouco pouquinho
            🫰	mao dedo indicador polegar cruzados 3
            🖕	dedo meio mao gesto ofensivo
            🫵	indicador apontando visualizador ai apontar cutucar
            ✊	punho levantado pedra solidariedade erguido
            👊	soco beleza fechado forca mao punho
            🤛	punho esquerdo fechada mao soco
            🤜	punho direito soco
            🫶	maos coracao 3 amei amor mao
            ✍️	escrevendo mao caneta
            🤳	selfie camera celular foto smartphone
            🦾	braco mecanico acessibilidade protese
            🦿	perna mecanica acessibilidade protese
            🦵	perna chute joelho membro pe dobrada
            🦶	pe calcanhar chutar chute pisao pisar
            👂	orelha corpo escutar ouvido
            🦻	ouvido aparelho auditivo acessibilidade surda surdo
            👃	nariz cheirar cheiro corpo odor rosto
            🫁	pulmoes anatomia espiracao exalacao inalacao orgao
            🦷	dente branco dentista
            🦴	osso cachorro esqueleto
            👁️	olho rosto parte corpo
            👅	lingua corpo rosto
            👄	boca batom beijo corpo labios rosto
            🫦	mordendo labio ansioso batom beijo flertar
            🧒	crianca filho jovem neto pequeno
            👦	menino garoto guri jovem loiro pessoas
            👧	menina filha garota menininha neta pessoas
            🧑	pessoa adulta adulto
            👱	pessoa cabelo louro loiro rosto loira
            👨	homem adulto amigo irmao marido namorado
            🧔	homem barba barbudo pessoa
            🧔‍♂️	homem barbudo barba
            🧔‍♀️	mulher barba barbuda
            👨‍🦰	homem ruivo cabelo vermelho
            👨‍🦱	homem cacheado cabelo cachos
            👨‍🦳	homem idoso cabelo branco
            👨‍🦲	homem careca calvicie cabelo
            👩	mulher adulta garota guria loira menina
            👩‍🦰	mulher ruivo cabelo vermelho
            🧑‍🦰	pessoa ruivo cabelo vermelho
            👩‍🦱	mulher cacheado cabelo cachos
            🧑‍🦱	pessoa cacheado cabelo cachos
            👩‍🦳	mulher idoso cabelo branco
            🧑‍🦳	pessoa idoso cabelo branco
            👩‍🦲	mulher careca calvicie cabelo
            🧑‍🦲	pessoa careca calvicie cabelo
            👱‍♀️	mulher cabelo loiro garota guria loira
            👱‍♂️	homem cabelo loiro louro
            🧓	idoso adulto avo sabio velho vovo
            🙍	franzindo sobrancelha brava bravo chateada chateado
            🙍‍♂️	homem franzindo sobrancelha carrancudo chateado desconfiado
            🙍‍♀️	mulher franzindo sobrancelha carrancuda desconfiada
            🙎	pessoa fazendo bico beicinho beico careta
            🙎‍♂️	homem fazendo bico apontando gesto cara
            🙎‍♀️	mulher fazendo bico cara feia
            🙅‍♂️	homem fazendo gesto nao mao menino
            🙅‍♀️	mulher fazendo gesto nao mao proibido
            🙆‍♂️	homem fazendo gesto ok cabeca exercicio
            🙆‍♀️	mulher fazendo gesto ok mao
            💁‍♂️	homem palma virada cima ajuda fofoca
            💁‍♀️	mulher palma virada cima
            🙋‍♂️	homem levantando mao eu gesto menino
            🙋‍♀️	mulher levantando mao gesto pedir palavra
            🧏	pessoa surda acessibilidade audicao orelha surdo
            🧏‍♂️	homem surdo surdez libras
            🧏‍♀️	mulher surda surdez libras
            🙇	pessoa fazendo reverencia arrependimento desculpa desculpe
            🙇‍♂️	homem fazendo reverencia desculpas gesto respeito
            🙇‍♀️	mulher fazendo reverencia desculpas desculpe gesto
            🤦‍♂️	homem decepcionado decepcao desapontamento inacreditavel como
            🤦‍♀️	mulher decepcionada decepcao desapontamento inacreditavel como
            🤷‍♂️	homem dando ombros duvida indiferenca menino
            🤷‍♀️	mulher dando ombros duvida garota indiferenca
            🧑‍⚕️	medico medica enfermagem saude hospital profissional
            👨‍⚕️	homem medico doutor enfermeiro saude hospital
            👩‍⚕️	mulher medica doutora enfermeira saude hospital
            🧑‍🎓	aluno estudante graduando
            👨‍🎓	estudante aluno formando graduacao homem colando
            👩‍🎓	aluna estudante formanda mulher
            🧑‍🏫	professor professora ensino aula escola educacao
            👨‍🏫	professor educador homem instrutor mestre
            👩‍🏫	professora instrutora mestra mulher
            🧑‍⚖️	juiz tribunal balanca
            👨‍⚖️	juiz balanca homem justica
            👩‍⚖️	juiza balanca justica mulher
            🧑‍🌾	agricultor jardineiro rancheiro
            👨‍🌾	fazendeiro agricultor homem jardineiro
            👩‍🌾	fazendeira agricultora jardineira mulher
            🧑‍🍳	chef cozinha cozinheiro
            👨‍🍳	cozinheiro chef homem restaurante
            👩‍🍳	cozinheira chef mulher
            🧑‍🔧	mecanico trabalhando eletricista encanador prestador servico
            👨‍🔧	mecanico eletricista encanador homem prestador servicos
            👩‍🔧	mecanica eletricista encanadora mulher prestadora servicos
            🧑‍🏭	funcionario fabrica industrial montagem trabalhador
            👨‍🏭	operario fabrica homem industria metalurgico trabalhador
            👩‍🏭	operaria fabrica industria mulher trabalhadora
            🧑‍💼	trabalhador escritorio arquiteto gerente negocios colarinho
            👨‍💼	funcionario escritorio arquiteto empresario gerente homem
            👩‍💼	funcionaria escritorio arquiteta branco colarinho empresaria
            🧑‍🔬	cientista biologo engenheiro fisico quimico
            👨‍🔬	cientista homem biologo fisico professor quimico
            👩‍🔬	cientista mulher biologa fisica professora quimica
            🧑‍💻	programador desenvolvedor inventor software tecnologo
            👨‍💻	tecnologo codificador computador criador desenvolvedor homem
            👩‍💻	tecnologa codificadora computador criadora desenvolvedora inventora
            🧑‍🎤	cantor ator entretenimento estrela rock
            👨‍🎤	cantor homem artista ator pop rock
            👩‍🎤	cantora artista atriz mulher pop rock
            🧑‍🎨	artista paleta pintura pintor
            👨‍🎨	artista plastico homem pintor pintura quadros
            👩‍🎨	artista plastica mulher pintura
            🧑‍✈️	piloto aviao aviacao
            👨‍✈️	piloto aviao homem
            👩‍✈️	piloto aviao mulher garota guria voando
            🧑‍🚀	astronauta foguete espaco
            👨‍🚀	astronauta homem espaco foguete
            👩‍🚀	astronauta mulher espaco foguete
            🧑‍🚒	bombeiro caminhao bombeiros
            👨‍🚒	bombeiro homem caminhao fogo incendio corpo
            👩‍🚒	bombeira caminhao fogo incendio mulher corpo
            👮	policial autoridade multar pessoa policia prender
            👮‍♂️	policial homem policia tira
            👮‍♀️	policial mulher autoridade multar policia prender
            🕵️	detetive espiao investigador rosto lupa
            🕵️‍♂️	detetive homem espiao investigador
            🕵️‍♀️	detetive mulher espia espionar investigadora investigar
            💂	guarda londres palacio pessoas seguranca
            💂‍♂️	guarda homem seguranca
            💂‍♀️	guarda mulher buckingham londres palacio realeza
            👷	trabalhador construcao civil capacete chapeu construir
            👷‍♂️	pedreiro construcao homem operario
            👷‍♀️	pedreira capacete construcao contrucao mulher operaria
            🫅	pessoa coroa monarca nobre princesa principe
            🤴	principe realeza rei
            👸	princesa conto coroa fadas fantasia loira
            👳	pessoa turbante cabeca
            👳‍♂️	homem turbante cabeca
            👳‍♀️	mulher turbante cabeca
            👲	homem bone guapimao pessoas chapeu chines
            🧕	mulher veu cabeca hijab lenco
            🤵	pessoa smoking festa gala homem noivo
            🤵‍♂️	homem smoking terno casamento
            🤵‍♀️	mulher smoking terno casamento
            👰	pessoa veu casamento noiva pessoas
            👰‍♂️	noivo veu casamento
            👰‍♀️	noiva veu casamento
            🤰	gravida gravidez mulher estou
            🫃	homem gravido barriga cheia cheio gravidez
            🫄	pessoa gravida barriga cheia cheio excesso
            🤱	amamentando amamentacao bebe leite mae materno
            👩‍🍼	mulher alimentando bebe amamentacao amamentando amor
            👨‍🍼	homem alimentando bebe amamentacao amamentando amor
            🧑‍🍼	pessoa alimentando bebe amamentacao amamentar homem
            👼	bebe anjo conto fadas rosto
            🤶	mamae noel comemoracao natal vovo
            🧑‍🎄	noel chapeu feliz festas gorro mamae
            🦸‍♂️	homem super heroi bom superpoder
            🦸‍♀️	super heroina boa heroi mulher superpoder
            🦹	supervilao bandido criminoso mal malvado mau
            🦹‍♂️	homem supervilao criminoso mal superpoder vilao
            🦹‍♀️	supervila criminosa ma mulher superpoder vila
            🧙	mago bruxa fantasia feiticeira feiticeiro feitico
            🧙‍♂️	homem mago feiticeiro
            🧙‍♀️	maga bruxa feiticeira
            🧚	fada asas conto encantado fantasia ser
            🧚‍♂️	homem fada magia asas
            🧚‍♀️	mulher fada magia asas
            🧛	vampiro assustador bruxas dia dracula fantasia
            🧛‍♂️	homem vampiro dracula
            🧛‍♀️	mulher vampira sangue dracula
            🧜	pessoa sereia canto cauda contos criatura
            🧜‍♂️	sereio tritao mar
            🧜‍♀️	sereia mulher mar
            🧝	elfo duende fantasia legolas magia magico
            🧝‍♂️	elfo homem duende
            🧝‍♀️	elfa duende mulher
            🧞	genio desejos fantasia lampada magica mistico
            🧞‍♂️	homem genio lampada desejos
            🧞‍♀️	mulher genio lampada desejos
            🧟	zumbi apocalipse assustador bruxas dia halloween
            🧟‍♂️	homem zumbi cazumbi
            🧟‍♀️	mulher zumbi cazumbi
            🧌	troll conto fadas fantasia gigante monstro
            💆	pessoa recebendo massagem facial pele relaxar
            💆‍♂️	homem recebendo massagem facial acalmar menino
            💆‍♀️	mulher recebendo massagem facial relaxamento
            💇	pessoa cortando cabelo beleza salao corte
            💇‍♂️	homem cortando cabelo barbeiro beleza corte
            💇‍♀️	mulher cortando cabelo corte salao beleza
            🚶‍♂️	homem andando andar caminhar caminhando
            🚶‍♀️	mulher andando andar caminhada caminhando caminhar
            🧍	pessoa pe parado
            🧍‍♂️	homem pe parado
            🧍‍♀️	mulher pe parada
            🧎	pessoa ajoelhando ajoelhado ajoelhar joelhos pedir
            🧎‍♂️	homem ajoelhando ajoelhado
            🧎‍♀️	mulher ajoelhando ajoelhada
            🧑‍🦯	pessoa bengala cego acessibilidade
            👨‍🦯	homem bengala cego accessibilidade
            👩‍🦯	mulher bengala cego accessibilidade cega deficiencia
            🧑‍🦼	pessoa cadeira rodas motorizada acessibilidade
            👨‍🦼	homem cadeira rodas motorizada acessibilidade
            👩‍🦼	mulher cadeira rodas motorizada acessibilidade
            🧑‍🦽	pessoa cadeira rodas manual acessibilidade
            👨‍🦽	homem cadeira rodas manual acessibilidade
            👩‍🦽	mulher cadeira rodas manual acessibilidade
            🏃‍♂️	homem correndo corrida esporte maratona
            🏃‍♀️	mulher correndo corredora corrida esporte garota
            🕴️	homem terno levitando
            👯	pessoas orelhas coelho amigas dancar dancarinas
            👯‍♂️	homens orelhas coelho dancarino festa homem
            👯‍♀️	mulheres orelhas coelho dancarinas festa orelha
            🧖	pessoa sauna banho homem mulher relaxar
            🧖‍♂️	homem sauna vapor
            🧖‍♀️	mulher sauna vapor
            🧗‍♂️	homem escalando escalar
            🧗‍♀️	mulher escalando escalar
            🧘‍♂️	homem posicao lotus ioga meditacao yoga
            🧘‍♀️	mulher posicao lotus ioga meditacao yoga
            🛀	pessoa tomando banho agua banheira espuma
            🛌	pessoa deitada cama boa cochilo dormindo
            🧑‍🤝‍🧑	pessoas maos dadas amigos casal gemeos
            👭	duas mulheres maos dadas amigas casal
            👫	homem mulher maos dadas amigos casal
            👬	dois homens maos dadas amigos casal
            💏	beijo amor beijar casal coracao pessoas
            👩‍❤️‍💋‍👨	beijo casal mulher homem romance amor
            👨‍❤️‍💋‍👨	beijo casal homens gay romance amor
            👩‍❤️‍💋‍👩	beijo casal mulheres lesbicas romance amor
            💑	casal apaixonado coracao pessoas romance
            👩‍❤️‍👨	casal apaixonado mulher homem coracao pessoas
            👨‍❤️‍👨	casal apaixonado homem coracao pessoas romance
            👩‍❤️‍👩	casal apaixonado mulher coracao pessoas romance
            👨‍👩‍👦	familia homem mulher menino filho mae
            👨‍👩‍👧	familia homem mulher menina filho mae
            👨‍👩‍👧‍👦	familia homem mulher menina menino filho
            👨‍👩‍👦‍👦	familia homem mulher menino filho mae
            👨‍👩‍👧‍👧	familia homem mulher menina filho mae
            👨‍👨‍👦	familia homem menino filho mae pai
            👨‍👨‍👧	familia homem menina filho mae pai
            👨‍👨‍👧‍👦	familia homem menina menino filho mae
            👨‍👨‍👦‍👦	familia homem menino filho mae pai
            👨‍👨‍👧‍👧	familia homem menina filho mae pai
            👩‍👩‍👦	familia mulher menino filho mae pai
            👩‍👩‍👧	familia mulher menina filho mae pai
            👩‍👩‍👧‍👦	familia mulher menina menino filho mae
            👩‍👩‍👦‍👦	familia mulher menino filho mae pai
            👩‍👩‍👧‍👧	familia mulher menina filho mae pai
            👨‍👦	familia homem menino filho mae pai
            👨‍👦‍👦	familia homem menino filho mae pai
            👨‍👧	familia homem menina filho mae pai
            👨‍👧‍👦	familia homem menina menino filho mae
            👨‍👧‍👧	familia homem menina filho mae pai
            👩‍👦	familia mulher menino filho mae pai
            👩‍👦‍👦	familia mulher menino filho mae pai
            👩‍👧	familia mulher menina filho mae pai
            👩‍👧‍👦	familia mulher menina menino filho mae
            👩‍👧‍👧	familia mulher menina filho mae pai
            🗣️	silhueta falando berro cabeca grito voz
            👤	silhueta busto misterio pessoas sombra
            👥	silhueta bustos amigos busto dupla pessoas
            🫂	pessoas abracando abraco adeus amizade amor
            👪	familia filho mae pai pessoas
            👣	pegadas corpo gravura pe rastro pes
        """),
        emojiGroup("Corações e símbolos", """
            ❌	errado nao cancelado
            ✅	certo ok feito confirmado
            💯	cem nota dez perfeito
            ❤️	coracao amor vermelho
            🧡	coracao laranja emocao
            💛	coracao amarelo amor
            💚	coracao verde emocao
            💙	coracao azul emocao
            💜	coracao roxo emocao
            🖤	coracao preto negro
            🤍	coracao branco s2
            🤎	coracao marrom s2
            💔	coracao partido amorosa
            💕	dois coracoes amor
            💞	coracoes girando adoravel
            💗	coracao crescendo amor
            💖	coracao brilhando brilhante
            💘	coracao flecha amor
            💝	coracao presente fita
            💟	coracao decorado decorativo
            💌	carta de amor
            💋	beijo batom marca
            ⭐	estrela branca media
            🌟	estrela brilhando brilhante
            ✨	brilho magia brilhos
            💫	tontura estrela zonzo
            ⚡	raio energia alta
            💥	explosao colisao emocao
            💢	raiva simbolo emocao
            💤	sono zzz cansada
            🔔	sino aviso capela
            🔕	sino cortado silencio
            ♻️	reciclagem simbolo reclicar
            ⚠️	atencao perigo aviso
            🚫	proibido entrada nao
            ❗	exclamacao importante ponto
            ❓	interrogacao duvida ponto
            💬	balao fala dialogo
            💭	balao pensamento engracado
            🔊	som alto volume
            🔇	mudo sem som
            🎵	musica nota musical
            🎶	musica notas musicais
            ✔️	confirmado marca selecao
            ➕	mais somar simbolo
            ➖	menos simbolo subtracao
            🔴	bolinha vermelha circulo
            🟢	bolinha verde online
            🟡	bolinha amarela circulo
            🔵	bolinha azul circulo
            ⚫	bolinha preta circulo
            ⚪	bolinha branca circulo
            🆗	ok botao sim
            🆕	novo botao new
            🔝	topo top seta
            🔞	dezoito proibido menores
            💲	dinheiro cifrao dolar
            ♾️	infinito eternidade ilimitado
            💓	coracao pulsante amor batendo emocao s2
            ❣️	exclamacao coracao amor pontuacao s2 sinal
            ❤️‍🔥	coracao chamas amor fogo luxuria sagrado
            ❤️‍🩹	coracao remendado bem bom curando melhorando
            🩷	coracao rosa adoravel adorei amor curtir
            🩵	coracao azul claro adorar adorei amor
            🩶	coracao cinza adorar adorei amor emocao
            💦	pingos suor borrifo emocao engracado splash
            💨	rapidez correr corrida emocao engracado fugir
            🕳️	buraco chao vazio
            👁‍🗨	olho balao dialogo testemunha
            🗨️	balao dialogo esquerda azul conversa
            🗯️	balao raiva direita briga conversa dialogo
            🏧	simbolo caixa automatico atm banco dinheiro
            🚮	simbolo lixeira lata lixo coloque
            🚰	agua potavel beber simbolo torneira
            ♿	simbolo cadeira rodas acesso sinal
            🚹	banheiro masculino homem lavatorio simbolo toalete
            🚺	banheiro feminino lavabo lavatorio mulher simbolo
            🚻	banheiro lavabo sanitario simbolo toalete
            🚼	simbolo bebe fralda fraldario trocar
            🚾	wc latrina lavabo privada toalete vaso
            🛂	controle passaportes passaporte
            🛃	alfandega aduana aduaneira bens federal impostos
            🛄	restituicao bagagem aeroporto area bagagens esteira
            🛅	deposito bagagem esquecida malas servico
            🚸	criancas atravessando crianca pedestre simbolo trafego
            ⛔	entrada proibida entre nao placa proibido
            🚳	proibido andar bicicleta nao permitidas sem
            🚭	proibido fumar cigarro simbolo nao permitido
            🚯	proibido jogar lixo chao nao simbolo
            🚱	agua nao potavel consumo impropria proibido
            🚷	proibida passagem pedestres nao pedestre permitidos
            📵	proibido uso telefone celular cel nao
            ☢️	radioativo perigo radiacao radiativo simbolo sinal
            ☣️	risco biologico ciencia perigo residuos biologicos
            ⬆️	seta cima cardinal direcao norte
            ↗️	seta cima direita diagonal direcao flecha
            ➡️	seta direita direcao flecha leste
            ↘️	seta baixo direita diagonal direcao flecha
            ⬇️	seta baixo abaixo cardinal direcao embaixo
            ↙️	seta baixo esquerda diagonal direcao flecha
            ⬅️	seta esquerda atras direcao flecha oeste
            ↖️	seta cima esquerda direcao flecha intercardinal
            ↕️	seta cima baixo flecha vertical
            ↔️	seta esquerda direita flecha horizontal lados
            ↩️	seta curva direita esquerda flecha retorno
            ↪️	seta curva esquerda direita flecha retorno
            ⤴️	seta direita curvada cima curva flecha
            ⤵️	seta direita curvada baixo curva embaixo
            🔃	setas verticais sentido horario flechas recarregar
            🔄	botao setas sentido anti horario atualizar
            🔙	seta back flecha voltar esquerda
            🔚	seta end fim final esquerda ir
            🔛	seta on flecha marca
            🔜	seta soon flecha direita breve simbolo
            🛐	local culto oracao religiao reza sagrado
            ⚛️	simbolo atomo ateismo ateu
            🕉️	om hindu religiao
            ✡️	estrela davi judaico judeu religiao
            ☸️	roda dharma budista religiao
            ☯️	yin yang religiao tao taoista taoistas
            ✝️	cruz latina cristao religiao
            ☦️	cruz ortodoxa cristao religiao
            ☪️	estrela lua crescente isla muculmano religiao
            ☮️	simbolo paz harmonia
            🕎	menora candelabro castical judaismo memorah religiao
            🔯	estrela seis pontas adivinhacao destino
            🪯	khanda fe khalsa religiao sikh sikhismo
            ♈	signo aries carneiro zodiaco
            ♉	signo touro boi zodiaco
            ♊	signo gemeos zodiaco
            ♋	signo cancer caranguejo zodiaco
            ♌	signo leao zodiaco
            ♍	signo virgem zodiaco
            ♎	signo libra balanca justica zodiaco
            ♏	signo escorpiao zodiaco
            ♐	signo sagitario arqueiro zodiaco
            ♑	signo capricornio cabra zodiaco
            ♒	signo aquario agua zodiaco
            ♓	signo peixes zodiaco
            ⛎	signo ofiuco cobra ophiuchus serpente zodiaco
            🔀	botao musicas aleatorias cruzadas flechas seta
            🔁	botao repetir flechas horario seta setas
            🔂	botao repetir unica faixa horario seta
            ▶️	botao reproduzir direita play seta triangulo
            ⏩	botao avancar direita dupla flechas rapido
            ⏭️	botao proxima faixa avancar seta flechas
            ⏯️	botao reproduzir ou pausar direita pause
            ◀️	botao voltar esquerda seta triangulo
            ⏪	botao retroceder dupla esquerda seta voltar
            ⏮️	botao faixa anterior seta triangulo voltar
            🔼	botao apontando cima acima seta triangulo
            ⏫	botao avanco cima dupla seta flechas
            🔽	botao apontando baixo seta triangulo vermelho
            ⏬	botao avanco baixo flechas seta retroceder
            ⏸️	botao pausar barra dupla pausado pause
            ⏹️	botao parar quadrado stop
            ⏺️	botao gravar circulo
            ⏏️	botao ejetar retirar
            🎦	cinema entretenimento filme simbolo
            🔅	botao diminuir brilho escurecer simbolo reduzir
            🔆	botao aumentar brilho simbolo
            📶	barras sinal antena celular conexao internet
            🛜	sem fio computador conectividade internet rede
            📳	modo vibratorio cel celular smartphone telefone
            📴	telefone celular desligado smartphone desligue
            ♀️	simbolo feminino mulher
            ♂️	simbolo masculino homem
            ⚧️	simbolo transgenero trans genero
            ✖️	sinal multiplicacao cancelar multiplicar preto
            ➗	simbolo divisao dividir matematica sinal grande
            🟰	sinal igual conta iguais igualdade matematica
            ‼️	dupla exclamacao explosao pontuacao ponto duplo
            ⁉️	exclamacao interrogacao pergunta pontuacao sinal
            ❔	ponto interrogacao branco delineado pergunta pontuacao
            ❕	ponto exclamacao branco delineado mark pontuacao
            〰️	travessao ondulado onda pontuacao
            💱	cambio moeda banco dinheiro
            ⚕️	simbolo medicina esculapio bastao asclepio
            ⚜️	flor lis cavaleiros simbolo
            🔱	emblema tridente ancora ferramenta navio simbolo
            📛	cracha identificacao nome
            🔰	simbolo japones principiante iniciante folha verde
            ⭕	circulo grande oco aro vermelho vazio
            ☑️	caixa selecao marcada tique check feito
            ❎	botao xis marcado multiplicar quadrado caixa
            ➰	laco encaracolado loop onda ondulado volta
            ➿	loop encaracolado duas vezes duplo encaradolado
            〽️	sinal japones indicando inicio musica canto
            ✳️	asterisco oito pontas estrela
            ✴️	estrela oito pontas
            ❇️	faisca brilho estrela
            ©️	simbolo copyright direitos autorais
            ®️	simbolo registrado marca registrada
            ™️	simbolo marca registrada tm
            #️⃣	tecla hashtag cerquilha jogo velha
            *️⃣	tecla asterisco estrela
            0️⃣	zero numero tecla 0
            1️⃣	um numero tecla 1
            2️⃣	dois numero tecla 2
            3️⃣	tres numero tecla 3
            4️⃣	quatro numero tecla 4
            5️⃣	cinco numero tecla 5
            6️⃣	seis numero tecla 6
            7️⃣	sete numero tecla 7
            8️⃣	oito numero tecla 8
            9️⃣	nove numero tecla 9
            🔟	dez numero tecla 10
            🔠	letras latinas maiusculas abcd digitacao maiusculos
            🔡	letras latinas minusculas abcd digitacao latino
            🔢	numeros 1234 digitacao inserir
            🔣	simbolos digitacao inserir
            🔤	letras latinas abc alfabeto digitacao ingles
            🅰️	botao tipo sanguineo sangue
            🆎	botao ab tipo sanguineo sangue
            🅱️	botao tipo sanguineo sangue
            🆑	botao cl limpar simbolo
            🆒	botao cool legal simbolo
            🆓	botao free gratis gratuito simbolo
            ℹ️	informacoes informacao simbolo
            🆔	botao id identidade simbolo
            Ⓜ️	letra m circulo metro
            🆖	botao ng simbolo
            🅾️	botao tipo sanguineo sangue
            🅿️	botao estacionamento estacionar garagem carros
            🆘	botao sos ajuda simbolo socorro
            🆙	botao up simbolo cima
            🆚	botao vs simbolo versus
            🈁	botao japones aqui katakana
            🈂️	botao japones taxa servico cobranca katakana
            🈷️	botao japones quantidade mensal ideograma valor
            🈶	botao japones nao gratuito gratis ideograma
            🈯	botao japones reservado ideograma
            🉐	botao japones barganha ideograma promocao
            🈹	botao japones desconto ideograma
            🈚	botao japones gratuito ideograma
            🈲	botao japones proibido ideograma
            🉑	botao japones aceitavel ideograma
            🈸	botao japones aplicacao aplicativo ideograma
            🈴	botao japones nota minima ideograma
            🈳	botao japones vago ideograma
            ㊗️	botao japones parabens ideograma
            ㊙️	botao japones segredo ideograma
            🈺	botao japones aberto negocios ideograma
            🈵	botao japones sem vagas ideograma
            🟠	circulo laranja bolinha
            🟣	circulo roxo bolinha
            🟤	circulo marrom bolinha
            🟥	quadrado vermelho forma
            🟧	quadrado laranja forma
            🟨	quadrado amarelo forma
            🟩	quadrado verde forma
            🟦	quadrado azul anil
            🟪	quadrado roxo purpura
            🟫	quadrado marrom forma
            ⬛	quadrado preto grande geometrico
            ⬜	quadrado branco grande geometrico
            ◼️	quadrado preto medio geometrico
            ◻️	quadrado branco medio geometrico
            ◾	quadrado preto medio menor geometrico
            ◽	quadrado branco medio menor geometrico
            ▪️	quadrado preto pequeno geometrico
            ▫️	quadrado branco pequeno geometrico
            🔶	losango laranja grande diamante geometrico cor
            🔷	losango azul grande balao diamante geometrico
            🔸	losango laranja pequeno diamante geometrico cor
            🔹	losango azul pequeno diamante geometrico
            🔺	triangulo vermelho cima geometrico apontando
            🔻	triangulo vermelho baixo geometrico apontando
            💠	diamante ponto comico dentro geometrico formato
            🔘	botao opcao geometrico radio
            🔳	botao quadrado branco preto
            🔲	botao quadrado preto geometrico branco
        """),
        emojiGroup("Animais", """
            🐶	cachorro rosto animal
            🐱	gato rosto animal
            🐭	rato rosto camundongo
            🐹	hamster rosto animal
            🐰	coelho rosto animal
            🦊	raposa rosto animal
            🐻	urso rosto animal
            🐼	panda rosto animal
            🐨	coala animal australia
            🐯	tigre rosto animal
            🦁	leao rosto animal
            🐮	vaca rosto animal
            🐷	porco rosto animal
            🐸	sapo animal rosto
            🐵	macaco rosto animal
            🐔	galinha animal ave
            🐧	pinguim animal antartica
            🐦	passaro animal ave
            🐤	pintinho perfil animal
            🦆	pato animal passaro
            🦅	aguia passaro ave
            🦉	coruja passaro sabedoria
            🦇	morcego animal vampiro
            🐺	lobo rosto animal
            🐗	javali animal selvagem
            🐴	cavalo rosto animal
            🦄	unicornio rosto animal
            🐝	abelha animal inseto
            🐛	lagarta bug inseto
            🦋	borboleta beleza inseto
            🐌	caracol lento animal
            🐞	joaninha animal besouro
            🐜	formiga animal inseto
            🦂	escorpiao animal deserto
            🐢	tartaruga animal casco
            🐍	cobra animal reptil
            🦎	lagartixa animal lagarto
            🐙	polvo animal oceano
            🦑	lula animal comida
            🦐	camarao animal comida
            🦀	caranguejo animal cancer
            🐡	baiacu animal peixe
            🐠	peixe tropical animal
            🐟	peixe animal peixes
            🐬	golfinho animal oceano
            🐳	baleia esguichando agua
            🦈	tubarao peixe mar
            🐊	jacare crocodilo animal
            🐘	elefante animal tromba
            🦒	girafa animal pescoco
            🦓	zebra animal listra
            🐄	boi gado vaca
            🐓	galo animal ave
            🦃	peru animal ave
            🕊️	pomba paz branca
            🐕	cao cachorro animal
            🐈	gato felino bichano
            🐇	coelhinho coelho animal
            🐁	ratinho camundongo animal
            🦔	ourico porco espinho
            🐒	macaco animal primata
            🦍	gorila animal macaco
            🦧	orangotango macaco primata
            🦮	cao guia acessibilidade cachorro cadela cega
            🐕‍🦺	cao servico accessibilidade assistencia cachorro cadela
            🐩	poodle animal cachorro
            🦝	guaxinim animal astuto curioso travesso
            🐈‍⬛	gato preto animal azar felino gata
            🐅	tigre animal felino
            🐆	leopardo animal onca
            🫎	alce animal cervo chifres galhada mamifero
            🫏	burro animais animal asno jegue mamifero
            🐎	cavalo animal corrida equestre
            🦌	cervo animal veado
            🦬	bisao animal bisonte bufalo rebanho
            🐂	boi animal touro zodiaco
            🐃	bufalo asiatico agua animal
            🐖	porco animal leitao
            🐽	nariz porco animal rosto
            🐏	carneiro animal aries chifre zodiaco
            🐑	ovelha animal fazenda la
            🐐	cabra animal bode capricornio zodiaco
            🐪	camelo animal corcova deserto dromedario so
            🐫	camelo duas corcovas animal corcova deserto
            🦙	lhama alpaca animal guanaco la vicunha
            🦣	mamute animal extinto grande lanoso pre
            🦏	rinoceronte animal chifre
            🦛	hipopotamo animal africa
            🐀	rato animal ratazana
            🐿️	esquilo animal roedor
            🦫	castor animal dentuco represa
            🐻‍❄️	urso polar artico branco
            🦥	preguica bicho devagar lenta lentidao preguicosa
            🦦	lontra brincalhona pesca
            🦨	gamba fedida fedido fedor
            🦘	canguru animal australia filhote joey marsupial
            🦡	texugo animal incomodar ratel mel
            🐾	patas animal pata pegada patinhas cachorro
            🐣	pintinho chocando animal ave bebe filhote
            🐥	pintinho frente animal ave bebe fihote
            🦢	cisne animal ave passaro patinho feio
            🦤	dodo animal ave extinto grande mauricio
            🪶	pena leve passaro plumagem voo
            🦩	flamingo ave extravagante tropical
            🦚	pavao animal ave colorido orgulhoso pavoa
            🦜	papagaio animal ave fala passaro pirata
            🪽	asa angelical anjo ascender aviacao celestial
            🐦‍⬛	passaro preto animal ave bico corvo
            🪿	ganso animal ave bobo gansos grasno
            🐲	rosto dragao animal
            🐉	dragao animal game of thrones
            🦕	sauropode braquiosaurus braquiossauro brontossauro brontossuro dinossauro
            🦖	tiranossauro rex dinossauro
            🐋	baleia animal mar oceano
            🦭	foca animal leao marinho oceano
            🐚	caramujo animal concha mar espiral
            🪸	coral mar oceano recife mudanca climatica
            🪼	agua viva ai animal aquario ferrao
            🪲	besouro animal bicho inseto
            🦗	grilo gafanhoto inseto
            🪳	barata animal baratas inseto nojento nojo
            🕷️	aranha animal halloween inseto dias bruxas
            🕸️	teia aranha halloween inseto
            🦟	mosquito aedes aegypti amarela animal chicungunha
            🪰	mosca animal apodrecendo doenca inseto larva
            🪱	minhoca anelideo animal parasita verme
            🦠	microbio ameba bacteria ciencia virus
        """),
        emojiGroup("Comida e bebida", """
            🍕	pizza fatia restaurante
            🍔	hamburguer burguer restaurante
            🍟	batata frita comida
            🌭	cachorro quente vina
            🥪	sanduiche pao forma
            🌮	taco mexicano tacos
            🌯	burrito mexicano comida
            🥗	salada verde comida
            🍝	macarrao espaguete comida
            🍜	lamen sopa comida
            🍲	panela ensopado restaurante
            🍛	curry arroz restaurante
            🍣	sushi restaurante sashimi
            🍤	tempura camarao frito
            🍱	marmita bento caixa
            🥟	guioza bolinho asiatico
            🍚	arroz cozido comida
            🥩	carne bife corte
            🍗	frango coxa aves
            🥓	bacon comida fome
            🥚	ovo comida fome
            🍳	ovo frito comida
            🧀	queijo comida fome
            🥐	croissant comida fome
            🥖	pao frances baguete
            🍞	pao restaurante torrada
            🥞	panqueca panquecas comida
            🧇	waffle doce gofre
            🍰	bolo fatia pao
            🎂	bolo aniversario comemoracao
            🧁	cupcake acucar bolinho
            🍩	rosquinha donut comida
            🍪	biscoito cookie bolacha
            🍫	chocolate barra doce
            🍬	bala doce balinha
            🍭	pirulito bala doce
            🍦	sorvete casquinha italiano
            🍨	sorvete pote creme
            🍎	maca vermelha fruta
            🍏	maca verde comida
            🍌	banana fruta comida
            🍇	uva uvas fruta
            🍓	morango comida fome
            🍒	cereja comida fome
            🍑	pessego bunda fruta
            🍍	abacaxi comida fome
            🥭	manga comida fruta
            🍉	melancia fruta comida
            🍊	laranja tangerina citrica
            🍋	limao azedo citrico
            🥥	coco fruta palmeira
            🥑	abacate comida fruta
            🍅	tomate fruta fruto
            🥦	brocolis brocoli verdura
            🌽	milho comida espiga
            🥕	cenoura comida fome
            🌶️	pimenta apimentada apimentado
            🍺	cerveja bar bebida
            🍻	brinde cervejas canecas
            🍷	vinho bar bebida
            🥂	brinde taca comemorar
            🍸	drink coquetel bar
            🍹	drink tropical bebida
            🥃	whisky copo bar
            🍾	champanhe comemorar garrafa
            ☕	cafe cafeina cafezinho
            🍵	cha xicara sem
            🧉	chimarrao mate bebida
            🥤	refrigerante copo canudo
            🧊	gelo cubo gelado
            🍿	pipoca filme balde
            🧂	sal comida condimento
            🍯	mel pote restaurante
            🍈	melao fruta comida
            🍐	pera comida fome fruta
            🫐	mirtilos alimento azul baga blueberry comida
            🥝	kiwi comida fruta
            🫒	azeitona alimento comida oliveira
            🍆	berinjela beringela comida legume vegetal
            🥔	batata comida fome legume vegetal
            🫑	pimentao alimento capsicum comida pimenta vegetal
            🥒	pepino comida fome legume picles salada
            🥬	verdura alface comida couve hamburguer repolho
            🧄	alho tempero cozinha
            🧅	cebola tempero cozinha
            🥜	amendoim comida petisco
            🫘	feijoes alimento comida feijao grao legume
            🌰	castanha planta comida
            🫚	gengibre cerveja comida erva especiaria natural
            🫛	vagem comida edamame ervilha feijao grao
            🫓	pao sirio alimento arepa comida lavash
            🥨	pretzel pao torcido
            🥯	bagel chimia comida confeitaria pao rosca
            🍖	carne churras churrasco osso restaurante
            🫔	tamale alimento comida enrolado mexicano milho
            🥙	pao recheado comida kebab recheio wrap
            🧆	falafel almondega grao bico
            🥘	cacarola comida fome paella panela rasa
            🫕	fondue alimento chocolate comida derretido panela
            🥣	tigela colher cereal cafe manha prato
            🧈	manteiga laticinio margarina
            🥫	comida enlatada lata
            🍘	biscoito arroz cracker bolacha
            🍙	arroz japones comida fome onigiri restaurante
            🍠	batata assada doce restaurante
            🍢	oden espetinho restaurante frutos mar
            🍥	bolinho peixe croquete redemoinho restaurante
            🥮	bolo lunar comida confeitaria festival outono
            🍡	dango comida doce fome japones mochi
            🥠	biscoito sorte profecia chines fortuna
            🥡	caixa viagem delivery marmita caixinha levar
            🦞	lagosta animal bisque caldo garras fruto
            🦪	ostra mar mergulho perola frutos
            🍧	raspadinha gelo restaurante sobremesa
            🥧	torta abobora carne doce fatia fruta
            🍮	pudim restaurante leite ovos
            🍼	mamadeira bebe leite leitinho mamar nenem
            🥛	copo leite bebida
            🫖	bule alimento bebida cha chaleira comida
            🍶	saque bar bebida copo garrafa japones
            🫗	derramando liquido acabou acidente agua bebida
            🧋	cha perolado bebida bolha comida leite
            🧃	suco caixa suquinho caixinha
            🥢	hashi pauzinhos comida japonesa
            🍽️	prato talheres almoco faca fome garfo
            🍴	garfo faca almoco comer comida fome
            🥄	colher talher cozinha
            🔪	faca cozinha chef cozinhar
            🫙	jarro armazenar condimento conserva jarra molho
            🏺	anfora vaso ornamento chines
        """),
        emojiGroup("Festa e esporte", """
            🎉	festa parabens comemoracao
            🎊	confete festa bola
            🎈	balao festa aniversario
            🎁	presente comemoracao embrulhado
            🎀	laco fita comemoracao
            🪅	pinhata comemoracao doce
            🕯️	vela acesa luz
            🎄	natal arvore comemoracao
            🎅	papai noel comemoracao
            🎆	fogos de artificio
            🎇	fogos brilho vela
            🧨	bomba dinamite bombinha
            ⚽	futebol bola jogar
            🏀	basquete bola cesta
            🏈	futebol americano bola
            ⚾	beisebol bola esportes
            🎾	tenis bola esporte
            🏐	volei bola jogar
            🏓	tenis de mesa
            🏸	badminton jogo peteca
            🥅	gol trave esporte
            🏆	trofeu campeao campea
            🥇	medalha ouro primeiro
            🥈	medalha prata segundo
            🥉	medalha bronze terceiro
            🎯	alvo mira certeiro
            🎲	dado sorte jogo
            🎮	videogame controle game
            🕹️	joystick atari controle
            🎰	cassino sorte caca
            🃏	coringa carta curinga
            ♟️	xadrez peao truque
            🎨	arte pintura paleta
            🎭	teatro mascaras mascara
            🎤	microfone cantar entretenimento
            🎧	fone de ouvido
            🎸	guitarra violao instrumento
            🥁	bateria tambor baquetas
            🎹	piano teclado musical
            🎺	trompete instrumento musica
            🎬	cinema claquete cena
            🏋️	academia peso pessoa
            🚴	bicicleta ciclismo ciclista
            🏊	natacao nadar pessoa
            🏄	surf surfista esporte
            ⛷️	esqui esquiador esporte
            🧗	escalada pessoa escalando
            🎣	pescaria pesca entretenimento
            🛹	skate rodinhas skatista
            🏹	arco e flecha
            🤺	esgrimista esgrima espada esporte pessoa
            🏇	corrida cavalos cavalo esporte jockey joquei
            🏂	praticante snowboard esporte inverno neve ski
            🏌️	golfista bola esporte golf golfe jogando
            🏌️‍♂️	homem golfista golfe jogando
            🏌️‍♀️	mulher golfista bola esporte garota golf
            🏄‍♂️	homem surfista surfando
            🏄‍♀️	mulher surfista esporte garota guria mar
            🚣	pessoa remando barco bote canoa mar
            🚣‍♂️	homem remando esporte remador remo
            🚣‍♀️	mulher remando barco caiaque canoa esporte
            🏊‍♂️	homem nadando nadar natacao
            🏊‍♀️	mulher nadando esporte homem nadadora nado
            ⛹️	pessoa jogando basquete arremesso atleta bola
            ⛹️‍♂️	homem jogando basquete bola esporte
            ⛹️‍♀️	mulher jogando basquete basketball bola esporte
            🏋️‍♂️	homem levantando peso esporte forca
            🏋️‍♀️	mulher levantando peso esporte forca garota
            🚴‍♂️	homem ciclista bicicleta esporte passeio
            🚴‍♀️	mulher ciclista bicicleta bike ciclismo esporte
            🚵	pessoa fazendo mountain bike bicicleta ciclismo
            🚵‍♂️	homem fazendo mountain bike bicicleta ciclista
            🚵‍♀️	mulher fazendo mountain bike bicicleta ciclista
            🤸	pessoa fazendo estrela animada animado esporte
            🤸‍♂️	homem fazendo estrela animado esporte estrelinha
            🤸‍♀️	mulher fazendo estrela animada esporte feliz
            🤼	pessoas lutando combate esporte greco romana
            🤼‍♂️	homens lutando combate esporte greco romana
            🤼‍♀️	mulheres lutando combate esporte greco romana
            🤽	pessoa jogando polo aquatico competicao esporte
            🤽‍♂️	homem jogando polo aquatico esporte jogador
            🤽‍♀️	mulher jogando polo aquatico esporte jogadora
            🤾	handebol atleta bola esporte gol handball
            🤾‍♂️	jogador handebol bola esporte handball homem
            🤾‍♀️	jogadora handebol bola esporte gol handball
            🤹	malabarista equilibrio habilidade malabares malabarismo multitarefa
            🤹‍♂️	homem malabarista equilibrio habilidade malabares malabarismo
            🤹‍♀️	mulher malabarista equilibrio habilidade malabarismo multitarefa
            🎋	arvore tanabata comemoracao estrelas festival japonesa
            🎍	decoracao pinhos bambu comemoracao japones pinha
            🎎	bonecas japonesas comemoracao festival japanesas japones
            🎏	bandeira carpas comemoracao koinobori
            🎐	carrilhao vento sino som
            🎑	contemplacao lua comemoracao cerimonia
            🧧	envelope vermelho dinheiro hongbao presente boa
            🎗️	fita lembrete celebracao comemoracao laco
            🎟️	ingresso cinema entrada ticket
            🎫	ingresso bilhete entrada
            🎖️	medalha militar condecoracao premio
            🏅	medalha esportiva vitoria
            🥎	softbol bola esporte luva softball
            🏉	bola rugby americano esporte futebol
            🥏	frisbee disco esporte ultimate voador
            🎳	boliche bola jogo strike
            🏏	criquete betes bola jogo taco
            🏑	hoquei campo bola jogo taco
            🏒	hoquei gelo disco jogo taco
            🥍	lacrosse bastao bola esporte gol raquete
            🥊	luva boxe esporte luta
            🥋	quimono artes marciais esporte faixa judo
            ⛳	bandeira buraco esporte golfe
            ⛸️	patins gelo patinacao
            🤿	mascara mergulho esnorquel mergulhador scuba snorkel
            🎽	camiseta corrida esporte faixa
            🎿	esqui bota esporte esquiar neve
            🛷	treno neve toboga
            🥌	pedra curling jogo
            🪀	ioio brinquedo flutua
            🪁	pipa papagaio planar voar
            🔫	pistola agua arma ferramenta revolver
            🎱	bilhar jogo bola oito
            🔮	bola cristal adivinhacao destino futuro prever
            🪄	varinha magica bruxa condao magia mago
            🧩	quebra cabeca dica encaixe peca
            🪩	globo espelhos balada boate bola brilho
            🪆	boneca russa matriosca matrioska
            ♠️	naipe espadas baralho carta jogo
            ♥️	naipe copas baralho carta jogo s2
            ♦️	naipe ouros baralho carta jogo ouro
            ♣️	naipe paus baralho carta jogo
            🀄	dragao vermelho mahjong jogo peca
            🎴	carta flores baralho hanafuda jogo
            🖼️	quadro emoldurado arte moldura museu pintura
            🧵	carretel agulha barbante costura linha
            🪡	agulha costura alfaiataria bordados linha pontos
            🧶	novelo bola croche la trico tricotar
            🪢	amarrar corda cordel emaranhado fio laco
        """),
        emojiGroup("Lugares e tempo", """
            🔥	fogo top incendio
            🏠	casa construcao domicilio
            🏡	casa jardim construcao
            🏢	predio escritorio edificio
            🏥	hospital doente internado
            🏦	banco predio dinheiro
            🏫	escola colegio predio
            ⛪	igreja capela crista
            🏖️	praia guarda sol
            🏝️	ilha deserta palmeira
            ⛰️	montanha natureza serra
            🌋	vulcao lava natureza
            🏕️	acampamento barraca camping
            🌅	nascer do sol
            🌄	amanhecer montanha aurora
            🌃	noite cidade estrelada
            🌆	por do sol cidade
            🚗	carro automovel veiculo
            🚕	taxi veiculo transporte
            🚙	suv carro trailer
            🚌	onibus veiculo transporte
            🚓	policia viatura policial
            🚑	ambulancia veiculo emergencia
            🚒	bombeiro carro corpo
            🏍️	moto motocicleta corrida
            🚲	bicicleta bike veiculo
            ✈️	aviao viagem aereo
            🚀	foguete rapido espaco
            🛸	disco voador alienigena
            🚁	helicoptero veiculo viagem
            🚂	trem locomotiva vapor
            ⛵	barco vela iate
            🚢	navio aquatico barco
            🗺️	mapa mundi geografia
            🧭	bussola direcao ima
            ☀️	sol clima raios
            🌤️	sol nuvem nuvens
            ☁️	nuvem clima nublado
            🌧️	chuva nuvem chovendo
            ⛈️	tempestade chuva trovao
            🌩️	raios nuvem relampago
            ❄️	neve frio floco
            ☃️	boneco de neve
            🌈	arco iris chuva
            🌊	onda mar agua
            🌪️	tornado clima furacao
            🌙	lua noite crescente
            🌕	lua cheia luar
            🌍	mundo terra globo
            🌱	broto planta muda
            🌳	arvore caidica desfolha
            🌵	cacto deserto natureza
            🌸	flor cerejeira cereja
            🌹	rosa flor amor
            🌻	girassol flor planta
            🍀	trevo sorte quatro
            🍁	folha outono bordo
            💧	gota agua engracado
            💐	buque aniversario flor planta romance
            💮	flor branca carimbo parabens
            🪷	lotus beleza budismo calma flor hinduismo
            🏵️	roseta flor planta primavera amarela
            🥀	flor murcha morrendo murchando
            🌺	hibisco flor planta primavera
            🌼	flor florescer planta
            🌷	tulipa flor jardim
            🪻	jacinto arbusto bluebonnet flor cranio dragao
            🪴	vaso planta casa chato decoracao nutrir
            🌲	conifera arvore floresta natal pinheirinho pinheiro
            🌴	palmeira arvore coqueiro
            🌾	planta arroz comida espiga fazenda graos
            🌿	erva folha planta
            ☘️	trevo irlanda irlandes planta tres folhas
            🍂	folhas caidas cair folha outono
            🍃	folha vento soprando
            🪹	ninho vazio aninhando galhos lar passarinho
            🪺	ninho ovos aninhando galhos lar ovo
            🍄	cogumelo champignon fungo mario planta
            🌎	globo mostrando americas america terra
            🌏	globo mostrando asia oceania australia terra
            🌐	globo meridianos mundo internet
            🗾	mapa japao arquipelago
            🏔️	montanha neve alpes frio
            🗻	monte fuji montanha
            🏜️	deserto clima seco
            🏞️	parque nacional rio reserva florestal
            🏟️	estadio arena futebol
            🏛️	predio grego arquitetura classico
            🏗️	construcao guindaste obra
            🧱	tijolo argamassa argila cimento muro parede
            🪨	pedra pedregulho pesado rocha seixo solido
            🪵	madeira lenha tora tronco serrada
            🛖	cabana abrigo casa yurt
            🏘️	casas bairro vizinhanca
            🏚️	casa abandonada ruina
            🏣	correio japones oriental predio
            🏤	correio europeu predio
            🏨	hotel predio hospedagem
            🏩	motel amor hotel predio
            🏪	loja conveniencia 24h predio 24 horas
            🏬	loja departamentos estabelecimento comercial
            🏭	fabrica predio industria
            🏯	castelo japones pagoda pagode predio
            🏰	castelo predio europeu medieval
            💒	capela casamento romance
            🗼	torre toquio japao
            🗽	estatua liberdade nova york
            🕌	mesquita isla muculmano religiao
            🛕	templo hindu religiao
            🕍	sinagoga judaismo judeu judia religiao templo
            ⛩️	santuario japones oriental religiao xintoismo
            🕋	caaba isla muculmano religiao
            ⛲	fonte agua chafariz praca
            ⛺	barraca acampamento acampar
            🌁	enevoado bruma cerracao neblina nevoa nevoeiro
            🏙️	cidade predios urbano
            🌇	sol anoitecer calor cidade entardecer predio
            🌉	ponte noite pontilhao
            ♨️	chamas calor fogo fumaca quente
            🎠	carrossel cavalo entretenimento parque diversao
            🛝	escorregador brincar brinquedo escorregar parquinho playground
            🎡	roda gigante entretenimento parque diversoes
            🎢	montanha russa entretenimento parque diversoes tematico
            💈	barbearia barbeiro poste barber pole cortar
            🎪	circo entretenimento lona tenda
            🚃	vagao trem bonde eletrico ferroviario transporte
            🚄	trem alta velocidade bala japones trens
            🚅	trem alta velocidade japones bala ferrovia
            🚆	trem ferrovia veiculo
            🚇	metro trem tunel veiculo subterraneo
            🚈	trem urbano chegada cheguei leve veiculo
            🚉	estacao metro trem
            🚊	bonde eletrico trolebus veiculo
            🚝	monotrilho trem veiculo
            🚞	estrada ferro montanha carro teleferico veiculo
            🚋	bonde carro trolebus veiculo eletrico
            🚍	onibus aproximando veiculo
            🚎	trolebus onibus veiculo bonde eletrico movido
            🚐	van onibus veiculo veraneio mini
            🚔	viatura policial aproximando policia veiculo
            🚖	taxi aproximando chegada transporte veiculo
            🚘	carro aproximando automovel carona chegada veiculo
            🛻	caminhonete automovel cacamba caminhao carro picape
            🚚	caminhao entrega veiculo
            🚛	caminhao semi trailer veiculo articulado
            🚜	trator obra veiculo
            🏎️	carro corrida automobilismo
            🛵	scooter monareta motinho moto motoca
            🦽	cadeira rodas manual acessibilidade
            🦼	cadeira rodas motorizada acessibilidade
            🛺	automovel riquixa autorriquixa tuk
            🛴	patinete brinquedo transporte
            🛼	patins rodas esporte patinacao roller
            🚏	ponto onibus bus busao transporte publico
            🛣️	estrada caminho viagem viajar
            🛤️	trilhos trem ferrovia
            🛢️	barril oleo lata latao
            ⛽	posto gasolina abastecer alcool bomba combustivel
            🛞	roda carro circulo direcao girar pneu
            🚨	sirene carro policia farol policial luz
            🚥	semaforo horizontal cruzamento luz sinal sinaleira
            🚦	semaforo vertical cruzamento luz sinal sinaleira
            🛑	sinal pare transito
            🚧	construcao obras barreira
            ⚓	ancora marinha navegar sinal
            🛟	boia colete flutuar guarda vidas guardar
            🛶	canoa barco remo
            🚤	lancha barco ferias praia veiculo verao
            🛳️	cruzeiro barco embarcacao navio passageiros veiculo
            ⛴️	balsa barco ferry boat navegar
            🛥️	barco lancha motor navio veiculo
            🛩️	aviao pequeno aereo jatinho jato veiculo
            🛫	aviao decolando decolar ferias fui partindo
            🛬	aviao aterrissando aterrissagem chegando pousando veiculo
            🪂	paraquedas asa delta paraquedista parasail saltar
            💺	assento cadeira poltrona
            🚟	estrada ferro suspensa suspensao trem veiculo
            🚠	teleferico montanha bonde cabo veiculo suspenso
            🚡	teleferico aerea bonde gondola linha veiculo
            🛰️	satelite antena espaco
            🛎️	sineta hotel portaria sino
            ⌛	ampulheta areia tempo
            ⏱️	cronometro relogio tempo
            ⏲️	relogio temporizador cronometro timer
            🕰️	relogio mesa antigo
            🕛	12 horas 12h00 doze meia noite
            🕧	doze meia 12h30 relogio
            🕐	1 hora 13h 1h 1h00 relogio
            🕜	meia 1 30 13 13h30 1h30
            🕑	2 horas 14h 2h 2h00 duas
            🕝	duas meia 2 30 2h30 relogio
            🕒	3 horas 15h 00 3h00 hora
            🕞	tres meia 3 30 3h30 relogio
            🕓	4 horas 00 4h00 horario quatro
            🕟	quatro meia 4h30 relogio
            🕔	5 horas 5h00 cinco relogio
            🕠	cinco meia 17h30 5h30 relogio
            🕕	6 horas 18h 6h00 relogio seis
            🕡	seis meia 18h30 6h30 relogio
            🕖	7 horas 19h 7h00 relogio sete
            🕢	sete meia 7 30 7h30 relogio
            🕗	8 horas 00 8h00 hora oito
            🕣	oito meia 8 30 8h30 relogio
            🕘	9 horas 21h 9h00 nove relogio
            🕤	nove meia 9 30 9h30 hora
            🕙	10 horas 10h00 22h dez relogio
            🕥	dez meia 10h30 22h30 relogio
            🕚	11 horas 00 11h00 onze relogio
            🕦	onze meia 11h30 23h30 relogio
            🌑	lua nova escuro noite negra
            🌒	lua crescente concava noite
            🌓	quarto crescente lua
            🌔	lua crescente convexa
            🌖	lua minguante convexa
            🌗	quarto minguante lua
            🌘	lua minguante concava
            🌚	rosto lua nova
            🌛	rosto lua quarto crescente noite
            🌜	rosto lua quarto minguante
            🌡️	termometro clima temperatura tempo
            🌝	rosto lua cheia
            🌞	rosto sol praia
            🪐	planeta aneis saturnino saturno
            🌠	estrela cadente cai
            🌌	via lactea ceu espaco estrelado
            ⛅	sol tras nuvens clima nublado nuvem
            🌥️	nublado clima nuvem sol
            🌦️	sol chuva clima nuvem
            🌨️	nuvem neve clima frio
            🌫️	nevoeiro clima furacao neblina nuvem
            🌬️	rosto vento clima nuvem soprar sopro
            🌀	ciclone clima espiral furacao tonto twister
            🌂	guarda chuva fechado chuvoso
            ☔	sombrinha chuva acessorio clima gotas guarda
            ⛱️	guarda sol chuva clima praia sombra
            ⛄	boneco neve sem frio inverno
            ☄️	cometa espaco meteoro satelite estrela cadente
        """),
        emojiGroup("Coisas", """
            💻	computador notebook laptop
            🖥️	monitor pc computador
            ⌨️	teclado acessorio computador
            🖱️	mouse acessorio computador
            📱	celular telefone movel
            ☎️	telefone gancho ligacao
            📞	ligacao chamada telefone
            📲	celular chamando telefone
            💾	disquete salvar computador
            💿	cd disco blu
            📷	camera foto selfie
            📹	filmadora video camera
            🎥	camera cinema entretenimento
            📺	televisao canal tv
            📻	radio radiola som
            🔌	tomada energia eletrica
            🔋	bateria pilha energia
            🪫	bateria fraca pouca
            💡	ideia lampada eletrica
            🔦	lanterna eletrica ferramenta
            🔍	lupa buscar procurar
            🔎	lupa pesquisa direita
            🔒	cadeado trancado seguro
            🔓	cadeado aberto destrancado
            🔑	chave senha trancado
            🗝️	chave antiga fechadura
            🔧	chave inglesa ferramenta
            🔨	martelo construcao ferramenta
            🛠️	ferramentas conserto martelo
            ⚙️	engrenagem configuracao ferramenta
            🧰	caixa de ferramentas
            🧲	ima atracao ferradura
            🧪	tubo de ensaio teste
            🧬	dna biologista biologo
            🔬	microscopio ciencia ferramenta
            🔭	telescopio ciencia ferramenta
            📡	antena satelite parabolica
            💊	remedio comprimido capsula
            🩹	curativo band aid
            🚿	chuveiro banho agua
            🛏️	cama dormir hotel
            🛋️	sofa luminaria hotel
            🚪	porta fechada entrada
            🪑	cadeira assento sentar
            🧹	vassoura limpar bruxa
            🧺	cesto roupa cesta
            🗑️	lixo apagar lixeira
            📦	caixa pacote embrulho
            📫	correio caixa fechada
            📝	anotar nota escrever
            📄	documento folha pagina
            📊	grafico barras barra
            📈	grafico subindo crescimento
            📉	grafico caindo diagrama
            📅	calendario data datas
            📌	alfinete fixar tacha
            📎	clipe anexo papel
            ✂️	tesoura cortar aberta
            📚	livros estudar biblioteca
            📖	livro aberto biblioteca
            💰	dinheiro saco dolares
            💵	dinheiro nota dolar
            💳	cartao credito banco
            🧾	recibo nota fiscal
            ⏰	despertador hora alarme
            ⏳	ampulheta tempo contando
            ⌚	relogio pulso hora
            🎓	formatura diploma chapeu
            🏷️	etiqueta identificar rotulo
            🔗	link corrente aneis
            🛒	carrinho compras mercado
            🧳	mala viagem bagagem
            ☂️	guarda chuva acessorio
            🪙	moeda dinheiro dolar
            🧸	ursinho pelucia bichinho
            👓	oculos acessorio visao
            🕶️	oculos escuros sol
            🥽	oculos protecao mergulho natacao olhos ski
            🥼	jaleco cientista dr experiencia experimento medico
            🦺	colete salva vidas emergencia refletivo seguranca
            👔	gravata acessorio roupa
            👕	camiseta camisa roupa
            👖	jeans calca roupa
            🧣	cachecol frio inverno pescoco
            🧤	luvas inverno luva mao
            🧥	casaco agasalho blusa frio jaqueta
            🧦	meias meia meiao
            👗	vestido roupa peca unica
            👘	quimono roupa vestir
            🥻	sari india indiana roupa vestido
            🩱	maio banho nadar praia roupa
            🩲	cueca banho intima nadar praia roupa
            🩳	shorts banho bermuda nadar praia roupa
            👙	biquini banho piscina praia roupa
            👚	roupas femininas azul blusa camiseta feminina
            🪭	leque dobravel abanar arejar balancar calor
            👛	bolsinha acessorio bolsa moeda niqueleira porta
            👜	bolsa mao acessorio fashion mala
            👝	bolsa pequena acessorio carteira fashion
            🛍️	sacolas compras presente shopping
            🎒	mochila bolsa escola viagem viajar
            🩴	chinelo alpargatas piscina praia rasteirinha sandalia
            👞	sapato masculino acessorio sapatos
            👟	tenis corrida acessorio correr esportivo sapato
            🥾	bota trekking acampamento acampar caminhada marrom
            🥿	sapatilha confortavel sapato bale sem fivela
            👠	sapato salto alto acessorio chique fashion
            👡	sandalia feminina acessorio feminino medio salto
            🩰	sapatilha bale bailarina danca
            👢	bota feminina acessorio calcado medio salto
            🪮	pente cabelo afro crespo pentear retro
            👒	chapeu feminino acessorio fashion
            🎩	cartola chapeu chique entretenimento roupa
            🧢	bone chapeu beisebol
            🪖	capacete militar exercito guerra guerreiro soldado
            ⛑️	capacete socorrista ajuda branca capacete cruz
            📿	rosario oracao acessorio religiao reza terco
            💄	batom cosmeticos makeup maquiagem vermelho
            💍	anel diamante romance
            💎	pedra preciosa diamante joia
            🔈	alto falante volume baixo mudo musica
            🔉	alto falante volume medio baixo diminuir
            📢	buzina alto falante aviso comunicado discurso
            📣	megafone aplausos comunicacao
            📯	corneta postal correios
            🎼	partitura musical musica
            🎙️	microfone estudio musica
            🎚️	controle volume musica
            🎛️	botoes giratorios controle musica
            🎷	saxofone instrumento musica sax musical
            🪗	acordeao concertina instrumento musica sanfona
            🎻	violino instrumento musica musical
            🪕	banjo cordas instrumento musica
            🪘	tambor comprido atabaque batida conga instrumento
            🪇	maracas chacoalhar chocalho danca festa instrumento
            🪈	flauta banda flautim instrumento marcial musica
            📟	pager comunicacao mensagem
            📠	fax comunicacao maquina
            🖨️	impressora acessorio computador documento impressao imprimir
            🖲️	trackball acessorio bolinha computador mouse
            💽	minidisc computacao disc disco hd md
            📀	dvd blu ray cd computador disco
            🧮	abaco calculadora calculo matematica numero
            🎞️	rolo filmes cinema filme
            📽️	projetor filmes cinema filme video
            📸	camera flash foto fotografia video
            📼	videocassete cassete fita vhs video
            🏮	lanterna vermelha papel bar izakaya restaurante
            🪔	lampada oleo diya
            📔	caderno decorado agenda capa decoracao escola
            📕	livro fechado apostila
            📗	livro verde biblioteca caderno colegio escola
            📘	livro azul apostila escola estudo leitura
            📙	livro laranja apostila cartilha
            📓	caderno folhas anotacoes
            📒	livro contabil caderno
            📃	pagina dobrada dobrado documento papel
            📜	pergaminho rolo papel
            📰	jornal noticias reportagem
            🗞️	jornal enrolado noticias
            📑	marcadores pagina marcador marcar
            🔖	marcador pagina livro
            💴	nota iene cedula dinheiro grana moeda
            💶	nota euro cedula dinheiro grana moeda
            💷	nota libra cedula dinheiro grana moeda
            💸	dinheiro voando asas banco cedula nota
            💹	grafico subindo iene ascendente ascensao crescimento
            ✉️	envelope carta correspondencia mail email
            📧	mail carta comunicacao correspondencia
            📨	envelope chegando carta comunicacao correspondencia mail
            📩	envelope seta carta comunicacao correspondencia mail
            📤	bandeja saida caixa comunicacao correspondencia enviada
            📥	bandeja entrada caixa comunicacao correspondencia mail
            📪	caixa correio fechada bandeira abaixada correspondencia
            📬	caixa correio aberta bandeira levantada correspondencia
            📭	caixa correio aberta bandeira abaixada correspondencia
            📮	caixa correio carta correspondencia enviar
            🗳️	urna eleitoral cedula eleicao votar voto
            ✏️	lapis escrever desenhar
            ✒️	ponta caneta tinteiro preto tinta
            🖋️	caneta tinteiro destro
            🖊️	caneta esferografica tinteiro
            🖌️	pincel pintando pintar
            🖍️	giz cera desenho vermelho
            💼	maleta mala pasta
            📁	pasta arquivos arquivo
            📂	pasta arquivos aberta abrir arquivo
            🗂️	divisores pastas arquivo divisor indice organizador
            📆	calendario folhas destacaveis data destacavel dia
            🗒️	bloco espiral caderno
            🗓️	calendario espiral bloco data
            📇	indice cartoes contatos
            📋	prancheta anotacoes lista
            📍	tacha redonda alfinete localizacao mapa
            🖇️	clipes papel conectados clipe
            📏	regua reta medir
            📐	regua triangular angulo geometria matematica triangulo
            🗃️	caixa arquivos arquivo cartao documentos ficheiro
            🗄️	gavetas escritorio arquivo gabinete
            🔏	cadeado caneta privacidade privado tinteiro
            🔐	cadeado fechado chave seguro trancado
            🪓	machado cortar madeira partir
            ⛏️	picareta ferramenta mineracao
            ⚒️	martelo picareta ferramenta geologia
            🗡️	adaga arma faca
            ⚔️	espadas cruzadas arma batalha guerra
            💣	bomba emocao engracado explosao explosivo perigo
            🪃	bumerangue aborigene arma australia rebote repercussao
            🛡️	escudo protecao defesa
            🪚	serrote carpinteiro cortar ferramenta madeira serra
            🪛	chave fenda ferramenta
            🔩	porca parafuso construcao ferramenta
            🗜️	bracadeira compressao ferramenta morsa torno
            ⚖️	balanca ferramenta justica libra peso zodiaco
            🦯	bengala cegos acessibilidade cega cego cegueira
            ⛓️	correntes corrente metal
            🪝	gancho bandido curva pegar prender atracao
            🪜	escada degrau mao subir
            ⚗️	alambique ferramenta quimica
            🧫	placa petri bacteria biologia biologista biologo
            💉	seringa agulha injecao medico remedio vacina
            🩸	gota sangue doacao medicina menstruacao sangrar
            🩼	muleta ajuda bastao bengala deficiencia desculpa
            🩺	estetoscopio coracao medica medicina medico tum
            🩻	raio doutor esqueleto medico ossos radiografia
            🛗	elevador acessibilidade descer elevar subir
            🪞	espelho especulo maquiagem refletor reflexao reflexo
            🪟	janela abertura ar fresco quadro transparente
            🚽	vaso sanitario banheiro patente privada toalete
            🪠	desentupidor banheiro coco encanador fezes sanitario
            🛁	banheira banho relaxar
            🪤	ratoeira armadilha atrair isca prender queijo
            🪒	lamina afiada afiado barbeador barbear depilar
            🧴	frasco locao condicionador creme hidratante protetor
            🧷	alfinete seguranca fralda punk rock
            🧻	rolo papel higienico toalha
            🪣	balde baldinho barril
            🧼	sabonete banho barra espuma limpar limpeza
            🫧	bolhas agua aquatica aquatico arroto bolha
            🪥	escova dentes banheiro dental higiene limpeza
            🧽	esponja absorcao absorvente encharcar limpeza porosa
            🧯	extintor incendio apagar extinguir fogo
            🚬	cigarro fumante fumar fumo simbolo permitido
            ⚰️	caixao funeral morte velorio funerario
            🪦	lapide cemiterio morto rip sepultura tumba
            ⚱️	urna funeraria cinzas funeral morte
            🧿	olho grego amuleto conta mau olhado
            🪬	hamsa amuleto fatima guia mao maria
            🗿	moai estatua moyai rosto
            🪧	placa aviso cartaz demonstracao letreiro piquete
            🪪	cartao identificacao carta carteira cracha credenciais
        """),
        emojiGroup("Bandeiras", """
            🇧🇷	bandeira brasil br brasileiro
            🇵🇹	bandeira portugal pt
            🇦🇷	bandeira argentina ar
            🇺🇸	bandeira estados unidos eua usa americano
            🇺🇾	bandeira uruguai uy
            🇵🇾	bandeira paraguai py
            🇨🇱	bandeira chile cl
            🇧🇴	bandeira bolivia bo
            🇵🇪	bandeira peru pe
            🇨🇴	bandeira colombia co
            🇻🇪	bandeira venezuela ve
            🇲🇽	bandeira mexico mx
            🇪🇸	bandeira espanha es
            🇫🇷	bandeira franca fr
            🇮🇹	bandeira italia it
            🇩🇪	bandeira alemanha de
            🇬🇧	bandeira reino unido britanico uk
            🇯🇵	bandeira japao jp
            🇨🇦	bandeira canada ca
            🏁	bandeira quadriculada chegada corrida esporte vitoria
            🚩	bandeira triangular informacoes localizacao poste vermelha
            🎌	bandeiras cruzadas bandeira comemoracao cruzar japao
            🏴	bandeira preta tremulando
            🏳️	bandeira branca paz tremulando
            🏳️‍🌈	bandeira arco iris bissexual gay lesbica
            🏳️‍⚧️	bandeira transgenero branco rosa azul claro
            🏴‍☠️	bandeira pirata caveira osso saque tesouro
            🇦🇨	bandeira ilha ascensao ac
            🇦🇩	bandeira andorra ad
            🇦🇪	bandeira emirados arabes unidos ae
            🇦🇫	bandeira afeganistao af
            🇦🇬	bandeira antigua barbuda ag
            🇦🇮	bandeira anguila ai
            🇦🇱	bandeira albania al
            🇦🇲	bandeira armenia am
            🇦🇴	bandeira angola ao
            🇦🇶	bandeira antartida aq
            🇦🇸	bandeira samoa americana as
            🇦🇹	bandeira austria at
            🇦🇺	bandeira australia au
            🇦🇼	bandeira aruba aw
            🇦🇽	bandeira ilhas aland ax
            🇦🇿	bandeira azerbaijao az
            🇧🇦	bandeira bosnia herzegovina ba
            🇧🇧	bandeira barbados bb
            🇧🇩	bandeira bangladesh bd
            🇧🇪	bandeira belgica be
            🇧🇫	bandeira burquina faso bf
            🇧🇬	bandeira bulgaria bg
            🇧🇭	bandeira barein bh
            🇧🇮	bandeira burundi bi
            🇧🇯	bandeira benin bj
            🇧🇱	bandeira sao bartolomeu bl
            🇧🇲	bandeira bermudas bm
            🇧🇳	bandeira brunei bn
            🇧🇶	bandeira paises baixos caribenhos bq
            🇧🇸	bandeira bahamas bs
            🇧🇹	bandeira butao bt
            🇧🇻	bandeira ilha bouvet bv
            🇧🇼	bandeira botsuana bw
            🇧🇾	bandeira bielorrussia by
            🇧🇿	bandeira belize bz
            🇨🇨	bandeira ilhas cocos keeling cc
            🇨🇩	bandeira congo kinshasa cd
            🇨🇫	bandeira republica centro africana cf
            🇨🇬	bandeira republica congo cg
            🇨🇭	bandeira suica ch
            🇨🇮	bandeira costa marfim ci
            🇨🇰	bandeira ilhas cook ck
            🇨🇲	bandeira camaroes cm
            🇨🇳	bandeira china cn
            🇨🇵	bandeira ilha clipperton cp
            🇨🇷	bandeira costa rica cr
            🇨🇺	bandeira cuba cu
            🇨🇻	bandeira cabo verde cv
            🇨🇼	bandeira curacao cw
            🇨🇽	bandeira ilha christmas cx
            🇨🇾	bandeira chipre cy
            🇨🇿	bandeira tchequia cz
            🇩🇬	bandeira diego garcia dg
            🇩🇯	bandeira djibuti dj
            🇩🇰	bandeira dinamarca dk
            🇩🇲	bandeira dominica dm
            🇩🇴	bandeira republica dominicana do
            🇩🇿	bandeira argelia dz
            🇪🇦	bandeira ceuta melilla ea
            🇪🇨	bandeira equador ec
            🇪🇪	bandeira estonia ee
            🇪🇬	bandeira egito eg
            🇪🇭	bandeira saara ocidental eh
            🇪🇷	bandeira eritreia er
            🇪🇹	bandeira etiopia et
            🇪🇺	bandeira uniao europeia eu
            🇫🇮	bandeira finlandia fi
            🇫🇯	bandeira fiji fj
            🇫🇰	bandeira ilhas malvinas fk
            🇫🇲	bandeira micronesia fm
            🇫🇴	bandeira ilhas faroe fo
            🇬🇦	bandeira gabao ga
            🇬🇩	bandeira granada gd
            🇬🇪	bandeira georgia ge
            🇬🇫	bandeira guiana francesa gf
            🇬🇬	bandeira guernsey gg
            🇬🇭	bandeira gana gh
            🇬🇮	bandeira gibraltar gi
            🇬🇱	bandeira groenlandia gl
            🇬🇲	bandeira gambia gm
            🇬🇳	bandeira guine gn
            🇬🇵	bandeira guadalupe gp
            🇬🇶	bandeira guine equatorial gq
            🇬🇷	bandeira grecia gr
            🇬🇸	bandeira ilhas georgia sul sandwich gs
            🇬🇹	bandeira guatemala gt
            🇬🇺	bandeira guam gu
            🇬🇼	bandeira guine bissau gw
            🇬🇾	bandeira guiana gy
            🇭🇰	bandeira hong kong rae china hk
            🇭🇲	bandeira ilhas heard mcdonald hm
            🇭🇳	bandeira honduras hn
            🇭🇷	bandeira croacia hr
            🇭🇹	bandeira haiti ht
            🇭🇺	bandeira hungria hu
            🇮🇨	bandeira ilhas canarias ic
            🇮🇩	bandeira indonesia id
            🇮🇪	bandeira irlanda ie
            🇮🇱	bandeira israel il
            🇮🇲	bandeira ilha man im
            🇮🇳	bandeira india in
            🇮🇴	bandeira territorio britanico oceano indico io
            🇮🇶	bandeira iraque iq
            🇮🇷	bandeira ira ir
            🇮🇸	bandeira islandia is
            🇯🇪	bandeira jersey je
            🇯🇲	bandeira jamaica jm
            🇯🇴	bandeira jordania jo
            🇰🇪	bandeira quenia ke
            🇰🇬	bandeira quirguistao kg
            🇰🇭	bandeira camboja kh
            🇰🇮	bandeira quiribati ki
            🇰🇲	bandeira comores km
            🇰🇳	bandeira sao cristovao nevis kn
            🇰🇵	bandeira coreia norte kp
            🇰🇷	bandeira coreia sul kr
            🇰🇼	bandeira kuwait kw
            🇰🇾	bandeira ilhas cayman ky
            🇰🇿	bandeira cazaquistao kz
            🇱🇦	bandeira laos la
            🇱🇧	bandeira libano lb
            🇱🇨	bandeira santa lucia lc
            🇱🇮	bandeira liechtenstein li
            🇱🇰	bandeira sri lanka lk
            🇱🇷	bandeira liberia lr
            🇱🇸	bandeira lesoto ls
            🇱🇹	bandeira lituania lt
            🇱🇺	bandeira luxemburgo lu
            🇱🇻	bandeira letonia lv
            🇱🇾	bandeira libia ly
            🇲🇦	bandeira marrocos ma
            🇲🇨	bandeira monaco mc
            🇲🇩	bandeira moldavia md
            🇲🇪	bandeira montenegro me
            🇲🇫	bandeira sao martinho mf
            🇲🇬	bandeira madagascar mg
            🇲🇭	bandeira ilhas marshall mh
            🇲🇰	bandeira macedonia norte mk
            🇲🇱	bandeira mali ml
            🇲🇲	bandeira mianmar birmania mm
            🇲🇳	bandeira mongolia mn
            🇲🇴	bandeira macau rae china mo
            🇲🇵	bandeira ilhas marianas norte mp
            🇲🇶	bandeira martinica mq
            🇲🇷	bandeira mauritania mr
            🇲🇸	bandeira montserrat ms
            🇲🇹	bandeira malta mt
            🇲🇺	bandeira mauricio mu
            🇲🇻	bandeira maldivas mv
            🇲🇼	bandeira malaui mw
            🇲🇾	bandeira malasia my
            🇲🇿	bandeira mocambique mz
            🇳🇦	bandeira namibia na
            🇳🇨	bandeira nova caledonia nc
            🇳🇪	bandeira niger ne
            🇳🇫	bandeira ilha norfolk nf
            🇳🇬	bandeira nigeria ng
            🇳🇮	bandeira nicaragua ni
            🇳🇱	bandeira paises baixos holanda neerlandes
            🇳🇴	bandeira noruega no
            🇳🇵	bandeira nepal np
            🇳🇷	bandeira nauru nr
            🇳🇺	bandeira niue nu
            🇳🇿	bandeira nova zelandia nz
            🇴🇲	bandeira oma om
            🇵🇦	bandeira panama pa
            🇵🇫	bandeira polinesia francesa pf
            🇵🇬	bandeira papua nova guine pg
            🇵🇭	bandeira filipinas ph
            🇵🇰	bandeira paquistao pk
            🇵🇱	bandeira polonia pl
            🇵🇲	bandeira sao pedro miquelao pm
            🇵🇳	bandeira ilhas pitcairn pn
            🇵🇷	bandeira porto rico pr
            🇵🇸	bandeira territorios palestinos ps
            🇵🇼	bandeira palau pw
            🇶🇦	bandeira catar qa
            🇷🇪	bandeira reuniao re
            🇷🇴	bandeira romenia ro
            🇷🇸	bandeira servia rs
            🇷🇺	bandeira russia ru
            🇷🇼	bandeira ruanda rw
            🇸🇦	bandeira arabia saudita sa
            🇸🇧	bandeira ilhas salomao sb
            🇸🇨	bandeira seicheles sc
            🇸🇩	bandeira sudao sd
            🇸🇪	bandeira suecia se
            🇸🇬	bandeira singapura sg
            🇸🇭	bandeira santa helena sh
            🇸🇮	bandeira eslovenia si
            🇸🇯	bandeira svalbard jan mayen sj
            🇸🇰	bandeira eslovaquia sk
            🇸🇱	bandeira serra leoa sl
            🇸🇲	bandeira san marino sm
            🇸🇳	bandeira senegal sn
            🇸🇴	bandeira somalia so
            🇸🇷	bandeira suriname sr
            🇸🇸	bandeira sudao sul ss
            🇸🇹	bandeira sao tome principe st
            🇸🇻	bandeira el salvador sv
            🇸🇽	bandeira sint maarten sx
            🇸🇾	bandeira siria sy
            🇸🇿	bandeira essuatini sz
            🇹🇦	bandeira tristao cunha ta
            🇹🇨	bandeira ilhas turcas caicos tc
            🇹🇩	bandeira chade td
            🇹🇫	bandeira territorios franceses sul tf
            🇹🇬	bandeira togo tg
            🇹🇭	bandeira tailandia th
            🇹🇯	bandeira tadjiquistao tj
            🇹🇰	bandeira tokelau tk
            🇹🇱	bandeira timor leste tl
            🇹🇲	bandeira turcomenistao tm
            🇹🇳	bandeira tunisia tn
            🇹🇴	bandeira tonga to
            🇹🇷	bandeira turquia tr
            🇹🇹	bandeira trinidad tobago tt
            🇹🇻	bandeira tuvalu tv
            🇹🇼	bandeira taiwan tw
            🇹🇿	bandeira tanzania tz
            🇺🇦	bandeira ucrania ua
            🇺🇬	bandeira uganda ug
            🇺🇲	bandeira ilhas menores distantes eua um
            🇺🇳	bandeira nacoes unidas un
            🇺🇿	bandeira uzbequistao uz
            🇻🇦	bandeira cidade vaticano va
            🇻🇨	bandeira sao vicente granadinas vc
            🇻🇬	bandeira ilhas virgens britanicas vg
            🇻🇮	bandeira ilhas virgens americanas vi
            🇻🇳	bandeira vietna vn
            🇻🇺	bandeira vanuatu vu
            🇼🇫	bandeira wallis futuna wf
            🇼🇸	bandeira samoa ws
            🇽🇰	bandeira kosovo xk
            🇾🇪	bandeira iemen ye
            🇾🇹	bandeira mayotte yt
            🇿🇦	bandeira africa sul za
            🇿🇲	bandeira zambia zm
            🇿🇼	bandeira zimbabue zw
        """),
    )
}
