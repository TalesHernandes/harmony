// The shape of the channel column (D12a), as pure functions: how the rooms
// fall into the top of the column and into folders, and what the whole order
// becomes after one drag. Nothing here touches the page; App.tsx does the
// pointer and the drawing, and the server gets the result whole (PUT
// /api/layout).
//
// Inside each place, text channels sit above voice rooms, as on Discord. A
// room's position is its place among the rooms of its own kind there, so a
// text channel dropped onto a voice room lands at the end of the text
// channels of that folder rather than between two rooms.

import type { Category, Channel, Layout } from './api';

export interface Place {
	folder: Category | null;
	texts: Channel[];
	voices: Channel[];
}

const byPlace = (a: Channel, b: Channel) =>
	(a.position ?? 0) - (b.position ?? 0) || a.name.localeCompare(b.name) || a.id.localeCompare(b.id);

// column splits the rooms into the top of the column and one place per folder,
// in order. A room pointing at a folder this page does not know (an event in
// flight) is drawn at the top rather than lost.
export function column(channels: Channel[], categories: Category[]): Place[] {
	const folders = [...categories].sort((a, b) => a.position - b.position || a.name.localeCompare(b.name));
	const known = new Set(folders.map((f) => f.id));
	const places: Place[] = [{ folder: null, texts: [], voices: [] }, ...folders.map((f) => ({ folder: f, texts: [], voices: [] }))];
	const at = new Map(places.map((p) => [p.folder?.id ?? '', p]));
	for (const c of channels) {
		if (c.kind === 'dm') continue;
		const p = at.get(c.category_id && known.has(c.category_id) ? c.category_id : '')!;
		(c.kind === 'voice' ? p.voices : p.texts).push(c);
	}
	for (const p of places) {
		p.texts.sort(byPlace);
		p.voices.sort(byPlace);
	}
	return places;
}

// Where a drop lands. A room goes before or after another room, or into a
// folder ('' is the top of the column) at the end of its own kind. A folder
// goes before or after another folder.
export type Drop =
	| { room: string; where: 'before' | 'after' }
	| { folder: string; where: 'into' }
	| { folder: string; where: 'before' | 'after' };

export interface Order {
	categories: string[];
	channels: Array<{ id: string; category_id: string }>;
}

function flatten(places: Array<{ id: string; texts: string[]; voices: string[] }>): Order {
	const channels: Order['channels'] = [];
	for (const p of places) {
		for (const id of [...p.texts, ...p.voices]) channels.push({ id, category_id: p.id });
	}
	return { categories: places.filter((p) => p.id !== '').map((p) => p.id), channels };
}

// moveRoom is the order after dropping one room. null when the drop changes
// nothing or makes no sense (a room onto itself, a kind it cannot sit among).
export function moveRoom(places: Place[], id: string, drop: Drop): Order | null {
	const plain = places.map((p) => ({
		id: p.folder?.id ?? '',
		texts: p.texts.map((c) => c.id),
		voices: p.voices.map((c) => c.id),
	}));
	const from = plain.find((p) => p.texts.includes(id) || p.voices.includes(id));
	if (!from) return null;
	const kind: 'texts' | 'voices' = from.texts.includes(id) ? 'texts' : 'voices';
	const before = JSON.stringify(plain);
	from[kind] = from[kind].filter((x) => x !== id);

	if ('room' in drop) {
		if (drop.room === id) return null;
		const to = plain.find((p) => p.texts.includes(drop.room) || p.voices.includes(drop.room));
		if (!to) return null;
		const list = to[kind];
		const i = list.indexOf(drop.room);
		if (i < 0) {
			// The other kind: the end of the texts, or the start of the voices,
			// which is the edge between the two that the pointer is nearest.
			if (kind === 'texts') list.push(id);
			else list.unshift(id);
		} else {
			list.splice(drop.where === 'before' ? i : i + 1, 0, id);
		}
	} else if (drop.where === 'into') {
		const to = plain.find((p) => p.id === drop.folder);
		if (!to) return null;
		to[kind].push(id);
	} else {
		return null;
	}
	if (JSON.stringify(plain) === before) return null;
	return flatten(plain);
}

// moveFolder is the order after dropping one folder before or after another.
export function moveFolder(places: Place[], id: string, drop: Drop): Order | null {
	if ('room' in drop || drop.where === 'into' || drop.folder === id) return null;
	const plain = places.map((p) => ({
		id: p.folder?.id ?? '',
		texts: p.texts.map((c) => c.id),
		voices: p.voices.map((c) => c.id),
	}));
	const top = plain[0];
	const folders = plain.slice(1);
	const moving = folders.find((p) => p.id === id);
	if (!moving) return null;
	const rest = folders.filter((p) => p.id !== id);
	const i = rest.findIndex((p) => p.id === drop.folder);
	if (i < 0) return null;
	rest.splice(drop.where === 'before' ? i : i + 1, 0, moving);
	const order = flatten([top, ...rest]);
	if (order.categories.join() === folders.map((p) => p.id).join()) return null;
	return order;
}

// apply puts a layout onto the page's own list of channels and folders, so the
// column moves under the pointer before the server answers, and so the event
// that comes back from the server lands on the same picture.
export function apply(channels: Channel[], layout: Layout): Channel[] {
	const at = new Map(layout.channels.map((p) => [p.id, p]));
	return channels.map((c) => {
		const p = at.get(c.id);
		return p ? { ...c, category_id: p.category_id, position: p.position } : c;
	});
}

// asLayout is the same order in the shape the server sends back, with the
// positions counted per place and per kind.
export function asLayout(order: Order, channels: Channel[], categories: Category[]): Layout {
	const kind = new Map(channels.map((c) => [c.id, c.kind]));
	const next = new Map<string, number>();
	return {
		categories: order.categories
			.map((id, position) => {
				const f = categories.find((c) => c.id === id);
				return f ? { ...f, position } : null;
			})
			.filter((f): f is Category => f !== null),
		channels: order.channels.map((c) => {
			const key = c.category_id + '/' + kind.get(c.id);
			const position = next.get(key) ?? 0;
			next.set(key, position + 1);
			return { id: c.id, category_id: c.category_id, position };
		}),
	};
}

// Which folders this device keeps closed. Per device, like the people panel:
// whether there is room to show a folder is a fact about the screen.
const CLOSED = 'harmony.pastas-fechadas';

export function closedFolders(): Set<string> {
	try {
		const raw = JSON.parse(localStorage.getItem(CLOSED) ?? '[]');
		return new Set(Array.isArray(raw) ? raw.filter((x): x is string => typeof x === 'string') : []);
	} catch {
		return new Set();
	}
}

export function saveClosedFolders(closed: Set<string>): void {
	try {
		localStorage.setItem(CLOSED, JSON.stringify([...closed]));
	} catch {
		// Storage switched off: this visit remembers, the next one does not.
	}
}
