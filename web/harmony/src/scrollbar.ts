// The scroll bar, drawn by the app (24/09/2026).
//
// Chromium lets a page style its bar (::-webkit-scrollbar) and Firefox does
// not: Firefox offers a colour and "thin", and on Windows 11 its bar is a hair
// that swells on hover and grows arrows. The owner uses Firefox and asked for
// the bar "sempre mais grosso" and without the arrows, which no stylesheet can
// ask Firefox for. So every .scroller hides the browser's bar (style.css) and
// gets this one instead, the same in every browser.
//
// How it is drawn, under this project's rules:
// * No inline style and no style assignment from script (security_test.go).
//   The thumb is an SVG <rect>, and x, y, width and height are the rect's own
//   attributes. Its colour is a class in style.css.
// * The layer is not a child of the scroller - a child would scroll away with
//   the content - but the last child of the scroller's parent, absolutely
//   positioned. Where exactly its containing block is does not matter: the
//   thumb is placed by measuring the scroller and the layer against the page
//   on every redraw, so the difference is right wherever the layer landed.
//   (Translation only: nothing between them is scaled, and a scaled ancestor
//   would need the layer's CTM here.)
// * It stays the last child. A foreign node among the children Preact manages
//   can make it move one of its own nodes to restore the order, and moving a
//   node that holds the typing box takes the cursor out of it; after each
//   change the layer is put back at the end, and moving the layer moves no
//   one's cursor.
// * Nothing on the screens changes. A MutationObserver finds scrollers as they
//   appear and drops the bars of the ones that went away.
//
// Audited by the Codex the day it was written: the per-bar observers, the
// typing box's input, the short track and the ancestor scroll are its finds.

const SVG = 'http://www.w3.org/2000/svg';
// The thumb: 8 wide, 2 in from the scroller's right edge, never shorter
// than 40 so there is something to take hold of on a long history.
const WIDTH = 8;
const INSET = 2;
const MIN = 40;

type Bar = {
	el: HTMLElement;
	layer: SVGSVGElement;
	thumb: SVGRectElement;
	// The scroller and its children: a new message or a picture that finishes
	// loading changes the content and not the scroller's box.
	sizes: ResizeObserver;
	watched: Set<Element>;
	// The geometry of the last draw, for the drag.
	track: number;
	size: number;
};

const bars = new Map<HTMLElement, Bar>();
let queued = false;
// Whether the document's children changed since the last look for scrollers:
// a scroll alone only needs the thumbs moved.
let changed = true;

function schedule() {
	if (queued) return;
	queued = true;
	requestAnimationFrame(() => {
		queued = false;
		sync();
	});
}

function attach(el: HTMLElement): Bar | null {
	const parent = el.parentElement;
	if (!parent) return null;
	const layer = document.createElementNS(SVG, 'svg');
	layer.setAttribute('class', 'scroll-layer');
	layer.setAttribute('aria-hidden', 'true');
	// A list that opens over the page takes its layer up with it (style.css
	// has a rule per level the app uses).
	const z = getComputedStyle(el).zIndex;
	if (z !== 'auto') layer.setAttribute('data-z', z);
	const thumb = document.createElementNS(SVG, 'rect');
	thumb.setAttribute('class', 'scroll-thumb');
	thumb.setAttribute('width', String(WIDTH));
	thumb.setAttribute('rx', String(WIDTH / 2));
	thumb.setAttribute('data-scroll-thumb', '');
	layer.appendChild(thumb);
	parent.appendChild(layer);
	const sizes = new ResizeObserver(schedule);
	sizes.observe(el);
	const bar: Bar = { el, layer, thumb, sizes, watched: new Set([el]), track: 0, size: 0 };
	// The typing box: its text is not a child, so typing or deleting changes
	// what scrolls without any node or box changing.
	el.addEventListener('input', schedule);
	grab(bar);
	return bar;
}

function detach(bar: Bar) {
	bar.el.removeEventListener('input', schedule);
	bar.sizes.disconnect();
	bar.layer.remove();
	bars.delete(bar.el);
}

// Dragging the thumb, and the wheel over it: the thumb is outside the
// scroller, so a wheel turned over it would otherwise scroll whatever is
// around the scroller instead.
function grab(bar: Bar) {
	const { thumb, el } = bar;
	thumb.addEventListener('pointerdown', (e) => {
		if (e.button !== 0) return;
		e.preventDefault();
		thumb.setPointerCapture(e.pointerId);
		thumb.classList.add('scroll-thumb-held');
		const id = e.pointerId;
		const y0 = e.clientY;
		const top0 = el.scrollTop;
		const move = (m: PointerEvent) => {
			if (m.pointerId !== id) return;
			const room = bar.track - bar.size;
			if (room > 0) el.scrollTop = top0 + ((m.clientY - y0) * (el.scrollHeight - el.clientHeight)) / room;
		};
		// The capture ends on release, on cancel and when anything else takes
		// it: one place to let go, however the drag ended.
		const end = (u: PointerEvent) => {
			if (u.pointerId !== id) return;
			thumb.classList.remove('scroll-thumb-held');
			thumb.removeEventListener('pointermove', move);
			thumb.removeEventListener('lostpointercapture', end);
		};
		thumb.addEventListener('pointermove', move);
		thumb.addEventListener('lostpointercapture', end);
	});
	thumb.addEventListener(
		'wheel',
		(e) => {
			// Ctrl and the wheel is the browser's zoom, not a scroll.
			if (e.ctrlKey) return;
			e.preventDefault();
			const line = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? el.clientHeight : 1;
			el.scrollTop += e.deltaY * line;
		},
		{ passive: false },
	);
}

function watch(bar: Bar) {
	const now = new Set<Element>([bar.el, ...bar.el.children]);
	for (const e of bar.watched) if (!now.has(e)) bar.sizes.unobserve(e);
	for (const e of now) if (!bar.watched.has(e)) bar.sizes.observe(e);
	bar.watched = now;
}

function draw(bar: Bar) {
	const { el, layer, thumb } = bar;
	const over = el.scrollHeight - el.clientHeight;
	const track = el.clientHeight - 2 * INSET;
	if (over <= 1 || track <= 0) {
		thumb.setAttribute('visibility', 'hidden');
		return;
	}
	// On a short list the least size gives way, so there is always some
	// travel left to drag through; never more than the track.
	const least = Math.min(MIN, track / 2);
	const size = Math.round(Math.max(least, (track * el.clientHeight) / el.scrollHeight));
	const box = el.getBoundingClientRect();
	const origin = layer.getBoundingClientRect();
	// The inside of the scroller's border, where its content is drawn.
	const top = box.top - origin.top + el.clientTop;
	const right = box.left - origin.left + el.clientLeft + el.clientWidth;
	const y = top + INSET + ((track - size) * Math.min(el.scrollTop, over)) / over;
	bar.track = track;
	bar.size = size;
	thumb.setAttribute('x', String(right - INSET - WIDTH));
	thumb.setAttribute('y', String(y));
	thumb.setAttribute('height', String(size));
	thumb.setAttribute('visibility', 'visible');
}

function sync() {
	if (changed) {
		changed = false;
		for (const bar of [...bars.values()]) {
			if (!bar.el.isConnected || !bar.el.matches('.scroller') || bar.layer.parentElement !== bar.el.parentElement)
				detach(bar);
		}
		for (const el of document.querySelectorAll<HTMLElement>('.scroller')) {
			if (!bars.has(el)) {
				const bar = attach(el);
				if (bar) bars.set(el, bar);
			}
		}
		for (const bar of bars.values()) {
			watch(bar);
			const parent = bar.layer.parentElement!;
			if (parent.lastChild !== bar.layer && !(parent.lastChild instanceof SVGSVGElement)) parent.appendChild(bar.layer);
		}
	}
	for (const bar of bars.values()) draw(bar);
}

export function startScrollbars() {
	new MutationObserver((records) => {
		// Our own thumbs moving are attribute changes, which this does not
		// watch; a layer going in is a child change, and the sync after it
		// finds nothing new and stops there.
		if (records.some((r) => r.type === 'childList')) {
			changed = true;
			schedule();
		}
	}).observe(document.body, { childList: true, subtree: true });
	// Any scroll, in capture: the scroller's own, and one around it that
	// carries the scroller somewhere else without changing its size.
	document.addEventListener('scroll', schedule, { capture: true, passive: true });
	window.addEventListener('resize', schedule);
	schedule();
}
