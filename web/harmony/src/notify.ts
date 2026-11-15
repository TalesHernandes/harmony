// Notificações com o app fechado, do lado do navegador.
//
// Three things are decided here, and they are the ones that make the difference
// between a feature that works and one that looks like it works.
//
// **When permission is asked.** Never on arrival. A browser that is asked for
// notification permission by a page the person has not used yet gets refused,
// and a refusal in Chrome and Firefox is *permanent* — there is no second
// prompt, only a padlock menu nobody knows about. So the prompt only ever comes
// from a press: the button in Ajustes, or the one line the app shows after
// somebody's first mention arrives while they are looking at it. That line can
// be dismissed and does not come back.
//
// **Where the registration lives.** The server hangs it on the login session
// (internal/harmony/store/push.go), so "este aparelho" is a real thing: logging
// this device out stops its notifications, and the phone is unaffected by what
// the computer does.
//
// **The iPhone.** Safari only allows notifications to a site that was added to
// the home screen, and only since iOS 16.4. In a normal Safari tab the
// `Notification` API does not exist at all, so there is nothing to ask for and
// nothing to turn on. The state below says so in words, and the settings screen
// prints the instructions, because a notification nobody knows how to switch on
// is the same as no notification.

import { api } from './api';
import { log } from './diag';

export type NotifyState =
	// This browser cannot do it at all (no service worker, no push).
	| 'unsupported'
	// iPhone or iPad in a normal Safari tab: add to the home screen first.
	| 'needs-home-screen'
	// The server has no VAPID keys: nobody on it can be notified.
	| 'server-off'
	// Never asked. This is where the button matters.
	| 'off'
	// Asked and refused, by this browser, for good. Only the padlock menu
	// undoes it.
	| 'denied'
	// Permission given but the server does not have this device: something went
	// wrong, or it was turned off here.
	| 'permitted'
	// Working.
	| 'on';

export interface NotifyStatus {
	state: NotifyState;
	// How many devices of this person the server would ring right now. It is
	// the honest answer to "está ligado?" — this browser can look right and the
	// registration still be missing.
	devices: number;
}

const isApple = /iPad|iPhone|iPod/.test(navigator.userAgent);

function standalone(): boolean {
	return (
		window.matchMedia?.('(display-mode: standalone)').matches === true ||
		(navigator as unknown as { standalone?: boolean }).standalone === true
	);
}

export function supported(): boolean {
	return 'serviceWorker' in navigator && 'PushManager' in window && 'Notification' in window;
}

// needsHomeScreen is the iPhone sentence: there is a way to switch this on, and
// it is not in this app.
export function needsHomeScreen(): boolean {
	return isApple && !standalone() && !('Notification' in window);
}

let registration: ServiceWorkerRegistration | null = null;

// register installs the service worker. It runs on every load, for everybody,
// signed in or not — registering a worker asks nothing of the person and shows
// nothing; it is what makes the app installable and what makes a later "ligar
// notificações" a one-press affair instead of two.
export async function register(): Promise<void> {
	if (!('serviceWorker' in navigator)) {
		log('sw nao suportado neste navegador');
		return;
	}
	try {
		registration = await navigator.serviceWorker.register('/sw.js');
		log('sw registrado');
	} catch (err) {
		log(`sw falhou: ${String(err)}`);
	}
}

async function ready(): Promise<ServiceWorkerRegistration | null> {
	if (registration) return registration;
	if (!('serviceWorker' in navigator)) return null;
	try {
		registration = await navigator.serviceWorker.ready;
		return registration;
	} catch {
		return null;
	}
}

async function endpointHere(): Promise<string> {
	const reg = await ready();
	if (!reg) return '';
	const sub = await reg.pushManager.getSubscription();
	return sub?.endpoint ?? '';
}

// status is what the settings screen draws. It asks the browser what it thinks
// and the server what it holds, because the interesting failure is exactly when
// the two disagree.
export async function status(): Promise<NotifyStatus> {
	if (needsHomeScreen()) return { state: 'needs-home-screen', devices: 0 };
	if (!supported()) return { state: 'unsupported', devices: 0 };
	let endpoint = '';
	try {
		endpoint = await endpointHere();
	} catch {
		// A browser that refuses to say is a browser with nothing registered.
	}
	const res = await api.pushStatus(endpoint);
	if (!res.enabled) return { state: 'server-off', devices: res.devices };
	if (Notification.permission === 'denied') return { state: 'denied', devices: res.devices };
	if (Notification.permission === 'default') return { state: 'off', devices: res.devices };
	return { state: res.subscribed ? 'on' : 'permitted', devices: res.devices };
}

// enable is the whole thing behind one press: ask, subscribe, tell the server.
// It must be called from a click — every browser requires the gesture, and the
// one that does not would still be rude.
export async function enable(): Promise<NotifyStatus> {
	if (needsHomeScreen()) return { state: 'needs-home-screen', devices: 0 };
	if (!supported()) return { state: 'unsupported', devices: 0 };

	const key = await api.pushKey();
	if (!key.enabled || !key.key) {
		log('push desligado no servidor');
		return { state: 'server-off', devices: 0 };
	}
	const permission = await Notification.requestPermission();
	log(`permissao de notificacao: ${permission}`);
	if (permission !== 'granted') return status();

	const reg = await ready();
	if (!reg) return { state: 'unsupported', devices: 0 };
	let sub = await reg.pushManager.getSubscription();
	if (!sub) {
		sub = await reg.pushManager.subscribe({
			// Not optional: every browser requires the promise that a push will
			// show something, and the service worker keeps it.
			userVisibleOnly: true,
			applicationServerKey: decodeKey(key.key),
		});
	}
	const json = sub.toJSON();
	const res = await api.pushSubscribe(sub.endpoint, {
		p256dh: json.keys?.p256dh ?? '',
		auth: json.keys?.auth ?? '',
	});
	log(`push ligado neste aparelho (${res.devices} no total)`);
	return { state: res.subscribed ? 'on' : 'permitted', devices: res.devices };
}

// disable turns this device off. The permission stays given — taking it back is
// the browser's business, not ours — but the server forgets the endpoint, so
// nothing rings here any more.
export async function disable(): Promise<NotifyStatus> {
	const reg = await ready();
	const sub = reg ? await reg.pushManager.getSubscription() : null;
	if (sub) {
		await api.pushUnsubscribe(sub.endpoint);
		await sub.unsubscribe().catch(() => undefined);
		log('push desligado neste aparelho');
	}
	return status();
}

// The one line the app may show by itself, and only after something happened
// that a notification would have been useful for. Dismissing it is final, and
// it is remembered per person on this device.
const NUDGE_KEY = 'harmony.notify.nudge';

export function nudgeDismissed(username: string): boolean {
	try {
		return localStorage.getItem(`${NUDGE_KEY}.${username}`) === '1';
	} catch {
		return false;
	}
}

export function dismissNudge(username: string): void {
	try {
		localStorage.setItem(`${NUDGE_KEY}.${username}`, '1');
	} catch {
		// A browser with storage switched off simply asks again next time.
	}
}

// base64url to the bytes pushManager.subscribe() wants. The explicit
// ArrayBuffer is not decoration: a plain Uint8Array is typed as possibly
// sitting on shared memory, which is not what a BufferSource may be.
function decodeKey(key: string): Uint8Array<ArrayBuffer> {
	const padded = key.replace(/-/g, '+').replace(/_/g, '/');
	const raw = atob(padded + '='.repeat((4 - (padded.length % 4)) % 4));
	const bytes = new Uint8Array(new ArrayBuffer(raw.length));
	for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
	return bytes;
}

// The sentence for each state, written for the owner's friends rather than for
// a browser vendor.
export function explain(s: NotifyState): string {
	switch (s) {
		case 'on':
			return 'Ligado neste aparelho. Menção, conversa privada e chamada tocam com o app fechado.';
		case 'permitted':
			return 'Você deu permissão, mas o servidor não tem o registro deste aparelho. Toque em "ligar" de novo.';
		case 'off':
			return 'Desligado. Ligue para ser avisado de menção, conversa privada e chamada com o app fechado.';
		case 'denied':
			return 'Este navegador bloqueou as notificações do harmony. Só dá para desbloquear no cadeado ao lado do endereço.';
		case 'needs-home-screen':
			return 'No iPhone, só funciona depois de adicionar o harmony à tela de início: toque em Compartilhar e em "Adicionar à Tela de Início". Depois abra por esse ícone e ligue aqui.';
		case 'server-off':
			return 'Este servidor está sem notificação configurada. O dono resolve isso na máquina.';
		case 'unsupported':
			return 'Este navegador não sabe avisar com o app fechado.';
	}
}

// ---------------------------------------------------------------- the sound

// The blip a new message makes, and the switch that turns it off.
//
// Three decisions, all of them small and all of them the kind that get noticed:
//
// It is **made**, not a file. Two notes out of the browser's own oscillator
// cost nothing to ship, cannot fail to load, and do not need a CSP hole — the
// same reasoning as the ring in DirectCall.tsx.
//
// It is **per device**, in this browser's own storage, because whether a sound
// is welcome is a fact about where you are sitting: at a desk with headphones,
// yes; in bed with the telephone on the pillow, no.
//
// And it **does not play for what you are looking at**. A blip for the message
// you are watching arrive, from the person you are talking to, is the noise
// that makes people turn sound off altogether — and then they miss the one that
// mattered. The rule is in App.tsx, where the message arrives.
const SOUND = 'harmony.sound';

export function soundOn(): boolean {
	try {
		return localStorage.getItem(SOUND) !== '0';
	} catch {
		return true;
	}
}

export function setSoundOn(on: boolean): void {
	try {
		localStorage.setItem(SOUND, on ? '1' : '0');
	} catch {
		// Storage switched off: this visit remembers, the next one does not.
	}
}

let blipCtx: AudioContext | null = null;
let lastBlip = 0;

// blip is two short notes, quiet. It refuses to play more than twice a second,
// so twenty messages arriving at once is a sound and not an alarm.
export function blip(): void {
	if (!soundOn()) return;
	const now = Date.now();
	if (now - lastBlip < 500) return;
	lastBlip = now;
	try {
		if (!blipCtx) blipCtx = new AudioContext();
		const ctx = blipCtx;
		// A page nobody has touched yet has no audio engine running (media.ts,
		// audioAwake). Nothing is lost by trying: it plays or it does not.
		void ctx.resume().catch(() => undefined);
		const at = ctx.currentTime;
		const gain = ctx.createGain();
		gain.gain.setValueAtTime(0.0001, at);
		gain.gain.exponentialRampToValueAtTime(0.08, at + 0.01);
		gain.gain.exponentialRampToValueAtTime(0.0001, at + 0.22);
		gain.connect(ctx.destination);
		const osc = ctx.createOscillator();
		osc.type = 'sine';
		osc.frequency.setValueAtTime(660, at);
		osc.frequency.setValueAtTime(880, at + 0.09);
		osc.connect(gain);
		osc.start(at);
		osc.stop(at + 0.24);
	} catch (err) {
		log(`sem som de mensagem: ${String(err)}`);
	}
}
