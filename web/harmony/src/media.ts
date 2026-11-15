import { silentSource, watchPreferences } from './blocks';
// The voice channel, browser side: the microphone, the camera, the screen, the
// connection, and the audio and video elements the other people come out of.
//
// Everything to do with media lives in this file and nowhere else, and none of
// it is ever put into the framework's state (CLAUDE.md). The `<audio>` and
// `<video>` elements are made by hand and live in a container outside the app's
// tree; a component that wants to show a picture asks for the element and puts
// it where it wants it, so a redraw of the channel column cannot tear down a
// stream mid-sentence and a screen does not blink when somebody sends a message.
// What the interface gets from here is a plain object of names and flags.
//
// The negotiation is one-sided by design. The server is always the one who
// offers (internal/harmony/sfu), so this file never calls createOffer and never
// has to resolve a collision — no perfect negotiation, no polite and impolite,
// because there is nothing to be polite about. Perfect negotiation stays where
// it belongs, in the direct 1:1 call, which this does not touch.
//
// The one PeerConnection is never rebuilt. Somebody joining or leaving, and a
// screen being switched on halfway through the evening, all arrive as a fresh
// offer on the live socket and are answered on the connection that is already
// up — the same rule the 1:1 app follows (PROTOCOL.md, replaceTrack).

import { api, ApiError, message, type VoiceOffer, type VoicePeer, type VoiceTrack } from './api';
import { log } from './diag';
import { watchNetwork } from './network';
import { speakingSample } from './speaking';
import { DEFAULT_LEVEL, audioContext, makeSuppressor, type Suppressor } from './suppressor';
import { ownChanged, roomChanged } from './sounds';

export type { VoicePeer };

// Sources, spelled the same way the server spells them (internal/harmony/sfu).
export const MIC = 'mic';
export const CAMERA = 'cam';
export const SCREEN = 'screen';
export const SCREEN_AUDIO = 'screen_audio';

// Tile is one picture on the wall, as plain data. The element that actually
// holds the picture is fetched separately, by key, and never travels through
// the framework.
export interface Tile {
	key: string;
	username: string;
	displayName: string;
	source: string;
	mine: boolean;
}

// A shared screen keeps its frame rate and lets the size be whatever fits.
//
// This is the reverse of the rule that stood from 10/09/2026 to 19/09/2026, and
// it was reversed by one sentence from the owner: **"quero compartilhar minha
// tela para jogos e filmes/series"**. Everything here had been reasoned as if a
// shared screen were code and windows, and for that the old rule was right —
// text that goes blurry is text nobody can read, and a still page can wait.
//
// A film is the opposite on both counts. Blurry is watchable and slow is not,
// and the price of the old rule turned out to be far worse than it sounded:
// 'maintain-resolution' never reduces the picture at all — the path that would
// ask for it returns "limit reached" — and the floor of what it does reduce is
// **two frames per second** (libwebrtc, call/adaptation/video_stream_adapter.cc,
// read 19/09/2026). So a line in trouble did not give a soft 1080p. It gave a
// sharp slideshow, which is what the owner's telephone showed: 1920x1080 at one
// to six frames.
//
// Now the picture shrinks instead — 1080p to 720p to whatever the line can
// carry — and comes back up on its own. The cost is the one the owner disliked
// in September, the size changing by itself; on a film it is barely visible,
// which is exactly why every service that streams video does it this way.
export const SCREEN_DEGRADATION: RTCDegradationPreference = 'maintain-framerate';

// How big and how smooth a shared screen is allowed to be. "auto" is 1080p at
// sixty, which is what everybody got before any of this existed.
//
// This is the one lever anybody has against a bad evening. The media server
// does not re-compress (PLANO-DISCORD.md §4), so there is a single picture for
// the whole room: if one person's line cannot carry it, the answer available
// today is for whoever is sharing to ask for less. A steady 720p30 that
// everybody can see beats a 1080p60 that falls apart.
export type ScreenQuality = 'auto' | '1080' | '720' | '480';
export type ScreenFps = 'auto' | '60' | '30' | '15';

// CameraDevice is one camera as a name and an id. Nothing about it is a
// stream, so it is allowed to travel through the interface like any other
// plain data.
export interface CameraDevice {
	id: string;
	label: string;
}

// VoiceView is everything the interface needs, and it is all plain data.
export interface VoiceView {
	channelID: string;
	sessionID: string;
	state: 'off' | 'joining' | 'live' | 'lost';
	// Which part of joining is under way, so the column can say what is
	// actually happening instead of one "Conectando…" for all of it (the
	// owner, 24/09/2026). The three are the three waits join() really has:
	// the browser opening the microphone, the server making the session, and
	// the route for the audio being found. Empty once live.
	step: '' | 'mic' | 'server' | 'route';
	// Which try this is after the connection died (recover()): 0 on a plain
	// join, 1–3 while coming back.
	attempt: number;
	muted: boolean;
	deafened: boolean;
	// What this device is sending right now, besides the microphone.
	sharingScreen: boolean;
	sharingCamera: boolean;
	screenQuality: ScreenQuality;
	screenFps: ScreenFps;
	// Which camera, and which ones there are. The list is empty until the
	// browser has been asked, and the names are only real once permission has
	// been given — a browser that has not been asked yet reports every camera
	// as the empty string, on purpose.
	cameraID: string;
	cameras: CameraDevice[];
	// Which microphone this device speaks into, and which speakers it listens
	// through. The second one is not everywhere: some browsers do not let a page
	// choose an output at all, and there `speakers` comes back empty.
	micID: string;
	microphones: CameraDevice[];
	speakerID: string;
	speakers: CameraDevice[];
	// Whose screens this device asked TO receive — a list of who is wanted,
	// not of who was refused. Joining a channel starts it empty: everybody in
	// the room is heard, and nobody's screen arrives until it is asked for.
	// Watching is a decision, not something that happens to you.
	watching: string[];
	// The wall, biggest first.
	tiles: Tile[];
	// The key of the tile the person chose to enlarge. Empty means "whichever
	// screen there is, or the first camera".
	focus: string;
	// The key of the picture this device took out of the page into a floating
	// window of its own (the Discord pop-out, 24/09/2026), or ''. One at a
	// time: the browser gives a page one such window.
	popped: string;
	// A sentence to show once, when something the person just pressed did not
	// work. error is a code the interface turns into words; this is already
	// words, because it comes from the server.
	notice: string;
	// Per-source loudness, 0 to 2, by the key volumeKey() builds. It is this
	// device's opinion about other people and never leaves the browser.
	volumes: Record<string, number>;
	// How loud this device's own microphone is *sent*, 0 to 2. The only number
	// here that changes what other people hear, and the only one that is not
	// about somebody else.
	micGain: number;
	// What the browser does to this device's voice before it leaves. All three
	// have been on since D2 and were not switchable; they are here because the
	// right answer is not the same for everybody — a good microphone in a quiet
	// room sounds *worse* with suppression on, and somebody on headphones does
	// not need echo cancellation at all.
	// `rnnoise` is the name the switch was saved under in D4e; since 25/09/2026
	// it switches DeepFilterNet (suppressor.ts), and the name stays so nobody's
	// saved choice is lost.
	micFilters: { noise: boolean; echo: boolean; agc: boolean; rnnoise: boolean };
	// How much the suppressor may take away, in dB (suppressor.ts, DEFAULT_LEVEL).
	noiseLevel: number;
	// Which suppressor the room is actually running: '' while there is none to
	// speak of (not in a room, switched off, still arriving). The switch is a
	// wish; this is what came of it, so that Ajustes never says DeepFilterNet
	// while RNNoise, or nothing, is what the room hears.
	noiseActive: '' | 'deepfilter' | 'rnnoise' | 'none';
	error: string;
}

// Where a person's choices are kept.
//
// They are kept per person and per device, and both halves of that are
// deliberate. Per person, because two people share a browser at least once in
// the life of a service like this and inheriting somebody else's microphone is
// a bad surprise. Per device, because the choices are mostly *about* the
// device: the identifier of a camera means nothing on another machine, and the
// telephone's camera and the desktop's camera were never going to be the same
// answer anyway.
const PREF = {
	volumes: 'volumes',
	quality: 'quality',
	fps: 'fps',
	camera: 'camera',
	mic: 'mic',
	micGain: 'micgain',
	micFilters: 'micfilters',
	noiseLevel: 'noiselevel',
	speaker: 'speaker',
} as const;

// Whose choices are loaded. Empty until the app says who is logged in, which
// is why everything below also reads the old shared key: a setting made before
// this file knew about people is inherited once, rather than lost.
let who = '';

function readPref(what: string): string | null {
	try {
		if (who) {
			const mine = localStorage.getItem(`harmony.voice.${who}.${what}`);
			if (mine !== null) return mine;
		}
		return localStorage.getItem(`harmony.voice.${what}`);
	} catch {
		return null;
	}
}

function writePref(what: string, value: string): void {
	try {
		localStorage.setItem(`harmony.voice.${who ? who + '.' : ''}${what}`, value);
	} catch {
		// A browser with storage switched off simply forgets between visits.
	}
}

// volumeKey keeps a person's voice and the sound of the film they are showing
// apart, because they are two different complaints: "fala mais alto" and "abaixa
// esse filme" have to be two different sliders or neither one works.
export function volumeKey(username: string, source: string): string {
	return source === SCREEN_AUDIO ? username + ':tela' : username;
}

function savedVolumes(): Record<string, number> {
	try {
		const raw = readPref(PREF.volumes);
		if (!raw) return {};
		const parsed = JSON.parse(raw) as Record<string, number>;
		const out: Record<string, number> = {};
		for (const [k, v] of Object.entries(parsed)) {
			if (typeof v === 'number' && v >= 0 && v <= 2) out[k] = v;
		}
		return out;
	} catch {
		return {};
	}
}

// The same, for the one number that is about this device's own voice. Anything
// unreadable is 100%, which is the microphone untouched.
//
// "Nothing saved" has to be asked for before the conversion: Number(null) is 0,
// and 0 is a volume this accepts — so every browser that had never touched
// the slider joined at 0%, raw microphone until the first touch and silence
// after it (found by the audit of 23/09/2026; it had been there since D4c).
function savedMicGain(): number {
	const stored = readPref(PREF.micGain);
	if (stored === null || stored.trim() === '') return 1;
	const raw = Number(stored);
	return Number.isFinite(raw) && raw >= 0 && raw <= 2 ? raw : 1;
}

// The same care as the gain: nothing saved is asked for first, because
// Number(null) is 0 and 0 dB would be a suppressor that removes nothing.
function savedNoiseLevel(): number {
	const stored = readPref(PREF.noiseLevel);
	if (stored === null || stored.trim() === '') return DEFAULT_LEVEL;
	const raw = Number(stored);
	return Number.isFinite(raw) && raw >= 10 && raw <= 100 ? raw : DEFAULT_LEVEL;
}

// The three filters, with the D2 answer as the fallback: all on. Anything
// unreadable in storage means all on as well, which is the safe direction —
// somebody who cannot be understood is worse off than somebody whose good
// microphone is being processed for nothing.
function savedMicFilters(): { noise: boolean; echo: boolean; agc: boolean; rnnoise: boolean } {
	// rnnoise on by default: it is the one the owner went looking for, and it
	// is the reason the browser's own is off underneath it (see micConstraints).
	const all = { noise: true, echo: true, agc: true, rnnoise: true };
	try {
		const raw = readPref(PREF.micFilters);
		if (!raw) return all;
		const parsed = JSON.parse(raw) as Record<string, unknown>;
		for (const k of Object.keys(all) as (keyof typeof all)[]) {
			if (typeof parsed[k] === 'boolean') all[k] = parsed[k] as boolean;
		}
		return all;
	} catch {
		return all;
	}
}

// A choice that is remembered has to survive being read back as nonsense: the
// value in storage was written by an older version of this file, or by hand.
function savedChoice<T extends string>(what: string, allowed: readonly T[], fallback: T): T {
	const raw = readPref(what) as T | null;
	return raw && allowed.includes(raw) ? raw : fallback;
}

// A device is remembered by the identifier the browser gives it, which is
// stable for this browser on this machine and means nothing anywhere else. An
// identifier that no longer names anything — the webcam was unplugged — is not
// an error here: it is caught when the device is asked for, and the browser's
// own default is used instead.
function savedDevice(what: string): string {
	return readPref(what) ?? '';
}

const QUALITIES = ['auto', '1080', '720', '480'] as const;
const FPS_CHOICES = ['auto', '60', '30', '15'] as const;
// The default rate for a shared screen: thirty, not sixty.
//
// Sixty was the default until 10/09/2026 and it was too many. A 1080p screen at
// sixty is at the very edge of what a machine encodes in software and of what a
// long route carries, and an encoder at its edge does not produce a steady
// stream — it produces bursts with gaps between them, which is what a hitch is.
// The owner's own measurement said so: the capture was running at sixty and the
// encoder was managing thirty-two, unevenly. Asking for thirty asks for
// something the machine can actually do, evenly, at half the packets.
//
// Sixty is still there for whoever wants it, one line down in the panel, and it
// is the right answer for a game.
const SCREEN_FPS_DEFAULT = 30;

// RMS is sampled every 25 ms. Floor and release are shared by self and
// remote microphones through speakingSample, using floating-point samples.
const METER_EVERY = 25; // ms

// How long an arriving screen is held before it is shown, in seconds. See
// hold(): it is the difference between a lost packet being replaced in time and
// a lost packet becoming a visible lurch, on a route this long.
// Four hundred milliseconds was right for a server in Germany, 211 ms away,
// where a lost packet had to be asked for and re-sent across the Atlantic
// before the moment to show it had passed. The comment above said so in as many
// words: "on a route this long".
//
// **That route is gone.** Since 18/09/2026 the server is in Sao Paulo and the
// owner measures 15-18 ms to it. The hold stopped being protection and became
// almost pure delay: his telephone reported `espera=336-493ms` on every sample
// while he was complaining that the live felt far behind.
//
// A hundred and fifty covered a retransmission ten times over, and on
// 19/09/2026 the owner asked whether more could come off. It can, and this is
// the arithmetic he is owed:
//
//   losing a packet is noticed about a packet later, the NACK crosses in ~8 ms,
//   the replacement crosses back in ~8 ms — call the whole errand 25 ms.
//
// Eighty still covers that errand **three times over**, and hands him seventy
// milliseconds back. The floor is not zero: below about fifty this stops being
// a buffer at all and every lost packet becomes a visible lurch, which is the
// D3 stutter all over again.
//
// The safety net under this is that jitterBufferTarget is a TARGET and not a
// cap: a browser that measures real jitter on its own line is still allowed to
// hold longer. So the number here is "hold no more than this when the line is
// behaving", not "hold this come what may" — which is why taking seventy
// milliseconds off is a smaller bet than it sounds.
//
// It was 400 for a server in Germany, 211 ms away. That route is gone: since
// 18/09/2026 the server is in Sao Paulo and the owner measures 15-18 ms to it.
const SCREEN_HOLD = 0.08;

// What each chosen size and rate is allowed to cost, in bits per second.
//
// **These nine numbers are the owner's, given one by one on 11/09/2026**, after
// watching 1080p at thirty look thin: "1080p para 30 fps ta sendo pouco mbps".
// They replace the arithmetic that used to produce them — a base per height
// times a factor per frame rate, anchored on the 2,7 Mbit/s measurement of
// sprint 8. That arithmetic was defensible and it was also wrong in the only
// way that counts: it priced a shared screen for a route, not for an eye.
//
// A table beats a formula here because the answer is not smooth. Doubling the
// frames does not double what a screen needs — most of a shared screen does not
// move — and halving the height does not quarter it, because the text on it
// still has to be sharp enough to read. Each cell is a judgement, and a
// judgement is what a table is for.
//
// **A ceiling is not a floor.** None of this makes anybody send more than their
// line can carry: WebRTC measures the route continuously and backs off on its
// own, and this number only says how far up it may go when the route turns out
// to have room. The old ceiling was the thing stopping a good connection from
// looking good.
// **Nao existe "sem teto", e isto foi medido em 18/09/2026.** O dono pediu para
// tirar o maximo; tiramos; e a imagem piorou em dois tercos — 6926 kbit/s de
// media viraram 2418, e apareceu `limite=bandwidth` com queda para 720p.
//
// A razao esta no libwebrtc e sao duas pecas. Sem `maxBitrate` na encoding, o
// encoder_stream_factory aplica um padrao por resolucao, e acima de 960x540 ele
// e **2500 kbit/s** — que e, com exatidao incomoda, a media entregue. E o
// ProbeController limita a sondagem a
// `min(max_bitrate, 2 x max_total_allocated_bitrate)`, de modo que um teto
// baixo tambem impede a estimativa de subir para descobrir que ha mais rota: a
// banda estimada ficou presa em 6000-6107 kbit/s por quatro minutos, onde antes
// subia ate 25261.
//
// Entao o teto nao e so um limite, e tambem a permissao para tentar crescer.
// A escolha nunca foi "com ou sem": e sempre qual numero, e o dono escolheu o
// dele — "um limite muito alto, por exemplo de 50mbps" (18/09/2026).
//
// E em 19/09/2026 ele desceu de volta para 10 Mbit, olhando o proprio log:
// "pode voltar para 10mbps". O que mudou entre uma escolha e a outra foi o
// D4r. Com UM fluxo so, o teto alto era de graca — a sala inteira recebia o
// mesmo, dimensionado pela melhor linha que estivesse nela, e um numero alto
// so dizia "pode crescer se couber". Com tres camadas o teto passou a
// descrever uma DELAS, e uma camada de 40 Mbit nao e uma camada: e uma que so
// existe para quem esta na fibra. No log dele a camada grande subia a 40-46
// Mbit/s, o telefone que a recebia acumulou 74 travadas, e a voz da chamada
// levou 23% de perda disputando o mesmo cano.
//
// E em 19/09/2026, mais tarde, ele subiu para 15: "vamos colocar o teto em
// 15mbps". Desta vez com o jogo rodando de verdade, que e a medicao que faltava
// — o log dele do Path of Exile 2 marcou `1280x720 ... limite=bandwidth`, ou
// seja, dez megabits deixaram de sobrar e a imagem encolheu para caber. Com
// area de trabalho parada dez bastavam; numa cena cheia, nao.
//
// A tabela inteira subiu junto, na mesma proporcao, para manter a forma que ele
// ja tinha aprovado. As duas camadas de baixo NAO mudam: elas existem para
// caber num telefone, e um telefone nao fica melhor porque o computador de quem
// transmite tem fibra.
const SCREEN_CEILING: Record<string, Record<string, number>> = {
	'1080': { '60': 15_000_000, '30': 12_000_000, '15': 9_000_000 },
	'720': { '60': 12_000_000, '30': 9_000_000, '15': 6_000_000 },
	'480': { '60': 6_000_000, '30': 6_000_000, '15': 6_000_000 },
};

// Onde o codificador COMECA, que deixou de poder ser o mesmo numero do teto.
//
// A partida e 60% do que esta aqui (START_SHARE), e ela existe porque um
// navegador que nao recebe instrucao comeca em ~300 kbit/s e sobe tateando: era
// o log do dono lendo 320x180, depois 960x540, e 1080p so quarenta segundos
// depois. Dizer "comece grande" resolveu isso.
//
// Enquanto o teto era 10 Mbit os dois podiam ser o mesmo numero: 60% de 10 sao
// 6 Mbit, uma partida boa. Com o teto em 50, 60% seriam **30 Mbit disparados no
// primeiro segundo** — precisamente a coisa que a partida foi escrita para
// evitar, e que custaria dez segundos de recuperacao numa linha que nao os
// tenha. Um numero fazendo dois trabalhos so funciona enquanto as duas
// respostas coincidem; deixaram de coincidir em 18/09/2026.
//
// Estes sao os valores que vinham servindo de teto ate hoje, que como ponto de
// partida estavam certos o tempo todo.
const SCREEN_START: Record<string, Record<string, number>> = {
	'1080': { '60': 10_000_000, '30': 8_000_000, '15': 6_000_000 },
	'720': { '60': 8_000_000, '30': 6_000_000, '15': 4_000_000 },
	'480': { '60': 4_000_000, '30': 4_000_000, '15': 4_000_000 },
};

// As tres camadas da tela (D4r).
//
// Simulcast e o navegador codificando a MESMA tela tres vezes, em tamanhos
// diferentes, e o servidor escolhendo uma por pessoa. Sem isso a sala inteira
// recebe um fluxo so — dimensionado pela melhor conexao que estiver nela — e o
// telefone de alguem paga pelo computador de outro.
//
// A ordem importa e e do maior para o menor: o servidor adota as vagas na
// ordem em que a oferta as traz.
//
// Os tetos das duas menores sao fixos de proposito. A camada de cima usa a
// tabela que o dono escolheu (SCREEN_CEILING, ate 15 Mbit); as de baixo existem
// para caber num telefone em rede movel, e um telefone nao fica melhor porque o
// computador de quem transmite tem fibra.
// Quanto tempo uma camada continua subindo depois de ninguém precisar mais
// dela. Ver screenWatchers(): desligar na hora faz o codificador piscar, e cada
// religada custa uma imagem completa. Cinco segundos é o padrão do LiveKit para
// a mesma decisão.
const SHRINK_WAIT = 5000;

const SCREEN_LAYERS: { rid: string; scale: number; bitrate?: number; fps: number }[] = [
	{ rid: 'alta', scale: 1, fps: 60 },
	{ rid: 'media', scale: 2, bitrate: 4_000_000, fps: 30 },
	{ rid: 'baixa', scale: 4, bitrate: 800_000, fps: 15 },
];

// A ordem de codecs da tela, que tem de ser pedida aqui desde o D4r.
//
// H.264 é decisão do dono (19/09/2026), por custo de CPU, e AV1 é proibido. Do
// lado do servidor isso é imposto com `SetCodecPreferences` na oferta dele
// (`screenCodecs()`, em codecs.go) — e a tela deixou de passar pela oferta
// dele quando este navegador começou a ofertá-la. Sem esta linha o Chrome
// oferta a lista dele, que começa em VP8, e a decisão do dono é desfeita sem
// que nada reclame. Foi o que aconteceu: o log de produção do primeiro dia do
// D4r dizia `codec=video/VP8`, e foi o dono quem reparou.
//
// A ordem é a mesma do servidor: H.264, depois VP9, depois VP8. O que não está
// na lista fica no fim, na ordem em que o navegador o pôs — inclusive a
// retransmissão, que não é uma escolha de qualidade.
// Quando a imagem que chega e grande demais para o APARELHO de quem assiste, e
// nao para a linha dele.
//
// Sao dois problemas diferentes e o servidor so enxerga um. Ele escolhe a
// camada pela perda de pacotes, que e o que a linha diz; um decodificador que
// nao da conta **nao perde pacote nenhum** — ele recebe tudo e simplesmente
// nao desenha. No log do dono de 19/09/2026, depois de a oscilacao acabar, o
// telefone ficou com `perda=0%` em todas as amostras e mesmo assim acumulou 76
// travadas em dez minutos, com trechos inteiros a 2 quadros por segundo
// recebendo 6 Mbit/s. Quem sabe disso e o proprio navegador de quem assiste.
//
// A partir desta fatia de quadros descartados, a imagem e grande demais.
const STATS_WINDOW = 500;
// The line is read every second by each of its readers; one reading serves
// every reader that asks inside the next 900ms (see link()).
const LINK_WINDOW = 900;
// And packet loss is counted over two seconds however often it is read — and
// never against a sample older than five: after a tab was frozen for a minute,
// the minute would dilute the last bad seconds (the Codex, 23/09/2026).
const LOSS_SPAN = 2000;
const LOSS_MAX = 5000;
const DECODE_PAIN = 0.1;
// E abaixo desta, a decodificacao esta saudavel.
const DECODE_FINE = 0.02;
// Quadros de menos num segundo nao dizem nada. Dois descartados entre dois
// decodificados sao 50%, e 50% de quatro quadros e ruido, nao diagnostico —
// foi o que fez a primeira versao disto disparar sozinha.
const DECODE_MIN_FRAMES = 10;
// E um segundo ruim tambem nao diz nada. Tem de doer seguidamente.
const DECODE_BAD_BEFORE_DOWN = 3;
// Segundos limpos seguidos antes de tentar uma imagem maior de novo. As
// medidas chegam a 1 Hz.
const DECODE_CLEAN_BEFORE_UP = 20;
// E essa espera CRESCE a cada vez que o teto precisa voltar, ate seis vezes.
//
// Sem isto a escada daqui e um oscilador, exatamente como a do servidor era
// antes de 19/09/2026 — e foi o que eu fiz na primeira versao, um andar acima
// do erro que eu tinha acabado de consertar um andar abaixo. No log do dono
// deu sete ciclos em nove minutos, e cada recuperacao caia em 20 segundos
// cravados, que e o proprio numero daqui.
const DECODE_BACKOFF_MAX = 6;
// E um teto que ficou fora por este tempo foi embora de verdade: a contagem
// recomeca, para quem trocou de rede ou fechou o que estava pesando nao ficar
// preso numa imagem pequena por uma tarde.
const DECODE_FORGET = 5 * 60_000;
// O piso entre duas mudancas de teto por decodificacao, nos dois sentidos.
// Maior que o da escada do servidor de proposito: trocar de camada custa uma
// imagem completa, e um aparelho que nao da conta nao muda de opiniao de um
// segundo para o outro.
const DECODE_HOLD = 20_000;

// Onde uma camada esta na ordem do maior para o menor. Fora da lista conta
// como a maior, que e "sem teto".
function layerIndexOf(rid: string): number {
	const i = SCREEN_LAYERS.findIndex((l) => l.rid === rid);
	return i < 0 ? 0 : i;
}

const SCREEN_CODECS = ['video/h264', 'video/vp9', 'video/vp8'];

function screenCodecPreferences(): RTCRtpCodec[] | null {
	const caps = RTCRtpSender.getCapabilities('video');
	if (!caps?.codecs?.length) return null;
	const rank = (c: RTCRtpCodec) => {
		const i = SCREEN_CODECS.indexOf(c.mimeType.toLowerCase());
		return i < 0 ? SCREEN_CODECS.length : i;
	};
	// Sorting is stable, so codecs of the same rank keep the order the browser
	// chose — which is what decides the H.264 profile, and it knows better.
	return [...caps.codecs].sort((a, b) => rank(a) - rank(b));
}

// screenConstraints is what the browser is asked to capture. A chosen size is a
// ceiling and not a wish — "720p" that arrives as 1080p is not a setting, it is
// a decoration — but an untouched setup asks for exactly what it always asked
// for.
//
// It is out here, and exported, because the direct 1:1 call shares a screen too
// (direct.ts) and the owner's choice of size and rate is one choice, not one per
// kind of call.
export function screenConstraints(quality: ScreenQuality, fps: ScreenFps): MediaTrackConstraints {
	const { width, height } = screenSize(quality);
	const rate = screenFrameRate(fps);
	// `max` on every axis, including "Automática", and that is the fix of
	// 19/09/2026.
	//
	// `ideal` is a preference and getDisplayMedia is free to ignore it, and it
	// does: "Automática" asked for 30 frames as an ideal with no ceiling, and
	// the owner's log shows the browser handing back **60** — twice the frames,
	// priced by the table at 30, from a monitor that simply runs at 60. The
	// same hole was open on size, where an untouched setting on a 1440p or 4K
	// monitor shared every one of those pixels while the ceiling was being
	// calculated for 1080p.
	//
	// A setting that the browser is allowed to disregard is not a setting. The
	// numbers were always meant as ceilings — the D4c conserto said it about
	// size in as many words, "720p that arrives as 1080p is not a setting, it
	// is a decoration" — and now they are ceilings on the frame rate too.
	return {
		width: { ideal: width, max: width },
		height: { ideal: height, max: height },
		frameRate: { ideal: rate, max: rate },
	};
}

function screenSize(quality: ScreenQuality): { width: number; height: number } {
	const height = quality === 'auto' ? 1080 : Number(quality);
	return { width: Math.round((height * 16) / 9), height };
}

// screenFrameRate is the number the encoder is actually asked for.
function screenFrameRate(fps: ScreenFps): number {
	return fps === 'auto' ? SCREEN_FPS_DEFAULT : Number(fps);
}

// screenBitrate prices one screen, from the size and the rate and nothing else.
// Untouched, that is 1080p at thirty: eight megabits.
/**
 * The film's own sound, as getDisplayMedia is asked for it. Only some browsers
 * offer it, and the person still has to tick the box; nothing fails if they do
 * not.
 *
 * restrictOwnAudio: sharing the whole screen records every sound of the PC,
 * including the voices this page plays, and the people watching heard
 * themselves come back (the owner, 16/10/2026). Chrome and Edge leave this
 * page's own sound out when asked; a browser that does not know the word
 * ignores it.
 */
export function screenAudioConstraints(): MediaTrackConstraints {
	return {
		echoCancellation: false,
		noiseSuppression: false,
		autoGainControl: false,
		restrictOwnAudio: true,
	} as MediaTrackConstraints;
}

export function screenBitrate(quality: ScreenQuality, fps: ScreenFps): number {
	return pick(SCREEN_CEILING, quality, fps);
}

// screenStart is where the encoder is asked to begin, which since 18/09/2026 is
// a different question from how far it may go. See SCREEN_START.
export function screenStart(quality: ScreenQuality, fps: ScreenFps): number {
	return pick(SCREEN_START, quality, fps);
}

function pick(
	table: Record<string, Record<string, number>>,
	quality: ScreenQuality,
	fps: ScreenFps,
): number {
	const row = table[quality === 'auto' ? '1080' : quality] ?? table['1080'];
	return row[String(screenFrameRate(fps))] ?? row['30'];
}
// A camera is a face in a corner and is priced like one. Three cameras at this
// rate plus a screen is the sprint's acceptance criterion, and it is inside the
// numbers the machine was measured at in D2.
export const CAMERA_BITRATE = 900_000;

// How much of the ceiling to ask for on the very first second.
//
// A browser that is told nothing starts a screen at about three hundred
// kilobits and feels its way up, doubling every few seconds. That is why the
// owner's log reads 320×180, then 960×540, then 1280×720, and only reaches
// 1080p forty seconds later: by the time the picture is worth looking at, the
// thing he wanted to show has been on screen for half a minute.
//
// There is a way to say "start here", and it is a line in the description the
// browser receives. Sixty per cent of the ceiling rather than all of it: the
// point is to arrive at the right size in the first second or two, not to fire
// four megabits into a line that may not have them and spend the next ten
// seconds recovering from it.
// How long a hang-up will wait for the server to acknowledge it before pulling
// the connection down anyway. Long enough for a round trip to Europe (211 ms
// from the owner's machine, measured 18/09/2026) and several more; short enough
// that leaving never feels stuck.
const LEAVE_WAIT = 2_000;

const START_SHARE = 0.6;
const START_FLOOR = 800; // kbit/s

// withStartBitrate writes that line into the description, for the slots this
// device sends pictures into and no others.
//
// It is fussy on purpose. It only ever *appends* to a codec's own parameter
// line, only inside a video section, only for the slots named, and only for the
// three codecs that carry pictures here — never for retransmission. Anything it
// does not recognise it leaves exactly as it found it, and the caller falls back
// to the untouched description if this throws, because a description that does
// not parse is a call that does not happen.
function withStartBitrate(sdp: string, kbpsFor: (mid: string) => number): string {
	const lines = sdp.split(/\r?\n/);
	while (lines.length > 0 && lines[lines.length - 1] === '') lines.pop();

	const out: string[] = [];
	let video = false;
	let kbps = 0;
	// Payload types in this section that carry pictures and have not had a
	// parameter line of their own yet. VP8 usually has none, so one is written.
	let wanted = new Set<string>();
	const close = () => {
		for (const pt of wanted) out.push(`a=fmtp:${pt} x-google-start-bitrate=${kbps}`);
		wanted = new Set();
		video = false;
		kbps = 0;
	};

	for (const line of lines) {
		if (line.startsWith('m=')) {
			close();
			video = line.startsWith('m=video');
			out.push(line);
			continue;
		}
		// The mid names the slot; a slot this device does not send a picture
		// into is worth nothing here and is left exactly as it was.
		if (video && line.startsWith('a=mid:')) {
			kbps = kbpsFor(line.slice('a=mid:'.length).trim());
		}
		if (kbps > 0) {
			const rtpmap = /^a=rtpmap:(\d+) (VP9|VP8|H264)\/90000/i.exec(line);
			if (rtpmap) wanted.add(rtpmap[1]);
			const fmtp = /^a=fmtp:(\d+) /.exec(line);
			if (fmtp && wanted.has(fmtp[1])) {
				wanted.delete(fmtp[1]);
				out.push(
					line.includes('x-google-start-bitrate')
						? line
						: `${line};x-google-start-bitrate=${kbps}`,
				);
				continue;
			}
		}
		out.push(line);
	}
	close();
	return out.join('\r\n') + '\r\n';
}

// LinkStats is how the connection is doing as a whole: the round trip to the
// media server in milliseconds (-1 while the browser has not measured one yet),
// how much of the voice never arrived in the last few seconds, how unevenly it
// is arriving, and — since 19/09/2026 — what the browser believes the line can
// carry.
//
// That last pair is the number that was missing on the day it was most needed.
// A watcher's log showed a picture falling from 4499 kbit/s to 31 with no loss
// at all, and nothing in the log could say whether the sender had been told to
// slow down or had simply stopped having anything to send. `outKbps` is the
// congestion controller's own estimate, which is the difference between those
// two stories.
export interface LinkStats {
	ms: number;
	lost: number;
	jitter: number;
	// -1 when the browser has not published an estimate yet. Firefox does not
	// always give the incoming one, so a missing number is a missing number
	// rather than a zero.
	outKbps: number;
	inKbps: number;
}

// TileStats is what the stats readout shows, at one second apart, in its own
// component. Plain numbers: nothing here is a stream.
export interface TileStats {
	key: string;
	width: number;
	height: number;
	fps: number;
	kbps: number;
	codec: string;
	// How much of it never arrived, in per cent of the last second. This is
	// what "a imagem fica péssima quando a conexão fica ruim" looks like as a
	// number, and it is the difference between a bad line and a bad setting.
	lost: number;
	// Why this device is sending less than it was asked for: 'bandwidth' is the
	// connection, 'cpu' is the machine. Empty means nothing is holding it back.
	// Only ever set on a picture this device is sending.
	limit: string;
	mine: boolean;
	// Quem está codificando, e se é o caminho barato. Só em coisa que este
	// aparelho manda.
	//
	// A pergunta que isto responde é a única que decide se simulcast custa
	// caro para quem transmite: as três camadas estão na placa de vídeo, ou o
	// Chrome caiu para software em alguma delas? Ter placa **não garante** que
	// esteja usando — o Chromium tem caminhos de recuo, e quando ele recorre a
	// software o custo vai para a CPU que o jogo queria.
	//
	// `encoderImplementation` é o nome do codificador; `powerEfficientEncoder`
	// é o navegador dizendo se aquilo é hardware. Cada camada responde por si,
	// então aqui vêm as três, separadas por barra quando diferirem.
	encoder: string;
	// Quantas camadas estão subindo, quando são mais de uma (D4r). Zero em
	// tudo que não é uma tela em camadas, e é isso que o painel lê para dizer
	// que os megabits são a soma das três.
	layers: number;
	// How many times this picture has stopped dead since it started arriving —
	// the browser's own count of it, not a guess. "Ta dando uns stutterings" is
	// this number, and it is the difference between a stutter somebody felt and
	// a stutter that can be counted. Only ever set on a picture arriving.
	freezes: number;
	// E quanto tempo, somado, essas travadas duraram. Sem isto a contagem nao
	// diz nada: 80 travadas que somam 2 segundos numa chamada de 11 minutos sao
	// soluco, e 80 que somam 60 segundos sao outra coisa inteiramente. Eu vinha
	// tratando o numero como ruim sem saber o que ele media. So em imagem que
	// chega.
	freezeSeconds: number;
	// Quadros que chegaram e o decodificador jogou fora no ultimo segundo. E o
	// numero que separa "a linha nao trouxe" de "este aparelho nao deu conta":
	// a perda responde a primeira pergunta e esta responde a segunda, e uma
	// pode ser zero enquanto a outra nao e. So em imagem que chega.
	dropped: number;
	// Full pictures: sent, on something this device sends; decoded, on
	// something arriving. A full picture is ten to fifty times an ordinary one,
	// and one of them per second is a hitch per second — so this number and the
	// one below it are what tell a hitch caused by the connection apart from a
	// hitch caused by everybody asking to start over.
	keyframes: number;
	// How many times the far end asked this device to start over. Only on
	// something this device sends.
	restarts: number;
	// How long an arriving picture is being held before it is shown, in
	// milliseconds. It goes up when the network is delivering unevenly.
	holdMs: number;
	// What the camera or the screen is producing, as opposed to what is being
	// sent. Only on something this device sends.
	sourceFps: number;
	// E o PISO desse numero nos ultimos dez segundos.
	//
	// A media esconde exatamente o que interessa. Um segundo a 29 quadros pode
	// conter um buraco de 200 ms, e e o buraco que vira travada do outro lado —
	// a media nao o mostra, o minimo mostra. Quando o piso e 2 e a media e 29, a
	// origem parou por quase um segundo dentro da janela.
	sourceFpsMin: number;
}

class Voice {
	private view: VoiceView = {
		channelID: '',
		sessionID: '',
		state: 'off',
		step: '',
		attempt: 0,
		muted: false,
		deafened: false,
		sharingScreen: false,
		sharingCamera: false,
		screenQuality: savedChoice(PREF.quality, QUALITIES, 'auto'),
		screenFps: savedChoice(PREF.fps, FPS_CHOICES, 'auto'),
		cameraID: savedDevice(PREF.camera),
		cameras: [],
		micID: savedDevice(PREF.mic),
		microphones: [],
		speakerID: savedDevice(PREF.speaker),
		speakers: [],
		watching: [],
		tiles: [],
		focus: '',
		popped: '',
		volumes: savedVolumes(),
		micGain: savedMicGain(),
		micFilters: savedMicFilters(),
		noiseLevel: savedNoiseLevel(),
		noiseActive: '',
		error: '',
		notice: '',
	};
	private listeners = new Set<() => void>();

	// None of what follows is ever allowed into a component's state.
	private pc: RTCPeerConnection | null = null;
	private mic: MediaStream | null = null;
	private sink: HTMLDivElement | null = null;
	// The floating window and the box in it the popped picture lives in.
	private popWin: Window | null = null;
	private popHold: HTMLElement | null = null;
	private players = new Map<string, HTMLAudioElement>(); // mid -> element
	private videos = new Map<string, HTMLVideoElement>(); // key -> element

	// Nada é desenhado enquanto o aplicativo está atrás de outra coisa.
	//
	// Pedido do dono em 19/09/2026, e ele tem razão pelo motivo que o D4q
	// mediu: quem transmite está jogando, e o navegador desenhando a sala
	// inteira — a prévia da própria tela inclusive — disputa a GPU com o laço
	// de renderização do jogo. Essa disputa é o que fazia a captura não
	// entregar os 60 quadros. Um `<video>` escondido continua desenhando; quem
	// faz parar é o `pause()`.
	//
	// Nenhum som passa por aqui: todo `<video>` deste serviço é mudo, porque
	// cada som chega numa vaga própria para ter volume próprio (D4b). Então
	// parar a imagem não cala ninguém, e a conversa continua com o aplicativo
	// escondido — que é o caso de uso inteiro.
	//
	// O que sobe **não** para. Quem está mostrando a tela está mostrando de
	// propósito, e o jogo está na frente justamente porque é ele que está
	// sendo mostrado.
	private painting = true;
	// O teto de camada que já foi pedido para a tela de cada pessoa, para não
	// repetir o pedido a cada pixel que o layout mexe.
	private asked = new Map<string, string>(); // username -> rid
	// O teto que o DECODIFICADOR de quem assiste impoe, por pessoa. Vazio
	// quando nao ha nenhum. Ver DECODE_PAIN e judgeDecode().
	private decodeCap = new Map<string, string>(); // username -> rid
	// Como vai a decodificacao de cada imagem que chega, por vaga.
	private decodeHealth = new Map<
		string,
		{
			dropped: number;
			decoded: number;
			received: number;
			clean: number;
			bad: number;
			movedAt: number;
		}
	>();
	// Quantas vezes o teto ja precisou voltar para esta pessoa, e quando ele
	// saiu pela ultima vez. E a memoria que impede o oscilador.
	private decodeFails = new Map<string, number>();
	private decodeLifted = new Map<string, number>();
	private sizeWatch: ResizeObserver | null = null;
	private sizeTimer: ReturnType<typeof setTimeout> | undefined;
	private readonly onVisibility = (): void => {
		const on = document.visibilityState !== 'hidden';
		if (this.painting === on) return;
		this.painting = on;
		for (const [key, el] of this.videos) {
			// The picture in the floating window is still being looked at: the
			// window is over the game, and the tab behind it is what went away.
			if (key === this.view.popped) continue;
			if (on) void el.play().catch(() => undefined);
			else el.pause();
		}
		log(
			on
				? `voz voltou a desenhar ${this.videos.size} imagem(ns)`
				: `voz parou de desenhar ${this.videos.size} imagem(ns): o app saiu da frente`,
		);
		// Não desenhar é metade; a outra metade é parar de RECEBER a imagem
		// grande que ninguém está olhando.
		this.measureSoon();
	};

	constructor() {
		watchPreferences(() => this.retune());
		watchNetwork((reason) => this.restartICE(`network ${reason}`));
		// Uma vez, na criação. Este serviço é um só e vive enquanto a página
		// viver, então não há o que desfazer depois.
		document.addEventListener('visibilitychange', this.onVisibility);
		// O mesmo para o tamanho das imagens. Um ResizeObserver dispara ao
		// observar cada elemento, então a primeira medida sai sozinha.
		if (typeof ResizeObserver !== 'undefined') {
			this.sizeWatch = new ResizeObserver(() => this.measureSoon());
		}
	}

	// A escolha da camada pelo tamanho em que a imagem está sendo desenhada.
	//
	// É a economia que vale dos dois lados de uma vez. Uma tela num quadradinho
	// de 200 px não precisa de 1080p, e mandar 1080p para lá é pago duas vezes:
	// na subida de quem mostra e no decodificador e na bateria de quem assiste.
	// O servidor não tem como saber disso — quem sabe é quem está desenhando.
	//
	// Os cortes são em pixels de verdade, não em CSS: num celular com tela
	// densa, 400 px de CSS são 1200 de imagem, e pedir a camada pequena ali
	// seria entregar uma imagem borrada numa tela que dava conta da boa.
	private layerFor(width: number): string {
		const real = width * (window.devicePixelRatio || 1);
		if (real >= 1200) return 'alta';
		if (real >= 600) return 'media';
		return 'baixa';
	}

	// judgeDecode olha se o decodificador de quem assiste esta dando conta da
	// imagem que esta chegando, e devolve quantos quadros ele jogou fora neste
	// segundo.
	//
	// O sinal preferido e `framesDropped`. Nem todo navegador publica esse
	// campo, e onde ele falta a diferenca entre o que chegou e o que foi
	// decodificado responde a mesma pergunta — pior, porque inclui o quadro
	// que ainda esta na fila, mas na ordem de grandeza certa.
	//
	// A escada aqui e a mesma do servidor, de proposito: desce um degrau de
	// cada vez, sobe so depois de muitos segundos limpos, e tem um piso entre
	// duas mudancas. O que ela move e um TETO, que se combina com o do tamanho
	// desenhado pela mesma regra de sempre — vale o menor dos dois.
	private judgeDecode(mid: string, s: any): number {
		const dropped = s.framesDropped ?? 0;
		const decoded = s.framesDecoded ?? 0;
		const received = s.framesReceived ?? 0;
		const was = this.decodeHealth.get(mid);
		const state = {
			dropped,
			decoded,
			received,
			clean: was?.clean ?? 0,
			bad: was?.bad ?? 0,
			movedAt: was?.movedAt ?? 0,
		};
		this.decodeHealth.set(mid, state);
		if (!was) return 0;
		const nd = Math.max(0, decoded - was.decoded);
		const perdidos =
			s.framesDropped === undefined
				? Math.max(0, received - was.received - nd)
				: Math.max(0, dropped - was.dropped);

		const tile = this.view.tiles.find((t) => t.key === mid);
		// So tela: e a unica coisa que tem camadas para pedir.
		if (!tile || tile.mine || tile.source !== SCREEN) return perdidos;
		const total = perdidos + nd;
		// Uma amostra pequena demais nao e uma medida.
		if (total < DECODE_MIN_FRAMES) return perdidos;

		const share = perdidos / total;
		const now = Date.now();
		if (now - state.movedAt < DECODE_HOLD) return perdidos;

		const atual = this.decodeCap.get(tile.username) ?? '';
		const i = atual ? layerIndexOf(atual) : 0;
		let novo = atual;
		if (share >= DECODE_PAIN) {
			state.clean = 0;
			state.bad += 1;
			if (state.bad >= DECODE_BAD_BEFORE_DOWN && i + 1 < SCREEN_LAYERS.length) {
				novo = SCREEN_LAYERS[i + 1].rid;
			}
		} else if (share <= DECODE_FINE) {
			state.bad = 0;
			state.clean += 1;
			// Quantos segundos limpos custam tirar o teto, que cresce cada vez
			// que ele precisou voltar. Ver DECODE_BACKOFF_MAX.
			const vezes = Math.min(this.decodeFails.get(tile.username) ?? 1, DECODE_BACKOFF_MAX);
			if (state.clean >= DECODE_CLEAN_BEFORE_UP * vezes && i > 0) {
				// Chegando ao topo o teto deixa de existir, em vez de virar
				// "alta": senao ele competiria com quem so quer uma imagem
				// pequena porque a janela e pequena.
				novo = i - 1 === 0 ? '' : SCREEN_LAYERS[i - 1].rid;
			}
		} else {
			state.bad = 0;
			state.clean = 0;
		}
		if (novo === atual) return perdidos;
		state.movedAt = now;
		state.clean = 0;
		state.bad = 0;
		if (novo) {
			this.decodeCap.set(tile.username, novo);
			const saiu = this.decodeLifted.get(tile.username) ?? 0;
			const antes = this.decodeFails.get(tile.username) ?? 0;
			// Um teto que ficou fora tempo bastante foi embora de verdade.
			this.decodeFails.set(
				tile.username,
				saiu > 0 && now - saiu >= DECODE_FORGET ? 1 : antes + 1,
			);
		} else {
			this.decodeCap.delete(tile.username);
			this.decodeLifted.set(tile.username, now);
		}
		const vezes = this.decodeFails.get(tile.username) ?? 1;
		log(
			novo
				? `voz a tela de ${tile.username} nao esta sendo decodificada a tempo (${Math.round(share * 100)}% de ${total} quadros, ${vezes}a vez): teto ${novo}`
				: `voz a tela de ${tile.username} voltou a ser decodificada a tempo: sem teto`,
		);
		void this.measure();
		return perdidos;
	}

	// measureSoon junta as medidas de um mesmo respiro. Arrastar a divisória
	// da janela dispara dezenas de medidas por segundo, e cada uma viraria um
	// pedido ao servidor.
	private measureSoon(): void {
		clearTimeout(this.sizeTimer);
		this.sizeTimer = setTimeout(() => void this.measure(), 400);
	}

	private async measure(): Promise<void> {
		const session = this.view.sessionID;
		if (!session) return;
		for (const tile of this.view.tiles) {
			if (tile.source !== SCREEN || tile.mine) continue;
			const el = this.videos.get(tile.key);
			if (!el) continue;
			// Escondido é o caso mais barato de todos: o aplicativo está atrás
			// de outra coisa, ou este pedaço não está na tela. Ninguém está
			// olhando, e a camada pequena chega instantânea quando voltar.
			const box = el.getBoundingClientRect();
			const olhando = this.painting || tile.key === this.view.popped;
			const pelaTela = !olhando || box.width < 1 ? 'baixa' : this.layerFor(box.width);
			// Duas perguntas diferentes, e vale a MENOR imagem das duas
			// respostas — a mesma regra que o servidor usa entre o teto e a
			// linha. O tamanho desenhado diz de quanta imagem esta pessoa
			// precisa; o decodificador diz de quanta ela da conta. Nenhum dos
			// dois manda no outro.
			const teto = this.decodeCap.get(tile.username) ?? '';
			const rid =
				SCREEN_LAYERS[Math.max(layerIndexOf(pelaTela), teto ? layerIndexOf(teto) : 0)].rid;
			if (this.asked.get(tile.username) === rid) continue;
			this.asked.set(tile.username, rid);
			log(`voz pedindo a camada ${rid} da tela de ${tile.username} (${Math.round(box.width)}px)`);
			try {
				await api.voiceLayer(session, tile.username, rid);
			} catch (err) {
				// Pedir de novo na próxima medida; o que está chegando continua
				// chegando, então isto não estraga nada agora.
				this.asked.delete(tile.username);
				log(`voz nao consegui pedir a camada: ${String(err)}`);
			}
		}
	}
	private info = new Map<string, VoiceTrack>(); // mid -> who and what
	// outgoing is what this device is sending, by source. A track goes in here
	// before the server is asked for a slot, because the offer that comes back
	// names the slot and the track has to be ready to go into it.
	private outgoing = new Map<string, MediaStreamTrack>();
	private outgoingStreams = new Map<string, MediaStream>();
	// Which slot the server gave each thing this device sends. It is what lets
	// the once-a-second readout find this device's own picture in the stats and
	// say why it is smaller than it was asked to be.
	private sendMids = new Map<string, string>(); // source -> mid
	// Set when a server offer rolled back our screen offer, so the screen can
	// be offered again instead of quietly never going up.
	private screenOfferLost = false;
	// wouldShrink says whether this answer turns something off that is on now.
	private wouldShrink(key: string): boolean {
		const now = this.screenSending;
		if (now === 'nenhuma') return false;
		const antes = now === 'todas' ? SCREEN_LAYERS.map((l) => l.rid) : now.split(',');
		const depois = new Set(key === 'nenhuma' ? [] : key.split(','));
		return antes.some((rid) => !depois.has(rid));
	}

	private shrinkTimer: ReturnType<typeof setTimeout> | undefined;

	// Which layers of this device's screen are going out, joined by commas —
	// or 'nenhuma' when nobody in the room is receiving it. See
	// screenWatchers(): the room decides this, not the person showing.
	private screenSending = 'todas';
	// Each incoming sound passes through a gain of its own on the way to the
	// speakers. An <audio> element alone cannot go above its own full volume,
	// and "louder than normal" is the half of a per-person volume control that
	// people actually reach for: the friend on a laptop microphone.
	private ctx: AudioContext | null = null;
	private gains = new Map<string, GainNode>(); // mid -> gain

	// For the stats readout: bytes and frames as they were a second ago, so a
	// rate can be worked out without the server being asked anything.
	// The voice counters as they were at the last reading, so that loss can be
	// "of the last few seconds" instead of "since the call started".
	// The gain on the way out, when there is one. Null is the ordinary case:
	// the microphone going into the connection untouched.
	private micGraph: {
		source: MediaStreamAudioSourceNode;
		gain: GainNode;
		dest: MediaStreamAudioDestinationNode;
	} | null = null;
	// Waiting for the first touch on the page, when the browser's audio engine
	// has not started (audioAwake). Null the rest of the time.
	private waking: (() => void) | null = null;
	// Counts rebuilds of the microphone graph; see micTrack.
	private micSeq = 0;
	// The last few seconds of audio packet counts, one entry per incoming
	// voice (see readLink).
	private linkSamples: { at: number; counts: Map<string, { packets: number; lost: number }> }[] = [];
	private lastStats = new Map<
		string,
		{ bytes: number; frames: number; packets: number; lost: number; at: number }
	>();

	// The jitter buffer's previous totals, kept apart because they are read
	// after rate() has already run.
	//
	// jitterBufferDelay and jitterBufferEmittedCount are both totals for the
	// whole call, so dividing one by the other gives the average wait SINCE
	// THE CALL BEGAN — a number that moves like a current reading and is not
	// one. On 18/09/2026 it read 400, 416, 469, 651, 682 in a log, which looks
	// like a buffer growing in front of your eyes and was in fact an average
	// slowly absorbing a bad patch. What a person reporting "está travando"
	// needs is the wait in the ten seconds they are living through, which is
	// the difference between two readings.
	private lastHold = new Map<string, { delay: number; emitted: number }>();

	// Os contadores de cada camada em separado, e a soma corrente que sai
	// deles.
	//
	// Somar as camadas presentes e subtrair a soma anterior da errado das duas
	// maneiras. Uma camada que SAI faz a soma andar para tras (os megabits
	// negativos de 19/09/2026, ja consertados com um Math.max). Uma camada que
	// VOLTA faz a soma pular pelos bytes da vida inteira dela: no log do dono
	// isso leu 138100 kbit/s, e os picos caiam exatamente onde o `codificador=`
	// passava de um nome para dois.
	//
	// O jeito certo e contar a diferenca de cada camada contra ela mesma e
	// somar as diferencas. Uma camada que acabou de aparecer contribui zero
	// nesta medida, que e a verdade: ela nao mandou nada enquanto esteve
	// desligada.
	private lastLayer = new Map<
		string,
		{ bytes: number; packets: number; lost: number; chaves: number; pedidos: number }
	>();
	private sentTotals = new Map<
		string,
		{ bytes: number; packets: number; lost: number; chaves: number; pedidos: number }
	>();

	// Who this is. It is only used to label this device's own pictures with the
	// same name everybody else's carry, so that the wall can be drawn as one
	// list of people instead of "them, and also me over here".
	private me = { username: '', displayName: '' };

	// Somebody whose screen was asked for and has not arrived yet, so that it
	// becomes the big picture the moment it does.
	private wantFocus = '';

	// Where this device wants to be, and how many times it has tried to get
	// back there. See recover().
	private wanted = '';
	private attempts = 0;
	private retry = 0;
	// Who was in this device's room, as last heard on the live socket. See
	// reconcile().
	private roomSeenPeers: VoicePeer[] | null = null;

	// Answering an offer is not re-entrant: two offers arriving together have
	// to be handled one after the other or the second is answered against a
	// connection the first has already moved.
	private queue: Promise<void> = Promise.resolve();
	private appliedOffer = '';
	private appliedAnswer = '';

	// identify says who is logged in. It is also when this person's own choices
	// are read: everything above was loaded before the page knew whose they
	// were, from the shared key an older version wrote, and this is the moment
	// that gets corrected.
	identify(username: string, displayName: string): void {
		this.me = { username, displayName };
		if (who === username) return;
		who = username;
		this.set({
				screenQuality: savedChoice(PREF.quality, QUALITIES, 'auto'),
			screenFps: savedChoice(PREF.fps, FPS_CHOICES, 'auto'),
			cameraID: savedDevice(PREF.camera),
			micID: savedDevice(PREF.mic),
			speakerID: savedDevice(PREF.speaker),
			volumes: savedVolumes(),
			micGain: savedMicGain(),
			micFilters: savedMicFilters(),
			noiseLevel: savedNoiseLevel(),
		});
		void this.refreshDevices();
	}

	snapshot(): VoiceView {
		return this.view;
	}

	subscribe(fn: () => void): () => void {
		this.listeners.add(fn);
		return () => {
			this.listeners.delete(fn);
		};
	}

	private set(patch: Partial<VoiceView>): void {
		this.view = { ...this.view, ...patch };
		ownChanged(this.view);
		for (const fn of this.listeners) fn();
	}

	// ------------------------------------------------------------ joining

	// Which stay in a room the work in flight belongs to. leave() moves it on,
	// and everything that waited on something — the microphone permission, the
	// screen picker, the camera — checks it when the wait is over. Without it,
	// pressing "sair" while the browser was still asking for the microphone
	// left the room on the screen and then walked straight back into it when
	// the permission arrived, microphone open; and a camera allowed after
	// leaving came on for nobody (the audit of 23/09/2026).
	private stay = 0;

	async join(channelID: string, resumeMedia = false): Promise<void> {
		// Already in it and well. A room somebody is in but whose connection has
		// died is not "already in it": pressing it again has to actually try
		// again, which is also what recover() leans on.
		if (
			!resumeMedia &&
			this.view.channelID === channelID &&
			(this.view.state === 'live' || this.view.state === 'joining')
		) {
			return;
		}
		await this.leave(resumeMedia);
		const stay = this.stay;
		// Where this device wants to be. It survives the connection dying, and
		// it is what tells a drop apart from somebody pressing "sair".
		this.wanted = channelID;
		this.roomSeenPeers = null;
		this.set({ channelID, state: 'joining', step: 'mic', attempt: this.attempts, error: '' });
		log(`voz entrando em ${channelID}`);

		const startedAt = Date.now();
		try {
			// The microphone first, and only then the connection. The server's
			// offer names a slot for this device to send into, and a slot with
			// nothing in it is answered as "nothing is coming" — which is a
			// person nobody can hear and no error anywhere.
			const mic = await this.micStream(this.view.micID);
			if (stay !== this.stay) {
				mic.getTracks().forEach((t) => t.stop());
				log('voz: saiu enquanto o microfone abria; nao entra');
				return;
			}
			this.mic = mic;
		} catch (err) {
			if (stay !== this.stay) return;
			log(`voz sem microfone: ${String(err)}`);
			this.dropLocal([SCREEN, SCREEN_AUDIO, CAMERA]);
			this.set({ state: 'off', channelID: '', error: 'mic' });
			return;
		}
		log(`voz microfone pronto em ${Date.now() - startedAt} ms`);
		// With the microphone allowed there is usually enough permission to
		// read the names of the devices, so the selectors have something to
		// show before anybody presses them.
		void this.refreshDevices();

		const pc = new RTCPeerConnection({
			// No STUN and no TURN. The media server answers on one public
			// address and one port, so there is a route to it from any network
			// that lets ordinary UDP out — and one side with a public address
			// is all a connection needs. A network that blocks UDP outright is
			// a known limit, written down in docs/DEPLOY.md.
			iceServers: [],
			bundlePolicy: 'max-bundle',
		});
		this.pc = pc;
		// A handle for the browser tests and for anybody debugging from the
		// console. It is the connection itself, which the page already owns.
		(window as unknown as { __harmonyPC: RTCPeerConnection }).__harmonyPC = pc;

		// What goes up first is the **plain** microphone, always, even when this
		// person has the suppressor or a gain switched on.
		//
		// Building the clean microphone means fetching a neural network and
		// registering a worklet, and until 18/09/2026 that sat in the middle of
		// this function, between "entrar" and the request that actually joins.
		// Measured against production from the owner's machine — 211 ms away —
		// the network alone was 1.6 s for the wasm and 1.1 s for the worklet,
		// and the column said "Conectando…" through all of it, for something
		// that has nothing to do with connecting.
		//
		// So it goes in afterwards, swapped into the slot it is already in
		// (CLAUDE.md — replaceTrack, never a new connection), and nobody in the
		// room sees anything except the noise going away a second later. It is
		// the same trade this file already makes when the audio engine has not
		// started: being heard without the filter beats being filtered without
		// being heard.
		const track = this.plainMic();
		if (track) {
			this.outgoing.set(MIC, track);
			this.applyMute();
			pc.addTrack(track, this.mic);
		}

		// What kinds of route this device managed to find, counted as they are
		// found and written down once when the search ends.
		//
		// This exists because of 18/09/2026, when a browser on the owner's own
		// PC never connected three times running and the log could not say
		// why. The media server could: the session that failed had sent it
		// **one** candidate, and the telephone that worked had sent nine. One
		// candidate is the local address, which a server on another continent
		// can do nothing with — so the real news was that no route over the
		// internet had been found at all, and none of it was visible here.
		//
		// Types and counts, never addresses. A diagnostic log is something the
		// owner pastes into a conversation, and "srflx=0" says everything that
		// matters without carrying anybody's address along with it.
		const found = new Map<string, number>();
		pc.onicecandidate = (ev) => {
			if (!ev.candidate) {
				// A null candidate is the browser saying it has finished
				// looking. Nothing about the search is worth reading until
				// then, and after it the summary is the whole story.
				const total = [...found.values()].reduce((a, b) => a + b, 0);
				const kinds = [...found.entries()].map(([k, n]) => `${k}=${n}`).join(' ');
				log(`voz caminhos: ${kinds || 'NENHUM'}`);
				// Only "none at all" is worth an alarm here, and the first
				// version of this line got that wrong on 19/09/2026: it
				// complained whenever srflx and relay were missing, which is
				// the rule for a call between two people behind routers. This
				// is not that call. The other end is a server with a public
				// address, so a host candidate reaches it directly — the
				// owner's Edge connected in 2,8 s with `host=8` and was told
				// it would not connect.
				//
				// Nothing at all is different, and it is not a slow connection:
				// with no candidate the browser never even starts checking, so
				// there is no `voz ice` line either and the column sits on
				// "Conectando…" until it gives up. That is a browser that
				// cannot open a UDP socket on this machine — a firewall rule
				// for that browser, or a setting inside it.
				if (total === 0) {
					log('voz ATENCAO: o navegador nao achou NENHUM caminho — nao vai conectar');
				}
				return;
			}
			// Firefox marks the end of its search with an empty candidate as
			// well as with the null one, and that empty marker has no type. It
			// was being counted as a path: the owner's phone read
			// `host=10 desconhecido=1`, and his desktop Firefox read
			// `desconhecido=1` alone — which looked like one path and was in
			// fact none.
			if (!ev.candidate.candidate) return;
			const kind = ev.candidate.type ?? 'desconhecido';
			found.set(kind, (found.get(kind) ?? 0) + 1);
			if (!this.view.sessionID) return;
			void api
				.voiceCandidate(this.view.sessionID, {
					candidate: ev.candidate.candidate,
					sdp_mid: ev.candidate.sdpMid,
					sdp_mline_index: ev.candidate.sdpMLineIndex,
				})
				.catch(() => undefined);
		};
		// Firefox reports this one reliably and has not always reported
		// connectionState the same way, so both are written down. On the day
		// this was added the owner's log had neither: the connection never
		// moved at all, and forty-five seconds of "Conectando…" left no trace.
		pc.oniceconnectionstatechange = () => {
			log(`voz ice ${pc.iceConnectionState}`);
			if (pc !== this.pc) return;
			if (pc.iceConnectionState === 'disconnected' || pc.iceConnectionState === 'failed') {
				this.restartICE(`ice ${pc.iceConnectionState}`);
			}
		};
		// Whether the browser ever went looking, which is a different question
		// from whether it found anything and was the one the log could not
		// answer on 18/09/2026.
		//
		// The owner's Firefox ended its search with no candidate at all in
		// 97 ms and never reached `checking` — and with only the line above,
		// "never reached checking" covers both "looked and found nothing" and
		// "never started". Those have different causes: the first is a browser
		// told to hide its addresses, the second is one that could not ask the
		// parent process for them. `gathering` appearing here separates them.
		pc.onicegatheringstatechange = () => {
			log(`voz coleta ${pc.iceGatheringState}`);
		};
		pc.ontrack = (ev) => this.attach(ev);
		// Se o "conectada em" ja foi escrito para esta conexao.
		let contada = false;
		pc.onconnectionstatechange = () => {
			log(`voz ${pc.connectionState}`);
			if (pc !== this.pc) return;
			if (pc.connectionState === 'connected') {
				// A connection that stood up is a clean slate for recover().
				this.attempts = 0;
				this.set({ state: 'live', step: '', attempt: 0 });
				// The number the column is showing "Conectando…" for. It is in
				// the log because this is the one measurement nobody can take
				// from here: the owner is 211 ms from the machine and every
				// round trip in the chain above costs that, so a complaint
				// about the wait arrives with its own arithmetic attached.
				// So a PRIMEIRA vez. Uma reconexao volta a passar por aqui, e
				// medi-la desde a entrada no canal escreve no log um numero
				// que nao mede nada: no log do dono de 19/09/2026, uma queda
				// de ICE que se resolveu em 24 ms leu "voz conectada em
				// 453265 ms". O instrumento tem de dizer a verdade, porque e
				// nele que eu confio para achar defeito.
				if (contada) {
					log('voz reconectada');
				} else {
					contada = true;
					log(`voz conectada em ${Date.now() - startedAt} ms`);
				}
			}
			if (pc.connectionState === 'failed') {
				this.restartICE('connection failed');
			}
			if (pc.connectionState === 'closed') this.set({ state: 'off' });
		};

		try {
			const askedAt = Date.now();
			this.set({ step: 'server' });
			const offer = await api.voiceJoin(channelID, [MIC]);
			log(`voz servidor respondeu em ${Date.now() - askedAt} ms`);
			// Left — or went to another room — while the server was making
			// the session. It exists there and nowhere here, so it is let go
			// of now. Adopting it put this room's session on the screen of a
			// device that had left (or on the next room's), and apply() on the
			// closed connection returned quietly — so nobody ever told the
			// server, and the room showed this person in it until the server
			// gave up on its own.
			if (stay !== this.stay) {
				log('voz: saiu enquanto o servidor respondia; devolve a sessao');
				void api.voiceLeave(offer.session_id).catch(() => undefined);
				pc.close();
				return;
			}
			// The relay goes in before the answer is applied, because applying
			// it is what starts the search: setLocalDescription is the moment
			// ICE begins gathering, and a server added after that would need an
			// ICE restart to be used at all.
			//
			// setConfiguration and not a second connection — bundlePolicy is
			// repeated because it may not change once set, and changing it
			// throws.
			// Same guard apply() uses below: leaving the channel while this
			// request was in flight closes the connection, and setConfiguration
			// on a closed one throws — which would surface as "falhou ao
			// entrar" for somebody who simply left.
			if (offer.ice_servers?.length && pc.signalingState !== 'closed') {
				pc.setConfiguration({
					iceServers: offer.ice_servers,
					bundlePolicy: 'max-bundle',
				});
			}
			this.set({ sessionID: offer.session_id, step: 'route' });
			await this.apply(offer);
			if (resumeMedia && stay === this.stay) {
				for (const source of [CAMERA, SCREEN]) {
					const track = this.outgoing.get(source);
					if (track?.readyState === 'live') this.showLocal(source, new MediaStream([track]));
				}
				const camera = this.outgoing.get(CAMERA)?.readyState === 'live';
				const screen = this.outgoing.get(SCREEN)?.readyState === 'live';
				this.set({ sharingCamera: camera, sharingScreen: screen });
				this.rebuild();
				if (camera) await this.publish([CAMERA]);
				if (screen) await this.offerScreen(this.outgoing.has(SCREEN_AUDIO) ? [SCREEN, SCREEN_AUDIO] : [SCREEN]);
				if (camera || screen) log('voice fallback resumed existing capture tracks');
			}
			// And now, off the critical path, the microphone this person
			// actually asked for.
			void this.reshapeMic('pela limpeza');
		} catch (err) {
			if (stay !== this.stay) return;
			log(`voz falhou ao entrar: ${String(err)}`);
			// A rejoin during an outage must preserve the desired room and
			// captures. Only deliberate leave or a permanent refusal ends it.
			const transient = !(err instanceof ApiError) || err.status === 0 ||
				err.status === 408 || err.status === 429 || err.status >= 500;
			if (resumeMedia && transient) {
				this.set({ state: 'lost', step: '', notice: 'Reconectando…' });
				this.recover();
				return;
			}
			await this.leave();
			this.set({ error: 'join' });
		}
	}

	// recover puts the person back in the room after the connection dies under
	// them.
	//
	// This is not a nicety. In the owner's log the network blinked, the
	// connection went to "failed", and nothing happened at all: the app sat
	// there for eighty seconds saying "sua tela" while sending one frame a
	// second to nobody, until he worked out that he had to leave and come back
	// by hand. The rule in CLAUDE.md is that losing the connection must not end
	// the call, and it was not being kept.
	//
	// Keep the chosen room until leave or a permanent refusal. Transient
	// outages back off to five seconds and keep the reconnect notice visible.
	private restartAt = 0;
	private restartApplied = false;
	private restartApplying = false;
	private restartCompletedAt = 0;
	private restartTimer = 0;
	private restartPoll = 0;
	private restartSample: Map<string, { bytes: number; packets: number }> | null = null;
	private restartSampleVersion = 0;

	private finishRestart(): void {
		if (!this.restartAt) return;
		log(`voz ICE restart connected in ${Date.now() - this.restartAt} ms; same session; inbound RTP growing; applied=${this.restartApplied}`);
		clearTimeout(this.restartTimer);
		clearTimeout(this.restartPoll);
		this.restartCompletedAt = Date.now();
		this.restartAt = 0;
		this.restartApplied = false;
		this.restartSample = null;
		this.set({ state: 'live', error: '', notice: '' });
	}

	private async inbound(pc: RTCPeerConnection): Promise<Map<string, { bytes: number; packets: number }>> {
		const counters = new Map<string, { bytes: number; packets: number }>();
		const stats = await pc.getStats();
		stats.forEach((s) => {
			if (s.type === 'inbound-rtp') counters.set(s.id, { bytes: s.bytesReceived ?? 0, packets: s.packetsReceived ?? 0 });
		});
		return counters;
	}

	private mediaGrowing(before: typeof this.restartSample, after: NonNullable<typeof this.restartSample>): boolean {
		return !!before && [...after].some(([id, now]) => {
			const old = before.get(id);
			return !!old && (now.bytes > old.bytes || now.packets > old.packets);
		});
	}

	private async pollRestart(pc: RTCPeerConnection, at: number): Promise<void> {
		if (this.restartAt !== at || this.pc !== pc) return;
		const version = this.restartSampleVersion;
		try {
			const counters = await this.inbound(pc);
			if (this.restartAt !== at || this.pc !== pc) return;
			if (version === this.restartSampleVersion) {
				const growing = this.mediaGrowing(this.restartSample, counters);
				this.restartSample = counters;
				if (!this.restartApplying && growing && ['connected', 'completed'].includes(pc.iceConnectionState)) {
					this.finishRestart();
					return;
				}
			}
		} catch { /* A failed stats read proves no recovery. The deadline checks again. */ }
		if (this.restartAt === at && this.pc === pc) this.restartPoll = window.setTimeout(() => void this.pollRestart(pc, at), 350);
	}

	private async restartDeadline(pc: RTCPeerConnection, at: number): Promise<void> {
		if (this.restartAt !== at || this.pc !== pc) return;
		let growing = false;
		try {
			const before = await this.inbound(pc);
			await new Promise((resolve) => window.setTimeout(resolve, 350));
			growing = this.mediaGrowing(before, await this.inbound(pc));
		} catch { /* No evidence of flowing media. */ }
		if (this.restartAt !== at || this.pc !== pc) return;
		if (growing) {
			this.finishRestart();
			return;
		}
		clearTimeout(this.restartPoll);
		this.restartAt = 0;
		log('voz ICE restart timeout; fallback rejoin; inbound RTP stopped');
		if (navigator.onLine) this.recover();
	}

	private restartICE(reason: string): void {
		const pc = this.pc;
		const sid = this.view.sessionID;
		if (!pc || !sid || !this.wanted) return;
		const broken = pc.iceConnectionState === 'disconnected' || pc.iceConnectionState === 'failed';
		const networkHint = /network|online|offline/i.test(reason);
		if (networkHint && !broken && this.restartCompletedAt && Date.now() - this.restartCompletedAt < 2000) return;
		log(`voz ICE restart requested: ${reason}`);
		if (!navigator.onLine || pc.iceConnectionState === 'disconnected' || pc.iceConnectionState === 'failed') {
			this.set({ state: 'lost', notice: 'Reconectando…' });
		}
		if (!navigator.onLine) {
			log('voz restart waiting for network');
			return;
		}
		if (this.restartAt) return;
		this.restartAt = Date.now();
		this.restartApplied = false;
		this.restartSample = null;
		this.restartSampleVersion++;
		const at = this.restartAt;
		void this.pollRestart(pc, at);
		this.restartTimer = window.setTimeout(() => void this.restartDeadline(pc, at), 8000);
		const attempt = async (): Promise<void> => {
			if (this.restartAt !== at || pc !== this.pc) return;
			try {
				const offer = await api.voiceRestart(sid);
				if (pc !== this.pc || sid !== this.view.sessionID) return;
				// A request cancelled by spontaneous recovery can still have created
				// a server offer. Answer it on this session, without reopening recovery.
				this.restartApplying = true;
				try { await this.apply(offer); } finally { this.restartApplying = false; }
				if (this.restartAt !== at || pc !== this.pc) return;
				this.restartApplied = !!offer.ice_restart;
				if (this.restartApplied) {
					this.restartSample = null;
					this.restartSampleVersion++;
					return;
				}
				log('voz replay answered; requesting fresh ICE credentials');
			} catch (err) {
				if (this.restartAt !== at || pc !== this.pc) return;
				if (err instanceof ApiError && err.status === 404 && err.code === 'not_found') {
					clearTimeout(this.restartTimer);
					clearTimeout(this.restartPoll);
					this.restartAt = 0;
					this.restartApplied = false;
					log('voz ICE restart session missing; fallback rejoin');
					this.recover(true);
					return;
				}
				log(`voz ICE restart retry: ${String(err)}`);
			}
			if (this.restartAt === at && pc === this.pc) {
				window.setTimeout(() => {
					this.queue = this.queue.then(attempt).catch((err) => log(`voz restart queue: ${String(err)}`));
				}, 300);
			}
		};
		this.queue = this.queue.then(attempt).catch((err) => log(`voz restart queue: ${String(err)}`));
	}

	private recover(immediate = false): void {
		const target = this.wanted;
		if (!target || this.retry) return;
		const wait = immediate ? 0 : Math.min(5000, 1000 * 2 ** Math.min(this.attempts, 3));
		this.attempts += 1;
		log(`voz caiu, voltando em ${wait}ms (tentativa ${this.attempts})`);
		this.retry = window.setTimeout(() => {
			this.retry = 0;
			if (this.wanted !== target) return; // somebody left in the meantime
			void this.join(target, true);
		}, wait);
	}

	// offer takes a renegotiation from the live socket. Anything that is not
	// for this connection is ignored: the socket carries one person's events,
	// and the same person may have another device in another room.
	offer(offer: VoiceOffer): void {
		if (!this.pc || offer.session_id !== this.view.sessionID) return;
		const pc = this.pc;
		this.queue = this.queue.then(() => this.apply(offer)).catch((err) => {
			log(`voz renegociacao falhou: ${String(err)}`);
			// The server holds this offer until it hears an answer, and every
			// later change for this device queues behind it — an offer left
			// unanswered is a call that silently stops hearing whoever comes
			// in next (PROTOCOL-HARMONY §4.4). Joining again is the way out.
			if (pc === this.pc) this.startOver('a resposta da renegociacao nao chegou');
		});
	}

	// roomSeen keeps the latest list of who is in this device's room, as the
	// live socket tells it.
	roomSeen(channelID: string, peers: VoicePeer[]): void {
		if (channelID !== this.view.channelID) return;
		this.roomSeenPeers = peers;
		roomChanged(channelID, peers, this.me.username, this.view.state !== 'live');
	}

	// A room snapshot cannot prove that this connection answered every offer.
	// In particular, a rejoin can finish before the gateway reconnects; room
	// events received afterwards can already match the snapshot while the
	// offer that added those peers was lost. Compare the advertised sources to
	// the negotiated tracks, not just to the last room event.
	reconcile(rooms: Record<string, VoicePeer[]>, expectedSession = this.view.sessionID): void {
		if (!this.wanted || !this.view.channelID || !expectedSession || expectedSession !== this.view.sessionID ||
			(this.view.state !== 'live' && this.view.state !== 'lost')) return;
		const before = this.roomSeenPeers;
		const now = rooms[this.view.channelID] ?? [];
		this.roomSeenPeers = now;
		// A restarted SFU forgets our session even in an otherwise empty room.
		// Comparing only other participants cannot detect that case.
		if (!now.some((p) => p.session_id === expectedSession)) {
			log('voice gateway resync: own session missing; rejoining');
			this.set({ state: 'lost', notice: 'Reconectando…' });
			this.recover(true);
			return;
		}
		const changed = before && roomSignature(before, this.me.username) !== roomSignature(now, this.me.username);
		const negotiated = new Set([...this.info.values()].map((t) => `${t.username}/${t.source}`));
		// publishing lists optional camera/screen sources; every voice peer
		// also has a microphone, even when their mute switch is on.
		const missing = now.some((p) => p.session_id !== expectedSession && [MIC, ...p.publishing].some((source) => {
			if ((source === SCREEN || source === SCREEN_AUDIO) && !this.view.watching.includes(p.username)) return false;
			return !negotiated.has(`${p.username}/${source}`);
		}));
		if (changed || missing) this.restartICE(changed ? 'a sala mudou com o canal ao vivo fechado' : 'gateway resync: missing negotiated tracks');
	}

	// startOver drops this connection and joins the same room again, through
	// the same door a dead connection uses.
	private startOver(why: string): void {
		if (!this.wanted || this.view.state !== 'live') return;
		this.restartICE(why);
	}

	private async apply(offer: VoiceOffer): Promise<void> {
		const pc = this.pc;
		if (!pc || pc.signalingState === 'closed') return;
		if (this.appliedOffer === offer.sdp && pc.signalingState === 'stable' && pc.localDescription?.type === 'answer') {
			log('voice replay: resend the existing answer');
			await api.voiceAnswer(offer.session_id, this.appliedAnswer);
			return;
		}

		// A tela e a unica coisa que este navegador oferta (D4r), e se uma
		// oferta nossa estiver no ar quando chega a do servidor, a nossa
		// perdeu: o servidor so oferta quando esta assentado e com a fila na
		// mao, ou seja, a nossa ainda nao chegou la. Desfazer e o certo —
		// engolir a oferta do servidor deixaria a sala sem ver alguem.
		if (pc.signalingState === 'have-local-offer') {
			log('voz desfazendo a oferta da tela: o servidor ofertou primeiro');
			await pc.setLocalDescription({ type: 'rollback' });
			this.screenOfferLost = true;
		}
		// Who is on which slot, recorded before the description is set, so that
		// a picture arriving in the same breath already knows whose it is.
		this.info.clear();
		for (const t of offer.tracks) this.info.set(t.mid, t);

		await pc.setRemoteDescription({
			type: 'offer',
			sdp: this.faster(offer.sdp, offer.send_mids ?? {}),
		});

		// Put each thing this device sends into the slot the server named for
		// it. On the first offer the microphone is usually matched already,
		// because the track was added before the description arrived; this is
		// what makes that certain rather than likely, and it is the only way a
		// screen switched on halfway through ever reaches its slot.
		this.sendMids.clear();
		this.asked.clear();
		for (const [source, mid] of Object.entries(offer.send_mids ?? {})) {
			this.sendMids.set(source, mid);
			const tr = pc.getTransceivers().find((t) => t.mid === mid);
			if (!tr) continue;
			const track = this.outgoing.get(source) ?? null;
			if (tr.sender.track !== track) {
				await tr.sender.replaceTrack(track);
			}
			// And say out loud that this slot is one we send into. A slot the
			// browser made for itself while reading the server's offer starts
			// as "recvonly" — the specification says so — and a recvonly slot
			// with a track inside it answers "nothing is coming from me" and
			// then sends nothing, silently. The microphone escaped this
			// because its slot is made by addTrack before any offer arrives,
			// and that is exactly why voice worked and screens did not.
			//
			// "sendrecv" rather than "sendonly": the server puts other
			// people's sound on the microphone's slot as well, and answering
			// sendonly there would hang up on the room. The answer's real
			// direction is this intersected with what was offered, so asking
			// for both is always the safe half of the question.
			if (track && (tr.direction === 'recvonly' || tr.direction === 'inactive')) {
				tr.direction = 'sendrecv';
			}
			if (track) await this.tune(tr, source);
		}

		const answer = await pc.createAnswer();
		await pc.setLocalDescription(answer);
		this.appliedOffer = offer.sdp;
		this.appliedAnswer = answer.sdp ?? '';
		await api.voiceAnswer(offer.session_id, answer.sdp ?? '');

		this.prune(offer.tracks);
		this.rebuild();
		this.retune();
		log(`voz negociada: ${offer.tracks.length} fluxo(s) chegando`);
	}

	// faster is the description with "start here" written into it, so that a
	// screen arrives at the size it was asked for in the first second or two
	// instead of climbing to it over the best part of a minute (START_SHARE).
	//
	// If anything about this goes wrong the untouched description is used: a
	// picture that starts small is a nuisance, and a description that does not
	// parse is a call that never happens.
	//
	// It takes the description and the slots by name because it is used twice:
	// on the server's offer, and on the server's ANSWER to the one offer this
	// browser makes — the screen (D4r). The screen is the main thing it is for,
	// so leaving it out of the second place would have quietly undone it.
	private faster(sdp: string, mids: Record<string, string>): string {
		const source = new Map<string, string>(); // mid -> source
		for (const [s, mid] of Object.entries(mids ?? {})) source.set(mid, s);
		try {
			return withStartBitrate(sdp, (mid) => {
				const what = source.get(mid);
				let ceiling = 0;
				if (what === SCREEN) ceiling = screenStart(this.view.screenQuality, this.view.screenFps);
				if (what === CAMERA) ceiling = CAMERA_BITRATE;
				if (!ceiling) return 0;
				return Math.max(START_FLOOR, Math.round((ceiling * START_SHARE) / 1000));
			});
		} catch (err) {
			log(`voz nao consegui pedir para comecar grande: ${String(err)}`);
			return sdp;
		}
	}

	// tune sets what a picture is allowed to cost. The media server does not
	// re-compress (PLANO-DISCORD.md §4), so this is the only place in the whole
	// system where the quality of a shared screen is decided — and it is decided
	// once, for the whole room.
	private async tune(tr: RTCRtpTransceiver, source: string): Promise<void> {
		if (source !== SCREEN && source !== CAMERA && source !== MIC) return;
		try {
			const params = tr.sender.getParameters();
			// A voz na frente da imagem quando o cano enche.
			//
			// A voz e a tela desta pessoa dividem a MESMA conexao e o mesmo
			// controle de congestionamento, entao uma tela de 40 Mbit/s nao
			// disputa banda com a internet do vizinho: disputa com a propria
			// voz de quem esta falando. Em 19/09/2026 o log do dono marcou
			// `perda-voz=23%` num momento em que a tela estava em 100 Mbit/s.
			//
			// O campo certo e `priority`, e nao `networkPriority`: o explainer
			// do W3C (w3c/webrtc-priority) diz que networkPriority so muda a
			// marcacao DSCP do pacote e "nao tem efeito nenhum na priorizacao
			// local", enquanto priority e o que muda a reparticao quando ha
			// congestionamento. Eu ia pendurar a decisao no campo errado.
			//
			// Nao custa imagem: a voz pega o que precisa (dezenas de kbit/s) e
			// para — o teto dela e o do codec, nao a prioridade.
			//
			// Sem inventar uma vaga que nao veio: setParameters so
			// aceita de volta a mesma lista que getParameters entregou.
			if (source === MIC) {
				if (!params.encodings?.length) return;
				params.encodings[0].priority = 'high';
				await tr.sender.setParameters(params);
				return;
			}
			if (!params.encodings || params.encodings.length === 0) {
				params.encodings = [{}];
			}
			if (source === SCREEN) {
				const { screenQuality, screenFps } = this.view;
				// The ceiling, which is also the permission to grow. See
				// SCREEN_CEILING: taking it away handed the encoder
				// libwebrtc's 2500 kbit/s default and froze the estimate at
				// where it started.
				const ceiling = screenBitrate(screenQuality, screenFps);
				const chosen = screenFps === 'auto' ? undefined : Number(screenFps);
				// One encoding or three, the loop is the same shape. Three is a
				// screen negotiated by this browser (D4r); one is everything
				// that came before it, and the camera.
				params.encodings.forEach((enc, i) => {
					const layer = params.encodings.length > 1 ? SCREEN_LAYERS[i] : undefined;
					// Only the top layer gets the owner's ceiling. The small
					// ones are sized for the person who needed them.
					enc.maxBitrate = layer?.bitrate ?? ceiling;
					if (layer) enc.scaleResolutionDownBy = layer.scale;
					// A chosen frame rate is a ceiling on the encoder as well
					// as on the capture, because the two are capped in
					// different places and only the pair of them holds. A
					// layer never asks for more than the person chose.
					const rate = layer ? Math.min(layer.fps, chosen ?? layer.fps) : chosen;
					if (rate) enc.maxFramerate = rate;
					else delete enc.maxFramerate;
				});
				// Keep the size that was asked for; let the rate fall instead.
				// See SCREEN_DEGRADATION.
				params.degradationPreference = SCREEN_DEGRADATION;
			} else {
				params.encodings[0].maxBitrate = CAMERA_BITRATE;
				params.degradationPreference = 'balanced';
			}
			await tr.sender.setParameters(params);
			// Say which of the two it is. A screen with no ceiling printing
			// "limitado a 0 kbit/s" would read as "switched off".
			const cap = params.encodings[0].maxBitrate;
			const camadas =
				params.encodings.length > 1 ? ` em ${params.encodings.length} camadas` : '';
			log(
				cap
					? `voz ${source} limitado a ${Math.round(cap / 1000)} kbit/s${camadas}`
					: `voz ${source} sem teto de banda${camadas}`,
			);
		} catch (err) {
			// An old browser that will not take the parameters still sends a
			// picture; it just sends whatever it feels like.
			log(`voz sem limite de banda em ${source}: ${String(err)}`);
		}
	}

	// ------------------------------------------------------------ sharing

	// startScreen is the button. The picture is taken first and the server is
	// asked for a slot second, because the offer that comes back has to find a
	// track already waiting for it.
	async startScreen(): Promise<void> {
		if (!this.pc || !this.view.sessionID || this.view.sharingScreen) return;
		const stay = this.stay;

		let stream: MediaStream;
		try {
			stream = await navigator.mediaDevices.getDisplayMedia({
				video: this.screenConstraints(),
				audio: screenAudioConstraints(),
			});
		} catch (err) {
			// Pressing cancel in the picker lands here, and it is not an error.
			log(`voz tela cancelada ou negada: ${String(err)}`);
			return;
		}

		const video = stream.getVideoTracks()[0];
		const audio = stream.getAudioTracks()[0];
		// The picker stays open for as long as somebody takes to choose, and
		// leaving the room meanwhile ends what it was opened for.
		if (!video || stay !== this.stay || !this.pc) {
			stream.getTracks().forEach((t) => t.stop());
			return;
		}
		// contentHint is the one word that tells the encoder what it is looking
		// at, and it is worth more than any bitrate.
		//
		// "motion" since 19/09/2026, and it was "detail" before — the same
		// mistake as SCREEN_DEGRADATION, made for the same reason. "detail"
		// holds a still picture sharp and lets movement smear, which is what
		// somebody reading code wants. A game and a film are movement: they
		// want the frames, and they forgive softness in the ones that move.
		// The owner shares his screen for games and films 95% of the time.
		//
		// It is the half of the pair that matches shrinking rather than
		// slowing (SCREEN_DEGRADATION); the two have to agree or they fight.
		video.contentHint = 'motion';
		this.outgoing.set(SCREEN, video);
		this.outgoingStreams.set(SCREEN, stream);
		const sources = [SCREEN];
		if (audio) {
			log(`voz tela: som do PC, sem o som desta página=${String((audio.getSettings() as { restrictOwnAudio?: boolean }).restrictOwnAudio ?? 'não encontrado')}`);
			this.outgoing.set(SCREEN_AUDIO, audio);
			sources.push(SCREEN_AUDIO);
		}
		// The browser's own "stop sharing" bar is a way out that this app never
		// sees a click on, so it is listened for here.
		video.onended = () => void this.stopScreen();

		this.showLocal(SCREEN, new MediaStream([video]));
		this.screenSending = 'todas';
		this.set({ sharingScreen: true, error: '', notice: '' });
		this.rebuild();
		const settings = video.getSettings();
		log(
			`voz compartilhando a tela (${settings.width ?? '?'}x${settings.height ?? '?'} a ` +
				`${Math.round(settings.frameRate ?? 0)} fps${audio ? ', com som' : ', sem som'})`,
		);
		await this.offerScreen(sources);
	}

	// offerScreen is the one place this app offers instead of answering.
	//
	// Three layers have to be declared when the slot is created — setParameters
	// can change what an encoding does but cannot add one, so by the time the
	// server has offered it is already too late. And the server's library can
	// receive three layers but cannot ask for them; that was measured, in
	// scripts/simulcast-probe, before a line of this was written.
	//
	// It falls back to the ordinary server-offered slot on any failure. One
	// layer is worse than three; no picture at all is worse than both.
	private async offerScreen(sources: string[]): Promise<void> {
		const pc = this.pc;
		const session = this.view.sessionID;
		if (!pc || !session) return;

		// Sharing a second time reuses the slot from the first, layers and all:
		// leaving is what closes it, not stopping the share.
		const mid = this.sendMids.get(SCREEN);
		const existing = mid ? pc.getTransceivers().find((t) => t.mid === mid) : undefined;
		if (existing && existing.sender.getParameters().encodings.length > 1) {
			log('voz reaproveitando a vaga de tres camadas da tela');
			await this.publish(sources);
			return;
		}

		const video = this.outgoing.get(SCREEN);
		const audio = this.outgoing.get(SCREEN_AUDIO);
		if (!video) return;
		// The slots are created once, outside the retry. Creating them per
		// attempt would leave two dead ones behind after the first failure,
		// and the server would be asked to adopt slots nobody is filling.
		//
		// The order here IS the agreement with the server: it adopts the new
		// slots in the order the sources were named.
		const stream = this.outgoingStreams.get(SCREEN);
		const tela = pc.addTransceiver(video, {
			direction: 'sendonly',
			streams: stream ? [stream] : [],
			// Firefox wants the shape of every layer at creation, not
			// afterwards, so the full description goes in here and tune() only
			// refines it.
			sendEncodings: SCREEN_LAYERS.map((l) => ({
				rid: l.rid,
				scaleResolutionDownBy: l.scale,
				maxBitrate:
					l.bitrate ?? screenBitrate(this.view.screenQuality, this.view.screenFps),
				maxFramerate: l.fps,
			})),
		});
		// The owner's codec decision, which used to ride on the server's offer.
		// A browser that refuses the list still carries a picture, so this is a
		// line in the log rather than a reason not to share a screen.
		try {
			const prefs = screenCodecPreferences();
			if (prefs) tela.setCodecPreferences(prefs);
		} catch (err) {
			log(`voz nao consegui pedir H.264 na tela: ${String(err)}`);
		}
		if (audio) pc.addTransceiver(audio, { direction: 'sendonly' });

		for (let attempt = 1; attempt <= 3; attempt++) {
			this.screenOfferLost = false;
			try {
				if (pc.signalingState !== 'stable') throw new Error('nao assentado');
				const offer = await pc.createOffer();
				await pc.setLocalDescription(offer);
				const answer = await api.voiceOffer(session, sources, pc.localDescription?.sdp ?? '');
				// The rollback above may have happened while the request was in
				// the air; applying an answer to an offer we took back would
				// put the connection somewhere neither side believes in.
				if (this.screenOfferLost) throw new Error('oferta desfeita');
				// Which slot each thing landed in is only known now: the mids
				// are assigned when the local description is set.
				const mine: Record<string, string> = {};
				for (const source of sources) {
					const want = this.outgoing.get(source);
					const tr = pc.getTransceivers().find((t) => t.sender.track === want);
					if (tr?.mid) mine[source] = tr.mid;
				}
				// The same "start big" line as on the server's offers. Without
				// it here the screen — which is the whole reason that line
				// exists — would go back to feeling its way up from three
				// hundred kilobits.
				await pc.setRemoteDescription({
					type: 'answer',
					sdp: this.faster(answer.sdp, mine),
				});
				for (const [source, mid] of Object.entries(mine)) {
					const tr = pc.getTransceivers().find((t) => t.mid === mid);
					if (!tr) continue;
					this.sendMids.set(source, mid);
					await this.tune(tr, source);
				}
				log(`voz tela ofertada em ${SCREEN_LAYERS.length} camadas`);
				return;
			} catch (err) {
				log(`voz oferta da tela falhou (${attempt}/3): ${String(err)}`);
				if (pc.signalingState === 'have-local-offer') {
					await pc.setLocalDescription({ type: 'rollback' }).catch(() => {});
				}
				if (attempt < 3) await new Promise((r) => setTimeout(r, 300 * attempt));
			}
		}
		log('voz tela indo numa camada so: a oferta em camadas nao passou');
		await this.publish(sources);
	}

	async stopScreen(): Promise<void> {
		if (!this.view.sharingScreen) return;
		log('voz parou de compartilhar a tela');
		this.dropLocal([SCREEN, SCREEN_AUDIO]);
		this.set({ sharingScreen: false });
		this.rebuild();
		await this.unpublish([SCREEN, SCREEN_AUDIO]);
	}

	async startCamera(): Promise<void> {
		if (!this.pc || !this.view.sessionID || this.view.sharingCamera) return;
		const stay = this.stay;
		let stream: MediaStream;
		try {
			stream = await this.cameraStream(this.view.cameraID);
		} catch (err) {
			if (stay !== this.stay) return;
			log(`voz sem camera: ${String(err)}`);
			this.set({ error: 'camera' });
			return;
		}
		const video = stream.getVideoTracks()[0];
		if (!video || stay !== this.stay || !this.pc) {
			stream.getTracks().forEach((t) => t.stop());
			return;
		}
		video.contentHint = 'motion';
		this.outgoing.set(CAMERA, video);
		this.outgoingStreams.set(CAMERA, stream);
		video.onended = () => void this.stopCamera();
		this.showLocal(CAMERA, new MediaStream([video]));
		this.set({ sharingCamera: true, error: '', notice: '' });
		this.rebuild();
		log('voz camera ligada');
		// The names of the cameras are only real once one of them has been
		// allowed, so the list is worth asking for again right here.
		void this.refreshDevices();
		await this.publish([CAMERA]);
	}

	async stopCamera(): Promise<void> {
		if (!this.view.sharingCamera) return;
		log('voz camera desligada');
		this.dropLocal([CAMERA]);
		this.set({ sharingCamera: false });
		this.rebuild();
		await this.unpublish([CAMERA]);
	}

	private screenConstraints(): MediaTrackConstraints {
		return screenConstraints(this.view.screenQuality, this.view.screenFps);
	}

	// reshape applies a size and a rate to a screen that is already up, so that
	// nobody has to pick their window again to change their mind. What actually
	// came back is written to the log, because a browser is free to ignore any
	// of it and then the numbers on screen are the only honest answer.
	private async reshape(): Promise<void> {
		const track = this.outgoing.get(SCREEN);
		if (!track) return;
		try {
			await track.applyConstraints(this.screenConstraints());
		} catch (err) {
			log(`voz tela nao aceitou o tamanho pedido: ${String(err)}`);
		}
		const s = track.getSettings();
		log(`voz tela agora em ${s.width ?? '?'}x${s.height ?? '?'} a ${Math.round(s.frameRate ?? 0)} fps`);
		const pc = this.pc;
		if (!pc) return;
		const tr = pc.getTransceivers().find((t) => t.sender.track === track);
		if (tr) await this.tune(tr, SCREEN);
	}

	// setScreenQuality and setScreenFps are the two selectors. They are the
	// answer this service has today to a line that cannot carry what is being
	// sent: there is one picture for the whole room, so somebody has to choose
	// a smaller one. Whether that stays the answer is the owner's call
	// (docs/DEPLOY.md, D3).
	setScreenQuality(quality: ScreenQuality): void {
		if (quality === this.view.screenQuality) return;
		this.set({ screenQuality: quality });
		writePref(PREF.quality, quality);
		log(`voz qualidade da tela: ${quality === 'auto' ? 'automática' : quality + 'p'}`);
		void this.reshape();
	}

	setScreenFps(fps: ScreenFps): void {
		if (fps === this.view.screenFps) return;
		this.set({ screenFps: fps });
		writePref(PREF.fps, fps);
		log(`voz quadros da tela: ${fps === 'auto' ? 'automático' : fps + ' fps'}`);
		void this.reshape();
	}

	// ------------------------------------------------------------ devices

	// refreshDevices asks the browser what there is. The names only become real
	// once permission has been given, so a device nobody has allowed yet is
	// numbered rather than named.
	//
	// The "default" and "communications" entries some systems add are kept: they
	// are what "whatever Windows is using right now" means, and for most people
	// that is the right answer and the one they want back after plugging in a
	// headset.
	async refreshDevices(): Promise<void> {
		try {
			const all = await navigator.mediaDevices.enumerateDevices();
			const pick = (kind: string, word: string) => {
				let n = 0;
				return all
					.filter((d) => d.kind === kind)
					.map((d) => ({ id: d.deviceId, label: d.label || `${word} ${++n}` }));
			};
			this.set({
				cameras: pick('videoinput', 'Câmera'),
				microphones: pick('audioinput', 'Microfone'),
				speakers: pick('audiooutput', 'Saída'),
			});
		} catch (err) {
			log(`voz nao consegui listar os aparelhos: ${String(err)}`);
		}
	}

	// setMicrophone swaps which microphone is being sent, inside the connection
	// that is already up, exactly like the camera. Nobody has to leave the room
	// to plug in a headset.
	async setMicrophone(id: string): Promise<void> {
		this.set({ micID: id });
		writePref(PREF.mic, id);
		log(`voz microfone escolhido: ${id ? id.slice(0, 8) : 'o do navegador'}`);
		await this.reopenMic();
		await this.refreshDevices();
	}

	// reopenMic opens the microphone again — the device this person has chosen,
	// with the filters they have chosen — and swaps the new track into the slot
	// the old one is in. Nothing about the connection is rebuilt (CLAUDE.md:
	// replaceTrack, never a new PeerConnection), so this happens mid-sentence
	// without the call noticing.
	//
	// It is the only way to change what the browser does to a voice. See
	// setMicFilter for why that sentence is in this file at all.
	private async reopenMic(): Promise<void> {
		if (!this.pc) return;
		let stream: MediaStream;
		try {
			stream = await this.micStream(this.view.micID);
		} catch (err) {
			log(`voz nao consegui abrir o microfone: ${String(err)}`);
			this.set({ notice: 'Não consegui abrir esse microfone.' });
			return;
		}
		const track = stream.getAudioTracks()[0];
		if (!track) {
			stream.getTracks().forEach((t) => t.stop());
			return;
		}
		const pc = this.pc;
		const old = this.outgoing.get(MIC);
		const tr = pc.getTransceivers().find((t) => t.sender.track === old);
		this.mic?.getTracks().forEach((t) => t.stop());
		this.mic = stream;
		// A gain, if this device has one, has to be rebuilt around the
		// microphone that was just opened: the old graph is reading from a
		// microphone that has been stopped.
		const built = await this.micTrack();
		// Overtaken while the suppressor arrived: a newer rebuild has already
		// put its track in, or this is no longer the room that asked. Sending
		// the raw microphone now would take the filter back off.
		if (pc !== this.pc || this.outgoing.get(MIC) !== old) return;
		const sent = built ?? track;
		this.outgoing.set(MIC, sent);
		this.applyMute();
		if (tr) await tr.sender.replaceTrack(sent);
	}

	// setSpeaker chooses where the room comes out. Every sound here goes through
	// one audio graph on its way to the speakers — that is what makes a volume
	// per person possible — so the graph is what has to be pointed somewhere
	// else, and the elements are only the fallback for a browser that has no
	// graph. A browser that refuses both simply keeps using the system default,
	// which is why the selector says so when it is empty.
	async setSpeaker(id: string): Promise<void> {
		this.set({ speakerID: id });
		writePref(PREF.speaker, id);
		log(`voz saida de som escolhida: ${id ? id.slice(0, 8) : 'a do sistema'}`);
		await this.applySpeaker();
	}

	private async applySpeaker(): Promise<void> {
		const id = this.view.speakerID;
		if (!id) return;
		const ctx = this.ctx as (AudioContext & { setSinkId?: (i: string) => Promise<void> }) | null;
		try {
			if (ctx?.setSinkId) await ctx.setSinkId(id);
		} catch (err) {
			log(`voz saida de som recusada pelo navegador: ${String(err)}`);
		}
		for (const el of this.players.values()) {
			const sink = el as HTMLAudioElement & { setSinkId?: (i: string) => Promise<void> };
			if (!sink.setSinkId) continue;
			try {
				await sink.setSinkId(id);
			} catch {
				// An element that will not take it keeps the default output.
			}
		}
	}

	// micConstraints is what this device asks the browser to do to its own
	// voice. One place, because it is used twice: when the microphone is opened
	// and when a switch is flipped on a microphone that is already open.
	private micConstraints(): MediaTrackConstraints {
		const f = this.view.micFilters;
		return {
			echoCancellation: f.echo,
			// The browser's own suppressor stands down while RNNoise is on.
			// Running both is not twice as clean: each one decides what is
			// voice and removes the rest, and the second one is deciding about
			// something the first has already carved up. Thin, underwater
			// speech is what that sounds like.
			noiseSuppression: f.noise && !f.rnnoise,
			autoGainControl: f.agc,
		};
	}

	// setMicFilter turns one of the three on or off, by **opening the
	// microphone again**.
	//
	// The first version of this asked applyConstraints to change them on the
	// track that was already running, which is what the specification says and
	// what the types allow. It does nothing: Chrome accepts the call, resolves
	// the promise, reports the new value back from getSettings, and carries on
	// applying exactly the processing the track was created with. The switches
	// looked like they worked and changed nothing at all, which is how the
	// owner found it — "os interruptores literalmente não funcionaram".
	//
	// Whether the browser does any of this is decided when the track is made,
	// so a different answer needs a different track. That is a getUserMedia and
	// a replaceTrack, which is a quarter of a second and no interruption to the
	// call; the old way was free and false, and free and false is worse.
	async setMicFilter(which: 'noise' | 'echo' | 'agc' | 'rnnoise', on: boolean): Promise<void> {
		const micFilters = { ...this.view.micFilters, [which]: on };
		this.set({ micFilters });
		writePref(PREF.micFilters, JSON.stringify(micFilters));
		log(`voz filtro ${which} ${on ? 'ligado' : 'desligado'}`);
		await this.reopenMic();
	}

	private async micStream(id: string): Promise<MediaStream> {
		const audio: MediaTrackConstraints = this.micConstraints();
		if (id) {
			try {
				return await navigator.mediaDevices.getUserMedia({
					audio: { ...audio, deviceId: { exact: id } },
					video: false,
				});
			} catch (err) {
				log(`voz microfone escolhido indisponivel, usando o padrao: ${String(err)}`);
			}
		}
		return navigator.mediaDevices.getUserMedia({ audio, video: false });
	}

	// setCamera switches which camera is being sent without touching the
	// connection: the track is swapped inside the slot it is already in
	// (CLAUDE.md — replaceTrack, never a new PeerConnection). Nobody in the room
	// sees anything except the picture changing.
	async setCamera(id: string): Promise<void> {
		if (id === this.view.cameraID && this.view.sharingCamera) return;
		this.set({ cameraID: id });
		writePref(PREF.camera, id);
		log(`voz camera escolhida: ${id ? id.slice(0, 8) : 'a do navegador'}`);
		if (!this.view.sharingCamera) return;

		let stream: MediaStream;
		try {
			stream = await this.cameraStream(id);
		} catch (err) {
			log(`voz nao consegui abrir a camera escolhida: ${String(err)}`);
			this.set({ notice: 'Não consegui abrir essa câmera.' });
			return;
		}
		const track = stream.getVideoTracks()[0];
		if (!track) {
			stream.getTracks().forEach((t) => t.stop());
			return;
		}
		track.contentHint = 'motion';
		const pc = this.pc;
		const old = this.outgoing.get(CAMERA);
		const tr = pc?.getTransceivers().find((t) => t.sender.track === old);
		this.outgoing.set(CAMERA, track);
		this.outgoingStreams.get(CAMERA)?.getTracks().forEach((t) => t.stop());
		this.outgoingStreams.set(CAMERA, stream);
		track.onended = () => void this.stopCamera();
		if (tr) {
			await tr.sender.replaceTrack(track);
			await this.tune(tr, CAMERA);
		}
		this.showLocal(CAMERA, new MediaStream([track]));
		await this.refreshDevices();
	}

	// cameraStream asks for the chosen camera and falls back to whichever one
	// the browser wants. A camera that was unplugged since it was chosen must
	// not turn the button into a dead end.
	private async cameraStream(id: string): Promise<MediaStream> {
		const video: MediaTrackConstraints = {
			width: { ideal: 1280 },
			height: { ideal: 720 },
			frameRate: { ideal: 30 },
		};
		if (id) {
			try {
				return await navigator.mediaDevices.getUserMedia({
					video: { ...video, deviceId: { exact: id } },
					audio: false,
				});
			} catch (err) {
				log(`voz camera escolhida indisponivel, usando a padrao: ${String(err)}`);
			}
		}
		return navigator.mediaDevices.getUserMedia({ video, audio: false });
	}

	private async publish(sources: string[]): Promise<void> {
		const session = this.view.sessionID;
		if (!session) return;
		try {
			await api.voicePublish(session, sources);
		} catch (err) {
			log(`voz nao conseguiu abrir espaco para ${sources.join(',')}`);
			this.dropLocal(sources);
			this.set({
				sharingScreen: sources.includes(SCREEN) ? false : this.view.sharingScreen,
				sharingCamera: sources.includes(CAMERA) ? false : this.view.sharingCamera,
				notice: message(err),
			});
			this.rebuild();
		}
	}

	// unpublish tells the server the slot can be closed. A failure here is worth
	// a line in the log rather than a message on the screen: this device has
	// already stopped sending, and what is left is a slot on the server that
	// closes itself when the connection does. It is the one case where the room
	// could be left looking at a picture that has stopped moving, so it is
	// written down where it can be found afterwards.
	private async unpublish(sources: string[]): Promise<void> {
		const session = this.view.sessionID;
		if (!session) return;
		try {
			await api.voiceUnpublish(session, sources);
		} catch (err) {
			log(`voz nao conseguiu fechar ${sources.join(',')}: ${String(err)}`);
		}
	}

	// dropLocal stops what this device was sending and forgets it. Stopping the
	// track is what makes the browser's own "you are sharing" bar go away, and
	// forgetting it is what makes the next renegotiation empty the slot.
	private dropLocal(sources: string[]): void {
		// A switch-off still waiting belongs to the screen going away. Left
		// armed, it fired on the next screen — somebody who stopped and shared
		// another window inside the five seconds had the new one switched off
		// before its first picture reached the server, and a screen the server
		// never sees is one it never says anything about (audit, 23/09/2026).
		if (sources.includes(SCREEN)) clearTimeout(this.shrinkTimer);
		for (const source of sources) {
			this.outgoing.get(source)?.stop();
			this.outgoing.delete(source);
			const stream = this.outgoingStreams.get(source);
			if (stream) {
				stream.getTracks().forEach((t) => t.stop());
				this.outgoingStreams.delete(source);
			}
			const el = this.videos.get('eu:' + source);
			if (el) {
				el.srcObject = null;
				this.sizeWatch?.unobserve(el);
				el.remove();
				this.videos.delete('eu:' + source);
			}
		}
	}

	// ------------------------------------------------------------ the wall

	// showLocal makes the little picture of your own screen or camera. It is
	// muted and it is yours: it never goes near the connection, it is only so
	// that you can see what everybody else is seeing.
	private showLocal(source: string, stream: MediaStream): void {
		const el = this.videoElement('eu:' + source);
		el.srcObject = stream;
		el.muted = true;
		void el.play().catch(() => undefined);
	}

	private videoElement(key: string): HTMLVideoElement {
		let el = this.videos.get(key);
		if (el) return el;
		el = document.createElement('video');
		// Um quadro que nasce com o aplicativo escondido nasce parado, senão
		// ele desenha uma vez e só obedece na próxima troca de visibilidade.
		el.autoplay = this.painting;
		el.playsInline = true;
		// A video element here never carries sound: every sound in this service
		// arrives on a slot of its own so that it can have a volume of its own.
		el.muted = true;
		el.className = 'h-full w-full bg-black object-contain';
		this.videos.set(key, el);
		this.sizeWatch?.observe(el);
		this.parkedSink().appendChild(el);
		return el;
	}

	// element hands a picture to whoever is drawing the wall. The element is
	// made once and moved, never rebuilt, which is what keeps a screen from
	// blinking every time the app redraws around it.
	element(key: string): HTMLVideoElement {
		// A tile can exist a moment before its picture arrives, so the element
		// is made on demand and filled in when the stream turns up.
		return this.videoElement(key);
	}

	// mount puts a picture inside a container the interface owns. Passing null
	// parks it back out of the tree, still playing, still holding its stream.
	mount(key: string, container: HTMLElement | null): void {
		// A picture in the floating window stays there: the page redrawing
		// around its empty place must neither take it back nor park it.
		if (key === this.view.popped && container !== this.popHold) return;
		const el = this.element(key);
		const target = container ?? this.parkedSink();
		if (el.parentElement === target) return;
		target.appendChild(el);
		// And made to play again, which is the whole of a bug the owner kept
		// hitting: "quando mudo da transmissão pro vídeo ou vice versa, o que
		// está menorzinho fica preto e não carrega".
		//
		// Taking a <video> out of the page pauses it — that is in the standard,
		// not a quirk — and putting it back does not undo that. Nothing was
		// broken about the stream: the element was simply stopped on whatever
		// frame it held, which on a picture that had only just arrived is a
		// black one. Swapping the big picture for a small one moves both
		// elements, so both of them stopped.
		if (container && (el as HTMLVideoElement).paused) {
			void (el as HTMLVideoElement).play().catch(() => undefined);
		}
	}

	// rebuild works out the wall from what is arriving and what this device is
	// sending. Screens first, then cameras: a screen is the reason people are
	// in the room and a camera is a face in the corner.
	private rebuild(): void {
		const tiles: Tile[] = [];
		for (const t of this.info.values()) {
			if (t.kind !== 'video') continue;
			tiles.push({
				key: t.mid,
				username: t.username,
				displayName: t.display_name || t.username,
				source: t.source,
				mine: false,
			});
		}
		if (this.view.sharingScreen) {
			tiles.push({
				key: 'eu:' + SCREEN, username: this.me.username,
				displayName: this.me.displayName || 'Você', source: SCREEN, mine: true,
			});
		}
		if (this.view.sharingCamera) {
			tiles.push({
				key: 'eu:' + CAMERA, username: this.me.username,
				displayName: this.me.displayName || 'Você', source: CAMERA, mine: true,
			});
		}
		tiles.sort((a, b) => {
			const rank = (t: Tile) => (t.source === SCREEN ? 0 : 1) + (t.mine ? 0.5 : 0);
			return rank(a) - rank(b);
		});
		let focus = tiles.some((t) => t.key === this.view.focus) ? this.view.focus : '';
		if (this.wantFocus) {
			// A screen that was asked for has arrived: it is what the person
			// pressed for, so it becomes the big picture.
			const wanted = tiles.find((t) => t.username === this.wantFocus && t.source === SCREEN);
			if (wanted) {
				focus = wanted.key;
				this.wantFocus = '';
			}
		}
		this.set({ tiles, focus });
	}

	// prune throws away the elements of pictures that have stopped arriving.
	private prune(tracks: VoiceTrack[]): void {
		const live = new Set(tracks.map((t) => t.mid));
		for (const [mid, el] of this.videos) {
			if (mid.startsWith('eu:') || live.has(mid)) continue;
			if (mid === this.view.popped) this.popIn();
			el.srcObject = null;
			this.sizeWatch?.unobserve(el);
			el.remove();
			this.videos.delete(mid);
			this.lastStats.delete(mid);
			this.lastHold.delete(mid);
			this.decodeHealth.delete(mid);
		}
		for (const [mid, el] of this.players) {
			if (live.has(mid)) continue;
			el.srcObject = null;
			el.remove();
			this.players.delete(mid);
			this.gains.get(mid)?.disconnect();
			this.gains.delete(mid);
			this.meters.get(mid)?.disconnect();
			this.meters.delete(mid);
		}
	}

	// watch makes one person's screen the big picture, by name rather than by
	// slot, because "assistir a fulano" is what somebody presses and the slot
	// is an accident of when they started sharing.
	watch(username: string): void {
		if (!this.view.watching.includes(username)) {
			// Nothing of theirs is arriving yet. Pressing it is the asking.
			void this.setWatching(username, true);
			return;
		}
		const tile = this.view.tiles.find((t) => t.username === username && t.source === SCREEN);
		if (tile) this.set({ focus: tile.key });
	}

	// setWatching turns one person's screen on or off for this device — a real
	// refusal at the server, so a telephone stops paying for a film it is not
	// looking at. The person stays in the room and stays heard.
	async setWatching(username: string, on: boolean): Promise<void> {
		const session = this.view.sessionID;
		if (!session) return;
		const before = this.view.watching;
		const after = on ? [...new Set([...before, username])] : before.filter((u) => u !== username);
		// The wish is applied here first: the picture goes away the moment the
		// button is pressed, and the server catches up.
		this.set({ watching: after });
		// The picture is not here yet — the server has to be told, and then to
		// offer it again. This remembers to make it the big one when it lands.
		if (on) this.wantFocus = username;
		log(`voz ${on ? 'voltando a assistir' : 'parando de assistir'} ${username}`);
		try {
			await api.voiceWatch(session, username, on);
		} catch (err) {
			log(`voz nao consegui mudar o que assisto: ${String(err)}`);
			this.set({ watching: before, notice: message(err) });
		}
	}

	// screenWatchers is the server saying whether anybody is receiving this device's
	// screen, and it is what keeps "watching is a decision" from costing the
	// person who shares.
	//
	// Since joining a channel no longer starts watching, a screen can be shared
	// into a room where nobody has asked for it — and without this the browser
	// would go on encoding three layers and uploading them to a server that
	// drops every packet. Turning the layers off is `active`, which is one of
	// the few things setParameters really can change.
	//
	// The person sharing still sees their own preview: that comes from the
	// camera track itself and never went near an encoder.
	async screenWatchers(data: unknown): Promise<void> {
		const msg = data as { screen?: boolean; layers?: string[] } | null;
		const on = Boolean(msg?.screen);
		// Which of the three are worth sending. An empty list with `screen`
		// true can only mean the server has not worked it out yet, and the
		// safe reading of "I do not know" is "keep sending".
		const wanted = new Set(Array.isArray(msg?.layers) ? msg.layers : []);
		const keep = (rid: string) => on && (wanted.size === 0 || wanted.has(rid));
		const key = on ? [...wanted].sort().join(',') || 'todas' : 'nenhuma';
		if (this.screenSending === key) {
			// O que está no ar já é o que a sala quer — e por isso qualquer
			// desligamento que estivesse agendado ficou obsoleto AGORA.
			//
			// Este cancelamento morava depois deste `return`, e nunca era
			// alcançado por este caminho. Em 20/09/2026 isso apagou a tela do
			// dono por quarenta segundos: ele saiu da aba (a sala passou a
			// querer só as duas menores, e a espera de cinco segundos foi
			// armada), voltou 1,3 s depois (a sala voltou a querer as três,
			// que era exatamente o que já estava sendo enviado — e a função
			// saiu aqui), e aos cinco segundos o temporizador velho disparou
			// e desligou a camada grande que a sala precisava.
			//
			// Uma guarda que diz "não há nada a fazer" também tem de cancelar
			// o que já ia ser feito.
			clearTimeout(this.shrinkTimer);
			return;
		}

		// Switching a layer ON happens now; switching one OFF waits.
		//
		// Somebody resizing a window, a tile being rebuilt, a second of bad
		// line — each of those moves the wanted set for a moment, and acting
		// on every one of them flaps the encoder on and off. Every switch back
		// on costs a full picture, which is ten to fifty times an ordinary
		// one, so flapping is expensive in exactly the currency this is trying
		// to save. The LiveKit default for the same thing is five seconds.
		//
		// Growing is not delayed: somebody who just made the window big is
		// looking at a small picture until this lands.
		clearTimeout(this.shrinkTimer);
		if (key !== 'todas' && this.wouldShrink(key)) {
			// O tempo de espera chama quem APLICA, e não quem decide: chamar
			// esta mesma função de novo faria ela decidir adiar outra vez, e o
			// desligamento nunca aconteceria.
			this.shrinkTimer = setTimeout(() => void this.applyScreenLayers(on, keep, key), SHRINK_WAIT);
			log(`voz tela vai encolher para ${key} em ${SHRINK_WAIT / 1000}s`);
			return;
		}
		await this.applyScreenLayers(on, keep, key);
	}

	private async applyScreenLayers(
		on: boolean,
		keep: (rid: string) => boolean,
		key: string,
	): Promise<void> {
		if (this.screenSending === key) return;
		const mid = this.sendMids.get(SCREEN);
		const tr = this.pc?.getTransceivers().find((t) => t.mid === mid);
		if (!tr || !tr.sender.track) return;
		const before = this.screenSending;
		this.screenSending = key;
		try {
			const params = tr.sender.getParameters();
			if (!params.encodings?.length) return;
			for (const [i, enc] of params.encodings.entries()) {
				// One encoding is everything that came before layers: it is on
				// whenever anybody is watching at all.
				const rid = params.encodings.length > 1 ? (SCREEN_LAYERS[i]?.rid ?? '') : '';
				enc.active = rid ? keep(rid) : on;
			}
			await tr.sender.setParameters(params);
			log(`voz tela subindo: ${key}`);
		} catch (err) {
			// Failing here costs upload, not the call, so it is written down
			// rather than shown to anybody.
			this.screenSending = before;
			log(`voz nao consegui mudar o que sobe da tela: ${String(err)}`);
		}
	}

	focusOn(key: string): void {
		this.set({ focus: this.view.focus === key ? '' : key });
	}

	// canPopOut says whether this browser can take a picture out of the page
	// into a window that floats over everything (Document Picture-in-Picture:
	// Chrome and Edge on a computer, 24/09/2026; not Firefox, not a telephone).
	canPopOut(): boolean {
		return typeof window !== 'undefined' && 'documentPictureInPicture' in window;
	}

	// popOut moves one picture into a floating window — the same <video>, moved
	// and never copied, so nothing is decoded twice. The window is the
	// browser's: it can be dragged anywhere on the monitor and stays on top of
	// the game. Closing it brings the picture back.
	async popOut(key: string, title: string): Promise<void> {
		const api = (window as any).documentPictureInPicture;
		if (!api) return;
		this.popIn();
		let win: Window;
		try {
			win = await api.requestWindow({ width: 640, height: 360 });
		} catch (err) {
			log(`voz janela flutuante recusada: ${String(err)}`);
			return;
		}
		const doc = win.document;
		// The app's own stylesheet, so the picture's classes mean the same
		// thing there. Same origin, so the page's policy lets it in.
		for (const link of document.querySelectorAll('link[rel="stylesheet"]')) {
			doc.head.appendChild(link.cloneNode(true));
		}
		doc.title = title;
		doc.documentElement.className = 'h-full';
		doc.body.className = 'm-0 h-full overflow-hidden bg-black';
		const hold = doc.createElement('div');
		hold.className = 'h-full w-full';
		doc.body.appendChild(hold);
		this.popWin = win;
		this.popHold = hold;
		const el = this.element(key);
		hold.appendChild(el);
		void el.play().catch(() => undefined);
		win.addEventListener('pagehide', () => {
			if (this.popWin === win) this.popIn();
		});
		win.addEventListener('resize', () => this.measureSoon());
		this.set({ popped: key });
		this.measureSoon();
		log(`voz imagem ${key} numa janela flutuante`);
	}

	// popIn brings the floating picture back to the page and closes its window.
	popIn(): void {
		const key = this.view.popped;
		const win = this.popWin;
		this.popWin = null;
		this.popHold = null;
		if (!key) return;
		// Parked first, and only then announced: the redraw that follows
		// mounts it back into its tile, and parking it after that would take
		// it away again.
		const el = this.videos.get(key);
		if (el) this.parkedSink().appendChild(el);
		this.set({ popped: '' });
		try {
			win?.close();
		} catch {
			// Already closed by the person, which is how most of these end.
		}
		this.measureSoon();
		log(`voz imagem ${key} voltou da janela flutuante`);
	}

	dismiss(): void {
		if (this.view.notice) this.set({ notice: '' });
	}

	// fullscreen is what makes a 1080p screen legible on a telephone: the tile
	// is small because the wall is small, and the picture underneath it is not.
	// It takes the box around the picture rather than the picture itself,
	// whenever there is one, because a `<video>` alone in fullscreen has no room
	// for a way out: the browser draws nothing over it and this app cannot
	// either. With the box in fullscreen, the app's own controls come along.
	//
	// iOS is the exception and always will be: it only knows how to do this to
	// the video itself, through a name of its own, and it draws its own player
	// with its own Done button. That is a way out too, so it is left alone.
	async fullscreen(key: string, box?: HTMLElement | null): Promise<void> {
		if (document.fullscreenElement) {
			try {
				await document.exitFullscreen();
				log('voz saiu da tela cheia');
			} catch (err) {
				log(`voz nao saiu da tela cheia: ${String(err)}`);
			}
			return;
		}
		const el = this.videos.get(key);
		if (!el) return;
		try {
			const webkit = el as HTMLVideoElement & { webkitEnterFullscreen?: () => void };
			if (box?.requestFullscreen) await box.requestFullscreen();
			else if (el.requestFullscreen) await el.requestFullscreen();
			else if (webkit.webkitEnterFullscreen) webkit.webkitEnterFullscreen();
			log(`voz tela cheia em ${key}`);
		} catch (err) {
			log(`voz tela cheia recusada: ${String(err)}`);
		}
	}

	private parkedSink(): HTMLDivElement {
		if (!this.sink) {
			this.sink = document.createElement('div');
			this.sink.hidden = true;
			document.body.appendChild(this.sink);
		}
		return this.sink;
	}

	// attach hangs one arriving stream on an element of its own. One element
	// per source, rather than one mixed together, is the whole reason a volume
	// slider per person — and a separate one for the film they are showing — is
	// possible at all.
	private attach(ev: RTCTrackEvent): void {
		const mid = ev.transceiver?.mid ?? '';
		if (!mid) return;
		const stream = ev.streams[0] ?? new MediaStream([ev.track]);
		const what = this.info.get(mid);

		// A shared screen is worth waiting for. See hold().
		if (what?.source === SCREEN || what?.source === SCREEN_AUDIO) {
			this.hold(ev.receiver, SCREEN_HOLD);
		}

		if (ev.track.kind === 'video') {
			const el = this.videoElement(mid);
			el.srcObject = stream;
			void el.play().catch(() => undefined);
			this.rebuild();
			log(`voz recebendo ${what?.source ?? 'video'} de ${what?.username ?? '?'}`);
			return;
		}

		let el = this.players.get(mid);
		if (!el) {
			el = document.createElement('audio');
			el.autoplay = true;
			this.players.set(mid, el);
			this.parkedSink().appendChild(el);
		}
		el.srcObject = stream;
		// A browser may refuse to play until the person has touched the page.
		// They have — they pressed "join" — but the promise still has to be
		// caught or it surfaces as an unhandled rejection.
		void el.play().catch(() => undefined);
		this.route(mid, stream, el);
		this.retune();
		log(`voz recebendo ${what?.source ?? 'audio'} de ${what?.username ?? '?'}`);
	}

	// hold asks the browser to sit on an arriving picture a little longer before
	// showing it, and it is the answer to the last thing that was still
	// stuttering.
	//
	// The measurement, from the owner's own logs: thirty frames a second
	// arriving steadily, loss almost always zero, and still one visible lurch
	// every few seconds. The reason is the distance. When a packet does go
	// missing, the browser asks for it again — and the reply has to cross the
	// Atlantic twice, which is around two hundred milliseconds, while the
	// browser was only holding a hundred and fifty milliseconds of picture. The
	// replacement arrives after the moment it was needed, so the frame is
	// thrown away and the picture stops. Every single lost packet becomes a
	// hitch, however few of them there are.
	//
	// Holding four hundred milliseconds is enough for that reply to arrive in
	// time. It costs four tenths of a second of delay on a shared screen, which
	// nobody watching a screen can perceive — and the screen's own sound is held
	// by the same amount so a film stays in step with itself. Voices are not
	// touched: a conversation is the one thing here where delay is felt.
	//
	// Two names for the same idea, because browsers disagree about which one to
	// support and neither is universal. Whichever one is not there is simply
	// ignored.
	private hold(receiver: RTCRtpReceiver | undefined, seconds: number): void {
		if (!receiver) return;
		const r = receiver as RTCRtpReceiver & {
			playoutDelayHint?: number;
			jitterBufferTarget?: number;
		};
		let held = '';
		try {
			r.jitterBufferTarget = seconds * 1000;
			if (r.jitterBufferTarget) held = 'jitterBufferTarget';
		} catch {
			// An older browser does not have it.
		}
		try {
			r.playoutDelayHint = seconds;
			if (r.playoutDelayHint) held += (held ? '+' : '') + 'playoutDelayHint';
		} catch {
			// Nor this one.
		}
		log(`voz segurando a tela ${seconds * 1000}ms (${held || 'o navegador nao deixou'})`);
	}

	// route sends one stream through a gain node on its way out. The element
	// keeps the stream alive and is silenced, because a browser will not pull
	// data from a MediaStream that nothing is playing.
	//
	// If any of this is refused — an old browser, audio switched off — the
	// element goes back to playing by itself and the slider simply cannot go
	// above a hundred per cent. Quieter than intended is a bad evening; silent
	// is a broken feature.
	private route(mid: string, stream: MediaStream, el: HTMLAudioElement): void {
		if (this.gains.has(mid)) return;
		try {
			if (!this.ctx) this.ctx = audioContext();
			void this.ctx.resume().catch(() => undefined);
			const source = this.ctx.createMediaStreamSource(stream);
			const gain = this.ctx.createGain();
			source.connect(gain);
			gain.connect(this.ctx.destination);
			this.gains.set(mid, gain);
			// And a meter, hung off the sound as it arrives rather than after
			// the volume: whether somebody is talking is a fact about them, not
			// about how loud this device has chosen to have them. Turning
			// somebody down must not make them stop lighting up.
			this.listen(mid, source);
			el.muted = true;
			// A chosen output has to be applied to the graph that was just
			// built, not only to the one that existed when it was chosen.
			void this.applySpeaker();
		} catch (err) {
			log(`voz sem controle de ganho, usando o elemento: ${String(err)}`);
			el.muted = false;
		}
	}

	// retune applies deafen and the per-source sliders to what is really
	// playing. It runs after anything that could have changed either.
	private retune(): void {
		for (const [mid, el] of this.players) {
			const what = this.info.get(mid);
			const wanted = this.view.volumes[volumeKey(what?.username ?? '', what?.source ?? MIC)] ?? 1;
			const silent = this.view.deafened || silentSource(what?.username ?? '',what?.source ?? MIC);
			const gain = this.gains.get(mid);
			if (gain) {
				gain.gain.value = silent ? 0 : wanted;
			} else {
				el.volume = Math.max(0, Math.min(1, wanted));
				el.muted = silent;
			}
		}
	}

	// ------------------------------------------------------------ stats

	// link is how the *connection* is doing, as opposed to how one picture is
	// doing — the number every service like this one puts in the corner, and
	// the one the owner asked for ("um lugarzinho que mostra o desempenho da
	// chamada").
	//
	// It is the round trip to the media server plus what never arrived of the
	// voice, which together separate the two complaints that sound identical
	// from the inside: "está travando" because the line is slow, and "está
	// picotando" because the line is losing packets. They have different
	// answers and only a number tells them apart.
	// One reading per window, for whoever asks — the rule stats() below already
	// lives by, and for the same reason. The loss here is a DIFFERENCE against
	// the previous reading, and since the call's redraw (23/09/2026) two parts
	// of the screen ask: the bars in the column and the chip in the call's bar.
	// Two readings 20 ms apart would hand the second one three packets to judge
	// the line by, and one of them lost is "33%, ruim".
	async link(): Promise<LinkStats | null> {
		const agora = Date.now();
		if (this.linkOut && agora - this.linkAt < LINK_WINDOW) return this.linkOut;
		if (this.linkPending) return this.linkPending;
		// A reading started before a leave belongs to the old connection: it
		// neither fills the cache nor clears the pending slot of a new one.
		const stay = this.stay;
		const pending = this.readLink().then(
			(out) => {
				if (stay !== this.stay) return null;
				this.linkOut = out;
				this.linkAt = Date.now();
				this.linkPending = null;
				return out;
			},
			() => {
				if (stay === this.stay) this.linkPending = null;
				return null;
			},
		);
		this.linkPending = pending;
		return pending;
	}

	private linkAt = 0;
	private linkOut: LinkStats | null = null;
	private linkPending: Promise<LinkStats | null> | null = null;

	private async readLink(): Promise<LinkStats | null> {
		const pc = this.pc;
		if (!pc || this.view.state !== 'live') return null;
		const stay = this.stay;
		let report: RTCStatsReport;
		try {
			report = await pc.getStats();
		} catch {
			return null;
		}
		// Left, or left and came back, while the browser was counting: these
		// are the old connection's numbers (the Codex, 23/09/2026).
		if (stay !== this.stay || pc !== this.pc) return null;
		let ms = -1;
		let jitter = 0;
		const counts = new Map<string, { packets: number; lost: number }>();
		let outKbps = -1;
		let inKbps = -1;
		report.forEach((v: any, key: string) => {
			// The pair actually carrying the call. Firefox does not always set
			// `nominated`, so "succeeded and has a round trip" is the test.
			if (v.type === 'candidate-pair' && v.state === 'succeeded') {
				const rtt = v.currentRoundTripTime;
				if (typeof rtt === 'number' && (ms < 0 || rtt * 1000 < ms)) ms = Math.round(rtt * 1000);
				// What the congestion controller thinks there is room for. It
				// is the ceiling every picture on this connection is sharing,
				// so it is read once here and not per picture.
				const up = v.availableOutgoingBitrate;
				if (typeof up === 'number') outKbps = Math.max(outKbps, Math.round(up / 1000));
				const down = v.availableIncomingBitrate;
				if (typeof down === 'number') inKbps = Math.max(inKbps, Math.round(down / 1000));
			}
			if (v.type === 'inbound-rtp' && v.kind === 'audio') {
				counts.set(v.id ?? key, { packets: v.packetsReceived ?? 0, lost: v.packetsLost ?? 0 });
				jitter = Math.max(jitter, Math.round((v.jitter ?? 0) * 1000));
			}
		});
		// Loss over the last few seconds, not since the call started: an
		// evening's average would hide the ten bad seconds somebody is living
		// through right now.
		//
		// Measured over LOSS_SPAN even though the meter now reads every second
		// (the owner, 23/09/2026: "poderia atualizar mais rapidamente"). One
		// second of one voice is about fifty packets, so a single lost packet
		// is 2% — the "média" line — and the bars would blink yellow for a
		// packet nobody heard. Against a sample two seconds old it is 1%.
		//
		// Counted voice by voice. Summed, somebody leaving the room took their
		// packets out of the total and the difference went negative, which
		// read as 0% however bad the line was; a voice that is only on one
		// side of the difference, or whose counter went backwards (its report
		// was replaced), is simply not in this reading (the Codex, 23/09/2026).
		const at = Date.now();
		const samples = (this.linkSamples = this.linkSamples.filter((x) => at - x.at <= LOSS_MAX));
		let before = samples[0];
		for (const x of samples) if (at - x.at >= LOSS_SPAN) before = x;
		let got = 0;
		let missed = 0;
		if (before && at > before.at) {
			for (const [id, now] of counts) {
				const then = before.counts.get(id);
				if (!then || now.packets < then.packets || now.lost < then.lost) continue;
				got += now.packets - then.packets;
				missed += now.lost - then.lost;
			}
		}
		const recent = got + missed > 0 ? Math.round((missed / (got + missed)) * 100) : 0;
		samples.push({ at, counts });
		// Keep one sample at least LOSS_SPAN old, and nothing older than it.
		while (samples.length > 1 && at - samples[1].at >= LOSS_SPAN) samples.shift();
		return { ms, lost: recent, jitter, outKbps, inKbps };
	}

	// stats is the once-a-second readout, and it is deliberately a pull rather
	// than a stream: it is asked for by small components that draw numbers,
	// nowhere near anything that holds a picture (CLAUDE.md).
	//
	// **Uma medida por janela, para quem quer que pergunte.** Tudo aqui dentro
	// e DIFERENCA contra a chamada anterior, e mais de uma parte da tela
	// pergunta: a grade e a imagem em tela cheia tem cada uma o seu contador,
	// e quem transmite tem ainda o aviso de limite. Duas chamadas separadas por
	// 20 ms davam a segunda uma janela de 20 ms para dividir — e foi isso, e
	// nao a rede, que escreveu `0 kbit/s` alternando com 9000 no log do dono, e
	// que fez o juiz de decodificacao ler "50% dos quadros" com dois quadros na
	// mao. Um estado de diferenca com dois donos nao tem dono.
	async stats(): Promise<TileStats[]> {
		const agora = Date.now();
		if (agora - this.statsAt < STATS_WINDOW) return this.statsOut;
		if (this.statsPending) return this.statsPending;
		this.statsPending = this.collectStats().then(
			(out) => {
				this.statsOut = out;
				this.statsAt = Date.now();
				this.statsPending = null;
				return out;
			},
			(err) => {
				this.statsPending = null;
				throw err;
			},
		);
		return this.statsPending;
	}

	private statsAt = 0;
	private statsOut: TileStats[] = [];
	private statsPending: Promise<TileStats[]> | null = null;

	private async collectStats(): Promise<TileStats[]> {
		const pc = this.pc;
		if (!pc) return [];
		let report: RTCStatsReport;
		try {
			report = await pc.getStats();
		} catch {
			return [];
		}
		const byId = new Map<string, any>();
		report.forEach((v, k) => byId.set(k, v));
		// Which slot belongs to which of this device's own pictures, the other
		// way round, so an outgoing stream can be labelled "eu:tela".
		const mine = new Map<string, string>();
		for (const [source, mid] of this.sendMids) mine.set(mid, 'eu:' + source);

		const out: TileStats[] = [];
		// Uma tela em camadas produz TRÊS `outbound-rtp` na mesma vaga, e este
		// painel foi escrito quando havia um. Elas são juntadas depois da
		// varredura, porque enquanto não estiverem todas na mão não dá para
		// saber qual é a de cima. Sem isso o painel mostrava a última que a
		// iteração calhasse de entregar — o tamanho da camada pequena, o aviso
		// de CPU de uma camada que é pequena de propósito, e **megabits
		// negativos**, que é a subtração entre duas camadas diferentes em
		// amostras seguidas. Foi o dono quem viu, em 19/09/2026.
		const camadas = new Map<string, any[]>();
		report.forEach((s: any) => {
			const video = s.kind === 'video';
			if (!video) return;
			const mid = String(s.mid ?? '');
			if (s.type === 'inbound-rtp') {
				if (!mid || !this.videos.has(mid)) return;
				const tile = this.rate(mid, false, s, byId, {
					bytes: s.bytesReceived ?? 0,
					frames: s.framesDecoded ?? 0,
					packets: s.packetsReceived ?? 0,
					lost: s.packetsLost ?? 0,
				});
				tile.freezes = Math.round(s.freezeCount ?? 0);
				tile.freezeSeconds = Math.round((s.totalFreezesDuration ?? 0) * 10) / 10;
				tile.dropped = this.judgeDecode(mid, s);
				tile.keyframes = Math.round(s.keyFramesDecoded ?? 0);
				const held = s.jitterBufferDelay ?? 0;
				const shown = s.jitterBufferEmittedCount ?? 0;
				const was = this.lastHold.get(mid);
				const frames = shown - (was?.emitted ?? 0);
				tile.holdMs =
					was && frames > 0
						? Math.round(((held - was.delay) / frames) * 1000)
						: shown > 0
							? Math.round((held / shown) * 1000)
							: 0;
				this.lastHold.set(mid, { delay: held, emitted: shown });
				out.push(tile);
				return;
			}
			if (s.type !== 'outbound-rtp') return;
			const key = mine.get(mid);
			if (!key || !this.videos.has(key)) return;
			const juntas = camadas.get(key);
			if (juntas) juntas.push(s);
			else camadas.set(key, [s]);
		});

		for (const [key, layers] of camadas) {
			// A de cima manda no que se lê: tamanho, quadros e o motivo de o
			// codificador estar segurando. Uma camada pequena está em 320×180
			// porque foi mandada ficar, e dizer isso na tela é uma acusação
			// falsa contra a máquina de quem transmite.
			const top = layers.reduce((a, b) =>
				(b.frameWidth ?? 0) > (a.frameWidth ?? 0) ? b : a,
			);
			// Os bytes, ao contrário, somam: o que sai da máquina é a soma das
			// três, e é isso que custa na conta de quem transmite. Mas a soma
			// é feita das DIFERENÇAS de cada camada, e não dos totais das que
			// estiverem presentes nesta medida — ver lastLayer.
			const corrida = this.sentTotals.get(key) ?? {
				bytes: 0,
				packets: 0,
				lost: 0,
				chaves: 0,
				pedidos: 0,
			};
			let remote: any = undefined;
			for (const l of layers) {
				// rid é o nome da camada; o ssrc serve a quem não publica rid.
				const id = `${key}|${l.rid ?? l.ssrc ?? ''}`;
				const r = byId.get(l.remoteId);
				// What the far end says it never got. It arrives on a report of
				// its own, one round trip late, which is close enough for a
				// readout that redraws every second.
				if (r) remote = r;
				const agora = {
					bytes: l.bytesSent ?? 0,
					packets: l.packetsSent ?? 0,
					lost: r?.packetsLost ?? 0,
					// As imagens completas e os pedidos delas vinham SÓ da
					// camada de cima, e por isso pulavam para os números de
					// outra camada toda vez que o conjunto mudava. Sem isso
					// os dois não são comparáveis, e é a diferença entre eles
					// que diz se uma imagem completa foi pedida ou se o
					// codificador a emitiu sozinho — a pergunta que o log de
					// 20/09/2026 não conseguiu responder.
					chaves: l.keyFramesEncoded ?? 0,
					pedidos: l.pliCount ?? 0,
				};
				const antes = this.lastLayer.get(id);
				// Só conta quem já estava aqui na medida passada, e só para a
				// frente: um contador que andou para trás é um contador que
				// recomeçou, e a resposta certa é recomeçar com ele.
				if (antes) {
					if (agora.bytes >= antes.bytes) corrida.bytes += agora.bytes - antes.bytes;
					if (agora.packets >= antes.packets) {
						corrida.packets += agora.packets - antes.packets;
					}
					if (agora.lost >= antes.lost) corrida.lost += agora.lost - antes.lost;
					if (agora.chaves >= antes.chaves) {
						corrida.chaves += agora.chaves - antes.chaves;
					}
					if (agora.pedidos >= antes.pedidos) {
						corrida.pedidos += agora.pedidos - antes.pedidos;
					}
				}
				this.lastLayer.set(id, agora);
			}
			this.sentTotals.set(key, corrida);
			const { bytes, packets, lost } = corrida;
			const s = top;
			const tile = this.rate(key, true, s, byId, {
				bytes,
				frames: s.framesEncoded ?? 0,
				packets,
				lost,
			});
			tile.layers = layers.length > 1 ? layers.length : 0;
			const nomes = layers.map(
				(l) =>
					String(l.encoderImplementation ?? '?') +
					(l.powerEfficientEncoder === false ? '(software)' : ''),
			);
			tile.encoder = [...new Set(nomes)].join('/');
			// No report is not the same as no loss, and until 19/09/2026 the
			// two printed identically as `perda=0%`. They are opposite news:
			// the far end saying "I got everything" versus the far end not
			// being heard from at all — and the second is one of the
			// documented reasons a browser lowers what it sends, because
			// libwebrtc adds the silence to its idea of the round trip and
			// backs off while nothing at all is lost.
			if (!remote) tile.lost = -1;
			const why = String(s.qualityLimitationReason ?? 'none');
			tile.limit = why === 'none' ? '' : why;
			// Somadas por camada e para sempre, não lidas da maior de agora.
			tile.keyframes = Math.round(corrida.chaves);
			tile.restarts = Math.round(corrida.pedidos);
			// What the capture is producing, as opposed to what is being sent.
			// A shared screen only makes a frame when something on it changes,
			// so a film at thirty simply does not produce sixty however fast
			// the machine is — and when these two numbers differ it is the
			// encoder that is behind, not the screen that is still.
			tile.sourceFps = Math.round(byId.get(s.mediaSourceId)?.framesPerSecond ?? 0);
			tile.sourceFpsMin = this.sourceFloor(key, tile.sourceFps);
			out.push(tile);
		}
		return out;
	}

	// sourceFloor guarda as ultimas leituras da captura e devolve a menor.
	//
	// So e chamado de dentro de collectStats(), que acontece uma vez por janela
	// — se fosse chamado por leitor, dois leitores encheriam a fila em metade do
	// tempo e o piso mediria dez segundos para um e cinco para outro.
	private sourceRing = new Map<string, number[]>();
	private sourceFloor(key: string, fps: number): number {
		const ring = this.sourceRing.get(key) ?? [];
		ring.push(fps);
		if (ring.length > 10) ring.shift();
		this.sourceRing.set(key, ring);
		return Math.min(...ring);
	}

	// rate turns two readings a second apart into the numbers people read. It
	// is the same arithmetic for something arriving and something leaving; only
	// the names of the counters differ.
	private rate(
		key: string,
		mine: boolean,
		s: any,
		byId: Map<string, any>,
		now: { bytes: number; frames: number; packets: number; lost: number },
	): TileStats {
		const at = s.timestamp ?? Date.now();
		const before = this.lastStats.get(key);
		let kbps = 0;
		let lost = 0;
		let fps = Math.round(s.framesPerSecond ?? 0);
		if (before && at > before.at) {
			const seconds = (at - before.at) / 1000;
			// Nunca negativo. Um contador que anda para tras é um contador que
			// recomeçou — uma vaga refeita, uma camada que saiu —, e a resposta
			// certa para isso é "ainda não sei", não um número com sinal
			// trocado na cara de quem está lendo.
			kbps = Math.max(0, Math.round(((now.bytes - before.bytes) * 8) / seconds / 1000));
			if (!fps) fps = Math.round((now.frames - before.frames) / seconds);
			const missing = Math.max(0, now.lost - before.lost);
			const arrived = Math.max(0, now.packets - before.packets);
			if (missing + arrived > 0) lost = Math.round((missing * 100) / (missing + arrived));
		}
		this.lastStats.set(key, { ...now, at });
		const codec = String(byId.get(s.codecId)?.mimeType ?? '');
		return {
			key,
			layers: 0,
			encoder: '',
			width: s.frameWidth ?? 0,
			height: s.frameHeight ?? 0,
			fps,
			kbps,
			codec: codec.replace('video/', ''),
			lost,
			limit: '',
			mine,
			freezes: 0,
			freezeSeconds: 0,
			dropped: 0,
			keyframes: 0,
			restarts: 0,
			holdMs: 0,
			sourceFps: 0,
			sourceFpsMin: 0,
		};
	}

	// ------------------------------------------------------------ controls

	setMuted(muted: boolean): void {
		this.set({ muted });
		this.applyMute();
		this.push();
		log(`voz microfone ${muted ? 'desligado' : 'ligado'}`);
	}

	// Deafening switches the microphone off too, on purpose: it means "I have
	// stepped away", and a room where somebody can hear nothing but is still
	// broadcasting is a room where people talk to a wall.
	setDeafened(deafened: boolean): void {
		this.set({ deafened });
		this.applyMute();
		this.retune();
		this.push();
		log(`voz ${deafened ? 'surdo' : 'ouvindo'}`);
	}

	// ------------------------------------------------- the microphone's volume
	//
	// The one lever on the way *out*, and the only piece of this device's sound
	// that is not exactly what the microphone produced.
	//
	// **At 100% there is no lever at all.** The microphone's own track is what
	// goes into the connection, untouched, which is what everybody has and what
	// has carried every call since D2. Moving the slider builds a small audio
	// graph — microphone into a gain into a track — and swaps that track into
	// the slot the old one is already in with replaceTrack (CLAUDE.md), so it
	// can be changed in the middle of a call without anybody's call blinking.
	// Back at 100% the graph is torn down and the raw track goes back in.
	//
	// It is done this way round on purpose: the price of the feature is paid
	// only by the person who asked for it, and a browser that will not give us
	// an audio graph still gets to talk.
	async setMicGain(value: number): Promise<void> {
		const micGain = Math.max(0, Math.min(2, value));
		this.set({ micGain });
		writePref(PREF.micGain, String(micGain));
		if (this.micGraph) {
			// Already going through the graph: the number is the only thing
			// that changes, and the connection is not touched at all.
			this.micGraph.gain.gain.value = micGain;
			if (micGain !== 1) return;
		}
		await this.reshapeMic();
	}

	// reshapeMic puts whatever is right for the current setting into the
	// connection. It is the only place the outgoing microphone track changes
	// for any reason other than choosing a different microphone.
	private async reshapeMic(why = 'pelo volume'): Promise<void> {
		const pc = this.pc;
		if (!pc || !this.mic) return;
		const old = this.outgoing.get(MIC);
		const tr = pc.getTransceivers().find((t) => t.sender.track === old);
		const track = await this.micTrack();
		// Nothing to change (no graph is wanted), or somebody left and came
		// back while the suppressor was being fetched and this is no longer the
		// connection that asked.
		if (!track || track === old || pc !== this.pc) return;
		this.outgoing.set(MIC, track);
		this.applyMute();
		if (tr) await tr.sender.replaceTrack(track);
		log(`voz microfone refeito ${why}: volume ${Math.round(this.view.micGain * 100)}%`);
	}

	// plainMic is the microphone exactly as the browser handed it over, with
	// nothing built on top and nothing to wait for. It is what goes up while
	// the room is being joined; micTrack is what replaces it a moment later.
	private plainMic(): MediaStreamTrack | null {
		const stream = this.mic;
		const raw = stream?.getAudioTracks()[0] ?? null;
		if (!stream || !raw) return null;
		this.dropMicGraph();
		this.meterMic(stream);
		return raw;
	}

	// micTrack is what belongs in the connection right now: the microphone
	// itself when nothing is being done to it, and otherwise the microphone
	// through a graph — the suppressor, the gain, or both.
	//
	// The plain case is still plain. With no suppressor and the volume at 100%
	// the raw track goes into the connection untouched, exactly as it has since
	// D2, and no audio graph is built at all.
	private async micTrack(): Promise<MediaStreamTrack | null> {
		const stream = this.mic;
		const raw = stream?.getAudioTracks()[0] ?? null;
		if (!stream || !raw) return null;
		const wantGain = this.view.micGain !== 1;
		const wantClean = this.view.micFilters.rnnoise;
		// Only the newest rebuild may finish. Two can be in flight at once — the
		// volume nudged while the suppressor is still arriving — and since the
		// suppressor is one node shared by every rebuild (see suppressor below),
		// both would plug the microphone into it and it would hear the voice
		// twice, 6 dB too loud.
		const seq = ++this.micSeq;
		if (!wantClean) this.releaseSuppressor();
		if (!wantGain && !wantClean) {
			this.dropMicGraph();
			this.meterMic(stream);
			return raw;
		}
		try {
			if (!this.ctx) this.ctx = audioContext();
			// A graph built on an engine that has not started is silent (audioAwake).
			// The plain microphone goes in instead, and the graph is built the moment
			// this person touches anything: being heard without the filter beats being
			// filtered without being heard.
			const awake = await audioAwake(this.ctx);
			if (seq !== this.micSeq) return null;
			if (!awake) {
				log('voz: o audio do navegador nao comecou; microfone vai cru ate o primeiro toque');
				this.micWhenAwake();
				this.dropMicGraph();
				this.meterMic(stream);
				return raw;
			}
			this.dropMicGraph();
			const ctx = this.ctx;
			const source = ctx.createMediaStreamSource(stream);
			const gain = ctx.createGain();
			gain.gain.value = this.view.micGain;
			const dest = ctx.createMediaStreamDestination();

			// microphone → [suppressor] → gain → the track that is sent.
			let last: AudioNode = source;
			if (wantClean) {
				const clean = await this.suppressor(ctx);
				if (seq !== this.micSeq) {
					// Overtaken while waiting; the newer rebuild owns the graph.
					source.disconnect();
					gain.disconnect();
					return null;
				}
				if (clean) {
					// Whatever an earlier graph left plugged into it goes first.
					clean.node.disconnect();
					source.connect(clean.node);
					last = clean.node;
				}
			}
			last.connect(gain);
			gain.connect(dest);
			const out = dest.stream.getAudioTracks()[0];
			if (!out) throw new Error('the audio graph produced no track');
			this.micGraph = { source, gain, dest };
			// The meter is tapped *after* the suppressor, so that the green ring
			// agrees with what the room actually hears: a fan that has been
			// removed from the voice must not go on lighting somebody up.
			this.listen('eu', last);
			return out;
		} catch (err) {
			log(`voz nao consegui montar o audio do microfone: ${String(err)}`);
			this.dropMicGraph();
			this.meterMic(stream);
			return raw;
		}
	}

	// meterMic taps this device's own voice, so that your own face lights up
	// the same way everybody else's does. It reads the microphone directly and
	// not the gain, for the same reason the others are read before the volume.
	private meterMic(stream: MediaStream): void {
		try {
			if (!this.ctx) this.ctx = audioContext();
			void this.ctx.resume().catch(() => undefined);
			this.meters.delete('eu');
			this.listen('eu', this.ctx.createMediaStreamSource(stream));
		} catch {
			// No meter for your own voice; everybody else's still works.
		}
	}

	// micWhenAwake builds the microphone graph again the first time this person
	// touches the page. Arming it twice is the same as arming it once, and it is
	// disarmed on the way out of the room.
	private micWhenAwake(): void {
		if (this.waking) return;
		this.waking = onFirstTouch(() => {
			this.waking = null;
			void this.reshapeMic('porque o audio do navegador acordou');
		});
	}

	private dropMicGraph(): void {
		this.dropSuppressor();
		this.meters.delete('eu');
		if (!this.micGraph) return;
		this.micGraph.source.disconnect();
		this.micGraph.gain.disconnect();
		this.micGraph = null;
	}

	// applyMute silences whichever track is actually in the connection. With
	// the graph in the way that is the graph's output and not the microphone:
	// switching off the microphone itself would feed silence into the gain and
	// the two would have to be kept in step for no gain at all.
	private applyMute(): void {
		const live = !this.view.muted && !this.view.deafened;
		const raw = this.mic?.getAudioTracks()[0];
		const out = this.outgoing.get(MIC);
		if (raw) raw.enabled = this.micGraph ? true : live;
		if (out && out !== raw) out.enabled = live;
	}

	// ------------------------------------------------------- who is talking
	//
	// The green ring around somebody's face, which is the one thing a call needs
	// that a list of names cannot give you: with four people and no cameras,
	// "quem falou?" is otherwise unanswerable.
	//
	// It is measured here, from the sound itself, and not asked of the server —
	// the server never decodes media (CLAUDE.md) and could not answer. Each
	// incoming stream gets an analyser tapped off it before the volume, and this
	// device's own microphone gets one of its own.
	//
	// Watchers are per person and are only called when the answer for *that*
	// person changes. That is what keeps a green ring from redrawing the call
	// ten times a second: during a sentence the answer changes twice, not
	// seventy times, and each change touches one small component and nothing
	// that holds a picture.
	private meters = new Map<string, AnalyserNode>(); // mid (or 'eu') -> meter
	private watchers = new Map<string, Set<(on: boolean) => void>>();
	private talking = new Set<string>();
	// When each person was last heard, so that a gap between two syllables is
	// not read as somebody having stopped.
	private lastLoud = new Map<string, number>();
	private meterTimer = 0;
	private frame = new Float32Array(0);

	private listen(key: string, source: AudioNode): void {
		const ctx = this.ctx;
		if (!ctx || this.meters.has(key)) return;
		try {
			const meter = ctx.createAnalyser();
			// A short time-domain window; speakingSample holds between syllables.
			meter.fftSize = 512;
			meter.smoothingTimeConstant = 0.6;
			source.connect(meter);
			this.meters.set(key, meter);
			if (this.frame.length < meter.fftSize) this.frame = new Float32Array(meter.fftSize);
		} catch {
			// No meter, no ring. Nothing else is affected.
		}
	}

	// watchSpeaking reports whether one person is talking. It hands back the
	// way to stop watching, which is what a component's effect returns.
	watchSpeaking(username: string, fn: (on: boolean) => void): () => void {
		const set = this.watchers.get(username) ?? new Set();
		set.add(fn);
		this.watchers.set(username, set);
		fn(this.talking.has(username));
		this.startMeters();
		return () => {
			const live = this.watchers.get(username);
			if (!live) return;
			live.delete(fn);
			if (live.size === 0) this.watchers.delete(username);
			if (this.watchers.size === 0) this.stopMeters();
		};
	}

	private startMeters(): void {
		if (this.meterTimer) return;
		this.meterTimer = window.setInterval(() => this.readMeters(), METER_EVERY);
	}

	private stopMeters(): void {
		clearInterval(this.meterTimer);
		this.meterTimer = 0;
		for (const who of this.talking) this.tell(who, false);
		this.talking.clear();
	}

	// Playback volume never affects speaking, and a live's soundtrack is not
	// the person's microphone. Only MIC readings feed this rule.
	private readMeters(): void {
		const at = Date.now();
		for (const [key, meter] of this.meters) {
			let who = '';
			if (key === 'eu') {
				// Muted is not quiet: the microphone is still producing sound,
				// it simply is not leaving. Lighting up while muted would be a
				// lie in the one direction that matters — and a microphone
				// turned all the way down is the same lie wearing a different
				// hat, which is what the owner caught ("quando mic ta em 0% o
				// verde detecta ainda"). The meter reads the microphone itself,
				// before the gain, so that turning somebody down never stops
				// them lighting up; that is right for other people and wrong
				// for the one case where the gain is silence.
				if (this.view.muted || this.view.deafened || this.view.micGain === 0) continue;
				who = this.me.username;
			} else {
				const info = this.info.get(key);
				if (info?.source !== MIC) continue;
				who = info.username;
			}
			if (who) {
				const last = speakingSample(this.lastLoud.get(who), this.level(meter), at);
				if (last !== undefined) this.lastLoud.set(who, last);
			}
		}
		const now = new Set<string>();
		for (const [who, when] of this.lastLoud) {
			if (speakingSample(when, 0, at) !== undefined) now.add(who);
			else this.lastLoud.delete(who);
		}
		for (const who of now) if (!this.talking.has(who)) this.tell(who, true);
		for (const who of this.talking) if (!now.has(who)) this.tell(who, false);
		this.talking = now;
	}

	// Float samples preserve quiet signals below the old byte quantization.
	private level(meter: AnalyserNode): number {
		try {
			const buf = this.frame.subarray(0, meter.fftSize);
			meter.getFloatTimeDomainData(buf);
			let sum = 0;
			for (let i = 0; i < buf.length; i++) {
				const d = buf[i];
				sum += d * d;
			}
			return Math.sqrt(sum / buf.length);
		} catch {
			return 0;
		}
	}

	private tell(username: string, on: boolean): void {
		for (const fn of this.watchers.get(username) ?? []) fn(on);
	}

	// ------------------------------------------------------- noise suppression
	//
	// RNNoise is a small neural network that decides, frame by frame, which
	// part of the sound is a voice and removes the rest. It is the same family
	// of thing as what the well-known services charge for, and it is the answer
	// to a complaint the browser's own suppressor cannot reach: a fan, a
	// mechanical keyboard, an air conditioner — steady noise that is not
	// *sudden*, which is what the simple suppressors are built to find.
	//
	// It runs in an AudioWorklet, which is its own thread: an ordinary hitch in
	// the page cannot make somebody's voice stutter. It costs a couple of per
	// cent of one core, on this device, and nothing at all on anybody else's —
	// the voice arrives already clean, because it is cleaned before it leaves.
	//
	// Everything here is prepared once and kept: the wasm is fetched once, the
	// worklet is registered once per audio context, and switching the thing on
	// and off after that is connecting or not connecting one node.
	//
	// Since 25/09/2026 it is DeepFilterNet (suppressor.ts), and that changed
	// one rule here: the suppressor is made **once per audio context** and
	// kept, rather than made again every time the microphone graph is rebuilt.
	// Making DeepFilterNet unpacks its whole model on the audio thread, and
	// that thread also plays everybody else's voice (route) — rebuilding it for
	// a nudge of the volume slider would make the room stutter. Rebuilding the
	// graph only unplugs it; leaving the room, or switching it off, lets it go.
	private clean: { ctx: AudioContext; s: Promise<Suppressor | null>; made: Suppressor | null } | null = null;

	private suppressor(ctx: AudioContext): Promise<Suppressor | null> {
		if (this.clean?.ctx !== ctx) {
			this.releaseSuppressor();
			const entry: NonNullable<typeof this.clean> = { ctx, s: makeSuppressor(ctx, this.view.noiseLevel), made: null };
			// Released while it was still being made: it is let go the moment
			// it exists, instead of running on in a graph nobody uses.
			entry.s = entry.s.then((x) => {
				if (this.clean !== entry) {
					x?.destroy();
					return null;
				}
				entry.made = x;
				// The slider may have moved during the download; what it says
				// now is what counts.
				x?.setLevel(this.view.noiseLevel);
				this.set({ noiseActive: x?.kind ?? 'none' });
				return x;
			});
			this.clean = entry;
		}
		return this.clean.s;
	}

	// Unplugged, not destroyed: the graph is being rebuilt around it. Done at
	// once, so that the rebuild that follows plugs it back in and not the
	// other way round.
	private dropSuppressor(): void {
		try {
			this.clean?.made?.node.disconnect();
		} catch {
			// A node from a context that has already been closed.
		}
	}

	private releaseSuppressor(): void {
		const made = this.clean?.made;
		if (this.clean) this.set({ noiseActive: '' });
		this.clean = null;
		try {
			made?.destroy();
		} catch {
			// Already gone with its context.
		}
	}

	// setNoiseLevel changes how much the suppressor may take away, on the one
	// that is running, without touching the connection.
	setNoiseLevel(db: number): void {
		const noiseLevel = Math.max(10, Math.min(100, Math.round(db)));
		this.set({ noiseLevel });
		writePref(PREF.noiseLevel, String(noiseLevel));
		this.clean?.made?.setLevel(noiseLevel);
		void directSuppressorLevel?.(noiseLevel);
	}

	// setVolume is this device's opinion about one sound, kept here and nowhere
	// else. Two hundred per cent is possible because a friend on a laptop
	// microphone is genuinely quieter than everybody else.
	setVolume(username: string, source: string, value: number): void {
		const key = volumeKey(username, source);
		const volumes = { ...this.view.volumes, [key]: Math.max(0, Math.min(2, value)) };
		this.set({ volumes });
		this.retune();
		writePref(PREF.volumes, JSON.stringify(volumes));
	}

	volume(username: string, source = MIC): number {
		return this.view.volumes[volumeKey(username, source)] ?? 1;
	}


	private push(): void {
		if (!this.view.sessionID) return;
		void api
			.voiceState(this.view.sessionID, this.view.muted, this.view.deafened)
			.catch(() => undefined);
	}

	async leave(keepMedia = false): Promise<void> {
		this.appliedOffer = '';
		clearTimeout(this.restartTimer);
		clearTimeout(this.restartPoll);
		this.restartSample = null;
		this.restartCompletedAt = 0;
		this.restartAt = 0;
		this.restartApplied = false;
		this.stay++;
		const session = this.view.sessionID;
		// Leaving is deliberate, so nothing may put this device back. join()
		// sets it again right after the leave it does itself.
		this.wanted = '';
		if (this.retry) {
			clearTimeout(this.retry);
			this.retry = 0;
		}
		if (!keepMedia) this.dropLocal([SCREEN, SCREEN_AUDIO, CAMERA]);
		this.popIn();
		for (const el of this.players.values()) {
			el.srcObject = null;
			el.remove();
		}
		this.players.clear();
		for (const el of this.videos.values()) {
			el.srcObject = null;
			el.remove();
		}
		this.videos.clear();
		this.info.clear();
		this.lastStats.clear();
		this.lastHold.clear();
		this.lastLayer.clear();
		this.sentTotals.clear();
		this.decodeHealth.clear();
		this.decodeCap.clear();
		this.sourceRing.clear();
		this.decodeFails.clear();
		this.decodeLifted.clear();
		this.statsAt = 0;
		this.statsOut = [];
		for (const g of this.gains.values()) g.disconnect();
		this.gains.clear();
		this.dropMicGraph();
		// A rebuild still waiting on the suppressor finds itself overtaken and
		// builds nothing (micTrack).
		this.micSeq++;
		this.releaseSuppressor();
		this.waking?.();
		this.waking = null;
		this.stopMeters();
		this.meters.clear();
		this.lastLoud.clear();
		this.linkSamples = [];
		this.linkPending = null;
		this.linkOut = null;
		this.linkAt = 0;
		void this.ctx?.close().catch(() => undefined);
		this.ctx = null;
		this.mic?.getTracks().forEach((t) => t.stop());
		this.mic = null;
		this.outgoing.delete(MIC);
		if (!keepMedia) {
			this.outgoing.clear();
			this.outgoingStreams.clear();
		}
		this.sendMids.clear();
		this.asked.clear();
		// Silenced now, closed at the very end. What happens in between is the
		// server being told, and it has to be told while the connection is
		// still up — see below.
		if (this.pc) {
			this.pc.onicecandidate = null;
			this.pc.ontrack = null;
			this.pc.onconnectionstatechange = null;
			this.pc.oniceconnectionstatechange = null;
			this.pc.onicegatheringstatechange = null;
		}
		this.set({
			channelID: '',
			sessionID: '',
			state: 'off',
			step: '',
			attempt: 0,
			error: '',
			notice: '',
			sharingScreen: false,
			sharingCamera: false,
			tiles: [],
			focus: '',
			// A new session on the server knows nothing about what this device
			// refused on the old one, so the two must not disagree.
			watching: [],
		});
		this.wantFocus = '';
		if (session) {
			log('voz saindo');
			// Told BEFORE the connection is closed, and that order is the whole
			// point. Closing it first makes the server drop the session by
			// itself (sfu.go, OnConnectionStateChange), so the request that
			// followed arrived at a session that no longer existed and was
			// answered 404 — every single hang-up, in production and in the
			// browser tests. Asking to leave after having already left.
			//
			// Bounded, because hanging up must not wait on a network that is
			// not answering: after this the connection goes either way, and the
			// server's own timeout is the guarantee it always was.
			await Promise.race([
				api.voiceLeave(session).catch(() => undefined),
				new Promise((r) => setTimeout(r, LEAVE_WAIT)),
			]);
		}
		this.pc?.close();
		this.pc = null;
	}
}

// audioAwake starts the browser's audio engine and says whether it actually
// started.
//
// This is the one fact about web audio that costs a whole feature when it is
// forgotten: **a browser does not start its audio engine until the person
// touches the page.** An AudioContext made before that is "suspended", and a
// graph built on a suspended context does not fail, does not throw and does not
// warn — it produces *silence*. The call connects, the meter on this side
// moves, the other person hears nothing, and every number on both screens says
// the call is fine.
//
// It matters here because of two doors into this app that open with no touch at
// all: answering a ring (the connection is built while it is still ringing) and
// tapping a notification to walk into a voice room. A telephone that came in
// through either one has never been touched by its owner.
export async function audioAwake(ctx: AudioContext): Promise<boolean> {
	try {
		await ctx.resume();
	} catch {
		// Refused: it stays asleep, and the caller sends the plain microphone.
	}
	return ctx.state === 'running';
}

// onFirstTouch runs fn the next time this person touches the page, once, and
// hands back a way to stop waiting. Every event here is one that counts as
// "user activation" to a browser, which is the thing that lets the audio start.
export function onFirstTouch(fn: () => void): () => void {
	const kinds = ['pointerdown', 'keydown', 'touchend'];
	const off = (): void => {
		for (const kind of kinds) window.removeEventListener(kind, once, true);
	};
	function once(): void {
		off();
		fn();
	}
	for (const kind of kinds) window.addEventListener(kind, once, true);
	return off;
}

// The direct call keeps its own suppressor (direct.ts) and registers here how
// to reach it, so that one slider in the settings moves both.
let directSuppressorLevel: ((db: number) => void) | null = null;
export function onNoiseLevel(fn: ((db: number) => void) | null): void {
	directSuppressorLevel = fn;
}

// One voice session per page. Being in two voice channels at once is not a
// thing this service does, and a singleton is what makes that true rather than
// hoped for.
// roomSignature is everybody else in a room and what each is sending, as one
// string: two that differ mean the room changed in a way that sends this
// device an offer. Mute is left out — it changes no offer — and so is this
// person, by name, since their own session is on the list before the join
// reply says which one it is.
export function roomSignature(peers: VoicePeer[], me: string): string {
	return peers
		.filter((p) => p.username !== me)
		.map((p) => `${p.session_id}:${[...p.publishing].sort().join(',')}`)
		.sort()
		.join(' ');
}

export const voice = new Voice();

// A handle for the browser tests and for anybody debugging from the console,
// next to the connection itself (__harmonyPC). It is the same object the app
// uses; nothing is exposed here that the page does not already own.
(window as unknown as { __harmonyVoice: Voice }).__harmonyVoice = voice;

// The tab going away should not leave a microphone in the room until the
// network notices. This is best effort — a browser may not run it — and the
// server's own timeout is the guarantee.
// A camera plugged in or unplugged while the app is open changes the list under
// the selector. Nobody should have to reload for that.
navigator.mediaDevices?.addEventListener?.('devicechange', () => {
	void voice.refreshDevices();
});

window.addEventListener('pagehide', () => {
	const id = voice.snapshot().sessionID;
	if (!id) return;
	navigator.sendBeacon?.(
		'/api/voice/leave',
		new Blob([JSON.stringify({ session_id: id })], { type: 'application/json' }),
	);
});
