// sounds.ts plays the harmony's own sounds: a room's comings and goings,
// mute, a screen starting and ending, and the direct call's ring.
//
// They were chosen by the owner on 29/10/2026 (sons/LISTA.md): soft pops made
// with ElevenLabs, and a ringtone from Pixabay. The files are part of the
// build, beside the bundle, like the font — nothing is fetched from elsewhere.
//
// One AudioContext for all of them, made on first use. A page nobody has
// touched yet has no audio engine running (media.ts, audioAwake): a sound asked
// for then is simply not heard, which is the browser's rule and not a fault.
// The message blip stays where it was (notify.ts).
import { log } from './diag';
import entrarURL from './sounds/entrar.wav?url';
import sairURL from './sounds/sair.wav?url';
import mutarURL from './sounds/mutar.wav?url';
import desmutarURL from './sounds/desmutar.wav?url';
import telaComecouURL from './sounds/tela-comecou.wav?url';
import telaAcabouURL from './sounds/tela-acabou.wav?url';
import toqueURL from './sounds/toque.mp3?url';

export type Sound = 'entrar' | 'sair' | 'mutar' | 'desmutar' | 'tela-comecou' | 'tela-acabou';

const URLS: Record<Sound | 'toque', string> = {
	entrar: entrarURL,
	sair: sairURL,
	mutar: mutarURL,
	desmutar: desmutarURL,
	'tela-comecou': telaComecouURL,
	'tela-acabou': telaAcabouURL,
	toque: toqueURL,
};

let ctx: AudioContext | null = null;
const buffers = new Map<string, Promise<AudioBuffer | null>>();
const lastPlayed = new Map<Sound, number>();

// Every sound asked for, in order, for the browser tests and for anybody
// debugging from the console (as __harmonyVoice). Names only.
const asked: string[] = [];
(window as unknown as { __harmonySounds: string[] }).__harmonySounds = asked;

function context(): AudioContext | null {
	try {
		if (!ctx) ctx = new AudioContext();
		void ctx.resume().catch(() => undefined);
		return ctx;
	} catch {
		return null;
	}
}

function buffer(name: Sound | 'toque'): Promise<AudioBuffer | null> {
	let b = buffers.get(name);
	if (!b) {
		const c = context();
		b = c
			? fetch(URLS[name])
					.then((r) => r.arrayBuffer())
					.then((data) => c.decodeAudioData(data))
					.catch((err) => {
						log(`som ${name} indisponível: ${String(err)}`);
						buffers.delete(name);
						return null;
					})
			: Promise.resolve(null);
		buffers.set(name, b);
	}
	return b;
}

// play sounds one of the short ones. The same sound twice inside 150 ms is
// one sound: three people arriving with one room update is a chord, not a
// burst of identical pops.
export function play(name: Sound): void {
	const now = Date.now();
	if (now - (lastPlayed.get(name) ?? 0) < 150) return;
	lastPlayed.set(name, now);
	asked.push(name);
	void buffer(name).then((b) => {
		const c = ctx;
		if (!b || !c) return;
		const src = c.createBufferSource();
		src.buffer = b;
		src.connect(c.destination);
		src.start();
	});
}

// The ring, looped while a call is asking. The person being called hears it
// at full volume; the person calling hears it from the other end of the line,
// about 10 dB lower (owner's choice, 29/10/2026).
let ring: { src: AudioBufferSourceNode | null; stopped: boolean } | null = null;

export function startRing(calling: boolean): void {
	stopRing();
	asked.push(calling ? 'toque-chamando' : 'toque');
	const mine = { src: null as AudioBufferSourceNode | null, stopped: false };
	ring = mine;
	void buffer('toque').then((b) => {
		const c = ctx;
		if (!b || !c || mine.stopped) return;
		const src = c.createBufferSource();
		src.buffer = b;
		src.loop = true;
		const gain = c.createGain();
		gain.gain.value = calling ? 0.32 : 1;
		src.connect(gain);
		gain.connect(c.destination);
		src.start();
		mine.src = src;
	});
}

export function stopRing(): void {
	if (!ring) return;
	ring.stopped = true;
	try {
		ring.src?.stop();
	} catch {
		// Never started.
	}
	ring = null;
}

// ------------------------------------------------------------ the room

// What the room looked like a moment ago, for this device, so that a change
// can be heard. Kept by username, not session: somebody whose connection
// blinks comes back with a new session, and that is not a leave and a join.
interface RoomSeen {
	channelID: string;
	people: Set<string>;
	screens: Set<string>;
}

let seen: RoomSeen | null = null;

// A person's screen flag can blink while the sharing is set up or torn down
// (seen in the test: on, off and on again in a few hundred ms). So a start
// counts only after the screen was gone this long, and an end only once the
// screen stayed gone for a moment.
const SCREEN_GONE_MS = 1000;
const SCREEN_END_MS = 1000;
const screenGoneAt = new Map<string, number>();
const screenEnding = new Map<string, ReturnType<typeof setTimeout>>();

// roomChanged is called with every list of who is in this device's room.
// While still joining (quiet) it only sets the baseline, and so does the
// first list of a room: everybody already there is not arriving.
export function roomChanged(
	channelID: string,
	peers: { username: string; publishing: string[] }[],
	me: string,
	quiet = false,
): void {
	const people = new Set(peers.filter((p) => p.username !== me).map((p) => p.username));
	const screens = new Set(peers.filter((p) => p.username !== me && p.publishing.includes('screen')).map((p) => p.username));
	const before = seen;
	seen = { channelID, people, screens };
	if (quiet || !before || before.channelID !== channelID) return;
	if ([...people].some((u) => !before.people.has(u))) play('entrar');
	if ([...before.people].some((u) => !people.has(u))) play('sair');
	const now = Date.now();
	let started = false;
	for (const u of screens) {
		if (before.screens.has(u)) continue;
		const ending = screenEnding.get(u);
		if (ending) {
			clearTimeout(ending);
			screenEnding.delete(u);
		}
		if (now - (screenGoneAt.get(u) ?? 0) >= SCREEN_GONE_MS) started = true;
	}
	if (started) play('tela-comecou');
	for (const u of before.screens) {
		if (screens.has(u)) continue;
		screenGoneAt.set(u, now);
		// Heard by the whole room, like its start (owner, 29/10/2026); a
		// person who left with the screen on is heard leaving instead.
		if (!people.has(u)) continue;
		clearTimeout(screenEnding.get(u));
		screenEnding.set(u, setTimeout(() => {
			screenEnding.delete(u);
			if (seen?.channelID === channelID && !seen.screens.has(u)) play('tela-acabou');
		}, SCREEN_END_MS));
	}
}

// The room this device left: nothing to compare the next one with.
export function roomForget(): void {
	seen = null;
	for (const t of screenEnding.values()) clearTimeout(t);
	screenEnding.clear();
	screenGoneAt.clear();
}

// ------------------------------------------------------------ this device

interface Own {
	state: string;
	muted: boolean;
	deafened: boolean;
	sharingScreen: boolean;
}

let own: Own | null = null;
let lostTimer: ReturnType<typeof setTimeout> | undefined;
// Coming back goes lost -> joining -> live: recovering says the joining is a
// return, not an arrival, and lostHeard whether the drop was long enough to
// have been heard.
let recovering = false;
let lostHeard = false;

// A connection that drops for a blink is not worth a sound; one that stays
// down this long is (owner, 29/10/2026: the leave sound, and join when back).
const LOST_AFTER_MS = 1200;

// ownChanged follows this device in its room: joining, leaving, the
// connection dropping and coming back, mute, deafen and its own screen.
//
// The join is heard at the click, not when the connection is ready (owner,
// 29/10/2026); the others hear it when this device shows up in their list.
export function ownChanged(now: Own): void {
	const before = own;
	own = { ...now };
	if (!before) return;
	if (before.state === 'off' && now.state !== 'off') {
		recovering = false;
		lostHeard = false;
		play('entrar');
	}
	if (now.state === 'live' && before.state !== 'live' && before.state !== 'off') {
		clearTimeout(lostTimer);
		// Back after a drop long enough to have been heard.
		if (recovering && lostHeard) play('entrar');
		recovering = false;
		lostHeard = false;
	}
	if (now.state === 'lost' && before.state === 'live') {
		recovering = true;
		clearTimeout(lostTimer);
		lostTimer = setTimeout(() => {
			if (own?.state !== 'live') {
				lostHeard = true;
				play('sair');
			}
		}, LOST_AFTER_MS);
	}
	if (now.state === 'off' && before.state !== 'off') {
		clearTimeout(lostTimer);
		// Leaving after the drop was already heard is not a second goodbye.
		if (!(recovering && lostHeard)) play('sair');
		recovering = false;
		lostHeard = false;
		roomForget();
	}
	// Its own screen, heard by whoever shares it too (owner, 29/10/2026).
	if (now.state !== 'off' && before.state !== 'off' && now.sharingScreen !== before.sharingScreen) {
		play(now.sharingScreen ? 'tela-comecou' : 'tela-acabou');
	}
	if (now.state === 'live' && before.state === 'live') {
		if (now.deafened !== before.deafened) play(now.deafened ? 'mutar' : 'desmutar');
		else if (now.muted !== before.muted) play(now.muted ? 'mutar' : 'desmutar');
	}
}
