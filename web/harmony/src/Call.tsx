import { isMuted } from './blocks';
import { ScreenButton } from './CallKit';
// The call, as it is drawn: one big picture when somebody is showing one, and
// everybody in the room as a tile — a camera if they have one on, a circle with
// their initials if they do not.
//
// Not one line of this file holds a stream. The `<video>` elements are made in
// media.ts and live outside the app's tree; a tile here is an empty box that
// asks for the element by key and puts it inside itself. Redrawing the wall —
// somebody joins, somebody mutes, the layout changes — moves the element and
// never rebuilds it, so no picture ever blinks. It is the same rule that keeps
// `MediaStream` out of the framework's state (CLAUDE.md).
//
// Nothing here scrolls. The call fills the space it is given and the pictures
// shrink to fit, because a call that pushes the page taller than the window is
// a call you have to hunt for.
//
// There is no filter, no backdrop-filter and no shadow anywhere near a video,
// and no frame ever passes through a canvas. Names sit on the picture in solid
// chips — never bare text on a frame, since there is no blur to lift it off.

import { FaceFill } from './Avatar';
import type { ComponentChildren } from 'preact';
import { useEffect, useLayoutEffect, useRef, useState } from 'preact/hooks';
import type { User, VoicePeer } from './api';
import { voice, SCREEN, SCREEN_AUDIO, CAMERA, type Tile, type TileStats, type VoiceView } from './media';
import { Slider } from './Volume';
import { log } from './diag';
import { nameOf } from './names';
import { Icon } from './Icon';
import { place } from './drag';
import { BAR, CHIP, CHIP_BIG, Deafened, IDLE, LEAVE, MUTED, OFF, ON, Quality } from './CallKit';

// initials takes a person's name and gives the two letters that go in the
// circle. Anything that is not a letter or a number is not a name — the owner
// saw himself drawn as "L(" because the label under the picture says
// "Fulano (você)" and the second word starts with a bracket.
//
// Two guards, and both are needed. This one throws away the pieces that are not
// words; the caller passes the plain name rather than the decorated label,
// which is the actual fix.
export function initials(name: string): string {
	const parts = name
		.trim()
		.split(/\s+/)
		.filter((p) => /^[\p{L}\p{N}]/u.test(p));
	const first = parts[0]?.[0] ?? '?';
	const second = parts.length > 1 ? parts[parts.length - 1][0] : '';
	return (first + second).toUpperCase();
}

// Picture is the empty box a video goes into. The effect moves the element in
// when the box appears and parks it back out when the box goes away.
function Picture({ tileKey }: { tileKey: string }) {
	const hold = useRef<HTMLDivElement | null>(null);
	useEffect(() => {
		voice.mount(tileKey, hold.current);
		return () => voice.mount(tileKey, null);
	}, [tileKey]);
	return <div ref={hold} class="absolute inset-0" />;
}

// Numbers is the once-a-second readout, in a component of its own so that the
// thing redrawing every second is nowhere near the thing holding a picture
// (CLAUDE.md). It is also how "está legível no celular?" gets an answer made of
// numbers instead of an opinion.
//
// quiet writes the log line and draws nothing. A screen in the grid is not in
// focus and has no readout, but the log still has to describe it: until
// 24/09/2026 a screen was always the big picture, so the line was always
// written, and the grid quietly stopped it for whoever was showing.
//
// wrap is the chip it sits in. The readout draws it itself, and only once
// there is a number: a chip drawn around nothing was "um quadrado a mais preto
// que não tem nada nele" (the owner, 24/09/2026).
function Numbers({ tileKey, quiet = false, wrap }: { tileKey: string; quiet?: boolean; wrap?: string }) {
	const [stats, setStats] = useState<TileStats | null>(null);
	useEffect(() => {
		let alive = true;
		let ticks = 0;
		const timer = setInterval(() => {
			void voice.stats().then((all) => {
				if (!alive) return;
				const mine = all.find((s) => s.key === tileKey) ?? null;
				setStats(mine);
				if (mine && ++ticks % 10 === 0) {
					// Everything that describes this picture, in one line, so
						// that "ficou ruim" can be answered from the log alone.
						log(
							`video ${tileKey}: ${mine.width}x${mine.height} ${mine.fps} fps ` +
								`${mine.kbps} kbit/s ${mine.codec} ` +
								(mine.lost < 0 ? 'perda=SEM-RELATORIO ' : `perda=${mine.lost}% `) +
								`travadas=${mine.freezes}` +
								(mine.mine ? '' : `(${mine.freezeSeconds}s)`) +
								` chaves=${mine.keyframes}` +
									(mine.mine
										? ` pedidos=${mine.restarts}` +
											// A diferença é a pergunta inteira: uma imagem
											// completa que ninguém pediu foi decisão do
											// codificador, e não nossa. Impressa pronta
											// porque foi ela que eu não soube ler no log de
											// 20/09/2026, e subtrair de cabeça duas colunas
											// que rebaseavam é como eu errei.
											(mine.keyframes > mine.restarts
												? `(${mine.keyframes - mine.restarts} sozinhas)`
												: '') +
											` captura=${mine.sourceFps}fps` +
											` piso=${mine.sourceFpsMin}fps` +
											(mine.layers > 1 ? ` camadas=${mine.layers}` : '') +
											(mine.encoder ? ` codificador=${mine.encoder}` : '')
										: ` espera=${mine.holdMs}ms descartadas=${mine.dropped}`) +
									(mine.limit ? ' limite=' + mine.limit : ''),
						);
				}
			});
		}, 1000);
		return () => {
			alive = false;
			clearInterval(timer);
		};
	}, [tileKey]);
	if (quiet || !stats || !stats.width) return null;
	// Grey numbers, and the ones that mean "working and getting worse" in
	// warning — they were danger, which is for things that have failed.
	const worse = 'text-warning-soft-fg';
	const line = (
		<span class="truncate text-foreground-muted tabular-nums">
			{stats.width}×{stats.height} · {stats.fps} fps · {(stats.kbps / 1000).toFixed(1)} Mbit/s
			{stats.codec ? ' · ' + stats.codec : ''}
			{/* Os megabits acima são a soma das três camadas, e dizer quantas
			    são é o que impede a leitura errada de "está subindo o triplo do
			    que eu escolhi". */}
			{stats.layers > 1 && <span> · {stats.layers} camadas</span>}
			{stats.lost >= 2 && <span class={worse}> · {stats.lost}% perdido</span>}
			{stats.freezes > 0 && <span class={worse}> · travou {stats.freezes}×</span>}
			{stats.limit === 'bandwidth' && <span class={worse}> · conexão segurando</span>}
			{stats.limit === 'cpu' && <span class={worse}> · máquina segurando</span>}
		</span>
	);
	return wrap ? <span class={wrap}>{line}</span> : line;
}

// Warning is the sentence that turns "a imagem fica péssima" into something a
// person can act on. It only appears when this device is measurably sending
// less than it was asked for, and it says which of the two things is to blame,
// because the answer to one is a smaller setting and the answer to the other is
// not.
//
// It is deliberately slow to appear. Every share starts held back: the browser
// does not know what the line can carry and finds out by trying, which takes the
// better part of ten seconds. Warning about that would be warning about nothing,
// every single time.
function Warning({ sharing }: { sharing: boolean }) {
	const [why, setWhy] = useState<{ limit: string; width: number; height: number } | null>(null);
	useEffect(() => {
		if (!sharing) {
			setWhy(null);
			return;
		}
		let alive = true;
		let bad = 0;
		const timer = setInterval(() => {
			void voice.stats().then((all) => {
				if (!alive) return;
				const worst = all.find((s) => s.mine && s.limit);
				if (worst) {
					if (++bad >= 10) setWhy({ limit: worst.limit, width: worst.width, height: worst.height });
				} else {
					bad = 0;
					setWhy(null);
				}
			});
		}, 1000);
		return () => {
			alive = false;
			clearInterval(timer);
		};
	}, [sharing]);
	if (!why) return null;
	// The design's warning band (Componentes §11): warning at 12% behind, the
	// sentence in the ordinary colour. It is "works and is getting worse" —
	// the picture is still going out — so it is not danger.
	return (
		<p class="flex shrink-0 items-center gap-3 rounded-control bg-warning-soft px-3 py-2 text-meta text-foreground">
			<span class="shrink-0 text-warning-soft-fg">
				<Icon name="display" />
			</span>
			<span class="min-w-0">
				{why.limit === 'cpu'
					? `Esta máquina não está dando conta de codificar: indo a ${why.width}×${why.height}. Diminua os quadros por segundo em ajustes da chamada.`
					: `Sua conexão não está dando conta: a imagem está indo a ${why.width}×${why.height}. Escolha uma qualidade menor em ajustes da chamada.`}
			</span>
		</p>
	);
}

// How long the fullscreen controls stay up after the last sign of life. The
// same idea as every video player: out of the way while you watch, back the
// moment you move.
const IDLE_MS = 2500;

// FullscreenControls is the way out, and the reason a box is what goes
// fullscreen rather than the bare video element: a <video> alone in
// fullscreen has only the browser's own controls, and those do not know
// about the call.
//
// It appears on any sign of a person — a mouse moving, a finger touching, a key
// — and fades after a moment of nothing. Fading is opacity, which is the only
// thing allowed to animate over a picture.
function FullscreenControls({
	label,
	tileKey,
	controls,
	flip = false,
	liveUsername,
	onExit,
}: {
	label: string;
	tileKey: string;
	controls?: ComponentChildren;
	// The floating cameras are in the top right, where the way out is: the
	// row turns round and the ✕ goes to the left.
	flip?: boolean;
	liveUsername?: string;
	onExit: () => void;
}) {
	const [shown, setShown] = useState(true);
	useEffect(() => {
		let timer = 0;
		const wake = () => {
			setShown(true);
			clearTimeout(timer);
			timer = window.setTimeout(() => setShown(false), IDLE_MS);
		};
		const events = ['pointermove', 'pointerdown', 'touchstart', 'keydown'];
		for (const e of events) document.addEventListener(e, wake, { passive: true });
		wake();
		return () => {
			clearTimeout(timer);
			for (const e of events) document.removeEventListener(e, wake);
		};
	}, []);
	const fade =
		' transition-opacity duration-200 ' +
		(shown ? 'opacity-100' : 'pointer-events-none cursor-none opacity-0');
	return (
		<>
			<div
				class={'absolute inset-x-0 top-0 z-10 flex items-start gap-2 p-3' + (flip ? ' flex-row-reverse' : '') + fade}
				data-fullscreen-controls={shown ? 'visivel' : 'escondido'}
			>
				<span class={CHIP_BIG + ' max-w-[70%]'}>
					<span class="truncate">{label}</span>
					<Numbers tileKey={tileKey} />
				</span>
				<button
					class={
						(flip ? 'mr-auto' : 'ml-auto') +
						' flex h-11 shrink-0 items-center gap-2 rounded-control bg-background px-4 text-meta font-medium text-foreground hover:bg-default'
					}
					onClick={onExit}
					aria-label="sair da tela cheia"
				>
					<Icon name="xmark" size={20} />
				</button>
			</div>
			{(controls || liveUsername) && (
				<div class={'absolute inset-x-0 bottom-0 z-10 flex flex-wrap items-center justify-center gap-2 p-3' + fade}>
					{liveUsername && <div class="w-52"><Slider username={liveUsername} source={SCREEN_AUDIO} title="som da live" compact /></div>}
					{controls}
				</div>
			)}
		</>
	);
}

// useFullscreen says whether this particular box is the one filling the screen.
function useFullscreen(box: HTMLElement | null): boolean {
	const [full, setFull] = useState(false);
	useEffect(() => {
		const check = () => setFull(!!box && document.fullscreenElement === box);
		check();
		document.addEventListener('fullscreenchange', check);
		return () => document.removeEventListener('fullscreenchange', check);
	}, [box]);
	return full;
}

// Speaking is the green ring around somebody who is talking — the owner's
// request, and the one thing a call with four people and no cameras genuinely
// needs, because "quem falou?" is otherwise unanswerable. It is the only green
// in the room (Componentes §09).
//
// It is a component of its own, and a very small one, for the reason the whole
// of this file is arranged the way it is: it changes several times a second and
// nothing that holds a picture may be redrawn at that rate (CLAUDE.md). This
// one draws a single empty span and subscribes only to its own person, so a
// ring lighting up costs one span, not one call.
//
// A 2px border on a span laid over the tile, and not the design's inset ring:
// the ring is a box-shadow, and no shadow may sit on a video. They draw the
// same two pixels.
export function useSpeaking(username: string): boolean {
	const [on, setOn] = useState(false);
	useEffect(() => {
		if (!username) return;
		return voice.watchSpeaking(username, setOn);
	}, [username]);
	return on;
}

function Speaking({ username, round = false }: { username: string; round?: boolean }) {
	const on = useSpeaking(username);
	if (!on) return null;
	return (
		<span
			class={
				'pointer-events-none absolute inset-0 border-2 border-success ' + (round ? 'rounded-full' : 'rounded-panel')
			}
			data-speaking={username}
		/>
	);
}

// The room works the way Discord's does (the owner, 24/09/2026, after trying
// the three layouts of the first round: "muito mais parecido com o discord").
//
//   grade — everybody is a block of the same size, a screen being shown is one
//           more block beside the person showing it, and the blocks are 16:9
//           and as big as the room lets them be.
//   foco  — pressing a block makes it the big one, with everybody else in a
//           strip under it; pressing the big one goes back to the grid. The
//           strip folds away with the button above it, and then the cameras
//           float over the picture, small, and can be dragged anywhere on it.
//
// Pressing is only ever "look at this". What you can do *about* a person is in
// their menu, on the right button, as in Discord — a left click that sometimes
// focused and sometimes opened a profile was the confusion the owner named.
//
// Whatever changes the size a screen is drawn at changes the layer this device
// asks for (media.ts, measure): the ResizeObserver on the <video> sees it.

function readPref(key: string): string | null {
	try {
		return localStorage.getItem(key);
	} catch {
		return null;
	}
}

function writePref(key: string, value: string): void {
	try {
		localStorage.setItem(key, value);
	} catch {
		// A private window. The choice holds until the page closes.
	}
}

type SeatInfo = {
	key: string;
	label: string;
	who: string;
	tile: Tile | null;
	peer: VoicePeer | null;
	watch?: string;
};

// The size of one block of the grid, by how many there are: as wide as the
// columns allow and as tall as the rows allow, whichever is smaller, at 16:9.
// Measured against the room itself (container units), so it is a class and
// never a number written by script. On a telephone, which stands up, the
// blocks stack instead and fill the room.
//
// Literal strings on purpose: Tailwind only makes the classes it can read.
export const GRID: Record<number, string> = {
	1: 'w-[min(calc(100cqw),calc(100cqh*16/9))] aspect-video max-sm:w-[calc(100cqw)] max-sm:h-[calc(100cqh)] max-sm:aspect-auto',
	2: 'w-[min(calc((100cqw_-_8px)/2),calc(100cqh*16/9))] aspect-video max-sm:w-[calc(100cqw)] max-sm:h-[calc((100cqh_-_8px)/2)] max-sm:aspect-auto',
	3: 'w-[min(calc((100cqw_-_8px)/2),calc((100cqh_-_8px)/2*16/9))] aspect-video max-sm:w-[calc(100cqw)] max-sm:h-[calc((100cqh_-_16px)/3)] max-sm:aspect-auto',
	4: 'w-[min(calc((100cqw_-_8px)/2),calc((100cqh_-_8px)/2*16/9))] aspect-video max-sm:w-[calc((100cqw_-_8px)/2)] max-sm:h-[calc((100cqh_-_8px)/2)] max-sm:aspect-auto',
	5: 'w-[min(calc((100cqw_-_16px)/3),calc((100cqh_-_8px)/2*16/9))] aspect-video max-sm:w-[calc((100cqw_-_8px)/2)] max-sm:h-[calc((100cqh_-_16px)/3)] max-sm:aspect-auto',
	6: 'w-[min(calc((100cqw_-_16px)/3),calc((100cqh_-_8px)/2*16/9))] aspect-video max-sm:w-[calc((100cqw_-_8px)/2)] max-sm:h-[calc((100cqh_-_16px)/3)] max-sm:aspect-auto',
	7: 'w-[min(calc((100cqw_-_16px)/3),calc((100cqh_-_16px)/3*16/9))] aspect-video max-sm:w-[calc((100cqw_-_16px)/3)] max-sm:h-[calc((100cqh_-_16px)/3)] max-sm:aspect-auto',
	8: 'w-[min(calc((100cqw_-_16px)/3),calc((100cqh_-_16px)/3*16/9))] aspect-video max-sm:w-[calc((100cqw_-_16px)/3)] max-sm:h-[calc((100cqh_-_16px)/3)] max-sm:aspect-auto',
	9: 'w-[min(calc((100cqw_-_16px)/3),calc((100cqh_-_16px)/3*16/9))] aspect-video max-sm:w-[calc((100cqw_-_16px)/3)] max-sm:h-[calc((100cqh_-_16px)/3)] max-sm:aspect-auto',
	10: 'w-[min(calc((100cqw_-_24px)/4),calc((100cqh_-_16px)/3*16/9))] aspect-video max-sm:w-[calc((100cqw_-_16px)/3)] max-sm:h-[calc((100cqh_-_24px)/4)] max-sm:aspect-auto',
};
// The big block in focus: the whole area at 16:9, centred.
export const BIG = 'w-[min(calc(100cqw),calc(100cqh*16/9))] aspect-video max-sm:w-[calc(100cqw)] max-sm:h-[calc(100cqh)] max-sm:aspect-auto';
// A block in the strip under it.
export const STRIP = 'h-full aspect-video min-w-0 shrink';

// Where the floating cameras are, as a fraction of the room they can move in:
// 0,0 is the top left and 1,1 the bottom right. A fraction and not pixels, so
// that it means the same place in a window, in fullscreen and after a resize.
type Spot = { x: number; y: number };
const SPOT_KEY = 'harmony.call.flutuante';
function savedSpot(): Spot {
	try {
		const v = JSON.parse(readPref(SPOT_KEY) ?? '');
		if (typeof v?.x === 'number' && typeof v?.y === 'number') {
			return { x: Math.min(1, Math.max(0, v.x)), y: Math.min(1, Math.max(0, v.y)) };
		}
	} catch {
		// Nothing saved, or something that is not ours.
	}
	return { x: 1, y: 1 };
}

// The air between the floating cameras and the edge of the picture.
const MARGIN = 12;

// How big the floating cameras are drawn, as a scale of their ordinary size,
// set by pulling their edges (the owner, 24/09/2026: "um grab para aumentar ou
// diminuir o tamanho da webcam"; a button in the corner came first and was
// turned down — "o arrastar deve ser pelas bordas"). Kept per device.
const SIZE_KEY = 'harmony.call.flutuante.tamanho';
const SIZE_MIN = 0.6;
const SIZE_MAX = 2.5;
function savedSize(): number {
	const v = Number(readPref(SIZE_KEY));
	return Number.isFinite(v) && v >= SIZE_MIN && v <= SIZE_MAX ? v : 1;
}

// Floating is the cameras over the picture when the strip is folded away:
// pictures only — no name, no circle for who has no camera (the owner: "tirar
// o nome da pessoa de dentro do vídeo e tirar o ícone de perfil"). They go
// wherever they are dropped, by the pixel; drag.ts is the one place allowed to
// write the transform that does it.
//
// A press that did not travel is a press: the camera becomes the big picture.
// One that travelled was a drag, and its click is swallowed.
// The edges of the floating stack that can be pulled, and where each strip
// sits: 8 px across the border, half in and half out; the corners a little
// bigger, so they are easy to catch.
type Edge = 'n' | 's' | 'e' | 'w' | 'ne' | 'nw' | 'se' | 'sw';
const EDGES: [Edge, string][] = [
	['n', '-top-1 inset-x-2 h-2 cursor-ns-resize'],
	['s', '-bottom-1 inset-x-2 h-2 cursor-ns-resize'],
	['e', '-right-1 inset-y-2 w-2 cursor-ew-resize'],
	['w', '-left-1 inset-y-2 w-2 cursor-ew-resize'],
	['nw', '-left-1.5 -top-1.5 h-3.5 w-3.5 cursor-nwse-resize'],
	['se', '-bottom-1.5 -right-1.5 h-3.5 w-3.5 cursor-nwse-resize'],
	['ne', '-right-1.5 -top-1.5 h-3.5 w-3.5 cursor-nesw-resize'],
	['sw', '-bottom-1.5 -left-1.5 h-3.5 w-3.5 cursor-nesw-resize'],
];

function Floating({
	cams,
	spot,
	onSpot,
	onFocus,
	onTuck,
}: {
	cams: SeatInfo[];
	spot: Spot;
	onSpot: (s: Spot) => void;
	onFocus: (s: SeatInfo) => void;
	onTuck: (s: SeatInfo) => void;
}) {
	const box = useRef<HTMLDivElement | null>(null);
	const at = useRef<Spot>(spot);
	const size = useRef(savedSize());
	const press = useRef<{ x: number; y: number; px: number; py: number; id: number } | null>(null);
	const grip = useRef<{
		edge: Edge;
		x: number;
		y: number;
		left: number;
		top: number;
		w: number;
		h: number;
	} | null>(null);
	const dragged = useRef(false);

	// The biggest the cameras may be drawn: never wider or taller than the
	// picture they float on, margins included.
	const maxSize = (el: HTMLElement, parent: HTMLElement) =>
		Math.min(
			SIZE_MAX,
			(parent.clientWidth - 2 * MARGIN) / Math.max(1, el.offsetWidth),
			(parent.clientHeight - 2 * MARGIN) / Math.max(1, el.offsetHeight),
		);

	// The room it can move in, less its own drawn size and the margin. The
	// scale grows it from its top left corner (origin-top-left), so the
	// drawn size is the laid-out size times the scale.
	const room = () => {
		const el = box.current;
		const parent = el?.offsetParent as HTMLElement | null;
		if (!el || !parent) return null;
		const s = Math.max(SIZE_MIN, Math.min(size.current, maxSize(el, parent)));
		return {
			el,
			s,
			w: Math.max(0, parent.clientWidth - el.offsetWidth * s - 2 * MARGIN),
			h: Math.max(0, parent.clientHeight - el.offsetHeight * s - 2 * MARGIN),
		};
	};
	const apply = () => {
		const r = room();
		if (r) place(r.el, MARGIN + r.w * at.current.x, MARGIN + r.h * at.current.y, r.s);
	};
	useLayoutEffect(() => {
		at.current = spot;
		apply();
	});

	// Pulling an edge: the stack keeps its shape (one scale for both sides),
	// and the corner across from the edge being pulled stays where it was, as
	// with any window.
	const resize = (ev: PointerEvent) => {
		const g = grip.current;
		const el = box.current;
		const parent = el?.offsetParent as HTMLElement | null;
		if (!g || !el || !parent) return;
		const W = Math.max(1, el.offsetWidth);
		const H = Math.max(1, el.offsetHeight);
		const dx = ev.clientX - g.x;
		const dy = ev.clientY - g.y;
		const west = g.edge.includes('w');
		const north = g.edge.includes('n');
		const byW = g.edge.includes('e') ? (g.w + dx) / W : west ? (g.w - dx) / W : null;
		const byH = g.edge.includes('s') ? (g.h + dy) / H : north ? (g.h - dy) / H : null;
		const was = g.w / W;
		// On a corner, the axis pulled furthest decides.
		let want =
			byW !== null && byH !== null
				? Math.abs(byW - was) >= Math.abs(byH - was)
					? byW
					: byH
				: (byW ?? byH ?? was);
		// The corner that stays, and the room on the side being pulled. The
		// side not being pulled does not hold it back: pulling the left edge of
		// a stack sitting on the bottom grows it, and it slides up to fit.
		const right = g.left + g.w;
		const bottom = g.top + g.h;
		const roomW = west ? right - MARGIN : byW !== null ? parent.clientWidth - MARGIN - g.left : Infinity;
		const roomH = north ? bottom - MARGIN : byH !== null ? parent.clientHeight - MARGIN - g.top : Infinity;
		want = Math.min(want, roomW / W, roomH / H, maxSize(el, parent));
		size.current = Math.max(SIZE_MIN, want);
		const left = west ? right - W * size.current : g.left;
		const top = north ? bottom - H * size.current : g.top;
		const r = room();
		if (!r) return;
		at.current = {
			x: r.w > 0 ? Math.min(1, Math.max(0, (left - MARGIN) / r.w)) : 0,
			y: r.h > 0 ? Math.min(1, Math.max(0, (top - MARGIN) / r.h)) : 0,
		};
		apply();
	};
	useEffect(() => {
		const el = box.current;
		const parent = el?.offsetParent;
		if (!el || !parent || typeof ResizeObserver === 'undefined') return;
		const watch = new ResizeObserver(() => apply());
		watch.observe(parent);
		watch.observe(el);
		return () => watch.disconnect();
	}, [cams.length]);

	if (cams.length === 0) return null;
	return (
		<div
			ref={box}
			class={
				'absolute left-0 top-0 z-[5] flex origin-top-left touch-none select-none gap-2 ' +
				(cams.length > 3 ? 'grid grid-cols-2 ' : 'flex-col ') +
				(press.current ? 'cursor-grabbing' : 'cursor-grab')
			}
			data-flutuantes={cams.length}
			onPointerDown={(ev) => {
				const r = room();
				if (!r) return;
				press.current = {
					x: ev.clientX,
					y: ev.clientY,
					px: r.w * at.current.x,
					py: r.h * at.current.y,
					id: ev.pointerId,
				};
				dragged.current = false;
			}}
			onPointerMove={(ev) => {
				const p = press.current;
				if (!p) return;
				const dx = ev.clientX - p.x;
				const dy = ev.clientY - p.y;
				if (!dragged.current) {
					if (Math.hypot(dx, dy) < 6) return;
					dragged.current = true;
					(ev.currentTarget as HTMLElement).setPointerCapture(p.id);
				}
				const r = room();
				if (!r) return;
				at.current = {
					x: r.w > 0 ? Math.min(1, Math.max(0, (p.px + dx) / r.w)) : 0,
					y: r.h > 0 ? Math.min(1, Math.max(0, (p.py + dy) / r.h)) : 0,
				};
				apply();
			}}
			onPointerUp={() => {
				if (dragged.current) {
					writePref(SPOT_KEY, JSON.stringify(at.current));
					onSpot(at.current);
				}
				press.current = null;
			}}
			onPointerCancel={() => {
				press.current = null;
				dragged.current = false;
				apply();
			}}
			onClickCapture={(ev) => {
				if (!dragged.current) return;
				dragged.current = false;
				ev.stopPropagation();
				ev.preventDefault();
			}}
		>
			{cams.map((s) => (
				<div key={s.key} class="group/cam relative h-[90px] w-40 max-sm:h-[54px] max-sm:w-24">
					<button
						class="absolute inset-0 overflow-hidden rounded-panel bg-surface-2"
						aria-label={'câmera de ' + s.who}
						data-tile={s.tile?.key ?? ''}
						// In fullscreen a press changes nothing: swapping the big
						// picture would take the box out of fullscreen with it.
						onClick={() => !document.fullscreenElement && onFocus(s)}
					>
						{s.tile && <Picture tileKey={s.tile.key} />}
						<Speaking username={s.peer?.username ?? s.tile?.username ?? ''} />
					</button>
					<button
						class={
							'absolute right-1 top-1 grid h-6 w-6 place-items-center rounded-control bg-background text-foreground ' +
							'opacity-0 transition-opacity duration-200 hover:bg-default group-hover/cam:opacity-100 ' +
							'focus-visible:opacity-100 [@media(hover:none)]:opacity-100'
						}
						aria-label={'tirar a câmera de ' + s.who + ' de cima da imagem'}
						data-recolher={s.key}
						onPointerDown={(ev) => ev.stopPropagation()}
						onClick={(ev) => {
							ev.stopPropagation();
							onTuck(s);
						}}
					>
						<Icon name="xmark" size={12} />
					</button>
				</div>
			))}
			{/* The edges: thin strips along the four sides and the four corners,
			    invisible, found by the cursor changing over them. Each is its own
			    pointer, so pulling one is never a drag of the whole stack. */}
			{EDGES.map(([edge, where]) => (
				<div
					key={edge}
					class={'absolute z-[1] ' + where}
					data-borda={edge}
					onPointerDown={(ev) => {
						ev.stopPropagation();
						const r = room();
						if (!r) return;
						grip.current = {
							edge,
							x: ev.clientX,
							y: ev.clientY,
							left: MARGIN + r.w * at.current.x,
							top: MARGIN + r.h * at.current.y,
							w: r.el.offsetWidth * r.s,
							h: r.el.offsetHeight * r.s,
						};
						(ev.currentTarget as HTMLElement).setPointerCapture(ev.pointerId);
					}}
					onPointerMove={(ev) => {
						if (!grip.current) return;
						ev.stopPropagation();
						resize(ev);
					}}
					onPointerUp={(ev) => {
						if (!grip.current) return;
						ev.stopPropagation();
						grip.current = null;
						writePref(SIZE_KEY, size.current.toFixed(3));
						writePref(SPOT_KEY, JSON.stringify(at.current));
						onSpot(at.current);
					}}
					onPointerCancel={() => {
						grip.current = null;
						apply();
					}}
					onClick={(ev) => ev.stopPropagation()}
				/>
			))}
		</div>
	);
}

// The watch button, under the person it is about (the owner, 24/09/2026: "tem
// de ser maior e ficar embaixo do ícone da pessoa"). A button of the bar's
// height, grey like the profile's, blue under the pointer.
const WATCH =
	'pointer-events-auto flex h-11 shrink-0 items-center gap-2 rounded-control bg-default px-4 text-meta ' +
	'font-medium text-foreground hover:bg-accent max-sm:h-9 max-sm:px-3';
// The circle, in both of its sizes. Shared, because the watch button is lined
// up under it by drawing an invisible copy of it in the layer above.
export const CIRCLE_SMALL = 'h-16 w-16 text-[24px] max-sm:h-14 max-sm:w-14 max-sm:text-[20px]';
export const CIRCLE_BIG = 'h-24 w-24 text-[36px]';

// Seat is one person in the room, whether or not they have a camera on, plus
// the screens people are showing. Pressing it puts it in focus; the right
// button opens the person's menu.
//
// The design's seat (Componentes §09): radius 24, no outline, a raised surface
// when there is a picture and the plain one when there is only a face; the
// name in a solid chip in the corner, and never on a bar under the picture —
// the bar was a strip of the tile nobody could look at the picture through.
function Seat({
	label,
	who,
	tile,
	peer,
	big,
	size,
	onFocus,
	onMenu,
	menu,
	onWatch,
	controls,
	overlay,
	flee,
	popped,
}: {
	label: string;
	// The plain name, for the circle. The label carries decorations — "(você)",
	// "tela de …" — and initials must not.
	who: string;
	tile: Tile | null;
	peer: VoicePeer | null;
	big: boolean;
	// How big the block is: a class, from GRID, BIG or STRIP.
	size: string;
	// Pressing the block: into focus, or out of it.
	onFocus: () => void;
	// The right button: this person's menu, drawn by the app as `menu`.
	onMenu?: () => void;
	menu?: ComponentChildren;
	// Set when this person is showing a screen that is not arriving here,
	// which is what the owner asked for: the way back in, under the person.
	onWatch?: () => void;
	controls?: ComponentChildren;
	// Drawn over the picture, and inside the box that goes fullscreen, so it
	// goes there too: the floating cameras.
	overlay?: ComponentChildren;
	// Where the floating cameras are, so the buttons on the picture can get
	// out of their way (the owner: they were sitting on "alguns botões
	// importantes"). top: the cameras are in the top right, where the buttons
	// live; bottom: in the bottom left, where the name does.
	flee?: { top: boolean; bottom: boolean };
	// This picture is in the floating window, not here.
	popped?: boolean;
}) {
	// The element itself, in state rather than in a ref, because whether it is
	// the one filling the screen is a question that has to be asked again after
	// it exists.
	const [box, setBox] = useState<HTMLDivElement | null>(null);
	const full = useFullscreen(box);
	const chip = big ? CHIP_BIG : CHIP;
	// Where a chip sits is set by its curve: 8 in from a 16px picture, with
	// its own 8px corner (the owner saw a round chip that did not follow the
	// picture's corner, 23/09/2026).
	const corner = flee?.bottom ? 'right-2 bottom-2' : 'left-2 bottom-2';
	const top = flee?.top ? 'left-2 top-2' : 'right-2 top-2';
	const username = peer?.username ?? tile?.username ?? '';
	return (
		<div
			ref={setBox}
			// The block itself does not clip: the person's menu can hang out
			// of it. The picture and everything on it are clipped by the layer
			// inside, which carries the corner.
			class={'group relative shrink-0 ' + size}
			data-tile={tile?.key ?? ''}
			data-person={username}
			data-big={big ? '1' : undefined}
			onContextMenu={
				onMenu
					? (ev) => {
							ev.preventDefault();
							onMenu();
						}
					: undefined
			}
		>
			<div class={'absolute inset-0 overflow-hidden rounded-panel ' + (tile ? 'bg-surface-2' : 'bg-surface')}>
				{full && tile && (
					<FullscreenControls
						label={label}
						tileKey={tile.key}
						controls={controls}
						liveUsername={tile.source === SCREEN && !tile.mine ? tile.username : undefined}
						flip={!!flee?.top}
						onExit={() => void voice.fullscreen(tile.key, box)}
					/>
				)}
				<button
					class="absolute inset-0"
					onClick={full ? undefined : onFocus}
				>
					{tile && popped ? (
						<span class="absolute inset-0 grid place-items-center text-meta text-foreground-muted">
							Na janela flutuante
						</span>
					) : tile ? (
						<Picture tileKey={tile.key} />
					) : (
						// With a watch button the circle moves up by half of it, so the
						// two are centred together; the button itself is drawn in the
						// layer below, since a button cannot sit inside a button.
						<span
							class={
								'absolute inset-0 flex flex-col items-center justify-center ' +
								(onWatch ? 'gap-3 max-sm:gap-1' : '')
							}
						>
							<span
								data-initials="1"
								data-com-assistir={onWatch ? '1' : undefined}
								class={
									'grid place-items-center rounded-full bg-surface-3 font-semibold text-foreground ' +
									(big ? CIRCLE_BIG : CIRCLE_SMALL)
								}
							>
								<FaceFill name={who} username={username} avatar={peer?.avatar} />
							</span>
							{onWatch && <span class="invisible h-11 max-sm:h-9" aria-hidden="true" />}
						</span>
					)}
				</button>
				{onWatch && (
					<div
						class={
							'pointer-events-none absolute inset-0 flex flex-col items-center ' +
							// On a camera the person is the picture, so the button sits on
							// its lower part, clear of the name chip; on a circle, it is
							// under the circle.
							(tile ? 'justify-end pb-10' : 'justify-center gap-3 max-sm:gap-1')
						}
					>
						{!tile && (
							<span
								class={'invisible block rounded-full ' + (big ? CIRCLE_BIG : CIRCLE_SMALL)}
								data-com-assistir="1"
								aria-hidden="true"
							/>
						)}
						{/* Over a camera only under the pointer (the owner, 17/10/2026:
						    it sat on the person's face); a touch screen has no
						    pointer to wait for, so there it stays (style.css). */}
						<button
							class={WATCH + (tile ? ' revela-camera' : '')}
							onClick={onWatch}
							aria-label="assistir à tela"
							data-assistir
						>
							<Icon name="display" size={20} /> Assistir
						</button>
					</div>
				)}
				{/* In fullscreen these chips would sit on the picture for as long as
				    you watch it. Everything on them worth having there is on the
				    overlay above, which goes away by itself. */}
				<div
					data-tilebar="1"
					class={
						'pointer-events-none absolute inset-0 ' +
						(full ? 'hidden' : 'block') +
						(big
							? ' opacity-0 transition-opacity duration-200 group-hover:opacity-100 group-focus-within:opacity-100 [@media(hover:none)]:opacity-100'
							: '')
					}
				>
					{/* The name is a label and not a button any more: a press on
					    the block is "look at this", everywhere on it, and what
					    you can do about the person is on the right button. */}
					<span class={chip + ' absolute max-w-[calc(100%-16px)] ' + corner} data-nome>
						{tile?.source === SCREEN && (
							<span class="shrink-0">
								<Icon name="display-pulse" size={12} />
							</span>
						)}
						{peer?.deafened ? (
							<span class="shrink-0 text-danger-soft-fg">
								<Deafened size={12} />
							</span>
						) : peer?.muted ? (
							<span class="shrink-0 text-danger-soft-fg">
								<Icon name="microphone-slash" size={12} />
							</span>
						) : null}
						{isMuted(peer?.username ?? tile?.username ?? '') && <span class="shrink-0 text-foreground-muted" title="Silenciado por você" aria-label="Silenciado por você" data-person-muted><Icon name="microphone-slash" size={12} /></span>}
						<span class="truncate">{label}</span>
					</span>
					<span
						class={'pointer-events-auto absolute flex max-w-[calc(100%-16px)] items-center gap-2 ' + top}
						data-botoes-topo={flee?.top ? 'esquerda' : 'direita'}
					>
						{big && tile && !popped && <Numbers tileKey={tile.key} wrap={chip + ' min-w-0 max-sm:hidden'} />}
						{/* Not in fullscreen. There the picture is the only thing on
						    the screen, and the way out of it is the ✕ — a second
						    button that leaves the call looking identical but with
						    nothing arriving is the owner's complaint, and he is
						    right: it is the wrong place for a decision like that. */}
						{tile && tile.source === SCREEN && !tile.mine && (
							<button
								class={chip + ' shrink-0 hover:bg-default'}
								aria-label="parar de assistir"
								onClick={() => void voice.setWatching(tile.username, false)}
							>
								Parar de assistir
							</button>
						)}
						{/* Discord's pop-out: the picture into a window of its own
						    that floats over everything, the game included, and
						    can be dragged anywhere on the monitor. Only where the
						    browser can (Chrome and Edge on a computer). */}
						{tile && big && (popped || voice.canPopOut()) && (
							<button
								class={chip + ' shrink-0 hover:bg-default'}
								aria-label={popped ? 'trazer de volta' : 'janela flutuante'}
								data-janela={popped ? 'fora' : 'dentro'}
								onClick={() => (popped ? voice.popIn() : void voice.popOut(tile.key, label))}
							>
								<Icon name="arrow-right-from-square" size={12} />
								<span class="max-sm:hidden">{popped ? 'Trazer de volta' : 'Janela'}</span>
							</button>
						)}
						{tile && big && !popped && (
							<button
								class={chip + ' shrink-0 hover:bg-default'}
								aria-label={full ? 'sair da tela cheia' : 'tela cheia'}
								onClick={() => void voice.fullscreen(tile.key, box)}
							>
								<Icon name="chevrons-expand-up-right" size={12} />
								<span class="max-sm:hidden">Tela cheia</span>
							</button>
						)}
					</span>
				</div>
				{!full && tile?.source === SCREEN && !tile.mine && !popped && (
					<div data-live-volume class="pointer-events-none absolute bottom-10 right-2 w-[min(220px,calc(100%-16px))] opacity-0 transition-opacity duration-200 group-hover:pointer-events-auto group-hover:opacity-100 group-focus-within:pointer-events-auto group-focus-within:opacity-100">
						<Slider username={tile.username} source={SCREEN_AUDIO} title="som da live" compact />
					</div>
				)}
				{overlay}
				{/* Every screen writes its line, in focus or not, here or in the
				    floating window: the log is how "travou" gets an answer. */}
				{tile?.source === SCREEN && (!big || popped) && <Numbers tileKey={tile.key} quiet />}
				<Speaking username={username} />
			</div>
			{menu}
		</div>
	);
}

// Invite is "chama o fulano" from inside the room, which is where the thought
// happens. Everything it does already existed — the doorbell is the D4 one, and
// the profile panel has always been able to ring it — but it was three presses
// away and in the wrong place: you notice somebody is missing while you are
// looking at who is here.
//
// Only people who are not already in this room are listed, and everybody else
// is, including whoever is offline: the telephone in their pocket is exactly
// what the doorbell is for. The server refuses on its own terms — the caller
// has to be inside the room and the person a member (api/ring.go) — and it is
// the same doorbell, with the same twenty seconds between rings for a pair.
export function Invite({
	members,
	peers,
	meUsername,
	onRing,
	onClose,
}: {
	members: User[];
	peers: VoicePeer[];
	meUsername: string;
	onRing: (username: string) => Promise<void>;
	onClose: () => void;
}) {
	const [called, setCalled] = useState<Record<string, boolean>>({});
	const card = useRef<HTMLDivElement | null>(null);
	// Through a ref, for the reason spelled out in Profile.tsx: the caller
	// writes onClose inline, so it changes on every render of the app, and a
	// listener that is taken off and put back on every render has a gap in it.
	const close = useRef(onClose);
	close.current = onClose;

	useEffect(() => {
		const away = (ev: MouseEvent) => {
			if (card.current && !card.current.contains(ev.target as Node)) close.current();
		};
		const key = (ev: KeyboardEvent) => {
			if (ev.key === 'Escape') close.current();
		};
		const timer = setTimeout(() => document.addEventListener('mousedown', away), 0);
		document.addEventListener('keydown', key);
		return () => {
			clearTimeout(timer);
			document.removeEventListener('mousedown', away);
			document.removeEventListener('keydown', key);
		};
	}, []);

	const here = new Set(peers.map((p) => p.username));
	const missing = members.filter((m) => m.username !== meUsername && !here.has(m.username));

	return (
		<div class="safe-fixed fixed inset-0 z-20 grid place-items-center bg-veil p-4">
			<div
				ref={card}
				class="w-full max-w-sm rounded-panel bg-surface p-4"
				role="dialog"
				aria-label="convidar para a chamada"
				data-invite
			>
				<div class="flex items-center gap-2 pb-2">
					<p class="text-item">Chamar para a sala</p>
					<button
						class="ml-auto grid h-9 w-9 place-items-center rounded-control text-foreground-muted hover:bg-surface-2 hover:text-foreground"
						onClick={onClose}
						title="Fechar"
					>
						<Icon name="xmark" />
					</button>
				</div>
				<div class="scroller max-h-72">
					{missing.map((m) => (
						<div key={m.username} class="flex items-center gap-2 py-1">
							<span class="min-w-0 flex-1 truncate text-item">{nameOf(m.username, m.display_name)}</span>
							<button
								// One width for both words and the ground kept when it
								// is done: "chamado" used to lose both and sit off
								// centre where the button had been (23/09/2026).
								class="h-9 w-24 shrink-0 rounded-control bg-surface-2 text-center text-meta font-medium text-foreground hover:bg-surface-3 disabled:bg-surface-2 disabled:text-foreground-muted"
								disabled={called[m.username]}
								data-ring={m.username}
								onClick={() => {
									setCalled((c) => ({ ...c, [m.username]: true }));
									void onRing(m.username);
								}}
							>
								{called[m.username] ? 'Chamado' : 'Chamar'}
							</button>
						</div>
					))}
					{missing.length === 0 && <p class="py-2 text-meta text-foreground-muted">Todo mundo já está aqui.</p>}
				</div>
			</div>
		</div>
	);
}

// Controls is the bar along the bottom of the call: the things people press
// during one, and the way out (Componentes §10, and CallKit.tsx for why each
// button is the colour it is).
//
// The same ones are in the channel column, and that is on purpose rather than
// by accident. The column is where you are before you are looking at the call;
// this is where your eyes already are once you are.
function Controls({
	view,
	onOpenPanel,
	onInvite,
}: {
	view: VoiceView;
	onOpenPanel: (what: 'screen' | 'camera') => void;
	// Null in fullscreen, where a dialog over the picture would be a trap.
	onInvite: (() => void) | null;
}) {
	return (
		<div class={BAR + ' w-full'} data-call-controls>
			{/* Not hearing anybody turns this one off too — it always has, and
			    now it says so instead of showing a live microphone that is
			    silent. */}
			<button
				class={view.muted || view.deafened ? MUTED : IDLE}
				title={
					view.deafened
						? 'Seu microfone está desligado porque você não está ouvindo'
						: view.muted
							? 'Ligar o microfone'
							: 'Desligar o microfone'
				}
				aria-label={view.muted ? 'ligar o microfone' : 'desligar o microfone'}
				aria-pressed={view.muted || view.deafened}
				disabled={view.deafened}
				onClick={() => voice.setMuted(!view.muted)}
			>
				{view.muted || view.deafened ? (
					<Icon name="microphone-slash" size={20} />
				) : (
					<Icon name="microphone" size={20} />
				)}
			</button>
			<button
				class={view.deafened ? MUTED : IDLE}
				title={view.deafened ? 'Voltar a ouvir' : 'Não ouvir ninguém'}
				aria-label={view.deafened ? 'voltar a ouvir' : 'não ouvir ninguém'}
				aria-pressed={view.deafened}
				onClick={() => voice.setDeafened(!view.deafened)}
			>
				{view.deafened ? <Deafened size={20} /> : <Icon name="headphones" size={20} />}
			</button>
			<button
				class={view.sharingCamera ? ON : OFF}
				title={view.sharingCamera ? 'Sua câmera' : 'Ligar a câmera'}
				aria-label={view.sharingCamera ? 'sua câmera' : 'ligar a câmera'}
				aria-pressed={view.sharingCamera}
				data-open="camera"
				onClick={() => onOpenPanel('camera')}
			>
				<Icon name="camera" size={20} />
			</button>
			{/* There is no stop button in this row. There was one, added
			    because stopping used to mean opening the panel that starts a
			    broadcast and finding the stop inside it; the owner took it out
			    on 19/09/2026. Stopping still lives in that panel, next to the
			    settings of the thing being stopped, which is where somebody
			    already is when they change their mind. */}
			<ScreenButton sharing={view.sharingScreen} onOpen={() => onOpenPanel('screen')} />
			{onInvite && (
				<button
					class={IDLE + ' bar-convite'}
					title="Chamar alguém para a sala"
					aria-label="chamar alguém para a sala"
					data-open="invite"
					onClick={onInvite}
				>
					<Icon name="person-plus" size={20} />
				</button>
			)}
			{/* The line, in the bar, where the design put it. Not on a
			    telephone: the row has no room, and the reading is one press
			    away in the column there as it is here. */}
			{/* What gives way when the bar is narrow is decided by the bar's
			    own width, not the window's (.bar-* in style.css): a telephone
			    of 360px pushed "Sair" off the screen, and so did a monitor
			    window of 768 with both columns open (the Codex, 23/09/2026). */}
			<span class="bar-linha">{view.state === 'live' && <Quality chip />}</span>
			<button
				class={LEAVE + ' bar-sair ml-auto'}
				title="Sair da chamada"
				aria-label="sair da chamada"
				onClick={() => void voice.leave()}
			>
				<Icon name="handset-arrow-out" size={20} />
				<span class="danger-action bar-palavra">Sair</span>
			</button>
		</div>
	);
}

// Call is the whole room. It is given the people from the server and the
// pictures from media.ts, and it puts the two together.
export function Call({
	view,
	peers,
	meUsername,
	onMenuPerson,
	menuFor,
	onOpenPanel,
	onInvite,
	here,
	onJoin,
}: {
	view: VoiceView;
	peers: VoicePeer[];
	meUsername: string;
	// The right button on a block. The key is the block's, because one person
	// can be two blocks (their camera and their screen) and only the one that
	// was pressed opens its menu.
	onMenuPerson: (key: string) => void;
	// The menu itself, drawn by the app (the same one the lists open), hung
	// inside the block or above it.
	menuFor: (key: string, username: string, anchor: 'inside' | 'above') => ComponentChildren;
	onOpenPanel: (what: 'screen' | 'camera') => void;
	// Opening the list of who is missing. The list itself is drawn by the app,
	// because the column offers the same thing and one panel with two openers
	// beats two panels.
	onInvite: () => void;
	// here is "I am in this room right now", which is not the same as looking
	// at it. Looking at a room you are not in shows who is inside and a way in;
	// the microphone and the hang-up belong to somebody who is actually in it.
	here: boolean;
	onJoin: () => void;
}) {
	// A person with no picture can be put in focus too, as in Discord. That
	// focus lives here, because media.ts only knows pictures.
	const [personFocus, setPersonFocus] = useState('');
	// Whether the strip under the big block is out, or folded away with the
	// cameras floating over the picture instead. Kept per device.
	const [strip, setStrip] = useState(() => readPref('harmony.call.tira') !== 'esconder');
	const [spot, setSpot] = useState<Spot>(savedSpot);
	// The cameras put away from over the picture with their ✕. They are in the
	// strip when it is out; opening the strip again brings everybody back to
	// float the next time it folds, which is how one comes back.
	const [tucked, setTucked] = useState<string[]>([]);

	const screens = view.tiles.filter((t) => t.source === SCREEN);
	const cameras = new Map<string, Tile>();
	for (const t of view.tiles) if (t.source === CAMERA) cameras.set(t.username, t);

	// Somebody showing a screen that is not arriving here. Since 18/09/2026
	// that is the ordinary case and not only a refusal: joining a channel does
	// not start watching anybody, so this button is how a screen is asked for
	// the first time as well as asked for again. It has to be where the person
	// is — on them, which is where the owner looked for it.
	const showing = new Set(screens.map((t) => t.username));
	const seats: SeatInfo[] = [];
	for (const p of peers) {
		const name = nameOf(p.username, p.display_name);
		const naoChega = (p.publishing ?? []).includes(SCREEN) && !showing.has(p.username);
		seats.push({
			key: 'p:' + p.session_id,
			label: name + (p.username === meUsername ? ' (você)' : ''),
			who: name,
			tile: cameras.get(p.username) ?? null,
			peer: p,
			watch: naoChega && p.username !== meUsername ? p.username : undefined,
		});
	}
	// A screen is one more block, beside the person showing it — not the big
	// one by itself any more (the owner: "ficar dividido dois blocos, um para a
	// live, outro para a outra pessoa").
	for (const s of screens) {
		seats.push({
			key: s.key,
			label: 'Tela de ' + s.displayName,
			who: s.displayName,
			tile: s,
			peer: peers.find((p) => p.username === s.username) ?? null,
		});
	}

	// Focus needs something to put beside: alone in the room, the one block
	// is simply the room, and pressing it changes nothing (the owner,
	// 24/09/2026 — it grew a little and shrank back, for no reason). The same
	// when everybody else leaves while something is in focus.
	const big =
		seats.length < 2
			? null
			: ((view.focus ? seats.find((s) => s.tile?.key === view.focus) : undefined) ??
				(personFocus ? seats.find((s) => s.key === personFocus) : undefined) ??
				null);
	const others = big ? seats.filter((s) => s !== big) : seats;

	// Pressing a block: into focus, or back to the grid if it already is.
	const focus = (s: SeatInfo) => {
		if (seats.length < 2) return;
		if (s.tile) {
			setPersonFocus('');
			voice.focusOn(s.tile.key);
		} else {
			if (view.focus) voice.focusOn('');
			setPersonFocus((cur) => (cur === s.key ? '' : s.key));
		}
	};

	const cams = big && !strip ? others.filter((s) => s.tile?.source === CAMERA && !tucked.includes(s.key)) : [];
	const floating = cams.length > 0;
	const flee = floating ? { top: spot.y < 0.5 && spot.x >= 0.5, bottom: spot.y >= 0.5 && spot.x < 0.5 } : undefined;

	const block = (s: SeatInfo, size: string, anchor: 'inside' | 'above') => (
		<Seat
			key={s.key}
			label={s.label}
			who={s.who}
			tile={s.tile}
			peer={s.peer}
			big={false}
			size={size}
			onFocus={() => focus(s)}
			onMenu={s.peer ? () => onMenuPerson(s.key) : undefined}
			menu={s.peer ? menuFor(s.key, s.peer.username, anchor) : null}
			onWatch={s.watch ? () => voice.watch(s.watch as string) : undefined}
			popped={!!s.tile && s.tile.key === view.popped}
		/>
	);

	return (
		<div
			// On a monitor the room is as wide as the header's island above it
			// (the owner, 23/09/2026) and every gap is the islands' 8: the
			// pictures and the bar are islands on the page's black too.
			class="flex h-full min-h-0 min-w-0 flex-col gap-4 overflow-hidden bg-background p-4 max-sm:gap-2 max-sm:p-3 md:gap-2 md:p-0 md:pb-2"
			data-call={here ? view.state : 'fora'}
			data-layout={big ? 'foco' : 'grade'}
		>
			<Warning sharing={view.sharingScreen || view.sharingCamera} />
			{big ? (
				<div class="flex min-h-0 flex-1 flex-col gap-2">
					<div class="grid min-h-0 flex-1 place-items-center [container-type:size]" data-grande>
						<Seat
							label={big.tile?.source === SCREEN ? big.who + ' está mostrando a tela' : big.label}
							who={big.who}
							tile={big.tile}
							peer={big.peer}
							big
							size={BIG}
							controls={here ? <Controls view={view} onOpenPanel={onOpenPanel} onInvite={null} /> : null}
							onFocus={() => focus(big)}
							onMenu={big.peer ? () => onMenuPerson(big.key) : undefined}
							menu={big.peer ? menuFor(big.key, big.peer.username, 'inside') : null}
							onWatch={big.watch ? () => voice.watch(big.watch as string) : undefined}
							flee={flee}
							popped={!!big.tile && big.tile.key === view.popped}
							overlay={
								floating ? (
									<Floating
										cams={cams}
										spot={spot}
										onSpot={setSpot}
										onFocus={focus}
										onTuck={(s) => setTucked((t) => [...t, s.key])}
									/>
								) : null
							}
						/>
					</div>
					{others.length > 0 && (
						<div class="flex shrink-0 flex-col items-center gap-2">
							{/* Discord's "⌄ 👥": the strip folds away, and then the
							    cameras float over the picture. */}
							<button
								class="flex h-7 items-center gap-1 rounded-control bg-surface px-2 text-foreground-muted hover:bg-default hover:text-foreground"
								title={strip ? 'Esconder quem está na sala' : 'Mostrar quem está na sala'}
								aria-label={strip ? 'esconder quem está na sala' : 'mostrar quem está na sala'}
								aria-pressed={!strip}
								data-esconder-tira={strip ? 'mostrando' : 'escondida'}
								onClick={() => {
									if (!strip) setTucked([]);
									setStrip(!strip);
									writePref('harmony.call.tira', strip ? 'esconder' : 'mostrar');
								}}
							>
								<span class={strip ? '' : 'rotate-180'}>
									<Icon name="chevron-down" size={14} />
								</span>
								<Icon name="persons" size={14} />
							</button>
							{strip && (
								<div class="flex h-28 w-full justify-center gap-2 max-sm:h-20" data-tira>
									{others.map((s) => block(s, STRIP, 'above'))}
								</div>
							)}
						</div>
					)}
				</div>
			) : seats.length > 0 ? (
				<div
					class="flex min-h-0 flex-1 flex-wrap content-center justify-center gap-2 [container-type:size]"
					data-grade={seats.length}
				>
					{seats.map((s) => block(s, GRID[Math.min(seats.length, 10)], 'inside'))}
				</div>
			) : (
				// An empty room is a sentence, not an illustration (BRIEF §7.9).
				<div class="grid flex-1 place-items-center">
					{here ? (
						<button
							class="h-11 rounded-control bg-default px-5 text-meta font-medium text-foreground hover:bg-default-hover"
							onClick={onInvite}
						>
							Você está sozinho na sala. Chamar alguém
						</button>
					) : (
						<p class="text-meta text-foreground-muted">Ninguém aqui ainda.</p>
					)}
				</div>
			)}
			{here ? (
				<Controls view={view} onOpenPanel={onOpenPanel} onInvite={onInvite} />
			) : (
				// The height of the account's island beside it, 48 (the owner,
				// 24/09/2026: "não está alinhado com o da esquerda"): a 32px
				// button, the gear's height, 8 in from a 16px corner. Not on a
				// telephone, where there is no island beside it and a thumb needs 44.
				<div class={BAR + ' justify-center'}>
					<button
						class="h-8 rounded-control bg-accent px-5 text-[14px] font-medium text-foreground hover:bg-accent-hover max-sm:h-11"
						onClick={onJoin}
					>
						Entrar na chamada
					</button>
				</div>
			)}
		</div>
	);
}
