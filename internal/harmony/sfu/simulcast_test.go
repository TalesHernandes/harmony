package sfu

import (
	"io"
	"log/slog"
	"strings"
	"testing"
	"time"

	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"
)

// The rewriting is the part that cannot be seen from outside: if it is wrong,
// the picture does not fail — it stutters on somebody else's machine, once in a
// while, in a way nobody can reproduce. So it is checked here, in arithmetic.

// A publication with no layers arriving: enough for the choosing to run and
// write its line, and askLayer finds nothing to ask, which is the truth.
func layered(t *testing.T, start string) *forwarder {
	t.Helper()
	quiet := slog.New(slog.NewTextHandler(io.Discard, nil))
	// Uma conexao de verdade, sem rede nenhuma: pedir um quadro novo escreve
	// RTCP nela e falha em silencio, que e exatamente o que tem de acontecer
	// quando a camada pedida nao esta chegando.
	pc, err := webrtc.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		t.Fatalf("peer connection: %v", err)
	}
	t.Cleanup(func() { _ = pc.Close() })
	sess := &session{id: "s1", username: "ana", pc: pc, room: &room{sfu: &SFU{log: quiet}}}
	pub := &publication{owner: sess, layers: map[string]*layer{}, outs: map[string]*forwarder{}}
	// want e lossWant nascem iguais, que e o estado de quem ja se acomodou numa
	// camada sem ninguem ter pedido teto nenhum.
	return &forwarder{
		pub: pub, sess: sess,
		want: start, lossWant: start,
		lastMove: time.Now().Add(-time.Hour),
	}
}

func pkt(seq uint16, ts uint32) *rtp.Packet {
	return &rtp.Packet{Header: rtp.Header{SequenceNumber: seq, Timestamp: ts}}
}

func TestASubscriberSeesOneUnbrokenStreamAcrossALayerChange(t *testing.T) {
	f := layered(t, "alta")

	// Three ordinary packets of the big layer.
	for i, p := range []*rtp.Packet{pkt(100, 9000), pkt(101, 9000), pkt(102, 12000)} {
		out, ok := f.next("alta", p, i == 0)
		if !ok {
			t.Fatalf("packet %d refused", i)
		}
		if out.SequenceNumber != p.SequenceNumber || out.Timestamp != p.Timestamp {
			t.Fatalf("packet %d was changed before any layer change: %v", i, out)
		}
	}

	// The small layer is refused while this subscriber is on the big one, even
	// when it is a fresh full picture: it has not been asked for.
	if _, ok := f.next("baixa", pkt(7000, 500000), true); ok {
		t.Fatal("a layer nobody asked for was forwarded")
	}

	// Now it is asked for, but the next packet is mid-picture.
	f.mu.Lock()
	f.want = "baixa"
	f.mu.Unlock()
	if _, ok := f.next("baixa", pkt(7001, 500000), false); ok {
		t.Fatal("changed layer in the middle of a picture")
	}

	// A fresh full picture: the change happens, and the numbers the subscriber
	// sees carry on from where they were — not from where the small layer is.
	out, ok := f.next("baixa", pkt(7002, 500000), true)
	if !ok {
		t.Fatal("refused a keyframe on the layer it had asked for")
	}
	if out.SequenceNumber != 103 {
		t.Fatalf("sequence jumped to %d, wanted 103", out.SequenceNumber)
	}
	// O tempo da troca e o tempo que passou de verdade, nunca zero e nunca
	// para tras. O teste roda em microssegundos, entao o salto e pequeno — o
	// que importa e que ele existe e e para a frente.
	troca := out.Timestamp
	if troca <= 12000 {
		t.Fatalf("a imagem nova diz %d, que nao e depois de 12000", troca)
	}
	if troca > 12000+90000 {
		t.Fatalf("a troca pulou %d ticks, mais de um segundo", troca-12000)
	}

	// And from there the small layer's own gaps come through as gaps, because
	// a gap is true and the subscriber has to know to ask for it again.
	out, _ = f.next("baixa", pkt(7005, 503000), false)
	if out.SequenceNumber != 106 {
		t.Fatalf("a gap of 3 came out as %d, wanted 106", out.SequenceNumber)
	}
	if out.Timestamp != troca+3000 {
		t.Fatalf("timestamp advanced to %d, wanted %d", out.Timestamp, troca+3000)
	}

	// The big layer is now somebody else's; it must not leak into this one.
	if _, ok := f.next("alta", pkt(103, 15000), true); ok {
		t.Fatal("the layer this subscriber left was forwarded to it")
	}
}

func TestSequenceNumbersWrapWithoutABreak(t *testing.T) {
	f := layered(t, "alta")
	f.next("alta", pkt(65534, 1000), true)
	out, _ := f.next("alta", pkt(65535, 2000), false)
	if out.SequenceNumber != 65535 {
		t.Fatalf("got %d", out.SequenceNumber)
	}
	out, _ = f.next("alta", pkt(0, 3000), false)
	if out.SequenceNumber != 0 {
		t.Fatalf("wrapping produced %d, wanted 0", out.SequenceNumber)
	}
}

// A line that is losing packets should fall, and a line that is fine should
// climb — but neither in a hurry, because a picture that changes size every
// two seconds is worse to watch than one that is slightly too small.

func TestABadLineFallsOneStepAtATime(t *testing.T) {
	f := layered(t, "alta")

	f.report(0.20)
	if got := f.watching(); got != "media" {
		t.Fatalf("heavy loss left the subscriber on %q", got)
	}
	// Immediately again: the hold is what stops a bad second from emptying the
	// whole ladder at once.
	f.report(0.20)
	if got := f.watching(); got != "media" {
		t.Fatalf("fell twice inside the hold, to %q", got)
	}

	f.mu.Lock()
	f.lastMove = time.Now().Add(-holdAfterMove - time.Second)
	f.mu.Unlock()
	f.report(0.20)
	if got := f.watching(); got != "baixa" {
		t.Fatalf("second fall landed on %q", got)
	}

	// And there is nothing below the bottom.
	f.mu.Lock()
	f.lastMove = time.Now().Add(-holdAfterMove - time.Second)
	f.mu.Unlock()
	f.report(0.20)
	if got := f.watching(); got != "baixa" {
		t.Fatalf("fell off the bottom to %q", got)
	}
}

func TestClimbingBackNeedsSeveralCleanReportsInARow(t *testing.T) {
	f := layered(t, "baixa")

	// Almost-fine reports never add up to a promotion.
	for i := 0; i < cleanBeforeUp*3; i++ {
		f.report(0.03)
	}
	if got := f.watching(); got != "baixa" {
		t.Fatalf("an almost-fine line was promoted to %q", got)
	}

	// Clean ones do, once there are enough of them.
	for i := 0; i < cleanBeforeUp-1; i++ {
		f.report(0)
	}
	if got := f.watching(); got != "baixa" {
		t.Fatalf("climbed on %d clean reports, before %d", cleanBeforeUp-1, cleanBeforeUp)
	}
	f.report(0)
	if got := f.watching(); got != "media" {
		t.Fatalf("did not climb after %d clean reports: %q", cleanBeforeUp, got)
	}
}

// O pacote atrasado, que foi o defeito que o Codex achou em 18/09/2026: ele
// anda com a numeracao de saida para tras, corretamente, e a proxima troca de
// camada nao pode partir dai — partiria de um numero ja usado, e o receptor
// jogaria a imagem nova fora achando que era repetida.
func TestALatePacketDoesNotMakeTheNextLayerReuseANumber(t *testing.T) {
	f := layered(t, "alta")
	f.next("alta", pkt(100, 9000), true)
	f.next("alta", pkt(102, 12000), false)
	atrasado, _ := f.next("alta", pkt(101, 9000), false)
	if atrasado.SequenceNumber != 101 {
		t.Fatalf("o pacote atrasado saiu como %d, e nao 101", atrasado.SequenceNumber)
	}

	f.mu.Lock()
	f.want = "baixa"
	f.mu.Unlock()
	novo, ok := f.next("baixa", pkt(5000, 700000), true)
	if !ok {
		t.Fatal("a troca foi recusada")
	}
	if novo.SequenceNumber != 103 {
		t.Fatalf("a camada nova comecou em %d; 102 ja tinha sido usado", novo.SequenceNumber)
	}
	// E o tempo tambem nao anda para tras: o atrasado levou o relogio de saida
	// de volta a 9000, e a troca parte de 12000, que foi o mais longe que este
	// assinante chegou.
	if novo.Timestamp <= 12000 {
		t.Fatalf("a imagem nova diz %d, que e antes de uma ja entregue", novo.Timestamp)
	}
}

// Reading the one byte that says "this picture stands on its own". Getting this
// wrong means either never changing layer or changing into a smear.

// As camadas chegam em ordem qualquer: quem assinar entre a primeira e a
// última não pode ficar preso na pequena.
func TestSomebodyWhoArrivedBeforeTheBigLayerIsLiftedToIt(t *testing.T) {
	f := layered(t, "")
	pub := f.pub
	pub.outs["s1"] = f

	// A pequena chega primeiro, como a sonda de fato viu acontecer.
	pub.addLayer("baixa", 1)
	if got := f.watching(); got != "baixa" {
		t.Fatalf("a primeira camada a chegar devia servir: %q", got)
	}
	pub.addLayer("alta", 2)
	if got := f.watching(); got != "alta" {
		t.Fatalf("nao subiu quando a camada grande chegou: %q", got)
	}
	// E a do meio, que chega depois, nao desfaz isso.
	pub.addLayer("media", 3)
	if got := f.watching(); got != "alta" {
		t.Fatalf("uma camada menor chegando derrubou a escolha: %q", got)
	}
}

func TestALineThatWasPushedDownIsNotLiftedBackByANewLayer(t *testing.T) {
	f := layered(t, "alta")
	pub := f.pub
	pub.outs["s1"] = f

	f.report(0.20) // a linha dele pediu para descer
	if got := f.watching(); got != "media" {
		t.Fatalf("nao desceu: %q", got)
	}
	pub.addLayer("alta", 2)
	if got := f.watching(); got != "media" {
		t.Fatal("uma camada chegando desfez a decisao que a linha dele tomou")
	}
}

func TestAKeyframeIsRecognisedInEveryShapeH264SendsIt(t *testing.T) {
	cases := []struct {
		name string
		b    []byte
		want bool
	}{
		{"IDR on its own", []byte{0x65, 0x88}, true},
		{"SPS", []byte{0x67, 0x42}, true},
		// PPS sozinho nao: e o unico dos tres que um emissor repete sem uma
		// imagem atras dele.
		{"PPS", []byte{0x68, 0xCE}, false},
		{"an ordinary picture", []byte{0x41, 0x9A}, false},
		{"STAP-A carrying an SPS", []byte{0x78, 0x00, 0x02, 0x67, 0x42, 0x00, 0x02, 0x68, 0xCE}, true},
		{"STAP-A carrying only a PPS", []byte{0x78, 0x00, 0x02, 0x68, 0xCE}, false},
		{"STAP-A carrying nothing useful", []byte{0x78, 0x00, 0x02, 0x41, 0x9A}, false},
		{"the first fragment of an IDR", []byte{0x7C, 0x85}, true},
		{"a later fragment of an IDR", []byte{0x7C, 0x05}, false},
		{"the first fragment of an ordinary picture", []byte{0x7C, 0x81}, false},
		{"nothing at all", []byte{}, false},
		{"a truncated STAP-A", []byte{0x78, 0x00}, false},
		{"FU-A with start and end both set", []byte{0x7C, 0xC5}, false},
		{"FU-B without its two bytes of order", []byte{0x7D, 0x85}, false},
		{"FU-B with them", []byte{0x7D, 0x85, 0x00, 0x01}, true},
	}
	for _, c := range cases {
		if got := h264Keyframe(c.b); got != c.want {
			t.Errorf("%s: got %v, wanted %v", c.name, got, c.want)
		}
	}
}

func TestAKeyframeIsRecognisedInVP8(t *testing.T) {
	cases := []struct {
		name string
		b    []byte
		want bool
	}{
		{"start of a key frame", []byte{0x10, 0x00}, true},
		{"start of an ordinary frame", []byte{0x10, 0x01}, false},
		{"not the start of a frame", []byte{0x00, 0x00}, false},
		{"a later partition", []byte{0x11, 0x00}, false},
		{"with a one-byte picture id", []byte{0x90, 0x80, 0x7F, 0x00}, true},
		{"with a two-byte picture id", []byte{0x90, 0x80, 0x80, 0x01, 0x00}, true},
		{"truncated after the descriptor", []byte{0x90, 0x80}, false},
		{"nothing at all", []byte{}, false},
	}
	for _, c := range cases {
		if got := vp8Keyframe(c.b); got != c.want {
			t.Errorf("%s: got %v, wanted %v", c.name, got, c.want)
		}
	}
}

func TestAnUnknownCodecStillLetsTheLayerChange(t *testing.T) {
	if !startsKeyframe("video/nobody-has-heard-of-this", []byte{0x41}) {
		t.Fatal("an unknown codec would be stuck on one layer forever")
	}
	if startsKeyframe("video/H264", []byte{0x41, 0x9A}) {
		t.Fatal("H264 should have been read, not waved through")
	}
}

// O teto que quem assiste pede, e a regra de que ele e a linha nao se anulam.
//
// Pedido do dono em 19/09/2026: otimizar a transmissao dos dois lados. Uma tela
// desenhada num quadradinho nao precisa de 1080p, e mandar 1080p para la e pago
// duas vezes — na subida de quem mostra e no decodificador de quem assiste.

func TestTheSubscriberCanAskForASmallerPicture(t *testing.T) {
	f := layered(t, "alta")
	f.pub.addLayer("alta", 1)
	f.pub.addLayer("media", 2)
	f.pub.addLayer("baixa", 3)
	f.pub.outs["s1"] = f

	f.setCeiling("baixa")
	if got := f.watching(); got != "baixa" {
		t.Fatalf("o teto pedido nao foi respeitado: %q", got)
	}

	// E tirar o teto devolve o que a linha permite.
	f.setCeiling("")
	if got := f.watching(); got != "alta" {
		t.Fatalf("tirar o teto nao devolveu a camada grande: %q", got)
	}
}

func TestACeilingAndABadLineDoNotCancelEachOther(t *testing.T) {
	f := layered(t, "alta")
	f.pub.addLayer("alta", 1)
	f.pub.addLayer("media", 2)
	f.pub.addLayer("baixa", 3)
	f.pub.outs["s1"] = f

	// Teto no meio: a imagem esta desenhada num tamanho medio.
	f.setCeiling("media")
	if got := f.watching(); got != "media" {
		t.Fatalf("o teto do meio nao pegou: %q", got)
	}

	// A linha piora. O teto nao impede de descer mais.
	f.report(0.20)
	if got := f.watching(); got != "baixa" {
		t.Fatalf("a linha ruim nao conseguiu descer abaixo do teto: %q", got)
	}

	// A linha melhora. Ela sobe de volta ate o TETO, e nao alem dele.
	//
	// O tempo anda com passa(), e nao so no lastMove: desde 19/09/2026 a
	// escada tambem espera antes de tentar de novo uma camada que quebrou, e
	// a camada media quebrou logo ali em cima.
	passa(f, holdAfterMove+time.Second)
	for i := 0; i < cleanBeforeUp*3; i++ {
		f.report(0)
		passa(f, holdAfterMove+time.Second)
	}
	if got := f.watching(); got != "media" {
		t.Fatalf("a linha boa passou por cima do teto: %q", got)
	}
}

func TestAnUnknownCeilingIsIgnoredRatherThanObeyed(t *testing.T) {
	f := layered(t, "alta")
	f.pub.addLayer("alta", 1)
	f.pub.outs["s1"] = f

	f.setCeiling("gigante")
	if got := f.watching(); got != "alta" {
		t.Fatalf("um teto que nao existe estragou a escolha: %q", got)
	}
}

// E o que sai disso para quem mostra: a lista de camadas que ainda valem a
// pena. Uma camada que ninguem recebe e um codificador rodando para ninguem.
// O que o remetente recebe e a FAIXA que alguem pode precisar, e nao a camada
// em que a escada esta parada neste segundo. As duas primeiras conferencias
// mudaram em 20/09/2026: antes bastava "alta", e foi essa economia que custou
// 70 travadas ao telefone do dono.
func TestTheSenderIsToldTheWholeRunwayAnybodyMayNeed(t *testing.T) {
	um := layered(t, "alta")
	pub := um.pub
	pub.addLayer("alta", 1)
	pub.addLayer("media", 2)
	pub.addLayer("baixa", 3)
	pub.outs["s1"] = um

	dois := &forwarder{
		pub: pub, sess: &session{id: "s2"},
		want: "alta", lossWant: "alta",
		lastMove: time.Now().Add(-time.Hour),
	}
	pub.outs["s2"] = dois

	// Os dois na camada grande: ela, e a pista inteira abaixo dela, que e para
	// onde a escada os leva se a linha vacilar.
	if got := strings.Join(pub.layersWanted(), ","); got != "alta,media,baixa" {
		t.Fatalf("a pista de baixo nao foi acesa antes de ser precisa: %q", got)
	}

	// Um deles encolhe a janela.
	um.setCeiling("baixa")
	if got := strings.Join(pub.layersWanted(), ","); got != "alta,media,baixa" {
		t.Fatalf("a lista nao acompanhou quem encolheu: %q", got)
	}

	// O outro tambem. Agora a camada grande nao serve mais a ninguem.
	dois.setCeiling("baixa")
	if got := strings.Join(pub.layersWanted(), ","); got != "baixa" {
		t.Fatalf("a camada grande continuou na lista sem ninguem nela: %q", got)
	}
}

// A que esta sendo VISTA nao pode sair da lista so porque outra foi pedida: a
// troca espera uma imagem completa, e ate ela chegar e a antiga que esta na
// tela da pessoa.
func TestTheLayerSomebodyIsStillWatchingStaysOnTheList(t *testing.T) {
	f := layered(t, "alta")
	pub := f.pub
	pub.addLayer("alta", 1)
	pub.addLayer("baixa", 3)
	pub.outs["s1"] = f

	// Ela esta recebendo a grande de fato.
	f.next("alta", pkt(10, 900), true)
	if f.current != "alta" {
		t.Fatalf("nao entrou na camada grande: %q", f.current)
	}

	f.setCeiling("baixa")
	got := strings.Join(pub.layersWanted(), ",")
	if got != "alta,media,baixa" {
		t.Fatalf("a camada que ela ainda ve saiu da lista: %q", got)
	}
}

// O defeito do log de Path of Exile, 20/09/2026: a escada de perda subindo e
// descendo fazia a lista de camadas mudar, e cada mudanca dessas manda o
// navegador desligar ou religar um codificador. Vinte e uma vezes em dezessete
// minutos, cada uma custando uma imagem completa e uma parada na captura.
//
// A janela de quem assiste nao mudou de tamanho uma unica vez na hora inteira.
// Nada do que ele PEDIU mudou; so onde a escada estava parada. Entao a lista
// nao pode mudar.
func TestTheLadderBouncingDoesNotSwitchLayersOffAndOnAtTheSender(t *testing.T) {
	f := layered(t, "alta")
	pub := f.pub
	pub.addLayer("alta", 1)
	pub.addLayer("media", 2)
	pub.addLayer("baixa", 3)
	pub.outs["s1"] = f
	// Ele esta assistindo em tela cheia: o teto e a camada grande, e fica.
	f.setCeiling("alta")

	inicial := strings.Join(pub.layersWanted(), ",")
	if inicial != "alta,media,baixa" {
		t.Fatalf("estado inicial errado: %q", inicial)
	}

	// Agora a linha do telefone faz o que fez a noite inteira: vacila, sara,
	// vacila de novo. Sete voltas, que e menos do que o log tem.
	//
	// mexeu guarda contra o pior tipo de teste verde: um que passa porque nada
	// aconteceu. Se a escada parar de se mexer, este teste passa sem provar
	// nada, e e justamente a escada se mexendo que ele existe para tolerar.
	mexeu := false
	for volta := 0; volta < 7; volta++ {
		antes := f.watching()
		passa(f, holdAfterMove+time.Second)
		f.report(0.2) // perda: desce um degrau
		if f.watching() != antes {
			mexeu = true
		}
		passa(f, holdAfterMove+time.Second)
		for i := 0; i < cleanBeforeUp+2; i++ {
			f.report(0) // limpo: sobe de volta
		}
		if got := strings.Join(pub.layersWanted(), ","); got != inicial {
			t.Fatalf("volta %d: a lista mudou sem ninguem pedir nada — %q, era %q",
				volta, got, inicial)
		}
	}
	if !mexeu {
		t.Fatal("a escada nao saiu do lugar: o teste passou sem testar nada")
	}
}

// passa faz o tempo andar para este encaminhador. Todas as decisoes da escada
// sao sobre ha quanto tempo alguma coisa aconteceu, entao e a unica forma de
// testa-las sem deixar o teste parado esperando de verdade.
func passa(f *forwarder, d time.Duration) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.lastMove = f.lastMove.Add(-d)
	for rid, at := range f.failedAt {
		f.failedAt[rid] = at.Add(-d)
	}
	for rid, at := range f.promoted {
		f.promoted[rid] = at.Add(-d)
	}
}

// O oscilador do log do dono, de 19/09/2026: vinte trocas de camada em oito
// minutos, com uma pessoa so assistindo e a janela dela nunca mudando de
// tamanho. A camada grande nao cabia na linha do telefone, a escada descia, a
// media cabia, os relatorios ficavam limpos e a escada subia de volta para a
// mesma parede — a cada holdAfterMove, para sempre, a uma imagem completa e um
// congelamento por volta.
func TestALayerThatKeepsBreakingTheLineIsTriedLessAndLessOften(t *testing.T) {
	f := layered(t, "alta")

	quebra := func() {
		t.Helper()
		if got := f.watching(); got != "alta" {
			t.Fatalf("o ciclo comecou em %q, nao na camada grande", got)
		}
		f.report(0.20)
		if got := f.watching(); got != "media" {
			t.Fatalf("a perda nao desceu a escada: %q", got)
		}
	}
	// volta: passado tanto tempo de linha limpa, a escada tenta a grande?
	volta := func(espera time.Duration) bool {
		passa(f, espera)
		for i := 0; i < cleanBeforeUp+2; i++ {
			f.report(0)
		}
		return f.watching() == "alta"
	}

	// A primeira tentativa acontece na hora de sempre. Isto e memoria sendo
	// acrescentada, nao paciencia: um segundo ruim continua custando o que
	// custava.
	quebra()
	if !volta(holdAfterMove + time.Second) {
		t.Fatal("a primeira volta para a camada grande foi adiada")
	}

	// A segunda, nao. A mesma espera de antes ja nao basta.
	passa(f, holdAfterMove+time.Second)
	quebra()
	if volta(holdAfterMove + time.Second) {
		t.Fatal("tentou de novo na mesma espera: a escada continua sendo um oscilador")
	}
	if !volta(retryBase) {
		t.Fatal("nunca mais tentou — o backoff virou desistencia")
	}

	// E a terceira espera e maior que a segunda.
	passa(f, holdAfterMove+time.Second)
	quebra()
	if volta(time.Duration(float64(retryBase) * retryFactor)) {
		t.Fatal("a terceira espera nao cresceu")
	}
	if !volta(retryMax) {
		t.Fatal("nunca mais tentou depois da terceira quebra")
	}
}

// E o contrario: quem carregou a camada por um bom tempo antes de quebrar nao
// esta no padrao acima. A linha melhorou de verdade em algum momento, e uma
// tarde de historico velho nao pode segurar essa pessoa na imagem pequena.
func TestALayerThatHeldForALongTimeStartsTheCountOver(t *testing.T) {
	f := layered(t, "alta")
	sobe := func() {
		t.Helper()
		for i := 0; i < cleanBeforeUp+2; i++ {
			f.report(0)
		}
	}

	f.report(0.20) // primeira quebra
	passa(f, holdAfterMove+time.Second)
	sobe()
	if got := f.watching(); got != "alta" {
		t.Fatalf("nao voltou para a camada grande na primeira tentativa: %q", got)
	}

	// Aguentou um minuto inteiro antes de quebrar de novo.
	passa(f, retryProven+time.Second)
	f.report(0.20)
	if got := f.watching(); got != "media" {
		t.Fatalf("a perda nao desceu a escada: %q", got)
	}

	// Entao a espera volta a ser a do primeiro tropeco, e nao a do segundo.
	passa(f, holdAfterMove+time.Second)
	sobe()
	if got := f.watching(); got != "alta" {
		t.Fatalf("uma camada que tinha aguentado um minuto foi punida pelo historico velho: %q", got)
	}
}

// Os dois defeitos que o Codex achou auditando layersWanted, 20/09/2026.

// Sem teto quer dizer "sem limite", e nao "nao conto". Se quiser dizer nada, o
// topo da faixa volta a seguir a escada e a oscilacao volta inteira para quem
// ainda nao mediu o proprio quadradinho — o que inclui todo mundo no primeiro
// segundo de uma transmissao.
func TestASubscriberWhoHasNotSaidASizeStillPinsTheTop(t *testing.T) {
	f := layered(t, "alta")
	pub := f.pub
	pub.addLayer("alta", 1)
	pub.addLayer("media", 2)
	pub.addLayer("baixa", 3)
	pub.outs["s1"] = f
	if f.ceiling != "" {
		t.Fatalf("o teste precisa de um assinante sem teto: %q", f.ceiling)
	}

	inicial := strings.Join(pub.layersWanted(), ",")
	if inicial != "alta,media,baixa" {
		t.Fatalf("sem teto deveria ser sem limite: %q", inicial)
	}
	mexeu := false
	for volta := 0; volta < 5; volta++ {
		antes := f.watching()
		passa(f, retryMax+time.Second)
		f.report(0.2)
		if f.watching() != antes {
			mexeu = true
		}
		// Aqui, e nao so no fim. A primeira versao deste teste conferia a
		// lista depois do ciclo inteiro, quando a escada ja tinha voltado ao
		// lugar — e por isso passava com o defeito de volta. E o buraco que a
		// escada abre NO MEIO do ciclo que interessa.
		if got := strings.Join(pub.layersWanted(), ","); got != inicial {
			t.Fatalf("volta %d: a lista encolheu enquanto a escada descia — %q",
				volta, got)
		}
		passa(f, retryMax+time.Second)
		for i := 0; i < cleanBeforeUp+2; i++ {
			f.report(0)
		}
		if got := strings.Join(pub.layersWanted(), ","); got != inicial {
			t.Fatalf("volta %d: a lista mudou sem teto nenhum ter mudado — %q",
				volta, got)
		}
	}
	if !mexeu {
		t.Fatal("a escada nao saiu do lugar: o teste passou sem testar nada")
	}
}

// O resgate de uma camada morta nao pode furar o teto. Um telefone que disse
// "o meu tamanho e 480" nao pode receber 1080 so porque a camada que ele queria
// parou de chegar.
func TestTheRescueFromADeadLayerStillObeysTheCeiling(t *testing.T) {
	f := layered(t, "media")
	pub := f.pub
	pub.addLayer("alta", 1)
	pub.addLayer("media", 2)
	pub.outs["s1"] = f
	f.setCeiling("media")

	// Ela esta recebendo a media de fato, e entao pede a baixa — que nao esta
	// chegando, porque quem mostra a desligou.
	f.next("media", pkt(10, 900), true)
	f.mu.Lock()
	f.want = "baixa"
	f.lossWant = "baixa"
	f.mu.Unlock()

	// Um relatorio qualquer: o alvo esta pendente e morto, e o resgate corre.
	f.report(0.01)

	if got := f.watching(); got == "alta" {
		t.Fatalf("o resgate furou o teto e entregou a camada grande: %q", got)
	}
	if got := f.watching(); got != "media" {
		t.Fatalf("o resgate deveria ter parado no teto: %q", got)
	}
}

// A tela morta de 20/09/2026, no Firefox do dono: `0 fps 0 kbit/s` por
// quarenta segundos, e so voltou porque ele parou e recomecou na mao.
//
// Ele estava assistindo a camada grande, acomodado nela — nao no meio de uma
// troca. O navegador de quem mostra desligou essa camada com `active: false`,
// o que NAO acaba a faixa: os pacotes so param, o laco de leitura fica
// bloqueado, `removeLayer` nunca roda e a camada continua marcada como viva
// para sempre. Nada aqui dentro tinha como mover essa pessoa.
func TestASubscriberSittingOnALayerThatWentQuietIsMovedOff(t *testing.T) {
	f := layered(t, "alta")
	pub := f.pub
	pub.addLayer("alta", 1)
	pub.addLayer("media", 2)
	pub.outs["s1"] = f

	// Acomodado na grande: recebendo de fato, sem troca pendente.
	f.next("alta", pkt(10, 900), true)
	if f.current != "alta" || f.want != "alta" {
		t.Fatalf("nao se acomodou na grande: current=%q want=%q", f.current, f.want)
	}

	// A camada grande para de chegar. A faixa continua la, e `live` tambem.
	pub.layMu.Lock()
	pub.layers["alta"].lastAt.Store(time.Now().Add(-2 * layerSilent).UnixNano())
	pub.layMu.Unlock()
	if !pub.layers["alta"].live.Load() {
		t.Fatal("o teste precisa da camada ainda marcada como viva")
	}

	// Um relatorio qualquer, sem perda: nada na escada manda mexer.
	f.report(0)

	if got := f.watching(); got == "alta" {
		t.Fatalf("continuou parado na camada que nao chega mais: %q", got)
	}
	if got := f.watching(); got != "media" {
		t.Fatalf("deveria ter caido para a que ainda chega: %q", got)
	}
}

// E o contrario: uma camada quieta por pouco tempo nao e uma camada morta. A
// menor manda quinze quadros por segundo, e um engasgo de meio segundo nela
// nao pode derrubar ninguem de lugar.
func TestABriefGapDoesNotCountAsALayerGoingAway(t *testing.T) {
	f := layered(t, "alta")
	pub := f.pub
	pub.addLayer("alta", 1)
	pub.addLayer("media", 2)
	pub.outs["s1"] = f
	f.next("alta", pkt(10, 900), true)

	pub.layMu.Lock()
	pub.layers["alta"].lastAt.Store(time.Now().Add(-layerSilent / 3).UnixNano())
	pub.layMu.Unlock()

	f.report(0)
	if got := f.watching(); got != "alta" {
		t.Fatalf("um engasgo derrubou a pessoa de camada: %q", got)
	}
}

// O espectador do autoteste de 08/10/2026 (r23): olhando pequeno, quem mostra
// desligou a camada grande (ninguem a pedia). Ao aumentar a imagem, o teto sai,
// mas a grande leva um instante para voltar a chegar. O relatorio desse
// instante gravava a pequena como "o que a linha aguenta" — e a pessoa subia
// de volta degrau por degrau, oito segundos cada, recebendo 30 quadros
// quatro segundos depois de pedir 60. Uma camada que nao chega e um fato
// sobre quem mostra, nao sobre a linha de quem assiste.
func TestAskingForTheBigLayerWhileItRestartsDoesNotForgetTheLine(t *testing.T) {
	f := layered(t, "alta")
	pub := f.pub
	pub.addLayer("alta", 1)
	pub.addLayer("media", 2)
	pub.addLayer("baixa", 3)
	pub.outs["s1"] = f
	f.next("alta", pkt(10, 900), true)

	// Olhando pequeno: o teto desce para a menor, e a troca acontece.
	f.setCeiling("baixa")
	f.next("baixa", pkt(11, 1800), true)
	if f.current != "baixa" {
		t.Fatalf("nao desceu para a menor: %q", f.current)
	}

	// Quem mostra desliga a grande e a media.
	silence := func(rid string, quiet bool) {
		at := time.Now()
		if quiet {
			at = at.Add(-2 * layerSilent)
		}
		pub.layMu.Lock()
		pub.layers[rid].lastAt.Store(at.UnixNano())
		pub.layMu.Unlock()
	}
	silence("alta", true)
	silence("media", true)

	// A pessoa aumenta a imagem antes de a grande voltar.
	f.setCeiling("")
	f.report(0)
	if got := f.watching(); got != "baixa" {
		t.Fatalf("enquanto a grande nao chega, deveria seguir na que chega: %q", got)
	}
	f.mu.Lock()
	line := f.lossWant
	f.mu.Unlock()
	if line != "alta" {
		t.Fatalf("a falta da camada apagou a memoria da linha: lossWant=%q", line)
	}

	// A grande volta: o proximo relatorio leva a pessoa direto para ela, sem
	// escada e sem esperar holdAfterMove.
	silence("alta", false)
	f.report(0)
	if got := f.watching(); got != "alta" {
		t.Fatalf("a grande voltou e a pessoa nao foi para ela: %q", got)
	}
}
