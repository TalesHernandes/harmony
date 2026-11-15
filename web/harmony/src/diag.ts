// The diagnostic log, as CLAUDE.md requires of every client and every new path.
//
// A fixed ring of one-line events with wall-clock stamps, in memory for the
// life of the page, shown behind the "Diagnóstico" panel and copyable in one
// press. Wall clock rather than relative, because the only reason to read one
// of these is to lay two devices' logs side by side.
//
// It never records what anybody wrote: message ids, channel ids, counts and
// sizes only. The same rule as the server's logs.

const CAPACITY = 400;

interface Entry {
	at: number;
	text: string;
}

const ring: Entry[] = new Array(CAPACITY);
let next = 0;
let count = 0;
const listeners = new Set<() => void>();

export function log(text: string): void {
	ring[next] = { at: Date.now(), text };
	next = (next + 1) % CAPACITY;
	if (count < CAPACITY) count++;
	for (const fn of listeners) fn();
}

export function subscribe(fn: () => void): () => void {
	listeners.add(fn);
	return () => {
		listeners.delete(fn);
	};
}

export function entries(): Entry[] {
	const out: Entry[] = [];
	const first = count < CAPACITY ? 0 : next;
	for (let i = 0; i < count; i++) out.push(ring[(first + i) % CAPACITY]);
	return out;
}

function stamp(at: number): string {
	const d = new Date(at);
	const pad = (n: number, w = 2) => String(n).padStart(w, '0');
	return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}`;
}

// ------------------------------------------------------------------ geometry
//
// Where the app thinks the screen ends, and where the box you type in actually
// is. This exists because of one bug and it is worth saying why: installed on a
// home screen, the owner's telephone drew the app taller than the glass and the
// composer fell off the bottom — and nothing visible on that screen could say
// whether it was missing, empty, or simply below the edge. These numbers tell
// those apart, and no guess is needed.
//
// Nothing here writes a style: it reads what the browser computed (the strips
// the system keeps arrive as #app's padding), which is the same rule the rest of
// the client follows.
export function geometry(): string {
	const view = window.visualViewport;
	const app = document.getElementById('app');
	const strips = app ? getComputedStyle(app) : null;
	const box = document.querySelector('[data-composer]')?.getBoundingClientRect();
	const bundle = (document.querySelector('script[type=module]') as HTMLScriptElement | null)?.src ?? '';
	return [
		`janela ${Math.round(window.innerWidth)}x${Math.round(window.innerHeight)}`,
		`doc ${document.documentElement.clientHeight}`,
		view ? `visivel ${Math.round(view.height)}` : 'visivel ?',
		app ? `app ${app.offsetHeight}` : 'app ?',
		strips ? `faixas ${strips.paddingTop}+${strips.paddingBottom}` : '',
		// What the browser *says* the bottom strip is, next to what we actually
		// paid for it. They differ on Firefox for Android in a tab, and that
		// difference was a dead band under the composer until D4c.
		`env-baixo ${getComputedStyle(document.documentElement).getPropertyValue('--safe-bottom').trim() || '?'}`,
		// The number that answers the question: bigger than the window means the
		// box exists and is past the bottom edge.
		box ? `escrever-ate ${Math.round(box.bottom)}` : 'escrever AUSENTE',
		matchMedia('(display-mode: standalone)').matches ? 'instalado' : 'aba',
		bundle ? `pacote ${bundle.split('/').pop()}` : '',
	]
		.filter(Boolean)
		.join(' ');
}

// The installed app and a Chrome tab on the same telephone are the same origin
// and share this storage. That is deliberate and it is the whole point: if the
// screen that copies this log is itself off the bottom of the installed app,
// the measurement can still be read from the tab, where the buttons work.
const GEO_KEY = 'harmony.geometry';

export function rememberGeometry(): void {
	const now = geometry();
	log(`medida ${now}`);
	try {
		localStorage.setItem(GEO_KEY, `${new Date().toLocaleTimeString('pt-BR')}  ${now}`);
	} catch {
		// A browser with storage off still has the line above, in memory.
	}
}

export function rememberedGeometry(): string {
	try {
		return localStorage.getItem(GEO_KEY) ?? '';
	} catch {
		return '';
	}
}

export function asText(): string {
	const head = [
		`harmony diag ${new Date().toISOString()}`,
		`ua ${navigator.userAgent}`,
		`url ${location.pathname}`,
		`agora ${geometry()}`,
		`ultima ${rememberedGeometry() || '(nenhuma)'}`,
		'',
	];
	return head.concat(entries().map((e) => `${stamp(e.at)}  ${e.text}`)).join('\n');
}

export function line(e: Entry): string {
	return `${stamp(e.at)}  ${e.text}`;
}

// The same text the "copiar" button produces, reachable from the browser's own
// console. It is how the browser tests read the log, and it is also the fastest
// way to get one out of somebody who is already looking at a console.
(window as unknown as { __harmonyDiag: () => string }).__harmonyDiag = asText;
