import { silentSource, watchPreferences } from './blocks';
// The direct call, browser to browser.
//
// Everything else that carries sound in this app goes through the server's own
// media server: a voice channel is a room, and a room of eight people needs
// somebody in the middle. A direct call is two people, and the audio, the
// camera and the shared screen go **straight from one browser to the other**.
// The server's part is the introduction — see internal/harmony/api/call.go —
// and it never sees a byte of what is said.
//
// The contract is docs/PROTOCOL.md, written for the 1:1 app that has been in
// the air since the first sprints of this project. What is implemented here is
// the W3C "perfect negotiation" pattern, and the reason it is not optional is
// in its name: when both browsers decide to renegotiate at the same instant —
// which happens the moment two people turn their cameras on together — one of
// them has to give way, or the negotiation deadlocks and the call is silent
// with both sides sure it is the other one's fault.
//
// Two rules from CLAUDE.md shape the whole file:
//
//   - **The PeerConnection is built once and never rebuilt.** Turning a camera
//     on, off, switching to a screen: all of it is replaceTrack on a sender
//     that already exists. Rebuilding would mean a new negotiation, new ICE and
//     a gap in the audio, every time somebody pressed a button.
//   - **Only the impolite side creates transceivers**, and it creates all four
//     of them up front, in a fixed order: microphone, camera, screen, the
//     screen's own sound. After that first negotiation nothing is ever added,
//     so nothing after it needs negotiating at all.
//
// The order of those four is the wire format between the two browsers. Both
// sides read it the same way: the transceiver's position in pc.getTransceivers()
// is the m-line order of the session description, on the side that created them
// and on the side that received them.

import { log } from './diag';
import { watchNetwork } from './network';
import { api, message } from './api';
import type { User } from './api';
import {
	CAMERA_BITRATE,
	SCREEN_DEGRADATION,
	audioAwake,
	onFirstTouch,
	onNoiseLevel,
	screenAudioConstraints,
	screenBitrate,
	screenConstraints,
	voice,
} from './media';
import { audioContext, makeSuppressor, type Suppressor } from './suppressor';
import { speakingSample } from './speaking';
import { play } from './sounds';

// The four slots, in the order the impolite side creates them.
export const SLOT_MIC = 0;
export const SLOT_CAM = 1;
export const SLOT_SCREEN = 2;
export const SLOT_SCREEN_AUDIO = 3;
const SLOTS = 4;

export type DirectState =
	// Nothing is happening. The only state in which "ligar" is drawn.
	| 'idle'
	// You pressed call and it is ringing over there.
	| 'calling'
	// Somebody is calling you and you have not decided yet.
	| 'ringing'
	// Somebody picked up and the two browsers are finding each other.
	| 'connecting'
	| 'live'
	| 'ended';

export interface DirectView {
	state: DirectState;
	id: string;
	peer: { username: string; display_name: string; avatar?: string } | null;
	// Whose turn it is to give way when both sides speak at once. The server
	// decides it and this side never guesses (PROTOCOL.md §5).
	polite: boolean;
	// What this device is sending.
	muted: boolean;
	// Not hearing the other side, as in a room: their voice and their screen's
	// sound stop here, and the microphone goes off with it (the owner,
	// 18/10/2026: the direct call had no "surdo").
	deafened: boolean;
	cam: boolean;
	screen: boolean;
	// What the other one says it is sending. It comes from them saying so, not
	// from what arrives, because a muted microphone that showed up only as
	// silence is indistinguishable from somebody who stopped talking.
	peerMuted: boolean;
	peerCam: boolean;
	peerScreen: boolean;
	// When the two were connected, for the clock on the screen.
	since: number;
	// Whether the call is out of the way. A call is not a modal: people look
	// something up, read a message, change a channel *while* talking, and the
	// first version of this covered the whole app until somebody hung up.
	small: boolean;
	// Something worth telling the person, in their own words.
	notice: string;
}

const EMPTY: DirectView = {
	state: 'idle',
	id: '',
	peer: null,
	polite: false,
	muted: false,
	deafened: false,
	cam: false,
	screen: false,
	peerMuted: false,
	peerCam: false,
	peerScreen: false,
	since: 0,
	small: false,
	notice: '',
};

// What one of the two browsers said about itself, as it arrives.
export interface CallSignal {
	id: string;
	kind: 'offer' | 'answer' | 'ice';
	payload: any;
}

class Direct {
	constructor() {
		watchPreferences(() => this.retunePersonal());
		watchNetwork((reason) => this.networkChanged(reason));
		window.addEventListener('harmony-gateway-open', () => {
			if (this.pc && this.restartAt) this.retrySignaling(this.pc, 0);
		});
	}

	private restartAt = 0;
	private restartLastAttempt = 0;
	private restartChecking = false;
	private restartCredential = '';
	private restartRetry = 0;
	private credential(pc: RTCPeerConnection): string {
		return pc.localDescription?.sdp.match(/a=ice-ufrag:([^\r\n]+)/)?.[1] ?? '';
	}
	private retrySignaling(pc: RTCPeerConnection, wait = 750): void {
		clearTimeout(this.restartRetry);
		this.restartRetry = window.setTimeout(() => this.serialise(async () => {
			if (pc !== this.pc || !this.restartAt || !this.live) return;
			try {
				if (pc.signalingState === 'have-local-offer' && pc.localDescription) {
					log('direct restart: replay pending offer after gateway recovery');
					await this.post('offer', pc.localDescription);
				} else if (pc.signalingState === 'stable') {
					if (this.credential(pc) !== this.restartCredential && ['connected', 'completed'].includes(pc.iceConnectionState)) {
						log(`direct ICE negotiation restored in ${Date.now() - this.restartAt} ms; same connection`);
						this.restartAt = 0;
						this.set({ notice: '' });
						return;
					}
					log('direct restart: negotiate again; gateway may have lost the offer');
					pc.restartIce();
				}
			} catch (err) {
				log(`direct restart signalling retry: ${String(err)}`);
			}
			if (this.restartAt && pc === this.pc) this.retrySignaling(pc);
		}), wait);
	}
	private networkChanged(reason: string): void {
		const pc = this.pc;
		if (!pc || !this.live) return;
		log(`chamada ICE restart requested: ${reason}`);
		if (!navigator.onLine || pc.iceConnectionState === 'disconnected' || pc.iceConnectionState === 'failed') {
			this.set({ notice: 'Reconectando…' });
		}
		if (!navigator.onLine) return;
		if (Date.now() - this.restartLastAttempt < 500) return;
		this.restartLastAttempt = Date.now();
		if (!this.restartAt) {
			this.restartAt = Date.now();
			this.restartChecking = false;
			this.restartCredential = this.credential(pc);
		}
		pc.restartIce(); // onnegotiationneeded keeps the perfect negotiation rules.
		this.retrySignaling(pc);
		if (pc.signalingState === 'have-local-offer' && pc.localDescription) {
			void this.post('offer', pc.localDescription).catch(() => {
				window.setTimeout(() => { if (pc === this.pc) this.networkChanged('retry'); }, 500);
			});
		}
	}
	private view: DirectView = EMPTY;
	private listeners = new Set<() => void>();

	private pc: RTCPeerConnection | null = null;
	// Building the connection is asynchronous — it waits for the microphone —
	// and two things ask for it: placing the call, and hearing that it was
	// answered. Keeping the *promise* rather than a flag makes the second one
	// wait for the first instead of starting a second connection beside it,
	// which is exactly what happened: the offer went out on one and the answer
	// came back to the other, and the call sat at "conectando…" for ever with
	// both browsers convinced they were doing the right thing.
	private opening: Promise<void> | null = null;
	// Which call the work in flight belongs to. finish() moves it on, and
	// everything that waited on something — the microphone, the camera, the
	// screen picker, which can stay open for as long as somebody likes —
	// checks it when the wait is over. Without it a call that ended during the
	// wait got its capture anyway: a microphone or a shared screen switched on
	// for a call that no longer existed, with nothing on the screen to turn it
	// off (Codex, 23/09/2026).
	private gen = 0;
	private ice: RTCIceServer[] = [];

	// The three flags of perfect negotiation, exactly as the W3C names them.
	private makingOffer = false;
	private isSettingRemoteAnswerPending = false;
	private ignoreOffer = false;

	// Signalling is not re-entrant: two descriptions arriving together have to
	// be applied one after the other, or the second is applied against a
	// connection the first has already moved.
	private queue: Promise<void> = Promise.resolve();
	// Anything that arrives before this side has built its connection. It is a
	// short list and usually empty; it exists because the network does not
	// promise that the answer to "I picked up" reaches us before the offer the
	// other side sent on hearing it.
	private early: CallSignal[] = [];
	// And anything this side wants to say before there is anybody to say it to.
	//
	// The connection is built the moment the call is placed, while it is still
	// ringing, because that is when there is time to spare: finding a path
	// between two machines is most of what makes a call take a few seconds to
	// start, and doing it after somebody has already picked up means they spend
	// those seconds looking at a silent screen. So the offer and the candidates
	// are made during the ring and held here — the server refuses to relay
	// anything before the call is answered, and it is right to, because until
	// then there is no second device to relay to.
	private outbox: { kind: string; payload: unknown }[] = [];
	// Whether there is somebody on the other end yet.
	private live = false;

	// What this device is sending, by slot. Kept so a track can be stopped when
	// the call ends and so that muting is a property of the track rather than a
	// message anybody has to be trusted to honour.
	private sending = new Map<number, MediaStreamTrack>();
	private micStream: MediaStream | null = null;
	private screenStream: MediaStream | null = null;
	private ctx: AudioContext | null = null;
	private micGraph: { source: MediaStreamAudioSourceNode; dest: MediaStreamAudioDestinationNode } | null =
		null;
	private clean: Suppressor | null = null;
	// Waiting for the first touch on the page, when the browser's audio engine
	// has not started yet. Null the rest of the time.
	private waking: (() => void) | null = null;

	// What arrives, by slot. These are the streams the screen draws, and they
	// are held here rather than in the framework's state: a redraw must never
	// be able to interrupt a picture (CLAUDE.md).
	private incoming = new Map<number, MediaStream>();
	// The elements the other side's sound comes out of, so that "surdo" can
	// silence them; and the microphone's state before it did.
	private sounds = new Set<HTMLMediaElement>();
	private soundSlots = new WeakMap<HTMLMediaElement, number>();
	private retunePersonal(): void { for (const el of this.sounds) el.muted = this.view.deafened || silentSource(this.view.peer?.username ?? '',this.soundSlots.get(el) === SLOT_MIC ? 'mic' : 'screen_audio'); }
	private mutedBeforeDeaf = false;
	// Who is talking, the room's way (speaking.ts): an analyser on what leaves
	// this side and one on what arrives, read every 25 ms while somebody looks.
	private talkWatchers = new Map<'me' | 'peer', Set<(on: boolean) => void>>();
	private talkMeters = new Map<'me' | 'peer', { id: string; node: AnalyserNode; source: MediaStreamAudioSourceNode }>();
	private talkLast = new Map<'me' | 'peer', number>();
	private talking = new Set<'me' | 'peer'>();
	private talkTimer = 0;
	private talkFrame = new Float32Array(512);
	// And what this device is showing of itself, for the little picture in the
	// corner: the same tracks that are being sent, wrapped for an element.
	private local = new Map<number, MediaStream>();

	snapshot(): DirectView {
		return this.view;
	}

	subscribe(fn: () => void): () => void {
		this.listeners.add(fn);
		return () => {
			this.listeners.delete(fn);
		};
	}

	private set(patch: Partial<DirectView>): void {
		this.view = { ...this.view, ...patch };
		for (const fn of this.listeners) fn();
	}

	// stream is what to show for one slot, or null when there is nothing.
	stream(slot: number): MediaStream | null {
		return this.incoming.get(slot) ?? null;
	}

	// mount puts a stream into an element. It is here and not in the component
	// for the reason above — and it presses play again, because an element that
	// is moved in the page is paused by the browser as it goes.
	mount(el: HTMLMediaElement | null, slot: number, mine = false): void {
		if (!el) return;
		const stream = (mine ? this.local : this.incoming).get(slot) ?? null;
		if (el.srcObject !== stream) el.srcObject = stream;
		if (!mine && (slot === SLOT_MIC || slot === SLOT_SCREEN_AUDIO)) {
			this.sounds.add(el);
			this.soundSlots.set(el,slot);
			this.retunePersonal();
		}
		if (stream) void el.play?.().catch(() => undefined);
	}

	// ------------------------------------------------------------ starting

	// ring is the button in a conversation: call this person.
	async ring(person: User): Promise<void> {
		if (this.view.state !== 'idle') return;
		this.set({
			state: 'calling',
			peer: { username: person.username, display_name: person.display_name, avatar: person.avatar },
			notice: '',
		});
		try {
			const res = await api.callStart(person.username);
			this.ice = res.ice_servers;
			this.set({ id: res.id, polite: res.polite });
			log(`chamada para @${person.username} tocando`);
			// Built now, not when they answer: see `outbox`. The microphone is
			// opened here too, which is also when a telephone lights its own
			// microphone — while the call is going out.
			await this.open(false);
		} catch (err) {
			this.set({ state: 'idle', peer: null, notice: message(err) });
		}
	}

	// called is somebody ringing this device.
	called(data: { id: string; from: { username: string; display_name: string } }): void {
		// Already busy: the server refuses a second call, so this is the rare
		// case of two calls crossing in flight. The newer one is let go.
		if (this.view.state !== 'idle') {
			void api.callHangup(data.id, 'hangup').catch(() => undefined);
			return;
		}
		this.set({ state: 'ringing', id: data.id, peer: data.from, notice: '' });
		log(`chamada de @${data.from.username}`);
	}

	// accept is picking up. The connection is built before anything is
	// answered, so that the offer the other side sends the instant it hears
	// about this has somewhere to land.
	async accept(): Promise<void> {
		if (this.view.state !== 'ringing') return;
		this.set({ state: 'connecting' });
		try {
			const res = await api.callAccept(this.view.id);
			this.ice = res.ice_servers;
			this.live = true;
			this.set({ polite: res.polite, peer: res.from });
			await this.open(true);
		} catch (err) {
			this.set({ notice: message(err) });
			this.finish();
		}
	}

	// accepted is the other side picking up, heard by the person who called.
	async accepted(peer: { username: string; display_name: string; avatar?: string }): Promise<void> {
		if (this.view.state !== 'calling') return;
		this.set({ state: 'connecting', peer });
		// Built during the ring; this is a no-op unless something went wrong
		// then. What is new is that there is finally somebody to talk to.
		await this.open(false);
		this.live = true;
		await this.flushOutbox();
	}

	// ------------------------------------------------------------ the call

	// open builds the one connection this call will ever have — once, however
	// many times it is asked for.
	private open(polite: boolean): Promise<void> {
		if (!this.opening) this.opening = this.build(polite);
		return this.opening;
	}

	private async build(polite: boolean): Promise<void> {
		if (this.pc) return;
		const gen = this.gen;
		const pc = new RTCPeerConnection({ iceServers: this.ice });
		(window as unknown as { __harmonyDirectPC: RTCPeerConnection }).__harmonyDirectPC = pc;

		// Only the impolite side creates them, and it creates all four at once.
		// Everything anybody does later — a camera, a screen — is a track
		// dropped into a slot that is already negotiated.
		if (!polite) {
			for (let i = 0; i < SLOTS; i++) {
				pc.addTransceiver(i === SLOT_MIC || i === SLOT_SCREEN_AUDIO ? 'audio' : 'video', {
					direction: 'sendrecv',
				});
			}
		}

		pc.onnegotiationneeded = () => {
			this.serialise(async () => {
				try {
					this.makingOffer = true;
					await pc.setLocalDescription();
					await this.post('offer', pc.localDescription);
				} catch (err) {
					log(`chamada oferta falhou: ${String(err)}`);
					if (this.restartAt) window.setTimeout(() => {
						if (pc === this.pc) this.networkChanged('offer retry');
					}, 500);
				} finally {
					this.makingOffer = false;
				}
			});
		};

		// Trickle: every candidate goes the moment it is found. Waiting for the
		// gathering to finish is how a call takes ten seconds to connect.
		pc.onicecandidate = ({ candidate }) => {
			if (!candidate) return;
			void this.post('ice', candidate).catch(() => undefined);
		};

		pc.ontrack = (ev) => {
			const slot = pc.getTransceivers().indexOf(ev.transceiver);
			if (slot < 0) return;
			this.incoming.set(slot, new MediaStream([ev.track]));
			// A track that ends is a camera switched off on the other side.
			ev.track.onended = () => {
				this.incoming.delete(slot);
				this.set({});
			};
			this.set({});
		};
		pc.oniceconnectionstatechange = () => {
			if (pc !== this.pc) return;
			log(`direct ICE ${pc.iceConnectionState}`);
			if (this.restartAt && ['checking', 'disconnected', 'failed'].includes(pc.iceConnectionState)) this.restartChecking = true;
			if (pc.iceConnectionState === 'disconnected' || pc.iceConnectionState === 'failed') {
				this.networkChanged(`ICE ${pc.iceConnectionState}`);
			} else if (this.restartAt && this.restartChecking && (pc.iceConnectionState === 'connected' || pc.iceConnectionState === 'completed')) {
				log(`chamada ICE connected in ${Date.now() - this.restartAt} ms; same connection`);
				this.restartAt = 0;
				this.set({ notice: '' });
			}
		};

		pc.onconnectionstatechange = () => {
			log(`chamada ${pc.connectionState}`);
			if (pc.connectionState === 'connected' && this.view.state !== 'live') {
				// Answered: the join sound, as a person arriving in a room.
				if (!this.view.since) play('entrar');
				this.set({ state: 'live', since: Date.now() });
			}
			if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected') {
				// One try at repairing the path before anybody is told. A
				// network that changed under a telephone looks exactly like
				// this, and it usually comes back.
				this.networkChanged(pc.connectionState);
			}
		};

		await this.openMic();
		if (gen !== this.gen) {
			// The call ended while the microphone was opening. finish() never
			// saw this connection, so it is closed here.
			pc.close();
			return;
		}
		// **Only now** is this the connection the app answers with, and the
		// order matters on the side that picked up: an offer applied while the
		// microphone is still opening would be answered with a description of a
		// call with nothing coming from this side, and fixing that afterwards
		// costs a whole second negotiation. Anything that arrives in the
		// meantime waits in `early`, three lines down.
		this.pc = pc;
		await this.flush();
		const early = this.early;
		this.early = [];
		for (const sig of early) this.signal(sig);
	}

	// post sends one signalling message, or keeps it until there is somebody to
	// send it to.
	private async post(kind: string, payload: unknown): Promise<void> {
		if (!this.live) {
			this.outbox.push({ kind, payload });
			return;
		}
		await api.callSignal(this.view.id, kind, payload);
	}

	// flushOutbox empties it, in order. Order is the whole reason it is a list:
	// the offer was made before the candidates that belong to it.
	private async flushOutbox(): Promise<void> {
		const waiting = this.outbox;
		this.outbox = [];
		for (const one of waiting) {
			try {
				await api.callSignal(this.view.id, one.kind, one.payload);
			} catch (err) {
				log(`chamada nao consegui mandar ${one.kind}: ${String(err)}`);
			}
		}
	}

	// serialise runs one signalling step at a time.
	private serialise(step: () => Promise<void>): void {
		this.queue = this.queue.then(step).catch((err) => {
			log(`chamada sinalizacao: ${String(err)}`);
		});
	}

	// signal is one message from the other browser. This is the perfect
	// negotiation pattern, and the comments are the reasons rather than the
	// steps — the steps are the W3C's.
	signal(sig: CallSignal): void {
		if (sig.id !== this.view.id) return;
		const pc = this.pc;
		if (!pc) {
			this.early.push(sig);
			return;
		}
		this.serialise(async () => {
			try {
				if (sig.kind === 'ice') {
					if (this.ignoreOffer) {
						log('direct candidate ignored during glare');
						return;
					}
					try {
						await pc.addIceCandidate(sig.payload);
					} catch (err) {
						// A candidate for an offer that was ignored belongs to a
						// negotiation that never happened. Anything else is real.
						if (!this.ignoreOffer) throw err;
					}
					return;
				}
				const description = sig.payload as RTCSessionDescriptionInit;
				// Glare: both sides offered at once. The impolite side pretends
				// it did not hear; the polite side rolls its own offer back and
				// answers. Somebody has to give way, and which one is decided by
				// the server, once, when the call is set up.
				const readyForOffer = !this.makingOffer && (pc.signalingState === 'stable' || this.isSettingRemoteAnswerPending);
				const collision = description.type === 'offer' && !readyForOffer;
				this.ignoreOffer = !this.view.polite && collision;
				if (this.ignoreOffer) {
					log('chamada oferta ignorada (glare)');
					return;
				}
				this.isSettingRemoteAnswerPending = description.type === 'answer';
				try {
					await pc.setRemoteDescription(description);
				} finally {
					this.isSettingRemoteAnswerPending = false;
				}
				if (description.type === 'offer') {
					// The offer is what brings the four slots to the side that
					// answered. Anything it has been waiting to send goes in
					// now, before the answer goes back, so the answer describes
					// a connection that already carries this person's voice.
					await this.flush();
					await pc.setLocalDescription();
					await this.post('answer', pc.localDescription);
				}
			} catch (err) {
				log(`chamada nao consegui aplicar ${sig.kind}: ${String(err)}`);
			}
		});
	}

	// state is the other side saying what it is doing.
	peerState(data: { mic: boolean; cam: boolean; screen: boolean }): void {
		this.set({ peerMuted: !data.mic, peerCam: data.cam, peerScreen: data.screen });
	}

	// ------------------------------------------------------------ what I send

	// openMic puts this person's voice into the call — their chosen microphone,
	// with their chosen noise suppression, the same as in a voice room.
	//
	// The order here is the whole point, and it is the opposite of the order this
	// file had until the D4i: **the plain microphone goes into the call first**,
	// and the suppressor is put in front of it afterwards, if it can be. What
	// "if it can be" is hiding is in suppress(), three methods down, and getting
	// this backwards is what made a telephone connect, hear the other person, and
	// send nothing at all.
	private async openMic(): Promise<void> {
		const chosen = voice.snapshot();
		const gen = this.gen;
		const raw = await this.rawMic(chosen);
		if (!raw) return;
		if (gen !== this.gen) {
			raw.stop();
			return;
		}
		// Off before it is sent, not after: muted (or deaf) is muted from the
		// first sample, not from the end of a wait.
		raw.enabled = !this.view.muted;
		await this.send(SLOT_MIC, raw);
		this.applyMute();
		// Not awaited: the filter is an improvement to a call that is already
		// carrying this person's voice, and making the connection wait for a wasm
		// file to arrive over 4G is the delay the D4h was about removing.
		if (chosen.micFilters.rnnoise) void this.suppress();
	}

	// rawMic is the microphone itself: the one this person chose, and — if that
	// one is not there any more — whichever one the device has.
	//
	// The second try is for telephones. A saved microphone id is a fact about a
	// browsing session as much as about a device, and an id that no longer
	// resolves comes back as a refusal shaped exactly like "this person said no
	// to the microphone": they said yes, and would have been silent for a reason
	// nobody on either end could see.
	private async rawMic(chosen: ReturnType<typeof voice.snapshot>): Promise<MediaStreamTrack | null> {
		const filters: MediaTrackConstraints = {
			echoCancellation: chosen.micFilters.echo,
			// The browser's own suppressor stands down when RNNoise is on, the same
			// as in a voice room: two of them in a row is one of them too many.
			noiseSuppression: chosen.micFilters.noise && !chosen.micFilters.rnnoise,
			autoGainControl: chosen.micFilters.agc,
		};
		const gen = this.gen;
		const open = async (exact: boolean): Promise<MediaStream> => {
			const stream = await navigator.mediaDevices.getUserMedia({
				audio: exact && chosen.micID ? { ...filters, deviceId: { exact: chosen.micID } } : filters,
			});
			// A permission prompt can outlast the call. What it hands back
			// then belongs to nobody, and is switched off at once.
			if (gen !== this.gen) {
				stream.getTracks().forEach((t) => t.stop());
				throw new Error('chamada encerrada');
			}
			return stream;
		};
		try {
			this.micStream = await open(true);
		} catch (err) {
			if (gen !== this.gen) return null;
			log(`chamada microfone escolhido nao abriu: ${String(err)}`);
			if (!chosen.micID) {
				this.set({ notice: 'Não consegui abrir o microfone.' });
				return null;
			}
			try {
				this.micStream = await open(false);
				log('chamada microfone: usando o do aparelho');
			} catch (err2) {
				if (gen !== this.gen) return null;
				this.set({ notice: 'Não consegui abrir o microfone.' });
				log(`chamada sem microfone: ${String(err2)}`);
				return null;
			}
		}
		return this.micStream.getAudioTracks()[0] ?? null;
	}

	// suppress puts the microphone through the noise suppressor — but only once
	// the browser's audio engine is actually running.
	//
	// This is the defect the owner found on 13/09/2026: his telephone joined a
	// call, heard the other person perfectly, and was silent. A browser does not
	// start its audio engine until the page has been touched; a graph built on a
	// stopped engine does not throw, it produces *silence*; and since the D4h the
	// connection is built **while the telephone is still ringing**, which is the
	// one moment in the life of this app when nobody has touched anything. So the
	// voice goes out raw and the filter catches up at the first touch — being
	// heard unfiltered beats being filtered and unheard.
	private async suppress(): Promise<void> {
		const stream = this.micStream;
		if (!stream || this.micGraph) return;
		// The call this belongs to is fixed before the first wait, together with
		// its microphone: hanging up and calling again during the wait must not
		// hand the new call a graph built on the old microphone.
		const gen = this.gen;
		try {
			if (!this.ctx) this.ctx = audioContext();
			const awake = await audioAwake(this.ctx);
			if (gen !== this.gen) return;
			if (!awake) {
				log('chamada: o audio do navegador nao comecou; voz vai crua ate o primeiro toque');
				this.suppressWhenAwake();
				return;
			}
			const clean = await makeSuppressor(this.ctx, voice.snapshot().noiseLevel);
			if (!clean) return;
			// Hung up while it was being made (24 MB the first time): let it go.
			if (gen !== this.gen || !this.ctx) {
				clean.destroy();
				return;
			}
			const source = this.ctx.createMediaStreamSource(stream);
			const dest = this.ctx.createMediaStreamDestination();
			source.connect(clean.node);
			clean.node.connect(dest);
			const track = dest.stream.getAudioTracks()[0];
			if (!track) {
				clean.destroy();
				return;
			}
			this.clean = clean;
			// The slider may have moved while it was being made.
			clean.setLevel(voice.snapshot().noiseLevel);
			onNoiseLevel((db) => this.clean?.setLevel(db));
			this.micGraph = { source, dest };
			track.enabled = !this.view.muted;
			await this.send(SLOT_MIC, track);
			this.applyMute();
			// The raw microphone may have gone out muted (openMic); it is no longer
			// what is sent, only what the filter reads, so it hears again.
			for (const raw of stream.getAudioTracks()) raw.enabled = true;
			log('chamada supressor de ruido ligado');
		} catch (err) {
			log(`chamada sem supressor: ${String(err)}`);
		}
	}

	// suppressWhenAwake waits for the first touch and tries again. Once.
	private suppressWhenAwake(): void {
		if (this.waking) return;
		this.waking = onFirstTouch(() => {
			this.waking = null;
			if (!this.pc) return;
			void this.suppress();
		});
	}

	// send says what this device wants to be sending in one slot, and puts it
	// there. This is the whole of "replaceTrack, never a new PeerConnection":
	// nothing here renegotiates.
	private async send(slot: number, track: MediaStreamTrack | null): Promise<void> {
		const old = this.sending.get(slot);
		// The plain microphone is the one thing never stopped here. When the
		// suppressor arrives it *replaces* the track in the call, and the graph it
		// just built is reading from that very microphone: stopping it would trade
		// a silence with a reason for a silence without one.
		const rawMic = this.micStream?.getAudioTracks()[0] ?? null;
		if (old && old !== track && old !== rawMic) old.stop();
		if (track) {
			this.sending.set(slot, track);
			if (track.kind === 'video') this.local.set(slot, new MediaStream([track]));
		} else {
			this.sending.delete(slot);
			this.local.delete(slot);
		}
		await this.attach(slot);
	}

	// attach puts what this device wants to send into the slot, **if the slot
	// exists yet**.
	//
	// On the side that placed the call it always does: that side created all
	// four before anything else happened. On the side that answered, it does
	// not — only the impolite side creates transceivers (CLAUDE.md), so the
	// person answering has no slots at all until the first offer arrives and
	// brings them. Their microphone is opened while they are still waiting, and
	// it used to be dropped on the floor for exactly that reason: the call
	// connected, and one of the two people was silent.
	private async attach(slot: number): Promise<void> {
		const tr = this.pc?.getTransceivers()[slot];
		if (!tr) return;
		const track = this.sending.get(slot) ?? null;
		// A slot that arrived with an offer is **receive-only**, by the standard:
		// a transceiver created while applying a remote description starts as
		// "recvonly", whatever the other side asked for. Putting a track in its
		// sender is then completely silent — the track is there, the connection
		// says "connected", and the other person hears nothing at all. It has to
		// be told it may send.
		if (track && tr.direction !== 'sendrecv') tr.direction = 'sendrecv';
		if (tr.sender.track === track) return;
		await tr.sender.replaceTrack(track);
		if (track && track.kind === 'video') await this.priceVideo(tr.sender, slot);
	}

	// flush is attach for every slot, run the moment the slots appear.
	private async flush(): Promise<void> {
		for (const slot of [SLOT_MIC, SLOT_CAM, SLOT_SCREEN, SLOT_SCREEN_AUDIO]) {
			await this.attach(slot);
		}
		this.applyMute();
	}

	// priceVideo puts a ceiling on one picture. The numbers are the owner's own
	// (media.ts), so a screen shared in a direct call costs what a screen
	// shared in a room costs — one choice, not one per kind of call.
	private async priceVideo(sender: RTCRtpSender, slot: number): Promise<void> {
		try {
			const params = sender.getParameters();
			if (!params.encodings || params.encodings.length === 0) params.encodings = [{}];
			const chosen = voice.snapshot();
			if (slot === SLOT_SCREEN) {
				// The same ceiling and for the same reason as media.ts: without
				// one the encoder takes libwebrtc's 2500 kbit/s default and the
				// estimate stops probing upward.
				params.encodings[0].maxBitrate = screenBitrate(chosen.screenQuality, chosen.screenFps);
				params.degradationPreference = SCREEN_DEGRADATION;
			} else {
				params.encodings[0].maxBitrate = CAMERA_BITRATE;
			}
			await sender.setParameters(params);
		} catch (err) {
			log(`chamada sem teto de banda: ${String(err)}`);
		}
	}

	// toggleMute is a property of the track and not a message: a microphone
	// that is off is off here, whatever the other side chooses to believe.
	toggleMute(): void {
		// Not hearing is also not speaking, as in a room: the microphone comes
		// back with the hearing, not on its own.
		if (this.view.deafened) return;
		this.set({ muted: !this.view.muted });
		if (this.view.since) play(this.view.muted ? 'mutar' : 'desmutar');
		this.applyMute();
		void this.tellPeer();
	}

	// toggleDeafen stops the other side's sound here and takes the microphone
	// with it; coming back gives the microphone back the way it was.
	toggleDeafen(): void {
		const deafened = !this.view.deafened;
		if (deafened) this.mutedBeforeDeaf = this.view.muted;
		this.set({ deafened, muted: deafened ? true : this.mutedBeforeDeaf });
		if (this.view.since) play(deafened ? 'mutar' : 'desmutar');
		this.retunePersonal();
		this.applyMute();
		log(`chamada direta surdo=${deafened}`);
		void this.tellPeer();
	}

	// watchSpeaking reports whether this side ('me') or the other ('peer') is
	// talking, for the green ring. It hands back the way to stop watching.
	watchSpeaking(who: 'me' | 'peer', fn: (on: boolean) => void): () => void {
		const set = this.talkWatchers.get(who) ?? new Set();
		set.add(fn);
		this.talkWatchers.set(who, set);
		fn(this.talking.has(who));
		if (!this.talkTimer) this.talkTimer = window.setInterval(() => this.readTalk(), 25);
		return () => {
			const live = this.talkWatchers.get(who);
			live?.delete(fn);
			if (live && live.size === 0) this.talkWatchers.delete(who);
			if (this.talkWatchers.size === 0) this.stopTalk();
		};
	}

	private talkMeter(who: 'me' | 'peer'): AnalyserNode | null {
		const track = who === 'me' ? this.sending.get(SLOT_MIC) : this.incoming.get(SLOT_MIC)?.getAudioTracks()[0];
		const had = this.talkMeters.get(who);
		if (!track || track.readyState === 'ended') {
			if (had) {
				had.source.disconnect();
				this.talkMeters.delete(who);
			}
			return null;
		}
		if (had?.id === track.id) return had.node;
		had?.source.disconnect();
		try {
			const ctx = audioContext();
			const source = ctx.createMediaStreamSource(new MediaStream([track]));
			const node = ctx.createAnalyser();
			node.fftSize = 512;
			source.connect(node);
			this.talkMeters.set(who, { id: track.id, node, source });
			return node;
		} catch {
			return null;
		}
	}

	private readTalk(): void {
		const at = Date.now();
		for (const who of ['me', 'peer'] as const) {
			// Muted is not quiet (media.ts readMeters): a microphone that is off
			// does not light up, whatever it hears.
			const silent = who === 'me' ? this.view.muted || this.view.deafened : this.view.peerMuted;
			const node = silent ? null : this.talkMeter(who);
			let level = 0;
			if (node) {
				node.getFloatTimeDomainData(this.talkFrame);
				let sum = 0;
				for (const d of this.talkFrame) sum += d * d;
				level = Math.sqrt(sum / this.talkFrame.length);
			}
			const last = speakingSample(this.talkLast.get(who), level, at);
			if (last === undefined) this.talkLast.delete(who);
			else this.talkLast.set(who, last);
			const on = last !== undefined;
			if (on !== this.talking.has(who)) {
				if (on) this.talking.add(who);
				else this.talking.delete(who);
				for (const fn of this.talkWatchers.get(who) ?? []) fn(on);
			}
		}
	}

	private stopTalk(): void {
		clearInterval(this.talkTimer);
		this.talkTimer = 0;
		for (const m of this.talkMeters.values()) m.source.disconnect();
		this.talkMeters.clear();
		this.talkLast.clear();
		for (const who of this.talking) for (const fn of this.talkWatchers.get(who) ?? []) fn(false);
		this.talking.clear();
	}

	private applyMute(): void {
		const track = this.sending.get(SLOT_MIC);
		if (track) track.enabled = !this.view.muted;
	}

	async toggleCam(): Promise<void> {
		if (this.view.cam) {
			await this.send(SLOT_CAM, null);
			this.set({ cam: false });
			void this.tellPeer();
			return;
		}
		try {
			const chosen = voice.snapshot();
			const gen = this.gen;
			const stream = await navigator.mediaDevices.getUserMedia({
				video: chosen.cameraID ? { deviceId: { exact: chosen.cameraID } } : true,
			});
			if (gen !== this.gen || !this.pc) {
				stream.getTracks().forEach((t) => t.stop());
				return;
			}
			const track = stream.getVideoTracks()[0];
			if (!track) return;
			track.contentHint = 'motion';
			await this.send(SLOT_CAM, track);
			this.set({ cam: true });
			void this.tellPeer();
		} catch (err) {
			log(`chamada sem camera: ${String(err)}`);
			this.set({ notice: 'Não consegui abrir a câmera.' });
		}
	}

	// startScreen shares a window, with its sound when the browser offers it.
	//
	// It needs no negotiation at all, and that is the payoff of creating the
	// four slots at the start: pressing this in the middle of a sentence puts a
	// picture into a place that was made for it when the call began.
	async startScreen(): Promise<void> {
		if (this.view.screen || !this.pc) return;
		const chosen = voice.snapshot();
		const gen = this.gen;
		let stream: MediaStream;
		try {
			stream = await navigator.mediaDevices.getDisplayMedia({
				video: screenConstraints(chosen.screenQuality, chosen.screenFps),
				audio: screenAudioConstraints(),
			});
		} catch (err) {
			// Pressing cancel in the picker lands here, and it is not an error.
			log(`chamada tela cancelada: ${String(err)}`);
			return;
		}
		const video = stream.getVideoTracks()[0];
		// The picker stays open for as long as somebody takes to choose, and
		// the other person may hang up meanwhile.
		if (!video || gen !== this.gen || !this.pc) {
			stream.getTracks().forEach((t) => t.stop());
			return;
		}
		// The one word that tells the encoder what it is looking at, and worth
		// more than any bitrate: spend the bits on movement rather than on
		// keeping a still picture sharp. Same choice, and the same reason, as
		// the group call — see SCREEN_DEGRADATION in media.ts.
		video.contentHint = 'motion';
		// The browser's own "stop sharing" bar is a way out this app never sees
		// a click on, so it is listened for.
		video.onended = () => void this.stopScreen();
		this.screenStream = stream;
		await this.send(SLOT_SCREEN, video);
		const audio = stream.getAudioTracks()[0];
		if (audio) await this.send(SLOT_SCREEN_AUDIO, audio);
		this.set({ screen: true });
		void this.tellPeer();
	}

	async stopScreen(): Promise<void> {
		if (!this.view.screen) return;
		await this.send(SLOT_SCREEN, null);
		await this.send(SLOT_SCREEN_AUDIO, null);
		this.screenStream?.getTracks().forEach((t) => t.stop());
		this.screenStream = null;
		this.set({ screen: false });
		void this.tellPeer();
	}

	private async tellPeer(): Promise<void> {
		if (!this.view.id || this.view.state === 'idle') return;
		try {
			await api.callState(this.view.id, !this.view.muted, this.view.cam, this.view.screen);
		} catch {
			// The other side keeps the last thing it heard. Nothing about the
			// call itself depends on this arriving.
		}
	}

	// ------------------------------------------------------------ ending

	// hangup is this side putting the telephone down, at any point: refusing a
	// call, giving up on one that is ringing, or ending one an hour in.
	async hangup(reason: 'hangup' | 'rejected' | 'missed' = 'hangup'): Promise<void> {
		const id = this.view.id;
		// Nobody answered: the "it did not happen" sound. Putting down a call
		// that was on: the leave sound. Refusing one makes none.
		if (reason === 'missed') play('tela-acabou');
		else if (reason === 'hangup' && this.view.state === 'live') play('sair');
		this.finish();
		if (!id) return;
		try {
			await api.callHangup(id, reason);
		} catch {
			// Already gone, or the network went first. The other side is told
			// by the server either way, and this side is already finished.
		}
	}

	// ended is the other side hanging up, or the server saying the call is over.
	ended(id: string, reason: string): void {
		if (id !== this.view.id) return;
		log(`chamada encerrada: ${reason}`);
		// Refused or not answered over there: "it did not happen". The other
		// person hanging up a call that was on: the leave sound. A call that
		// stops ringing here (they gave up) just goes quiet.
		if (this.view.state !== 'ringing') {
			if (reason === 'rejected' || reason === 'missed') play('tela-acabou');
			else play('sair');
		}
		this.finish(reason === 'rejected' ? 'Chamada recusada.' : '');
	}

	// taken is another of this person's own devices answering. This one stops
	// ringing and says nothing else: it was never in the call.
	taken(id: string): void {
		if (id !== this.view.id || this.view.state !== 'ringing') return;
		this.finish('Atendida em outro aparelho.');
	}

	// finish tears everything down. It is the only place that does, so there is
	// one answer to "what is left open after a call" and it is "nothing".
	private finish(notice = ''): void {
		clearTimeout(this.restartRetry);
		this.restartAt = 0;
		this.restartLastAttempt = 0;
		this.restartChecking = false;
		this.gen++;
		for (const track of this.sending.values()) track.stop();
		this.sending.clear();
		this.incoming.clear();
		this.local.clear();
		this.micStream?.getTracks().forEach((t) => t.stop());
		this.micStream = null;
		this.screenStream?.getTracks().forEach((t) => t.stop());
		this.screenStream = null;
		try {
			this.clean?.destroy();
		} catch {
			// A node from a context that is already closed.
		}
		this.clean = null;
		onNoiseLevel(null);
		this.micGraph = null;
		this.waking?.();
		this.waking = null;
		if (this.pc) {
			this.pc.onnegotiationneeded = null;
			this.pc.onicecandidate = null;
			this.pc.ontrack = null;
			this.pc.onconnectionstatechange = null;
			this.pc.close();
		}
		this.pc = null;
		this.opening = null;
		this.early = [];
		this.outbox = [];
		this.live = false;
		this.makingOffer = false;
		this.ignoreOffer = false;
		for (const el of this.sounds) el.muted = false;
		this.sounds.clear();
		for (const m of this.talkMeters.values()) m.source.disconnect();
		this.talkMeters.clear();
		this.talkLast.clear();
		for (const who of this.talking) for (const fn of this.talkWatchers.get(who) ?? []) fn(false);
		this.talking.clear();
		this.view = { ...EMPTY, notice };
		for (const fn of this.listeners) fn();
	}

	// shrink puts the call out of the way, or brings it back. Nothing about the
	// connection changes — the audio elements are mounted by the part that is
	// always drawn, so this is only about what is on the screen.
	shrink(small: boolean): void {
		this.set({ small });
	}

	// dismiss clears a message the person has read.
	clearNotice(): void {
		if (this.view.notice) this.set({ notice: '' });
	}
}

// One call at a time, per page. Being on two at once is not a thing this
// service does — the server refuses it as well — and a singleton is what makes
// that true rather than hoped for.
export const direct = new Direct();

(window as unknown as { __harmonyDirect: Direct }).__harmonyDirect = direct;

// A tab closing in the middle of a call should not leave the other person
// listening to nothing. Best effort: a browser may not run it, and the server's
// own sweep is the guarantee.
window.addEventListener('pagehide', () => {
	const id = direct.snapshot().id;
	if (!id) return;
	navigator.sendBeacon?.('/api/calls/' + encodeURIComponent(id) + '/hangup');
});
