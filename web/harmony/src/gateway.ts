// The live connection: one WebSocket that only ever receives.
//
// Nothing is sent over it. Every write in this app is an ordinary request, so
// the socket has no authorisation surface of its own — if it drops, the app
// keeps working and only stops updating by itself.
//
// It reconnects with a backoff, and on every open — the first one included —
// it tells the app to fetch again, because whatever happened while the socket
// was not open did not arrive on it.
//
// The first open used to be excluded, on the grounds that the app had just
// loaded everything itself. That is only true if the loading finished *after*
// the socket opened, and nothing makes it so: the two start together. Somebody
// who opened the app in the same half-second as somebody else spoke got a page
// of history taken before that line existed and no event to tell them about it,
// and the line was missing until they reloaded. One extra pair of small
// requests per visit is the price of the rule being simple enough to state:
// the page is in step only from the moment the socket is open.

import { log } from './diag';
import { watchNetwork } from './network';

export type GatewayEvent = { type: string; data: any };

const FIRST_DELAY = 500;
const MAX_DELAY = 5_000;

export class Gateway {
	private ws: WebSocket | null = null;
	private delay = FIRST_DELAY;
	private timer: number | undefined;
	private stopped = false;
	private unwatch?: () => void;

	constructor(
		private readonly onEvent: (e: GatewayEvent) => void,
		// Called every time the socket opens, the first time included. See the
		// note at the top for why the first one is not an exception.
		private readonly onResync: () => void,
		private readonly onState: (online: boolean) => void,
	) {}

	start(): void {
		this.unwatch?.();
		this.unwatch = watchNetwork((reason) => {
			log(`ws network ${reason}`);
			if (reason !== 'offline') this.reconnect();
			else this.onState(false);
		});
		this.stopped = false;
		this.open();
	}

	stop(): void {
		this.unwatch?.();
		this.unwatch = undefined;
		this.stopped = true;
		clearTimeout(this.timer);
		this.ws?.close();
		this.ws = null;
	}

	private open(): void {
		if (this.stopped) return;
		const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
		const ws = new WebSocket(`${scheme}://${location.host}/ws`);
		this.ws = ws;
		ws.onopen = () => {
			if (this.ws !== ws || this.stopped) return;
			log('ws aberto');
			this.delay = FIRST_DELAY;
			this.onState(true);
			this.onResync();
			window.dispatchEvent(new Event('harmony-gateway-open'));
		};
		ws.onmessage = (ev) => {
			if (this.ws !== ws || this.stopped) return;
			try {
				const parsed = JSON.parse(String(ev.data)) as GatewayEvent;
				log(`ws ${parsed.type}`);
				this.onEvent(parsed);
			} catch {
				log('ws mensagem ilegivel');
			}
		};
		ws.onclose = () => {
			if (this.ws !== ws) return;
			this.ws = null;
			this.onState(false);
			log(`ws fechado, tentando de novo em ${this.delay} ms`);
			this.schedule();
		};
		ws.onerror = () => {
			// onclose always follows; there is nothing useful to do twice.
		};
	}

	private schedule(): void {
		if (this.stopped) return;
		clearTimeout(this.timer);
		this.timer = setTimeout(() => this.open(), this.delay) as unknown as number;
		this.delay = Math.min(this.delay * 2, MAX_DELAY);
	}

	private reconnect(): void {
		if (this.stopped) return;
		clearTimeout(this.timer);
		const old = this.ws;
		this.ws = null;
		old?.close();
		this.delay = FIRST_DELAY;
		this.onState(false);
		log('ws reconnect immediately');
		this.open();
	}
}
