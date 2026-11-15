// "Estou aqui" / "sai um pouco", do lado do navegador.
//
// The server does not guess (internal/harmony/api/presence.go). This file is
// what it believes, and it reports one of three things about this tab:
//
//   - `active`: it is on the screen, with somebody using it.
//   - `hidden`: it is not on the screen. A phone with its screen off, a phone
//     whose owner switched to another app, a computer where the browser was
//     minimised or another window came to the front. The browser knows all four
//     for certain, which is why this is reported rather than inferred.
//   - `away`: five minutes with the tab open and nothing touched. A window left
//     open on a second monitor is not somebody who is there, and a friend who
//     sees "online" and gets no answer stops trusting the dot.
//
// `hidden` used to be reported as `away`, and on 13/09/2026 the owner asked for
// the difference: "ausente no pc tem que ser 5 minutos fora da aba". Flicking
// to another window for a moment is not leaving. The five minutes are counted
// **by the server**, because a telephone freezes its page the instant the
// screen goes off and cannot report later that time has passed.
//
// And being in a call is being in the app, whatever window is on top — his
// second sentence, the same evening. That is reported *alongside* the window
// rather than instead of it, because the two answer different questions: the
// window decides whether a notification is worth pushing to this person, and a
// telephone in a pocket with a call running is still a telephone that should
// buzz when somebody writes.
//
// Anything but `active` is sent with sendBeacon, not fetch: a phone being
// locked freezes the page within moments, and a beacon is the one request the
// browser promises to finish anyway. Coming back is an ordinary request,
// because there is by definition a page running to make it.

import { log } from './diag';

const IDLE_AFTER = 5 * 60_000;

// The events that count as "somebody is there". Movement is not among them on
// purpose: a mouse crossing the window on the way somewhere else is not use.
const TOUCHES = ['keydown', 'pointerdown', 'wheel', 'focus'] as const;

type Reported = 'active' | 'hidden' | 'away';

export class Presence {
	// What the server already believes, which on arrival is "online": a socket
	// that just opened is somebody who is here, and saying so again would be a
	// request per page load that tells nobody anything. Only changes are sent.
	private reported: Reported = 'active';
	// busy is "the app is in use for a reason that has nothing to do with which
	// window is on top". There is exactly one such reason and it is a call.
	private busy = false;
	private timer: number | undefined;
	private stopped = false;
	private readonly onActivity = () => this.touched();
	private readonly onVisibility = () => {
		if (document.visibilityState === 'hidden') this.hidden();
		else this.touched();
	};

	start(): void {
		this.stopped = false;
		for (const name of TOUCHES) window.addEventListener(name, this.onActivity, { passive: true });
		document.addEventListener('visibilitychange', this.onVisibility);
		// The state on arrival is whatever the tab is: a page loaded in a
		// background tab is not somebody looking at it.
		if (document.visibilityState === 'hidden') this.hidden();
		else this.touched();
	}

	// setBusy is a call starting or ending. It changes nothing about what this
	// says of the window; it is the second half of the sentence.
	setBusy(busy: boolean): void {
		if (this.stopped || this.busy === busy) return;
		this.busy = busy;
		this.send();
	}

	stop(): void {
		this.stopped = true;
		clearTimeout(this.timer);
		for (const name of TOUCHES) window.removeEventListener(name, this.onActivity);
		document.removeEventListener('visibilitychange', this.onVisibility);
	}

	private touched(): void {
		if (this.stopped) return;
		this.report('active');
		clearTimeout(this.timer);
		this.timer = setTimeout(() => this.report('away'), IDLE_AFTER) as unknown as number;
	}

	// hidden is the tab going behind something. The countdown starts again from
	// here — five minutes *out of the tab*, not five minutes since the last key
	// pressed before leaving it. The server counts the same five minutes from
	// the same fact, and is the one that gets the answer right when this page is
	// frozen before it can finish counting.
	private hidden(): void {
		if (this.stopped) return;
		this.report('hidden');
		clearTimeout(this.timer);
		this.timer = setTimeout(() => this.report('away'), IDLE_AFTER) as unknown as number;
	}

	// report tells the server, and only when the answer changed: an evening of
	// typing is one request, not one per key.
	private report(state: Reported): void {
		if (this.stopped || this.reported === state) return;
		this.reported = state;
		this.send();
	}

	private send(): void {
		if (this.stopped) return;
		const state = this.reported;
		log(`presenca: ${state}${this.busy ? ' (em chamada)' : ''}`);
		const body = JSON.stringify({ state, busy: this.busy });
		if (state !== 'active' && navigator.sendBeacon) {
			// A Blob with the JSON type, so the request looks exactly like the
			// ordinary one and the server's content-type check is not a special
			// case for this path.
			const ok = navigator.sendBeacon('/api/presence', new Blob([body], { type: 'application/json' }));
			if (ok) return;
		}
		void fetch('/api/presence', {
			method: 'POST',
			credentials: 'same-origin',
			headers: { 'Content-Type': 'application/json' },
			body,
			keepalive: true,
		}).catch(() => undefined);
	}
}

// The dot next to a name. "em chamada" is not a presence state on the server:
// it is derived here, from the voice rooms this person already receives, which
// is why the server never has to answer "is fulano in a call" about a room the
// asker cannot see.
export type PersonState = 'online' | 'away' | 'offline' | 'in_call';

// Green for here, amber for stepped away, and a dot with the light off for
// gone. Green is shared with "somebody is talking right now" on purpose and by
// the owner's decision (docs/PLANO-DESIGN.md §7.1): what separates the two is
// never the colour, it is the shape. Presence is always a dot. Talking is
// always a ring around the face. Where both appear on one person - a name
// listed under a voice room - the shape is what settles it.
export function dotClass(state: PersonState): string {
	switch (state) {
		// In a call is a kind of online, and the handset beside the name is
		// what says the rest.
		case 'in_call':
			return 'bg-success';
		case 'online':
			return 'bg-success';
		case 'away':
			return 'bg-warning';
		default:
			return 'bg-foreground-muted/40';
	}
}

export function stateLabel(state: PersonState): string {
	switch (state) {
		case 'in_call':
			return 'Em chamada';
		case 'online':
			return 'Online';
		case 'away':
			return 'Ausente';
		default:
			return 'Offline';
	}
}
