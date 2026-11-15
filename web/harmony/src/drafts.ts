// What you started writing and did not send.
//
// A draft belongs to one conversation and never follows you out of it. It lives
// in memory while the app is open, and it touches the disk **only while the app
// is not in front of you**: written when the page is hidden or going away,
// deleted the moment it is back in the box.
//
// That window is the whole privacy rule, and it was chosen rather than
// inherited. Nothing anybody types is stored on this device today — the
// preferences are (the column width, the sound, the emojis used lately), the
// messages are not — so a draft is the first written sentence to land in a
// file here. An unsent one is also the only sentence with no other copy: it is
// not on the server, not in the nightly backup, not in anybody else's client.
// Keeping it is therefore worth something, and keeping it *briefly* is the
// price. On a telephone the page is thrown out of memory while you are away,
// which is exactly the case this exists for.
//
// Your own text only. The message being answered travels with a draft in
// memory, and never to the disk: that sentence is somebody else's, and it is
// already on the server where it belongs.
//
// Written for one tab, which is how the app is used. A second tab of the same
// account does not corrupt anything — each writer only ever touches the
// conversations it has open — but it may not hand its drafts to the first one.

const KEY = 'harmony.drafts';

// Once somebody has signed out, nothing more is written, ever, by this page.
//
// It is a latch and not a tidy boolean because of an ordering that bites:
// signing out **navigates**, and navigating fires the very `pagehide` that
// saves drafts on the way out. Without this the drafts were cleared and then
// written straight back by the page's own departure, a few milliseconds later —
// which is the failure a test caught before anybody had to.
let signedOut = false;

// One entry per conversation, oldest out first. A server with twenty channels
// never reaches this; a browser used for a year should not carry every
// conversation it ever saw, which is the same reason the scroll memory has a
// ceiling.
const MAX_CONVERSATIONS = 20;

// Nothing survives a week away. A draft is a thing you were about to send, and
// after seven days it is not that any more.
const MAX_AGE_MS = 7 * 24 * 60 * 60 * 1000;

// What the server would take anyway (store.MaxMessageRunes). Storing more would
// only be storing something that cannot be sent.
const MAX_RUNES = 4000;

// Whose drafts these are. A draft is keyed by conversation, and a conversation
// two accounts share has the same id for both — so without the owner, a
// sentence one person left in "geral" became the next person's opening line
// on the same browser, whenever the first one's session ended any other way
// than by pressing "Sair" (Codex, 23/09/2026). The owner is written with the
// drafts, and a file that is somebody else's is thrown away unread.
type Stored = { at: number; owner: string; items: Record<string, string> };

// The account this page is signed in as, once it is known. Nothing is read
// before it and nothing is written without it.
let owner: string | null = null;

function read(): Stored {
	const none: Stored = { at: 0, owner: owner ?? '', items: {} };
	try {
		const raw = localStorage.getItem(KEY);
		if (!raw) return none;
		const s = JSON.parse(raw) as Stored;
		if (!s || typeof s.at !== 'number' || typeof s.items !== 'object') return none;
		if (Date.now() - s.at > MAX_AGE_MS) return none;
		// An older file carries no owner, and one from another account is not
		// ours to open: both read as empty, and the next write replaces them.
		if (owner === null || s.owner !== owner) return none;
		return { at: s.at, owner, items: s.items ?? {} };
	} catch {
		// Storage switched off, full, or holding something from an older shape.
		// A lost draft is a disappointment; a screen that will not open is a
		// fault, so this never throws.
		return none;
	}
}

function write(items: Record<string, string>): void {
	try {
		if (Object.keys(items).length === 0) {
			localStorage.removeItem(KEY);
			return;
		}
		if (owner === null) return;
		localStorage.setItem(KEY, JSON.stringify({ at: Date.now(), owner, items } satisfies Stored));
	} catch {
		// See read(): the draft is the thing that gives way, never the app.
	}
}

// take reads what this account left on the way out and **removes it in the same
// breath**. The caller now holds it in memory, which is the only place it should
// be while somebody is looking at the screen. It is called once the server has
// said who is signed in, and not before: that is the only answer to "whose".
// A file belonging to anybody else is removed with it, unread.
export function take(user: string): Record<string, string> {
	owner = user;
	const { items } = read();
	write({});
	return items;
}

// keep writes the drafts this tab is holding, merging rather than replacing so
// that a conversation this tab never opened is left alone. A draft that has
// been emptied is removed, because an empty draft is not a draft.
export function keep(mine: Record<string, string>): void {
	if (signedOut || owner === null) return;
	const all = read().items;
	for (const [id, text] of Object.entries(mine)) {
		const trimmed = [...text].slice(0, MAX_RUNES).join('');
		if (trimmed.trim() === '') delete all[id];
		else all[id] = trimmed;
	}
	const ids = Object.keys(all);
	for (const id of ids.slice(0, Math.max(0, ids.length - MAX_CONVERSATIONS))) delete all[id];
	write(all);
}

// drop removes exactly these conversations from the disk, and is what runs when
// the app comes back to the front: the drafts are in the box again, so the copy
// in the file has no reason to exist.
export function drop(ids: string[]): void {
	if (ids.length === 0) return;
	const all = read().items;
	for (const id of ids) delete all[id];
	write(all);
}

// forget is "Sair desta conta". Nothing typed on a shared computer should
// outlive the session it was typed in.
export function forget(): void {
	signedOut = true;
	write({});
}
