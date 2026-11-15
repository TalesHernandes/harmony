import { BlockedConversation } from './PersonalSafety';
import { isBlocked, preferencesReady, refreshPreferences, usePreferences } from './blocks';
// The app: the channel column, one conversation, and the people in it.
//
// The client never decides who may see what. It draws what /api/channels
// answered, and every id it asks about comes back checked. Refusing to draw a
// button is a courtesy to the reader, not a control.

import { useCallback, useEffect, useMemo, useRef, useState } from 'preact/hooks';
import { api, message, type Category, type Channel, type Layout, type Message, type Role, type User, type VoicePeer } from './api';
import { colorOf, hoistOf, nameClass, permissionsOf, rolesOf } from './ranks';
import { Composer } from './Composer';
import { Icon, MARK } from './Icon';
import { Gateway } from './gateway';
import { Messages } from './Messages';
import { Settings } from './Panels';
import { NewChannel } from './NewChannel';
import { Call, Invite } from './Call';
import { CameraPanel, ScreenPanel } from './Broadcast';
import { PersonMenu, type PersonMenuItem } from './PersonMenu';
import { Profile } from './Profile';
import { Search } from './Search';
import { Pins } from './Pins';
import { apply, asLayout, closedFolders, column as columnOf, moveFolder, moveRoom, saveClosedFolders, type Drop } from './reorder';
import { VoiceBar, VoiceChannels, VoiceToggles, useVoice } from './Voice';
import { voice } from './media';
import { log, rememberGeometry } from './diag';
import { drop, keep, take } from './drafts';
import { watchForNewBuild } from './version';
import { IncomingRing, NotifyNudge, RingBox } from './Notifications';
import { blip } from './notify';
import { direct } from './direct';
import { CallStrip, DirectCall, useDirect } from './DirectCall';
import { nameOf, setNicknames } from './names';
import { Avatar } from './Avatar';
import { Confirm } from './Confirm';
import { Presence, stateLabel, type PersonState } from './presence';
import {
	dismissNudge,
	enable as enableNotifications,
	nudgeDismissed,
	register as registerWorker,
	status as notifyStatus,
} from './notify';

type Loaded = { messages: Message[]; hasMore: boolean; hasNewer: boolean };

// ringingCall asks whether a call from a notification is still ringing, and
// puts it back on the screen if it is. A call that was answered on another
// device, or given up on, answers "no" — either by being gone (the server
// forgot it) or by saying it has been picked up — and nothing is drawn.
async function ringingCall(id: string): Promise<void> {
	try {
		const call = await api.callGet(id);
		if (call.answered) return;
		direct.called({ id: call.id, from: call.from });
	} catch {
		// Gone: it rang, nobody was there, and the server has already forgotten
		// it. There is nothing to tell anybody about a call that is over.
	}
}

// remember pulls the nicknames out of a list of people and hands the list back,
// so that every place the list is set also sets the names (names.ts).
function remember(people: User[]): User[] {
	const nicks: Record<string, string> = {};
	for (const p of people) if (p.nickname) nicks[p.username] = p.nickname;
	setNicknames(nicks);
	return people;
}

// A copy without one key. Sending or cancelling should leave *this*
// conversation with no quotation above the box, and leave every other one
// exactly as it was.
function without<T>(map: Record<string, T>, key: string): Record<string, T> {
	const next = { ...map };
	delete next[key];
	return next;
}

function channelLabel(c: Channel): string {
	if (c.kind === 'dm') {
		const who = c.peer?.username ?? '';
		return who ? nameOf(who, c.peer?.display_name) : 'Conversa';
	}
	// A voice channel is a room, not a conversation, and the column and the
	// header say so with the speaker icon. The name itself is only the name:
	return (c.kind === 'voice' ? '' : '#') + c.name;
}

// The same name, for the one place that already draws the mark beside it.
//
// The conversation's header was reading "# #geral": the box to the left of it
// holds the hash - it is where the kit's hashtag glyph is going - and then
// channelLabel put a second one in the text. Everywhere else the label is used
// there is no mark next to it ("Escrever em #geral", "ligar para ...", the
// title of a row), so it keeps the hash and this is the exception.
function headerLabel(c: Channel): string {
	return c.kind === 'text' ? c.name : channelLabel(c);
}

// How wide the channel column may be, as steps rather than as any number of
// pixels — and that is a rule, not a simplification.
//
// Nothing in this client may carry a style (security_test.go reads the source
// looking for it), so the width cannot be a number computed while a mouse
// moves: it has to be a class. From a column that still holds "recanto dos
// nóia" (208) to one that starts squeezing the conversation (448).
//
// The two ends are the point of it. A column dragged to nothing is not a
// smaller column, it is a broken one, and the widest step is where the
// conversation starts paying for the list.
//
// It was eight rungs, 16 to 64 pixels apart, and the owner found it "muito
// agressivo" (24/09/2026): the edge jumped away from the pointer. Option 10A
// is every 8 pixels between the same two ends, thirty-one classes written out
// in full because Tailwind only builds a class it can read in the source.
const COLUMN = [
	'w-52', 'w-54', 'w-56', 'w-58', 'w-60', 'w-62', 'w-64', 'w-66',
	'w-68', 'w-70', 'w-72', 'w-74', 'w-76', 'w-78', 'w-80', 'w-82',
	'w-84', 'w-86', 'w-88', 'w-90', 'w-92', 'w-94', 'w-96', 'w-98',
	'w-100', 'w-102', 'w-104', 'w-106', 'w-108', 'w-110', 'w-112',
] as const;
const COLUMN_MIN = 208;
const COLUMN_STEP = 8;
const COLUMN_PX = COLUMN.map((_, i) => COLUMN_MIN + i * COLUMN_STEP);
const COLUMN_DEFAULT = (256 - COLUMN_MIN) / COLUMN_STEP; // w-64
// The eight rungs the width used to be stored as, under the old key, read
// once so nobody's chosen width changes with the ladder.
const OLD_COLUMN_PX = [208, 224, 240, 256, 288, 320, 384, 448];

function savedColumn(): number {
	try {
		// Asked for before converting: Number(null) is 0, the narrowest rung,
		// and that is what every new browser opened on (the same slip as the
		// microphone volume in media.ts).
		const stored = localStorage.getItem('harmony.column.px');
		const px = stored === null || stored.trim() === '' ? NaN : Number(stored);
		if (Number.isFinite(px)) return nearestColumn(px);
		const old = localStorage.getItem('harmony.column');
		const raw = old === null || old.trim() === '' ? NaN : Number(old);
		if (Number.isInteger(raw) && raw >= 0 && raw < OLD_COLUMN_PX.length) {
			return nearestColumn(OLD_COLUMN_PX[raw]);
		}
	} catch {
		// A browser with storage switched off simply starts at the default.
	}
	return COLUMN_DEFAULT;
}

function rememberColumn(step: number): void {
	try {
		// The width itself this time, not a position on a ladder: a ladder
		// that changes, as this one just did, changes what a stored rung means.
		localStorage.setItem('harmony.column.px', String(COLUMN_PX[step]));
	} catch {
		// Nothing to do: this session keeps the width, the next one does not.
	}
	log(`coluna ${COLUMN_PX[step]}px`);
}

// nearestColumn is the step closest to a width in pixels, which is how a drag
// that happens in pixels lands on a class.
function nearestColumn(px: number): number {
	const i = Math.round((px - COLUMN_MIN) / COLUMN_STEP);
	return Math.max(0, Math.min(COLUMN.length - 1, i));
}

// Resizer is the edge of the column you pull. The width it sets is kept by the
// app above it; this is only the handle, and it is remembered per device rather
// than per person, because it is a fact about the screen.
function Resizer({ step, onStep }: { step: number; onStep: (next: number) => void }) {
	return (
		<div
			// The column's own colour, so it is four more pixels of column until
			// you reach for it (a strip of any other colour would be the divider
			// the design took off). What shows under the pointer is a thin grey
			// line in the middle of it, faded in after a moment: it was the full
			// strip in the accent blue, which the owner found "muito vibrante e
			// aparece do nada" (24/09/2026) - every pass of the mouse across the
			// edge flashed it. Double-click puts the column back to the default.
			// It spans the whole 8px gap between the column's islands and the
			// header's (the islands end 4 inside the column): at 4px from the
			// column's edge the line sat 1px off "# geral" and 5px off the
			// channels, and the owner saw it touching (16/10/2026). -ml-1 keeps
			// every width where it was.
			class="-ml-1 hidden w-2 shrink-0 cursor-col-resize justify-center bg-background md:flex"
			role="separator"
			aria-label="largura da coluna"
			aria-valuenow={COLUMN_PX[step]}
			aria-valuemin={COLUMN_PX[0]}
			aria-valuemax={COLUMN_PX[COLUMN.length - 1]}
			title="Arraste para mudar a largura, dois cliques para voltar ao normal"
			tabIndex={0}
			data-resizer
			onDblClick={() => {
				onStep(COLUMN_DEFAULT);
				rememberColumn(COLUMN_DEFAULT);
			}}
			onPointerDown={(ev) => {
				const handle = ev.currentTarget as HTMLElement;
				handle.setPointerCapture(ev.pointerId);
				handle.dataset.dragging = '';
				const startX = ev.clientX;
				const startPx = COLUMN_PX[step];
				let last = step;
				const move = (e: PointerEvent) => {
					const next = nearestColumn(startPx + e.clientX - startX);
					if (next === last) return;
					last = next;
					onStep(next);
				};
				const up = () => {
					handle.removeEventListener('pointermove', move);
					handle.removeEventListener('pointerup', up);
					handle.removeEventListener('pointercancel', up);
					delete handle.dataset.dragging;
					if (last !== step) rememberColumn(last);
				};
				handle.addEventListener('pointermove', move);
				handle.addEventListener('pointerup', up);
				handle.addEventListener('pointercancel', up);
				ev.preventDefault();
			}}
			// The same thing without a mouse, which is also what makes it
			// reachable at all on a keyboard.
			onKeyDown={(ev) => {
				if (ev.key !== 'ArrowLeft' && ev.key !== 'ArrowRight') return;
				ev.preventDefault();
				const next = Math.max(
					0,
					Math.min(COLUMN.length - 1, step + (ev.key === 'ArrowRight' ? 1 : -1)),
				);
				onStep(next);
				rememberColumn(next);
			}}
		>
			<span class="resizer-line w-0.5 bg-outline-muted" data-resizer-line />
		</div>
	);
}

// Typing is the line above the box: who is writing something, right now.
//
// It is deliberately one line and deliberately never taller than one line — it
// sits between the conversation and the box you type in, and anything that
// changes height there pushes the conversation about while somebody is reading
// it. Three names or thirty, it is the same single line.
function Typing({ names }: { names: string[] }) {
	if (names.length === 0) return null;
	const said =
		names.length === 1
			? `${names[0]} está digitando…`
			: names.length === 2
				? `${names[0]} e ${names[1]} estão digitando…`
				: `${names.length} pessoas estão digitando…`;
	return (
		<p class="truncate px-5 pb-1 text-meta text-foreground-muted" data-typing={names.length}>
			{said}
		</p>
	);
}

function savedPeople(): boolean {
	// Never open on a telephone, whatever was chosen last time: there the list
	// is a panel over the conversation, and an app that opens with a panel over
	// the thing you came to read is an app asking to be dismissed before it can
	// be used. The choice below is a choice about a monitor.
	if (window.innerWidth < 768) return false;
	try {
		const raw = localStorage.getItem('harmony.people');
		if (raw === '1' || raw === '0') return raw === '1';
	} catch {
		// Storage switched off: fall through to the size of the window.
	}
	return window.innerWidth >= 768;
}

// The counter on a row, and there are two of them.
//
// Which one you get is the difference between "there is something to read" and
// "somebody said your name", and that difference earns two shapes because it
// is the one that decides whether you open the app now. Plain unread is a
// white pill with dark ink - the loudest thing in the palette that is not a
// colour, and it has to be, because a number of messages is not a warning and
// the red it used to wear made every busy channel look like an emergency. A
// mention is the accent wash with the arroba in it, so the thing that is
// *about you* is the one carrying the colour that means "you"
// (docs/PLANO-DESIGN.md §3).
//
// The arroba is the character, not a glyph. The kit's `at` is not among the
// twenty-four we were given, and something drawn to look like it would be off
// by a pixel against the twenty-four that are real. It becomes an icon the day
// that glyph arrives and nothing else here changes.
function Badge({ n, mention, place = 'row' }: { n: number; mention: boolean; place?: 'row' | 'tab' }) {
	if (n <= 0) return null;
	const text = n > 99 ? '99+' : String(n);
	const pull = place === 'row' ? 'ml-auto ' : '';
	return mention ? (
		<span
			class={
				pull +
				'flex shrink-0 items-center gap-0.5 rounded-full bg-accent-soft text-accent-soft-fg ' +
				(place === 'row' ? 'h-6 px-2 text-label' : 'h-5 px-1.5 text-label')
			}
		>
			<Icon name="at" size={place === 'row' ? 12 : 10} />
			{text}
		</span>
	) : (
		<span
			class={
				pull +
				'h-6 min-w-6 shrink-0 rounded-full bg-foreground px-2 text-center text-meta font-semibold leading-6 text-surface'
			}
		>
			{text}
		</span>
	);
}

export function App() {
	usePreferences();
	const [me, setMe] = useState<User | null>(null);
	const [channels, setChannels] = useState<Channel[]>([]);
	// The folders of the column (D12a), and which of them this device keeps
	// closed — per device, like the people panel.
	const [categories, setCategories] = useState<Category[]>([]);
	const categoriesRef = useRef<Category[]>([]);
	categoriesRef.current = categories;
	const [closed, setClosed] = useState(closedFolders);
	// A name being typed: a new folder (id ''), or a new name for a room or a
	// folder. One panel for all three (NewChannel.tsx).
	const [naming, setNaming] = useState<{ kind: 'text' | 'voice' | 'folder'; id: string; name: string } | null>(null);
	// The drag in progress, if any: what is moving and where it would land.
	const [dragging, setDragging] = useState<{ type: 'room' | 'folder'; id: string; target: Drop | null } | null>(null);
	// Set for the click that follows a drag's release, which is not a click.
	const dragged = useRef(false);
	// Whether the pinned list of the open conversation is showing.
	const [pinsOpen, setPinsOpen] = useState(false);
	const [pinSerial, setPinSerial] = useState(0);
	const channelVersion = useRef(0);
	const [members, setMembers] = useState<User[]>([]);
	// The roles (D12c), whole, highest first: GET /api/roles once, then the
	// `roles` event replaces them. What this person may do is worked out from
	// them on every render, so a role given or taken shows in the buttons at
	// once; the server still checks every request.
	const [roles, setRoles] = useState<Role[]>([]);
	const allowed = permissionsOf(me, roles);
	const canManage = allowed.has('manage_channels');
	const canModerateMessages = allowed.has('manage_messages');
	const nameColor = (username: string) => nameClass(colorOf(username, roles));
	const [active, setActive] = useState('');
	const [loaded, setLoaded] = useState<Record<string, Loaded>>({});
	const [loadingMore, setLoadingMore] = useState(false);
	const [jump, setJump] = useState<{ channel: string; id: number; serial: number } | null>(null);
	const jumpSerial = useRef(0);
	const historyVersion = useRef<Record<string, number>>({});
	const paging = useRef(new Set<string>());
	const [online, setOnline] = useState(false);
	// Whether to *say* the connection is down, which is not the same as it
	// being down. See the dot below.
	const [showOffline, setShowOffline] = useState(false);
	const [settings, setSettings] = useState(false);
	// Which kind of channel is being named, if any.
	const [creating, setCreating] = useState<'text' | 'voice' | null>(null);
	// Whether the server is serving a build newer than this page (version.ts).
	// One-way: once it is true nothing takes it back, because nothing can.
	const [stale, setStale] = useState(false);
	const [drawer, setDrawer] = useState(false);
	// Whether the search panel is open.
	const [searching, setSearching] = useState(false);
	// The people list starts open, which is the owner's call and the right one:
	// on a server for twenty friends, "quem está aí" is most of what the app is
	// for, and a panel that has to be opened every visit is a panel nobody
	// opens.
	//
	// Not on a telephone, where it would leave a hundred and eighty pixels for
	// the conversation — and after that it is remembered per device, because
	// whether there is room for it is a fact about the screen.
	const [people, setPeople] = useState(savedPeople);
	const [error, setError] = useState('');
	// Who is in each voice room. Names and flags only: nothing about anybody's
	// connection is in the app's state (CLAUDE.md), it is all in voice.ts.
	const [rooms, setRooms] = useState<Record<string, VoicePeer[]>>({});
	// The read mark as it was when the conversation was opened, so the "não
	// lidas" line stays where it was instead of running away as messages are
	// read — **and the id of the conversation it belongs to**.
	//
	// The name matters as much as the number. Changing channel is two steps: the
	// screen is drawn with the new channel first, and the photograph of where you
	// stopped in it is taken a moment later. In between, a number on its own gets
	// used against the wrong conversation — one frame with the previous channel's
	// mark laid over this channel's messages, which is a red line that appears
	// and vanishes in about 25 ms. That is what the owner kept seeing, and it was
	// never the line being erased: it was a line that should never have been
	// drawn. Carrying the id means the mark can say "not mine" and draw nothing.
	const [opened, setOpened] = useState<{ channel: string; at: number }>({ channel: '', at: 0 });
	// Whose profile is open, by username. One panel, opened from anywhere a
	// person appears.
	const [profile, setProfile] = useState('');
	// Whose menu is open, and **where**: 'sala:ana', 'pessoas:ana', 'chamada:<block>'.
	// The menu is drawn inside the row that asked for it and needs no
	// coordinates (see PersonMenu.tsx for why that is a rule here and not a
	// preference). It carried only the name until 24/09/2026, and the same
	// person is a row in the room's list on the left *and* in the people on
	// the right — so both drew the menu, and a press on one was a press
	// "outside" the other, which closed them both before "Ver perfil" landed.
	const [menu, setMenu] = useState('');
	// Which half of the service the column is showing. The owner's request, and
	// the shape every service like this one has: the server on one side, the
	// conversations with people on the other, so that a channel and a person are
	// not two entries in the same list.
	const [space, setSpace] = useState<'server' | 'dms'>('server');
	const [columnStep, setColumnStep] = useState(savedColumn);
	// The list of who is missing from the room you are in. It lives here rather
	// than in the call because two places open it — the call's own row of
	// controls and the + on the room in the column — and two panels for one
	// list is one panel too many.
	const [inviting, setInviting] = useState(false);
	// The channel the owner asked to delete, while the box asks "sure?".
	const [deleting, setDeleting] = useState<Channel | null>(null);
	// The message being answered, if any. It lives here rather than in the
	// composer because it is chosen up in the conversation and used down in the
	// box, and because leaving the channel has to drop it.
	// Who you are answering, per conversation. It used to be one value for the
	// whole app, emptied whenever you changed channel; keeping it per
	// conversation is what makes leaving a conversation and coming back land you
	// exactly where you were, quotation included.
	//
	// It stays in memory and never reaches the disk: the sentence being answered
	// is somebody else's, and it is already on the server (drafts.ts).
	const [replies, setReplies] = useState<Record<string, Message>>({});

	// What was being written in each conversation, and the one piece of state
	// here that is a ref rather than a useState.
	//
	// That is deliberate. It is written on every keystroke, and a useState would
	// redraw the conversation above the box — the whole message list — once per
	// letter. Nothing renders from this map directly: the box reads its own
	// conversation once, when it is built, and owns it from then on.
	//
	// take() also **removes** from the disk what it read. From here the only copy
	// is this one, in memory, which is where an unsent sentence belongs while
	// somebody is looking at the screen (drafts.ts).
	// Filled in by the first load, once the server has said who this is.
	const drafts = useRef<Record<string, string>>({});
	// Who is online, who stepped away. "em chamada" is not in here: it is
	// derived below from the voice rooms this page already receives.
	const [presence, setPresence] = useState<Record<string, 'online' | 'away'>>({});
	// Somebody calling, while this app is open. With the app closed the same
	// call arrives as a notification from the service worker.
	const [ring, setRing] = useState<IncomingRing | null>(null);
	// Who is writing something, and until when. Kept as a moment rather than a
	// flag because the only honest way to switch it off is time: the person who
	// closed their laptop mid-sentence will never send the "stopped" that a
	// flag would be waiting for.
	const [typing, setTyping] = useState<Record<string, { name: string; until: number }>>({});
	// The one line that offers to switch notifications on, shown only after
	// something has arrived that a notification would have been useful for.
	const [nudge, setNudge] = useState(false);
	// Which of the two broadcast panels is open. Starting a screen or a camera,
	// changing it and stopping it all happen in one place, opened by the button
	// that starts it — the owner's design.
	const [panel, setPanel] = useState<'' | 'screen' | 'camera'>('');

	const activeRef = useRef('');
	activeRef.current = active;
	// The channel list as it is *now*, for the live socket's handler: that
	// closure is built once and would otherwise be looking at the list as it
	// was when the socket opened.
	const channelsRef = useRef<Channel[]>([]);
	channelsRef.current = channels;
	const membersRef = useRef<User[]>([]);
	membersRef.current = members;
	// Messages that arrived while this conversation's history was still on its
	// way. The window is small and the loss was total: a page of history is a
	// photograph taken before the message existed, and it was landing *after*
	// the message did and quietly replacing it — so somebody who opened the app
	// in the same half-second as somebody else spoke never saw that line at
	// all, until a reload. A key exists here only while a fetch is in flight.
	//
	// Changes are kept too — an edit, a deletion, a reaction. The page that is
	// coming was taken before them, and landing it as it was brought a deleted
	// message back on the screen (Codex, 23/09/2026).
	const arrived = useRef<Record<string, { msg: Message; changed: boolean }[]>>({});
	// The read mark on its way to the server, per conversation, so the effect
	// that sends it does not send it again on every render while it travels.
	const marking = useRef<Record<string, number>>({});
	// The first load, and how many times it has been tried. A failure used to
	// leave "Carregando…" on the screen for ever: the error was shown, cleared
	// five seconds later like any other, and nothing ever asked again.
	const [bootTry, setBootTry] = useState(0);
	const [bootError, setBootError] = useState('');

	// The voice session, as plain data. No stream ever comes through here: the
	// call asks media.ts for the elements directly.
	const view = useVoice();
	// Entering a room folds the people column away (the owner, 24/09/2026:
	// "ao entrar na chamada, esconder a aba de pessoas online"). Only on the
	// way in — it is not remembered, and opening it again during the call
	// keeps it open.
	const inRoom = view.state !== 'off' ? view.channelID : '';
	useEffect(() => {
		if (inRoom) setPeople(false);
	}, [inRoom]);

	const current = channels.find((c) => c.id === active);

	const refresh = useCallback(async (afterGap = false) => {
		const version = channelVersion.current;
		const res = await api.channels();
		// A live channel event received during this fetch is newer than its
		// snapshot. The email confirmation reload makes this startup race easy
		// to hit: do not erase a room that just arrived over the socket.
		if (version === channelVersion.current) {
			setChannels(res.channels);
			setCategories(res.categories ?? []);
		}
		try {
			setRoles((await api.roles()).roles);
		} catch {
			// The names keep the colours they had.
		}
		try {
			// The people too: somebody who accepted an invite after this page
			// loaded is somebody this page has never heard of, and the profile
			// panel would have nothing to show for them.
			const people = remember((await api.members()).members);
			setMembers(people);
			setMe((cur) => {
				const avatar = people.find((u) => u.username === cur?.username)?.avatar;
				return cur && avatar !== undefined && avatar !== cur.avatar ? { ...cur, avatar } : cur;
			});
		} catch {
			// The list that is already drawn is not wrong, only older.
		}
		try {
			const voiceSession = voice.snapshot().sessionID;
			const rooms = (await api.voiceRooms()).rooms;
			setRooms(rooms);
			// After a gap in the socket, the call checks itself against the
			// room: an offer sent meanwhile was lost (media.ts reconcile).
			if (afterGap) voice.reconcile(rooms, voiceSession);
		} catch {
			// A server without voice is still a server. The rooms simply stay
			// empty and the column draws them as empty.
		}
		try {
			setPresence((await api.presence()).presence);
		} catch {
			// The dots go grey, which is the safe way to be wrong.
		}
		return res.channels;
	}, []);

	// ------------------------------------------------------------ load

	useEffect(() => {
		let retry: ReturnType<typeof setTimeout> | undefined;
		(async () => {
			try {
				const [who, list, ppl] = await Promise.all([api.me(), api.channels(), api.members()]);
				setBootError('');
				// Before anything is drawn: the box reads its conversation's
				// draft once, when it is built, and it is built from here.
				drafts.current = take(who.user.username);
				setMe(who.user);
				// media.ts labels this device's own pictures with the same
				// name everybody else's carry.
				voice.identify(who.user.username, who.user.display_name);
				setChannels(list.channels);
				setCategories(list.categories ?? []);
				try {
					setRoles((await api.roles()).roles);
				} catch {
					// No colours and no role buttons; the server decides anyway.
				}
				setMembers(remember(ppl.members));
				try {
					setRooms((await api.voiceRooms()).rooms);
				} catch {
					// see refresh()
				}
				try {
					setPresence((await api.presence()).presence);
				} catch {
					// see refresh()
				}
				// Where to land. A notification that was tapped says so in the
				// address bar: ?c= is a conversation to open, ?join= is a voice
				// room to walk into.
				const wanted = new URLSearchParams(location.search);
				const open = wanted.get('c') ?? '';
				const join = wanted.get('join') ?? '';
				// ?call= is a direct call that was ringing before this page
				// existed — the app opened from the notification, telephone in
				// hand. The live event went to the screens that were already
				// open, and this was not one of them, so it is asked for.
				const calling = wanted.get('call') ?? '';
				if (calling) void ringingCall(calling);
				const first =
					list.channels.find((c) => c.id === (open || join)) ??
					list.channels.find((c) => c.kind === 'text') ??
					list.channels[0];
				if (first) setActive(first.id);
				if (join && list.channels.some((c) => c.id === join && c.kind === 'voice')) {
					log('entrando pela notificacao');
					void voice.join(join);
				}
				if (open || join || wanted.get('call')) history.replaceState(null, '', '/');
				log(`entrou como @${who.user.username}`);
			} catch (err) {
				if ((err as { status?: number }).status === 401) {
					location.href = '/login';
					return;
				}
				// A network that dropped for a moment is the usual cause, so it
				// is asked again by itself, waiting a little longer each time.
				setBootError(message(err));
				const wait = Math.min(30_000, 2_000 * 2 ** bootTry);
				log(`carga inicial falhou, de novo em ${wait / 1000}s`);
				retry = setTimeout(() => setBootTry((n) => n + 1), wait);
			}
		})();
		return () => clearTimeout(retry);
		// eslint-disable-next-line react-hooks/exhaustive-deps
	}, [bootTry]);

	// ------------------------------------------------------------ live

	useEffect(() => {
		if (!me) return;
		const gw = new Gateway(
			(ev) => {
				if (ev.type === 'person_preferences') { void refreshPreferences().then(() => refresh()).catch(err => setError(message(err))); }
				else if (ev.type === 'message') {
					const msg = ev.data as Message;
					// A message in a conversation this page has never heard of
					// means the column is out of date, so it is fetched again.
					//
					// The column is normally kept current by a "channel" event,
					// sent once, when the conversation is created. Once is
					// enough right up until it is not: a socket that happens to
					// be reconnecting in that instant misses it, and then the
					// conversation does not appear until the page is reloaded —
					// somebody writes to you and nothing shows up. A message
					// arriving is the signal that is always there, and asking
					// again costs one request in the rare case and nothing at
					// all in the ordinary one.
					if (!channelsRef.current.some((c) => c.id === msg.channel_id)) void refresh();
					// If this conversation's history is in the air right now, hold
					// on to the message: the page that is coming does not have it,
					// and is about to become the whole truth.
					arrived.current[msg.channel_id]?.push({ msg, changed: false });
					setLoaded((cur) => {
						const page = cur[msg.channel_id];
						if (!page) return cur;
						if (page.hasNewer) return cur;
						if (page.messages.some((m) => m.id === msg.id)) return cur;
						return { ...cur, [msg.channel_id]: { ...page, messages: [...page.messages, msg] } };
					});
					// Something arrived that a notification would have been
					// useful for. This is the only moment the app offers to ask
					// for permission, and it offers — it does not ask.
					const forMe =
						preferencesReady() && !isBlocked(msg.author.username) && msg.author.username !== me.username &&
						(msg.mentions.includes(me.username) ||
							channelsRef.current.find((c) => c.id === msg.channel_id)?.kind === 'dm');
					// And the sound, for what you are *not* looking at. Somebody else's
					// message, in a conversation that is not silenced, that is either
					// somewhere else, behind another window, or has your name in it.
					// Anything less careful than this is the noise that makes people
					// turn the sound off and then miss the one that mattered.
					const silenced = !preferencesReady() || isBlocked(msg.author.username) || channelsRef.current.find((c) => c.id === msg.channel_id)?.muted;
					const elsewhere =
						msg.channel_id !== activeRef.current ||
						document.visibilityState !== 'visible' ||
						!document.hasFocus();
					// A pin notice is news, not a call for attention: it counts
					// as unread but makes no sound.
					if (
						msg.author.username !== me.username &&
						msg.kind !== 'pin' &&
						!silenced &&
						(elsewhere || msg.mentions.includes(me.username))
					) {
						blip();
					}
					if (forMe && !nudgeDismissed(me.username)) {
						void notifyStatus().then((st) => {
							if (st.state === 'off') setNudge(true);
						});
					}
					setChannels((cur) =>
						cur.map((c) => {
							if (c.id !== msg.channel_id) return c;
							// A silenced channel is counted nowhere, and that has
							// to be true *here* as well as on the server. The
							// server does not count it (store.ListChannels), but
							// the page keeps its own running total between
							// refreshes — a message arriving down the socket bumps
							// it without asking anybody — so the silence has to be
							// respected in both places or it only lasts until the
							// next thing somebody says.
							const quiet =
								c.muted || isBlocked(msg.author.username) ||
								msg.channel_id === activeRef.current ||
								msg.author.username === me.username;
							return {
								...c,
								last_message_id: msg.id,
								unread: quiet ? c.unread : c.unread + 1,
								mentions:
									!c.muted && !isBlocked(msg.author.username) &&
									msg.channel_id !== activeRef.current &&
									msg.mentions.includes(me.username)
										? c.mentions + 1
										: c.mentions,
							};
						}),
					);
				} else if (ev.type === 'message_changed') {
					const msg = ev.data as Message;
					setPinSerial((n) => n + 1);
					arrived.current[msg.channel_id]?.push({ msg, changed: true });
					setLoaded((cur) => {
						const page = cur[msg.channel_id];
						if (!page) return cur;
						return {
							...cur,
							[msg.channel_id]: {
								...page,
								messages: page.messages.map((m) => (m.id === msg.id ? msg : m)),
							},
						};
					});
				} else if (ev.type === 'typing') {
					const who = ev.data as {
						channel_id: string;
						username: string;
						display_name: string;
					};
					if (who.username !== me.username) {
						setTyping((cur) => ({
							...cur,
							[who.channel_id + '/' + who.username]: {
								name: who.display_name || who.username,
								until: Date.now() + 5000,
							},
						}));
					}
				} else if (ev.type === 'channel') {
					channelVersion.current++;
					const ch = ev.data as Channel;
					// A channel already in the column is *updated*, not ignored.
					// What arrives is the server's current view of it — the
					// other person's name, whether it is silenced — and the
					// only things this page knows better are the two counters
					// it keeps running between refreshes.
					setChannels((cur) =>
						cur.some((c) => c.id === ch.id)
							? cur.map((c) =>
									c.id === ch.id ? { ...ch, unread: c.unread, mentions: c.mentions } : c,
								)
							: [...cur, ch],
					);
				} else if (ev.type === 'layout') {
					// The owner moved something, or made, renamed or deleted a
					// folder. The whole shape comes every time, so this page ends
					// up with the same picture as every other one.
					channelVersion.current++;
					const layout = ev.data as Layout;
					setCategories(layout.categories);
					setChannels((cur) => apply(cur, layout));
				} else if (ev.type === 'roles') {
					// A role was made, changed, moved, deleted, given or taken.
					// The whole list comes every time, like the column's layout.
					const list = (ev.data as { roles: Role[] }).roles;
					setRoles(list);
					log('roles: event, ' + list.length + ' roles');
				} else if (ev.type === 'channel_removed') {
					channelVersion.current++;
					// The owner deleted a channel or a room. It leaves the column
					// at once, and whoever was reading it lands on another text
					// channel instead of on a pane for something that is gone.
					const { id } = ev.data as { id: string };
					const left = channelsRef.current.filter((c) => c.id !== id);
					setChannels((cur) => cur.filter((c) => c.id !== id));
					if (activeRef.current === id) {
						const next = left.find((c) => c.kind === 'text');
						if (next) {
							setActive(next.id);
							setDrawer(false);
						}
					}
					// The server refuses a room with people in it, so this is
					// only the race of somebody joining in the same instant.
					const now = voice.snapshot();
					if (now.channelID === id && now.state !== 'off') void voice.leave();
				} else if (ev.type === 'call') {
					// Somebody is ringing this person. It arrives on every device
					// they have open, and whichever one answers becomes the call.
					const call = ev.data as { id: string; from: { username: string; display_name: string } };
					direct.called(call);
				} else if (ev.type === 'call_accepted') {
					const { peer } = ev.data as { peer: { username: string; display_name: string } };
					void direct.accepted(peer);
				} else if (ev.type === 'call_signal') {
					// An offer, an answer or a candidate from the other browser.
					// This page does not read it either: it hands it to the
					// connection exactly as it arrived.
					direct.signal(ev.data as never);
				} else if (ev.type === 'call_state') {
					direct.peerState(ev.data as { mic: boolean; cam: boolean; screen: boolean });
				} else if (ev.type === 'call_taken') {
					direct.taken((ev.data as { id: string }).id);
				} else if (ev.type === 'call_ended') {
					const { id, reason } = ev.data as { id: string; reason: string };
					direct.ended(id, reason);
				} else if (ev.type === 'profile') {
					const { username, avatar } = ev.data as { username: string; avatar: string };
					updateAvatar(username, avatar);
				} else if (ev.type === 'presence') {
					const { username, state } = ev.data as { username: string; state: string };
					// Somebody this page has never heard of just opened the app:
					// a friend who accepted an invite after this tab loaded.
					// Their name belongs in the list before their dot does.
					if (!membersRef.current.some((m) => m.username === username)) {
						void api
							.members()
							.then((res) => setMembers(remember(res.members)))
							.catch(() => undefined);
					}
					setPresence((cur) => {
						const next = { ...cur };
						if (state === 'online' || state === 'away') next[username] = state;
						else delete next[username];
						return next;
					});
				} else if (ev.type === 'ring') {
					const incoming = ev.data as IncomingRing;
					log(`chamada de @${incoming.from.username}`);
					setRing(incoming);
				} else if (ev.type === 'voice_room') {
					const { channel_id, peers } = ev.data as { channel_id: string; peers: VoicePeer[] };
					setRooms((cur) => ({ ...cur, [channel_id]: peers }));
					voice.roomSeen(channel_id, peers);
				} else if (ev.type === 'voice_watchers') {
					// Whether anybody is receiving this device's screen. Since
					// watching became a decision, a screen can be shared into a
					// room where nobody asked for it, and there is no reason to
					// upload it.
					void voice.screenWatchers(ev.data);
				} else if (ev.type === 'voice_offer') {
					// The server's half of the negotiation. It is handed
					// straight to voice.ts, which decides whether it is for
					// this device's session.
					voice.offer(ev.data);
				} else if (ev.type === 'read') {
					const { channel_id, last_read_id } = ev.data as {
						channel_id: string;
						last_read_id: number;
					};
					setChannels((cur) =>
						cur.map((c) =>
							c.id === channel_id
								? { ...c, last_read_id, unread: 0, mentions: 0 }
								: c,
						),
					);
				}
			},
			() => {
				// The socket is open: whatever happened before it was did not
				// arrive on it. True after a gap, and true on the first open as
				// well — the page and the socket load at the same time, and
				// nothing decides which of them wins.
				log('socket aberto, recarregando');
				void refreshPreferences().then(() => refresh()).catch(err => setError(message(err)));
				void refresh(true);
				const id = activeRef.current;
				// Whatever happened while the socket was down happened in every
				// conversation, not only this one. The others are forgotten, so
				// opening one asks the server again instead of showing the copy
				// from before the gap.
				setLoaded((cur) => (id && cur[id] ? { [id]: cur[id] } : {}));
				if (id) void openHistory(id, true);
			},
			(up) => {
				setOnline(up);
				// A socket that closes is the network, or a session that ended:
				// signed out elsewhere, a new password, the account switched off
				// by the owner (D9). The socket cannot say which — a refused
				// upgrade has no status a page can read — so the page asks, and
				// a 401 goes to the login the way the first load does, instead of
				// reconnecting for ever to a door that will not open.
				if (!up) {
					void api.me().catch((err) => {
						if ((err as { status?: number }).status === 401) location.href = '/login';
					});
				}
			},
		);
		gw.start();
		return () => gw.stop();
	}, [me?.username, refresh]);

	// The measurement of where the screen ends and where the box you type in is.
	// Taken once the app is drawn, and again whenever the screen changes size —
	// which is the keyboard opening, the phone turning, and the browser's own
	// bars sliding in and out.
	useEffect(() => {
		if (!me) return;
		let timer: number | undefined;
		const measure = () => {
			clearTimeout(timer);
			timer = setTimeout(rememberGeometry, 300) as unknown as number;
		};
		// Twice on the way in, and the second one is not belt and braces: the
		// first measurement of the owner's telephone said "escrever AUSENTE",
		// which was true and misleading — the history had not arrived yet, so
		// there was no box on the page to find. A measurement that can be read
		// as a different bug is worse than none.
		measure();
		const late = setTimeout(rememberGeometry, 2500) as unknown as number;
		window.addEventListener('resize', measure);
		window.visualViewport?.addEventListener('resize', measure);
		return () => {
			clearTimeout(timer);
			clearTimeout(late);
			window.removeEventListener('resize', measure);
			window.visualViewport?.removeEventListener('resize', measure);
		};
	}, [me]);

	// --------------------------------------------------- presence and the phone

	const reporter = useRef<Presence | null>(null);
	useEffect(() => {
		if (!me) return;
		const p = new Presence();
		reporter.current = p;
		p.start();
		return () => {
			p.stop();
			reporter.current = null;
		};
	}, [me]);

	// Being in a call is being in the app, whichever window happens to be on
	// top — the owner's words, 13/09/2026. Both kinds count: a voice room and a
	// direct call are the same thing to somebody looking at the dot.
	const call = useDirect();
	const inCall =
		(view.state !== 'off' && !!view.channelID) ||
		call.state === 'live' ||
		call.state === 'connecting';
	useEffect(() => {
		reporter.current?.setBusy(inCall);
	}, [inCall]);

	useEffect(() => {
		if (!me) return;
		// Registering the worker asks nothing of anybody and shows nothing. It
		// is what makes the app installable, and what makes turning
		// notifications on later one press instead of two.
		void registerWorker();
		if (!('serviceWorker' in navigator)) return;
		// A notification tapped while the app was already open arrives here
		// instead of as a fresh page load.
		const onMessage = (ev: MessageEvent) => {
			const data = ev.data as { t?: string; url?: string };
			if (data?.t !== 'open' || !data.url) return;
			const url = new URL(data.url, location.origin);
			const wanted = url.searchParams.get('c') ?? url.searchParams.get('join') ?? '';
			const join = url.searchParams.get('join') ?? '';
			// The same notification, tapped while the app happens to be open.
			const calling = url.searchParams.get('call') ?? '';
			if (calling) {
				void ringingCall(calling);
				return;
			}
			if (!wanted) return;
			log('notificacao tocada com o app aberto');
			setActive(wanted);
			setDrawer(false);
			if (join) void voice.join(join);
		};
		navigator.serviceWorker.addEventListener('message', onMessage);
		return () => navigator.serviceWorker.removeEventListener('message', onMessage);
	}, [me]);

	// Is this page the build the server is serving?
	//
	// Only once somebody is signed in: before that there is a login screen on
	// the glass, whoever is looking at it has just arrived, and a page that
	// has been open for ten seconds cannot be out of date.
	const askVersion = useRef<(() => void) | null>(null);
	useEffect(() => {
		if (!me) return;
		const watch = watchForNewBuild(() => setStale(true));
		askVersion.current = watch.check;
		return () => {
			askVersion.current = null;
			watch.stop();
		};
	}, [me]);

	// And ask again every time the socket comes back.
	//
	// This is the trigger that does not depend on anything firing: a deploy
	// restarts the server, so every page that is open loses its socket and
	// gets it back a second later. The reconnection *is* the deploy, and the
	// watcher's own floor keeps the one that happens on every page load from
	// costing a request.
	useEffect(() => {
		if (online) askVersion.current?.();
	}, [online]);

	// ------------------------------------------------------------ history

	const openHistory = useCallback(async (id: string, force = false, around?: number) => {
		if (!force && loaded[id]) return true;
		const version = (historyVersion.current[id] ?? 0) + 1;
		historyVersion.current[id] = version;
		// From here until the answer lands, anything the socket delivers for
		// this conversation is kept (see `arrived` above).
		const buffered: { msg: Message; changed: boolean }[] = [];
		arrived.current[id] = buffered;
		try {
			const res = await api.history(id, undefined, around ? { around } : undefined);
			if (historyVersion.current[id] !== version) return false;
			const late = buffered;
			// In the order they arrived, so the last word on each message wins.
			// A new message is added if the page does not have it; a change only
			// replaces what the page has — one to a message older than the page
			// is not this page's to show.
			const byId = new Map(res.messages.map((m) => [m.id, m]));
			for (const { msg, changed } of late) {
				if (changed) {
					if (byId.has(msg.id)) byId.set(msg.id, msg);
				} else if (!res.has_newer || byId.has(msg.id)) {
					byId.set(msg.id, msg);
				}
			}
			const messages = [...byId.values()].sort((a, b) => a.id - b.id);
			setLoaded((cur) => ({ ...cur, [id]: { messages, hasMore: res.has_more, hasNewer: !!res.has_newer } }));
			return true;
		} catch (err) {
			if (historyVersion.current[id] === version) setError(message(err));
			return false;
		} finally {
			if (arrived.current[id] === buffered) delete arrived.current[id];
		}
	}, [loaded]);

	async function jumpToMessage(id: string, messageID: number) {
		const serial = ++jumpSerial.current;
		const startedAt = activeRef.current;
		const present = loaded[id]?.messages.some((m) => m.id === messageID && !m.deleted);
		if (!present && !(await openHistory(id, true, messageID))) return;
		if (jumpSerial.current !== serial || activeRef.current !== startedAt) return;
		setJump({ channel: id, id: messageID, serial });
		setActive(id);
		setDrawer(false);
	}

	async function goRecent() {
		const id = active;
		if (!(await openHistory(id, true))) return;
		setJump({ channel: id, id: 0, serial: ++jumpSerial.current });
	}

	// An error is news, not a state of the world. It used to sit at the top of
	// the conversation until the page was reloaded, which made a passing "não
	// encontrado" look like something that was still wrong. Five seconds, and an
	// X for whoever wants it gone now — the owner's own words.
	useEffect(() => {
		if (!error) return;
		const timer = setTimeout(() => setError(''), 5000);
		return () => clearTimeout(timer);
	}, [error]);

	// The photograph is taken once per arrival. Everything after that moment —
	// reading the channel, which moves the mark on the server, and the server
	// saying so back down the socket — must not retake it, or the line is erased
	// by the act of looking at it.
	useEffect(() => {
		if (!active) return;
		const ch = channels.find((c) => c.id === active);
		if (ch?.kind === 'voice') {
			// A room has no history and no line — but stepping into one is still
			// leaving the conversation, and coming back from it is an arrival
			// that deserves a new photograph.
			setOpened({ channel: active, at: 0 });
			return;
		}
		if (opened.channel !== active) {
			setOpened({ channel: active, at: ch?.last_read_id ?? 0 });
		}
		void openHistory(active);
		// eslint-disable-next-line react-hooks/exhaustive-deps
	}, [active]);

	// Leaving a conversation drops the answer being written in it, and the
	// column follows wherever the conversation is: opening a private one from a
	// profile, from a notification or from a link has to move the rail with it,
	// or the column ends up pointing at the half of the service you are not in.
	//
	// Only `active` is watched on purpose. The channel list is a new array on
	// every refresh, and watching it would move the rail back under somebody
	// who had just moved it themselves.
	useEffect(() => {
		const c = channelsRef.current.find((x) => x.id === active);
		if (c) setSpace(c.kind === 'dm' ? 'dms' : 'server');
		// eslint-disable-next-line react-hooks/exhaustive-deps
	}, [active]);

	// Reading is what clears the badge: the channel is open, so the newest
	// message in it has been seen.
	useEffect(() => {
		const page = loaded[active];
		const c = channels.find((x) => x.id === active);
		if (!page || !c || page.messages.length === 0 || page.hasNewer) return;
		const newest = page.messages[page.messages.length - 1].id;
		if (newest <= c.last_read_id) return;
		if ((marking.current[active] ?? 0) >= newest) return;
		// Moved here only once the server has it. Moving it first made a
		// refused mark look done: the badge cleared, the number here said read,
		// and nothing ever sent it again — while every other device, and the
		// notifications, went on counting it as unread.
		const id = active;
		marking.current[id] = newest;
		api
			.markRead(id, newest)
			.then(() => {
				setChannels((cur) =>
					cur.map((x) =>
						x.id === id && x.last_read_id < newest
							? { ...x, last_read_id: newest, unread: 0, mentions: 0 }
							: x,
					),
				);
			})
			.catch(() => {
				// Tried again the next time anything here changes.
				if (marking.current[id] === newest) delete marking.current[id];
			});
	}, [active, loaded, channels]);

	async function loadMore(newer = false) {
		const page = loaded[active];
		if (!page || page.messages.length === 0 || paging.current.has(active)) return;
		const id = active;
		const version = historyVersion.current[id];
		const buffered: { msg: Message; changed: boolean }[] = [];
		arrived.current[id] = buffered;
		paging.current.add(id);
		setLoadingMore(true);
		try {
			const res = newer
				? await api.history(id, undefined, { after: page.messages[page.messages.length - 1].id })
				: await api.history(id, page.messages[0].id);
			if (historyVersion.current[id] !== version) return;
			// Added to the page as it is **now**, not as it was when the request
			// left: whatever the socket delivered meanwhile — a new message, an
			// edit, a deletion — is newer than both, and building from the old
			// copy threw it away.
			setLoaded((cur) => {
				const now = cur[id];
				if (!now) return cur;
				const byId = new Map(res.messages.map((m) => [m.id, m]));
				for (const { msg, changed } of buffered) {
					if (byId.has(msg.id) || (newer && !res.has_newer && !changed)) byId.set(msg.id, msg);
				}
				// The current page wins over a snapshot for rows already shown.
				for (const msg of now.messages) byId.set(msg.id, msg);
				const messages = [...byId.values()].sort((a, b) => a.id - b.id);
				return { ...cur, [id]: {
					...now,
					messages,
					hasMore: newer ? now.hasMore : res.has_more,
					hasNewer: newer ? !!res.has_newer : now.hasNewer,
				} };
			});
		} catch (err) {
			setError(message(err));
		} finally {
			if (arrived.current[id] === buffered) delete arrived.current[id];
			paging.current.delete(id);
			setLoadingMore(false);
		}
	}

	// ------------------------------------------------------------ actions

	async function openDM(username: string) {
		try {
			const ch = await api.openDM(username);
			setChannels((cur) => (cur.some((c) => c.id === ch.id) ? cur : [...cur, ch]));
			setActive(ch.id);
			setDrawer(false);
			setPeople(false);
		} catch (err) {
			setError(message(err));
		}
	}

	// Opens the panel that names it (NewChannel.tsx). A refusal from the server
	// is thrown back to the panel, which shows it under the box it was typed
	// in rather than in the app's error strip.
	function newChannel(kind: 'text' | 'voice' = 'text') {
		setCreating(kind);
	}
	async function createChannel(kind: 'text' | 'voice', name: string) {
		const ch = await api.createChannel(name, '', kind);
		await refresh();
		if (kind === 'text') setActive(ch.id);
	}

	const totalMentions = useMemo(
		() => channels.reduce((n, c) => n + c.mentions + (c.kind === 'dm' ? c.unread : 0), 0),
		[channels],
	);
	useEffect(() => {
		document.title = totalMentions > 0 ? `(${totalMentions}) harmony` : 'harmony';
	}, [totalMentions]);

	// The drafts touch the disk here, and only here.
	//
	// Written when the page is hidden or going away, deleted when it comes back
	// to the front — so an unsent sentence exists in a file only while the app is
	// not in front of you, which is the one window it is needed for. A telephone
	// throws the page out of memory while you are away, and `pagehide` is the
	// signal actually delivered when it does; `unload` is not, on a telephone,
	// which is why it is not used.
	//
	// drop() names the conversations this tab is holding rather than wiping the
	// lot, so a second tab's drafts are not taken down with them.
	useEffect(() => {
		const out = () => keep(drafts.current);
		const back = () => drop(Object.keys(drafts.current));
		const onVisible = () => (document.visibilityState === 'hidden' ? out() : back());
		document.addEventListener('visibilitychange', onVisible);
		window.addEventListener('pagehide', out);
		return () => {
			document.removeEventListener('visibilitychange', onVisible);
			window.removeEventListener('pagehide', out);
		};
	}, []);

	// A file dropped on this page never replaces this page.
	//
	// The browser's own answer to a dropped file is to open it, which throws the
	// app away — the sentence being typed, the call in progress, all of it — and
	// it does that anywhere the drop is not refused. The composer refuses it
	// properly, and puts the file in the message; this only catches the rest of
	// the time, when there is no box to type in: a voice room on screen, a panel
	// open, a conversation not chosen yet. Two listeners, one job, and both of
	// them run before the browser gets its turn.
	useEffect(() => {
		const stop = (e: DragEvent) => {
			if (!Array.from(e.dataTransfer?.types ?? []).includes('Files')) return;
			e.preventDefault();
		};
		window.addEventListener('dragover', stop);
		window.addEventListener('drop', stop);
		return () => {
			window.removeEventListener('dragover', stop);
			window.removeEventListener('drop', stop);
		};
	}, []);

	// Sweeping up the people who stopped typing. One timer for the lot, and it
	// only runs while somebody is actually writing: a service where nobody is
	// typing costs nothing at all.
	useEffect(() => {
		if (Object.keys(typing).length === 0) return;
		const timer = setInterval(() => {
			const now = Date.now();
			setTyping((cur) => {
				const next: typeof cur = {};
				for (const [k, v] of Object.entries(cur)) if (v.until > now) next[k] = v;
				return Object.keys(next).length === Object.keys(cur).length ? cur : next;
			});
		}, 1000);
		return () => clearInterval(timer);
	}, [typing]);

	// The dot waits two seconds before claiming anything is wrong.
	//
	// Every load starts disconnected — the socket takes a moment — so a dot
	// that draws the instant `online` is false is a dot that greets everybody
	// with a warning about nothing, every single time the page opens. That is
	// what the owner saw: "quando eu recarrego o app a bolinha ainda fica".
	//
	// Two seconds is longer than an ordinary connection takes and shorter than
	// anybody's patience, so what is left is the case it was built for: the
	// connection really is down and is staying down.
	useEffect(() => {
		if (online) {
			setShowOffline(false);
			return;
		}
		const timer = setTimeout(() => setShowOffline(true), 2000);
		return () => clearTimeout(timer);
	}, [online]);

	if (!me) {
		return (
			<div class="grid h-full place-items-center text-foreground-muted">
				{bootError ? (
					<div class="space-y-2 text-center">
						<p class="text-danger-soft-fg">Não consegui carregar: {bootError}</p>
						<p>Tentando de novo…</p>
						<button
							class="h-9 rounded-control bg-default px-4 text-meta font-medium text-foreground hover:bg-default-hover"
							onClick={() => setBootTry((n) => n + 1)}
						>
							Tentar agora
						</button>
					</div>
				) : error ? (
					<p class="text-danger-soft-fg">{error}</p>
				) : (
					<p>Carregando…</p>
				)}
			</div>
		);
	}

	// whereIs answers "is this person in a voice room, and which one" from the
	// same list the column draws. It is how the people list can show a speaker
	// next to somebody, and how the profile panel knows what to offer.
	function whereIs(username: string): { channel: Channel; peer: VoicePeer } | null {
		for (const [id, people] of Object.entries(rooms)) {
			const peer = people.find((p) => p.username === username);
			const channel = channels.find((c) => c.id === id);
			if (peer && channel) return { channel, peer };
		}
		return null;
	}

	// A profile event updates every list that can already be drawing this face.
	function updateAvatar(username: string, avatar: string) {
		setMe((cur) => cur?.username === username && cur.avatar !== avatar ? { ...cur, avatar } : cur);
		setMembers((cur) => cur.map((u) => u.username === username ? { ...u, avatar } : u));
		setChannels((cur) => cur.map((c) =>
			c.peer?.username === username ? { ...c, peer: { ...c.peer, avatar } } : c,
		));
		setRooms((cur) => Object.fromEntries(Object.entries(cur).map(([id, peers]) => [
			id, peers.map((p) => p.username === username ? { ...p, avatar } : p),
		])));
	}

	// personState is the dot. "em chamada" wins over everything else, because it
	// is the most useful thing to know about somebody, and it comes from the
	// rooms this page already has rather than from an answer the server would
	// have to decide what to reveal in.
	function personState(username: string): PersonState {
		if (whereIs(username)) return 'in_call';
		return presence[username] ?? 'offline';
	}

	// The things you can do to a person, in one place, because they are now
	// offered from two: the profile panel and the menu on the person's row. Whether each one is drawn is a courtesy to the reader — every
	// one of them is checked again by the server (api/ring.go wants the caller
	// inside the room and the target a member of the server; a conversation with
	// yourself answers the same 404 as a name that does not exist).
	const callInto = async (username: string) => {
		try {
			await api.ring(view.channelID, username);
			log(`chamou @${username}`);
		} catch (err) {
			setError(message(err));
		}
	};

	const canCall = (username: string) =>
		!!view.channelID &&
		view.state !== 'off' &&
		username !== me.username &&
		whereIs(username)?.channel.id !== view.channelID;

	// What the menu offers about a channel or a conversation. Two things, and
	// both are this person's own opinion about a shared thing: nobody else's
	// column changes, and nothing is deleted by either of them.
	const channelMenuFor = (c: Channel): PersonMenuItem[] => {
		// A conversation is a channel *and* a person, so its menu is both. They
		// are one menu and not two because the row is one row: a right click has
		// to have one answer.
		const items: PersonMenuItem[] = c.peer ? menuFor(c.peer.username) : [];
		items.push(
			{
				label: c.muted ? 'Voltar a avisar' : 'Silenciar',
				onPick: async () => {
					try {
						await api.mute(c.id, !c.muted);
						await refresh();
					} catch (err) {
						setError(message(err));
					}
				},
			},
		);
		if (c.kind === 'dm') {
			items.push({ label: 'Fechar conversa', danger: true, onPick: () => void closeDM(c) });
		}
		// Deleting is the owner's, and asks first: it is the one entry on this
		// menu that changes the column of everybody else.
		if (canManage && c.kind !== 'dm') {
			items.push({ label: 'Renomear canal', onPick: () => setNaming({ kind: 'text', id: c.id, name: c.name }) });
			items.push({ label: 'Apagar canal', danger: true, onPick: () => setDeleting(c) });
		}
		return items;
	};

	// The room's menu is only the owner's: silencing is about text, and a room
	// has none. Nobody else gets a menu on a room at all.
	const roomMenuFor = (c: Channel): PersonMenuItem[] =>
		canManage
			? [
					{ label: 'Renomear canal', onPick: () => setNaming({ kind: 'voice', id: c.id, name: c.name }) },
					{ label: 'Apagar canal', danger: true, onPick: () => setDeleting(c) },
				]
			: [];

	// A folder's menu, and the column heading's: the owner's alone (D12a).
	// Deleting a folder deletes nothing in it — the rooms go to the top of the
	// column (store.DeleteCategory) — so it does not ask first.
	const newFolder: PersonMenuItem = { label: 'Nova categoria', onPick: () => setNaming({ kind: 'folder', id: '', name: '' }) };
	const folderMenuFor = (f: Category): PersonMenuItem[] => [
		{ label: 'Renomear categoria', onPick: () => setNaming({ kind: 'folder', id: f.id, name: f.name }) },
		newFolder,
		{
			label: 'Apagar categoria',
			danger: true,
			onPick: async () => {
				try {
					await api.removeCategory(f.id);
					await refresh();
				} catch (err) {
					setError(message(err));
				}
			},
		},
	];

	async function saveName(what: { kind: 'text' | 'voice' | 'folder'; id: string }, name: string) {
		if (what.kind === 'folder') {
			if (what.id) await api.renameCategory(what.id, name);
			else await api.createCategory(name);
		} else {
			const ch = await api.renameChannel(what.id, name);
			setChannels((cur) => cur.map((c) => (c.id === ch.id ? { ...c, name: ch.name } : c)));
		}
		await refresh();
	}

	const toggleFolder = (id: string) =>
		setClosed((cur) => {
			const next = new Set(cur);
			if (!next.delete(id)) next.add(id);
			saveClosedFolders(next);
			return next;
		});

	// ------------------------------------------------------------ dragging
	//
	// By hand, with the browser's own pointer events and no library (the
	// owner's rule for D12a). Owner only, mouse and pen only: on a telephone a
	// finger on the list is a scroll, and telling the two apart is the
	// Android app's job (D12b). A press becomes a drag after six pixels, so a
	// click is still a click. Where it would land is worked out from the row
	// under the pointer (data-drop-room / data-drop-folder) and drawn as a
	// class (style.css), never as a style (drag.ts is the only exception, and
	// it is not this). The order goes to the server whole on release; the
	// column moves at once and the server's event lands on the same picture.

	function dropAt(type: 'room' | 'folder', x: number, y: number): Drop | null {
		const el = document.elementFromPoint(x, y)?.closest<HTMLElement>('[data-drop-room],[data-drop-folder]');
		if (!el) return null;
		const r = el.getBoundingClientRect();
		const where = y < r.top + r.height / 2 ? 'before' : 'after';
		const room = el.dataset.dropRoom;
		const folder = el.dataset.dropFolder;
		if (type === 'room') {
			if (room !== undefined) return { room, where };
			if (folder !== undefined) return { folder, where: 'into' };
			return null;
		}
		if (folder) return { folder, where };
		const first = columnOf(channelsRef.current, categoriesRef.current)[1]?.folder;
		if (folder === '' && first) return { folder: first.id, where: 'before' };
		return null;
	}

	async function commitDrag(type: 'room' | 'folder', id: string, target: Drop) {
		const places = columnOf(channelsRef.current, categoriesRef.current);
		const order = type === 'room' ? moveRoom(places, id, target) : moveFolder(places, id, target);
		if (!order) return;
		const layout = asLayout(order, channelsRef.current, categoriesRef.current);
		setCategories(layout.categories);
		setChannels((cur) => apply(cur, layout));
		try {
			await api.saveLayout(order.categories, order.channels);
		} catch (err) {
			setError(message(err));
			await refresh();
		}
	}

	const grab = (type: 'room' | 'folder', id: string) =>
		canManage
			? {
					onPointerDown: (ev: PointerEvent) => {
						if (ev.button !== 0 || ev.pointerType === 'touch') return;
						const x0 = ev.clientX;
						const y0 = ev.clientY;
						let started = false;
						let target: Drop | null = null;
						const move = (e: PointerEvent) => {
							if (!started && Math.hypot(e.clientX - x0, e.clientY - y0) < 6) return;
							started = true;
							target = dropAt(type, e.clientX, e.clientY);
							setDragging({ type, id, target });
						};
						const stop = (commit: boolean) => {
							window.removeEventListener('pointermove', move);
							window.removeEventListener('pointerup', up);
							window.removeEventListener('pointercancel', cancel);
							if (!started) return;
							dragged.current = true;
							setTimeout(() => {
								dragged.current = false;
							}, 0);
							setDragging(null);
							if (commit && target) void commitDrag(type, id, target);
						};
						const up = () => stop(true);
						const cancel = () => stop(false);
						window.addEventListener('pointermove', move);
						window.addEventListener('pointerup', up);
						window.addEventListener('pointercancel', cancel);
					},
				}
			: {};

	// The class that draws where a drop would land, for one row or heading.
	const dropMark = (kind: 'room' | 'folder', id: string): string => {
		if (!dragging) return '';
		if (dragging.id === id) return ' opacity-50';
		const t = dragging.target;
		if (!t) return '';
		if (kind === 'room' && 'room' in t && t.room === id) return t.where === 'before' ? ' drop-before' : ' drop-after';
		if (kind === 'folder' && 'folder' in t && t.folder === id) {
			return t.where === 'into' ? ' drop-into' : t.where === 'before' ? ' drop-before' : ' drop-after';
		}
		return '';
	};

	const deleteChannel = async (c: Channel) => {
		setDeleting(null);
		try {
			await api.removeChannel(c.id);
			// The event does the rest here as on everybody else's screen; this
			// is for the socket that happens to be reconnecting.
			setChannels((cur) => cur.filter((x) => x.id !== c.id));
			if (active === c.id) {
				const next = channelsRef.current.find((x) => x.id !== c.id && x.kind === 'text');
				if (next) open(next.id);
			}
			await refresh();
		} catch (err) {
			setError(message(err));
		}
	};

	// Closing a conversation, from the X on the row and from the menu. Nothing
	// is deleted (store.HideChannel); it comes back with the next message or
	// the next time it is opened.
	const closeDM = async (c: Channel) => {
		try {
			await api.hide(c.id);
			// Leaving the conversation you were reading open would show a pane
			// for something that is no longer in the column, so somewhere else
			// has to be chosen — and *another conversation* first, if there is
			// one. Landing on a channel would take the whole column with it, to
			// the other half of the service, which is a long way to be moved by
			// closing one tab.
			if (active === c.id) {
				const left = channelsRef.current.filter((x) => x.id !== c.id && x.kind !== 'voice');
				const next = left.find((x) => x.kind === 'dm') ?? left[0];
				if (next) open(next.id);
			}
			await refresh();
		} catch (err) {
			setError(message(err));
		}
	};

	// The menu itself, drawn inside whichever row asked for it. Three lists open
	// it now — the people on the right, the conversations, and the room in the
	// column — and they all get the same one.
	const drawChannelMenu = (c: Channel) =>
		menu === 'canal:' + c.id ? (
			<PersonMenu
				title={channelLabel(c)}
				items={c.kind === 'voice' ? roomMenuFor(c) : channelMenuFor(c)}
				onClose={() => setMenu('')}
			/>
		) : null;

	const drawMenu = (key: string, username: string, label?: string, anchor?: 'inside' | 'above') =>
		menu === key && username !== '' ? (
			<PersonMenu
				title={label || members.find((m) => m.username === username)?.display_name || username}
				items={menuFor(username)}
				onClose={() => setMenu('')}
				anchor={anchor}
			/>
		) : null;

	// The column split into the top and its folders (reorder.ts).
	const places = columnOf(channels, categories);

	// What the drag needs on a voice room's row (Voice.tsx draws it).
	const roomProps = (c: Channel) => ({
		class: 'relative' + dropMark('room', c.id),
		'data-drop-room': c.id,
		...grab('room', c.id),
	});

	const texts = channels.filter((c) => c.kind === 'text');
	// Conversations, newest first — sorted here and not only on the server,
	// because the column is kept alive between fetches: a conversation somebody
	// starts arrives on the socket and is added to the end of the list, and a
	// message arriving moves nothing at all. Sorted here, the column reorders
	// itself the moment somebody speaks.
	//
	// The id of the last message *is* the order: they are handed out in one
	// sequence for the whole server, so a bigger one is later, full stop — no
	// clock, no timezone, nothing to disagree about. Zero means a conversation
	// with nothing in it yet, which is one that has just been opened, and that
	// goes to the top: it is where the person who opened it is looking.
	const dms = channels
		.filter((c) => c.kind === 'dm')
		.sort((a, b) => (b.last_message_id || Infinity) - (a.last_message_id || Infinity));
	const voices = channels.filter((c) => c.kind === 'voice');
	const page = loaded[active];

	const dmUnread = dms.reduce((n, c) => n + c.unread, 0);
	// The other half of the service, counted for the tab that is *not* open -
	// which is the only tab that carries a number (the design's own rule: the
	// open tab is already showing you the list, so a total over it is a number
	// about something you can see). A voice room has nothing to read, so only
	// the text channels count towards it.
	const serverUnread = texts.reduce((n, c) => n + c.unread, 0);
	const serverMentions = texts.reduce((n, c) => n + c.mentions, 0);

	// open is what every entry in the column does: show the conversation and, on
	// a telephone, get the drawer out of the way.
	const open = (id: string) => {
		setActive(id);
		setDrawer(false);
	};

	// One row of the column: a channel, a room, a conversation. Thirty-two
	// pixels tall and twelve of radius, which is the design's row everywhere.
	//
	// Three states and they are three weights, not three colours: parked is
	// muted text at 500, unread is full white at 600 (the number beside it is
	// how many, the weight is *that* there are any), and the one you are in is
	// the raised surface plus the accent bar drawn by the caller. Reading it as
	// weight is what lets the eye find the unread ones down a list of twenty
	// without stopping on each.
	const entry = (id: string, unread: number, tall = false) =>
		'relative flex w-full items-center gap-1.5 rounded-control px-2 text-left text-item ' +
		(tall ? 'h-10 ' : 'h-9 ') +
		(id === active
			? 'bg-default font-semibold text-foreground'
			: unread > 0
				? 'font-semibold text-foreground hover:bg-surface-2'
				: 'text-foreground-muted hover:bg-surface-2 hover:text-foreground');

	// One text channel's row, at the top of the column or inside a folder.
	const textRow = (c: Channel) => (
		<div key={c.id} class={"relative" + dropMark('room', c.id)} data-drop-room={c.id} {...grab('room', c.id)}>
			<button
				class={entry(c.id, c.unread) + ' channel-entry'}
				// The hash is drawn in its own box now, so the name the
				// row reports would have lost it - and "#geral" is what
				// this row is called, by a person and by a test alike.
				// It survives the glyph arriving, too.
				aria-label={'#' + c.name}
				onClick={() => open(c.id)}
				onContextMenu={(ev) => {
					ev.preventDefault();
					setMenu('canal:' + c.id);
				}}
				title={
					c.muted
						? 'Silenciado — botão direito para voltar a avisar'
						: 'Botão direito para as opções'
				}
			>
				{here(c.id)}
				{/* The hash sits in the box the kit's hashtag
					glyph will occupy - the same box the speaker
					above a voice room sits in (MARK, Voice.tsx
					uses it too), so every name in this column
					starts on one line and the swap is one line
					of code the day the glyph arrives.

					At the row's size, and not at the glyph's.
					It was 18px for three rounds, on the theory
					that a character standing in for a drawing
					should be the drawing's size - and that is
					what made it impossible to line up. An 18px
					hash beside a 15px word is 13.2px of ink
					beside 11.0: the tops met and the baselines
					were 2.1px apart, which is what the owner
					finally put a ruler on. Nothing you can do
					by sliding one of them fixes two sizes.

					Same size, no class, nothing centred by
					hand: both are one line of 15px text in a
					22px line box, so the browser puts them on
					the same baseline and the hash's ink (0.73em)
					and an ascender (0.734em) end up the same
					height. The alignment is not a number any
					more, it is the same font at the same size.

					The drawn glyph arrived on 24/09/2026 and
					went back to 18, centred in the box like the
					headphones of a voice room. */}
				<span class={MARK} aria-hidden="true">
					<Icon name="hashtag" size={18} />
				</span>
				<span class="min-w-0 flex-1 truncate">{c.name}</span>
				{/* Silenced is the bell and nothing else.
					Half-fading the row said the same thing
					twice and cost the name its legibility,
					and a silenced channel never carries an
					unread count anyway - so there was
					nothing left for the fade to mean. */}
				{c.muted && <span class="shrink-0"><Icon name="bell-slash" size={14} /></span>}
				<Badge n={c.mentions > 0 ? c.mentions : c.unread} mention={c.mentions > 0} />
			</button>
			{drawChannelMenu(c)}
		</div>
	);

	// The people column, in the three groups the design sorts it into. The
	// groups do the work the dot alone was doing: a list of twenty names with
	// five coloured dots scattered down it makes you read all twenty to find
	// out who is actually around, and the heading answers that before the
	// first name is read.
	//
	// Somebody in a call goes in Online, not in a fourth group of their own.
	// They *are* online, the handset beside the name says the rest, and a
	// group for it would move them out of the list their friends look in.
	//
	// A role marked "show apart" (D12c) gets a group of its own above those,
	// for whoever has it and is not offline, as on Discord: somebody sits in
	// the group of their highest such role, and offline is offline whatever
	// role they have.
	const apart = roles.filter((r) => r.hoist).sort((a, b) => a.position - b.position);
	const groups: Array<{ label: string; who: User[]; role?: Role }> = [
		...apart.map((r) => ({ label: r.name, who: [] as User[], role: r })),
		{ label: 'Online', who: [] },
		{ label: 'Ausente', who: [] },
		{ label: 'Offline', who: [] },
	];
	for (const m of members) {
		const st = personState(m.username);
		const hoisted = st === 'offline' ? null : hoistOf(m.username, roles);
		const at = hoisted ? apart.findIndex((r) => r.id === hoisted.id) : apart.length + (st === 'away' ? 1 : st === 'offline' ? 2 : 0);
		groups[at].who.push(m);
	}

	// The mark on the row you are in: three pixels of accent in the column's
	// own eight-pixel gutter, so it sits against the edge of the app rather
	// than inside the row. Rounded on the right only, which is what makes it
	// read as something the row is resting against.
	const here = (id: string) =>
		id === active ? (
			<span class="absolute inset-y-0 -left-2 w-[3px] rounded-r-full bg-accent" />
		) : null;

	// What the menu offers about one person. Built here because two lists open
	// it — the people on the right and the conversations in the column — and it
	// has to say the same things in both.
	const menuFor = (username: string): PersonMenuItem[] => {
		const at = whereIs(username);
		const items: PersonMenuItem[] = [{ label: 'Ver perfil', onPick: () => setProfile(username) }];
		if (username !== me.username) {
			items.push({ label: 'Conversar no privado', onPick: () => void openDM(username) });
		}
		if (canCall(username)) {
			items.push({ label: 'Chamar para a sala', onPick: () => void callInto(username) });
		}
		if (at && at.channel.id !== view.channelID) {
			items.push({
				label: `Entrar em ${at.channel.name}`,
				onPick: () => {
					open(at.channel.id);
					void voice.join(at.channel.id);
				},
			});
		}
		return items;
	};

	// Server on one side, the conversations with people on the other — the same
	// split as before, moved to where the owner asked for it on the second
	// look: two words under the name of the service and above the channels,
	// instead of a column of icons down the left edge.
	//
	// He is right, and the reason is the fourteen pixels it gives back. A rail
	// of its own is what a service with *many* servers needs, because there the
	// rail is a list. Here there is one server, forever (CLAUDE.md), so the rail
	// was a permanent column holding two buttons — and it was taking width from
	// the only list that grows.
	const tabs = (
		// No rule under them any more. The design takes every 1px line out of
		// the app that is not saying something (docs/PLANO-DESIGN.md §7.1),
		// and this one was only drawing the shape of the box the tabs are in -
		// which the eight pixels of air under them draw for free.
		<div class="ml-2 mr-2 flex md:mr-1 shrink-0 gap-1 rounded-tile bg-surface p-2" data-island="abas">
			{(
				[
					['server', 'Servidor', serverUnread, serverMentions],
					['dms', 'Mensagens', dmUnread, 0],
				] as const
			).map(([which, label, badge, mentions]) => (
				<button
					key={which}
					class={
						'flex h-9 flex-1 items-center justify-center gap-1.5 rounded-control text-item ' +
						(space === which
							? 'bg-default font-semibold text-foreground'
							: 'font-medium text-foreground-muted hover:bg-surface-2 hover:text-foreground')
					}
					aria-pressed={space === which}
					data-space={which}
					onClick={() => {
						setSpace(which);
						// On a monitor the column and the conversation are side by
						// side, so a half of the service with nothing open in it
						// would show the *other* half's conversation next to this
						// half's list. Opening the first thing fixes that.
						//
						// On a telephone the column is a menu over the conversation,
						// and doing the same thing there means pressing "Mensagens"
						// throws you into somebody's conversation and shuts the menu
						// in the same gesture — which is not what anybody meant by
						// pressing it. There the menu stays up until a conversation
						// is chosen, or until you press outside it.
						if (drawer) return;
						const list = which === 'dms' ? dms : [...texts, ...voices];
						if (!list.some((c) => c.id === active) && list[0]) open(list[0].id);
					}}
				>
					<span class="truncate">{label}</span>
					{which !== space && (
						<Badge
							n={mentions > 0 ? mentions : badge}
							mention={mentions > 0}
							place="tab"
						/>
					)}
				</button>
			))}
		</div>
	);

	const column = (
		// No border down the right edge any more. The column is #18181b and
		// the conversation is #060607, and in a dark room that step *is* the
		// separation - a line on top of it is a second answer to a question
		// that has already been answered (docs/PLANO-DESIGN.md §7.1).
		// Islands, since 23/09/2026 (D4u, third round). The owner drew it over a
		// screenshot, in black marker: the column's three blocks — the tabs, the
		// list, and you at the bottom — as rounded pieces standing apart on the
		// page's own black, "tipo ilhas". It is not the surface step he refused
		// in the D4t (a black frame around one flat grey list): here every block
		// is its own rounded piece and the black is only the 8px between them.
		// Inside each island the content lines are 16 from its edge, the same
		// 16 the column had from its own.
		<nav class={'channel-column flex h-full shrink-0 flex-col bg-background ' + COLUMN[columnStep]}>
			{/* Fifty-six pixels, the same as the conversation's header beside
				it, so the top of the app is one line across all three columns
				rather than three bars of three heights. */}
			<div class="flex h-16 shrink-0 items-center gap-2 px-6">
				<span class="truncate text-title">harmony</span>
				{/* Both of these live at the right edge and both are silent
				    almost always, so they share one box rather than fighting
				    over `ml-auto` depending on which of them exists. */}
				<span class="ml-auto flex shrink-0 items-center gap-2">
					{/* The dot only appears when it has something to say. Green
					    for "connected" was on screen every second of every
					    evening to report the ordinary case, which is the one
					    nobody needs reporting — and an indicator that is always
					    the same colour stops being read at all, so it was not
					    even there when it finally turned red. */}
					{showOffline && (
						<span
							class="h-2 w-2 shrink-0 rounded-full bg-danger"
							title="reconectando"
							data-offline
						/>
					)}
					{/* The character and not a glyph, in exactly the box the
					    glyph will take — the same arrangement the # and the +
					    are in while they wait (PLANO-DESIGN §11.1). The kit's
					    twenty-nine have no circular arrow and this project does
					    not invent icon geometry; `arrows-rotate` is in
					    docs/prompt-glifos-2.md and the swap is one line.

					    It was a pill reading "Atualizar" for about ten minutes,
					    and the test that checks where it sits is what killed
					    that: ninety-four pixels of word in a column that can be
					    208 wide left the wordmark being truncated to make room
					    for a button that is on screen once per deploy. A round
					    28 fits at every width the column has.

					    It was the ↻ character, drawn by the system font because
					    it is outside the Inter subset; since 24/09/2026 it is the
					    kit's own arrow (arrows-rotate). */}
					{stale && (
						<button
							class="grid h-7 w-7 shrink-0 place-items-center rounded-control bg-accent text-foreground hover:bg-accent-hover"
							title="Há uma versão nova no servidor — recarregar"
							aria-label="recarregar para a versão nova"
							data-update
							onClick={() => location.reload()}
						>
							<Icon name="arrows-rotate" size={16} />
						</button>
					)}
				</span>
			</div>
			{tabs}
			{/* The column's three blocks — where you are, what there is, and
			    you — divided, at the owner's request of 23/09/2026, drawn in
			    red across his screenshot: under the tabs, and above the voice
			    strip. After four fios, a surface step and "nenhuma divisão"
			    (D4t §14), what was wrong with the fios was never the idea of a
			    line: it was four of them, bleeding edge to edge, between rows.
			    So: two, between blocks and never between rows, and each one
			    starts and ends on the column's content lines (16 from each
			    edge) instead of crossing the whole column. A painted 1px box
			    rather than a border, so the one test that forbids borders in
			    here still means what it says. */}
			<div class="ml-2 mr-2 mt-2 flex md:mr-1 min-h-0 flex-1 flex-col rounded-tile bg-surface" data-island="lista">
			<div
				class={'scroller flex-1 space-y-0.5 p-2' + (dragging ? ' select-none' : '')}
				// The click a browser sends after a drag is released is not a
				// click: it would open the room or close the folder it ended on.
				onClickCapture={(ev) => {
					if (!dragged.current) return;
					ev.preventDefault();
					ev.stopPropagation();
				}}
			>
				{space === 'server' ? (
					<>
						<div
							class="relative flex items-center pb-1 pl-2 pt-1 text-label uppercase text-foreground-muted"
							data-drop-folder=""
							onContextMenu={(ev) => {
								if (!canManage) return;
								ev.preventDefault();
								setMenu('canais');
							}}
						>
							<span>Canais</span>
							{menu === 'canais' && <PersonMenu title="Canais" items={[newFolder]} onClose={() => setMenu('')} />}
							{canManage && (
								<button
									class="ml-auto -my-1 grid h-8 w-8 place-items-center rounded-control hover:bg-surface-2 hover:text-foreground"
									title="Novo canal"
									aria-label="novo canal"
									onClick={() => newChannel()}
								>
									<Icon name="plus" size={12} />
								</button>
							)}
						</div>
						{places[0].texts.map(textRow)}

						<VoiceChannels
							channels={places[0].voices}
							rooms={rooms}
							meUsername={me.username}
							canManage={canManage}
							onCreate={() => newChannel('voice')}
							activeID={active}
							onOpen={open}
							onOpenPerson={(u) => setProfile(u)}
							onMenuPerson={(u) => setMenu('sala:' + u)}
							menuFor={(u) => drawMenu('sala:' + u, u)}
							onMenuRoom={(c) => {
								if (roomMenuFor(c).length > 0) setMenu('canal:' + c.id);
							}}
							roomMenu={drawChannelMenu}
							onInvite={() => setInviting(true)}
													rowProps={roomProps}
						/>

						{places.slice(1).map((p) => {
							const f = p.folder!;
							const shut = closed.has(f.id);
							// Closed, a folder still shows the room you are in and
							// any voice room with somebody inside, as on Discord:
							// closing a heading should not hide where you are.
							const texts = shut ? p.texts.filter((c) => c.id === active) : p.texts;
							const voices = shut
								? p.voices.filter((c) => c.id === active || (rooms[c.id]?.length ?? 0) > 0)
								: p.voices;
							return (
								<div key={f.id} class="mt-2 space-y-0.5" data-folder={f.id}>
									<div
										class={'relative' + dropMark('folder', f.id)}
										data-drop-folder={f.id}
										{...grab('folder', f.id)}
									>
										<button
											class="flex h-8 w-full items-center gap-1 pl-1 text-left text-label uppercase text-foreground-muted hover:text-foreground"
											aria-expanded={!shut}
											aria-label={'categoria ' + f.name}
											title={canManage ? 'Clique para abrir ou fechar; arraste para mudar de lugar; botão direito para as opções' : 'Clique para abrir ou fechar'}
											onClick={() => toggleFolder(f.id)}
											onContextMenu={(ev) => {
												if (!canManage) return;
												ev.preventDefault();
												setMenu('pasta:' + f.id);
											}}
										>
											<span class={'grid h-4 w-4 shrink-0 place-items-center transition-transform ' + (shut ? '-rotate-90' : '')}>
												<Icon name="chevron-down" size={10} />
											</span>
											<span class="min-w-0 truncate">{f.name}</span>
										</button>
										{menu === 'pasta:' + f.id && (
											<PersonMenu title={f.name} items={folderMenuFor(f)} onClose={() => setMenu('')} />
										)}
									</div>
									{texts.map(textRow)}
									<VoiceChannels
										bare
										channels={voices}
											rooms={rooms}
											meUsername={me.username}
											canManage={canManage}
											onCreate={() => newChannel('voice')}
											activeID={active}
											onOpen={open}
											onOpenPerson={(u) => setProfile(u)}
											onMenuPerson={(u) => setMenu('sala:' + u)}
											menuFor={(u) => drawMenu('sala:' + u, u)}
											onMenuRoom={(c) => {
												if (roomMenuFor(c).length > 0) setMenu('canal:' + c.id);
											}}
											roomMenu={drawChannelMenu}
											onInvite={() => setInviting(true)}
										
										rowProps={roomProps}
									/>
								</div>
							);
						})}
					</>
				) : (
					<>
						{/* No section label here: the tab above already says
						    Mensagens. And no "Nova conversa" row any more (the
						    owner, 17/10/2026: "pode tirar esse botao"); a
						    conversation starts from the person, in the people list. */}
						{dms.map((c) => {
							const who = c.peer?.username ?? '';
							const state = personState(who);
							return (
								<div key={c.id} class="group relative">
									<button
										// Four pixels taller than a channel,
										// because this row carries a face and
										// a channel carries a glyph.
										class={entry(c.id, c.unread, true)}
										onClick={() => open(c.id)}
										onContextMenu={(ev) => {
											ev.preventDefault();
											setMenu('canal:' + c.id);
										}}
										title={
											c.muted
												? 'Silenciada — botão direito para as opções'
												: 'Botão direito para as opções'
										}
									>
										{here(c.id)}
										<Avatar
											username={c.peer?.username}
											avatar={members.find((m) => m.username === c.peer?.username)?.avatar ?? c.peer?.avatar}
											name={channelLabel(c)}
											state={state}
										/>
										<span class="min-w-0 flex-1 truncate">{channelLabel(c)}</span>
										{c.muted && <span class="shrink-0"><Icon name="bell-slash" size={14} /></span>}
										{/* Never the arroba here. Everything
											said in a conversation of two is
											addressed to you, so the mention
											pill would be on every single row
											and would stop meaning anything. */}
										<Badge n={c.unread} mention={false} />
									</button>
									{/* The way out, where the hand already is. Closing a
									    conversation was only in the menu, which means it
									    was only findable by somebody who already knew it
									    was there — and it is the one thing on that menu
									    people want often. Over the row, not in it, so
									    nothing moves when it appears. */}
									<button
										class="revela absolute right-1 top-1/2 h-8 w-8 -translate-y-1/2 place-items-center rounded-control bg-surface-3 text-foreground-muted hover:text-danger-soft-fg"
										title="Fechar esta conversa"
										aria-label={'fechar a conversa com ' + channelLabel(c)}
										data-close-dm={c.id}
										onClick={(ev) => {
											ev.stopPropagation();
											void closeDM(c);
										}}
									>
										<Icon name="xmark" size={14} />
									</button>
									{drawChannelMenu(c)}
								</div>
							);
						})}
						{dms.length === 0 && <p class="px-2 pt-1 text-meta text-foreground-muted">Nenhuma conversa ainda.</p>}
					</>
				)}
			</div>
			</div>
			<div class="m-2 flex md:mr-1 shrink-0 flex-col rounded-tile bg-surface py-2" data-island="voce">
			<VoiceBar
				onOpenScreen={() => setPanel('screen')}
				channels={voices}
				onOpenCall={(id) => {
					setActive(id);
					setDrawer(false);
				}}
			/>
			{/* One line, not two: the design puts the face here and the
				name beside it, and the @name that used to sit under it went
				into the tooltip. It was answering "which account am I in",
				which is a question asked about once a month and answered in
				two other places - and it was costing a second line at the
				bottom of every evening. No rule over it either; the eight
				pixels of air are the separation. */}
			{/* px-4 and not p-2: the face used to start at 8, on the edge of the
				row chips, while everything with meaning in this column - the
				wordmark above, the group headings, every mark - starts at 16. It
				was aligned to a line that is never drawn. The gear moves with it
				and lands on 192, which is where the + buttons already were. */}
			{/* More room under it than over it: it is the last thing in the
			    window, and the owner found it sitting on the bottom edge
			    (23/09/2026). */}
			{/* pr-2 since 24/09/2026: the gear's square ends where the channel
			    rows' pills and the + squares end, 8 inside the island - the
			    owner saw the + sitting 8 short of the row under it. */}
			<div class="flex shrink-0 items-center gap-1 pl-4 pr-2" data-account title={'@' + me.username}>
				<button aria-label="Meu perfil" onClick={() => setProfile(me.username)}><Avatar username={me.username} avatar={me.avatar} name={me.display_name || me.username} state={personState(me.username)} /></button>
				<button onClick={() => setProfile(me.username)} class="account-name ml-0.5 min-w-0 flex-1 truncate text-left text-item" title={'@' + me.username}>
					{me.display_name}
				</button>
				<VoiceToggles />
				<button
					class="grid h-8 w-8 shrink-0 place-items-center rounded-control bg-surface-2 text-foreground hover:bg-surface-3"
					title="Ajustes"
					aria-label="ajustes"
					onClick={() => setSettings(true)}
				>
					<Icon name="gear" size={20} />
				</button>
			</div>
			</div>
		</nav>
	);

	return (
		<div class="relative flex h-full">
			<div class="hidden md:flex">
				{column}
				<Resizer step={columnStep} onStep={setColumnStep} />
			</div>
			{/* The same column, a second time, as a menu over the conversation.
			    It carries `data-drawer` because both copies are in the page at
			    once and only one of them is the one on screen: without a name
			    for this one, "the Mensagens button" means the invisible copy. */}
			{drawer && (
				<div class="absolute inset-0 z-10 flex md:hidden" data-drawer>
					{column}
					<button class="flex-1 bg-veil" onClick={() => setDrawer(false)} aria-label="fechar" />
				</div>
			)}

			{/* The page's black, with the header and the people column as
			    islands on it like the three in the channel column (the owner,
			    23/09/2026: "vamos transformar a parte de cima em uma ilha
			    também... o menu direito tem que ser uma grande ilha"). The
			    conversation stands on the black itself: no frame, so no grey
			    strip under it. A telephone has no islands here: the header is
			    the full width, as it has to be under a thumb. */}
			<main class="flex min-w-0 flex-1 flex-col bg-background md:pr-2">
				{/* The third piece of the top line, and it had to move with the
					other two. The design paints it the colour of the columns
					and gives it their fifty-six pixels, so the head of the app
					is one bar the width of the window with the conversation
					hanging below it - instead of a header of one height beside
					a column head of another, divided by a rule. The rule is
					gone for the same reason it went everywhere else. */}
				<header
					class={"flex h-16 shrink-0 items-center gap-3 bg-surface px-4 md:mt-2 md:h-12 md:rounded-tile md:pl-4 md:pr-2" + (current?.kind === 'text' ? ' channel-header' : '')}
					data-island="topo"
				>
					<button
						class="grid h-11 w-11 shrink-0 place-items-center rounded-control text-foreground-muted hover:bg-surface-2 hover:text-foreground md:hidden"
						onClick={() => setDrawer(true)}
						aria-label="canais"
					>
						<Icon name="bars" size={22} />
					</button>
					{/* The headphones, or the # standing in for the glyph that has not
					    arrived. Its box is 28 and not 24, because a box has to hold
					    the line height of what is inside it and text-title is 20/28.
					    The Codex caught this at 20px against a 24px line, and it
					    survived the first correction — the line grew in the same
					    breath the box did. */}
					<span class="shrink-0 text-foreground-muted" aria-hidden="true">
						{current?.kind === 'voice' ? (
							<Icon name="headphones" size={22} />
						) : current?.kind === 'text' ? (
							<span class="grid h-7 w-7 place-items-center">
								<Icon name="hashtag" size={22} />
							</span>
						) : null}
					</span>
					<span class="shrink-0 truncate text-title">
						{current ? headerLabel(current) : ''}
					</span>
					{/* No topic here, and no tick before it. The owner asked for
					    both to go on 20/09/2026 - "tira essas paradas de | e A sala
					    de voz de todo mundo... e nao coloca mais" - and he is right
					    twice over: nobody in this app can even type a topic (the
					    client sends '' when it creates a channel), so the line was a
					    sentence somebody wrote into the database once, repeated over
					    every conversation forever. The name of the channel is what
					    the header is for. */}
					{current && current.kind !== 'voice' && (
						<button
							class="ml-auto grid h-11 w-11 shrink-0 place-items-center rounded-control text-foreground-muted hover:bg-surface-2 hover:text-foreground md:h-8 md:w-8"
							title="Mensagens fixadas"
							aria-label="fixadas"
							data-pins-open
							onClick={() => setPinsOpen(true)}
						>
							<Icon name="pin" size={18} />
						</button>
					)}
					<button
						class={(current && current.kind !== 'voice' ? '' : 'ml-auto ') + "grid h-11 w-11 shrink-0 place-items-center rounded-control text-foreground-muted hover:bg-surface-2 hover:text-foreground md:h-8 md:w-8"}
						title="Procurar uma mensagem"
						aria-label="procurar"
						data-search-open
						onClick={() => setSearching(true)}
					>
						<Icon name="magnifier" size={18} />
					</button>
					{/* Calling somebody is a thing you do to a person, so the button
					    is in the one place that is about a person: their
					    conversation. A channel has a voice room instead, which is a
					    different thing and is in the column. */}
					{current?.kind === 'dm' && current.peer && (
						<button
							class="grid h-11 w-11 shrink-0 place-items-center rounded-control text-foreground-muted hover:bg-surface-2 hover:text-foreground md:h-8 md:w-8"
							title={'Ligar para ' + channelLabel(current)}
							aria-label="ligar"
							data-call-start
							onClick={() => {
								const peer = current.peer;
								if (peer) void direct.ring(peer as User);
							}}
						>
							<Icon name="handset" size={18} />
						</button>
					)}
					<button
						class={
							'grid h-11 w-11 shrink-0 place-items-center rounded-control md:h-8 md:w-8 ' +
							(people
								? 'bg-default-soft text-foreground'
								: 'text-foreground-muted hover:bg-surface-2 hover:text-foreground')
						}
						onClick={() =>
							setPeople((p) => {
								try {
									localStorage.setItem('harmony.people', p ? '0' : '1');
								} catch {
									// This visit remembers it; the next one does not.
								}
								return !p;
							})
						}
						title={people ? 'Esconder quem está aqui' : 'Ver quem está aqui'}
						aria-label="pessoas"
						aria-pressed={people}
					>
						<Icon name="persons" size={18} />
					</button>
				</header>

				{/* The conversation, on the page's black and 8px under the
				    header's island — so its top is the top of the tabs' island
				    in the column beside it. */}
				<div class="flex min-h-0 flex-1 flex-col overflow-hidden bg-background md:mt-2">
				{/* A call that has been put away. In the layout, not over it: on a
				    telephone anything floating would sit on the box you type in. */}
				<CallStrip />

				{error && (
					<div class="mx-4 mb-2 flex items-center gap-2 rounded-control bg-danger-soft py-1 pl-3 pr-1 text-meta text-danger-soft-fg" role="alert">
						<span class="min-w-0 flex-1">{error}</span>
						<button
							class="grid h-8 w-8 shrink-0 place-items-center rounded-control hover:bg-danger-soft"
							aria-label="fechar aviso"
							onClick={() => setError('')}
						>
							<Icon name="xmark" />
						</button>
					</div>
				)}

				{nudge && (
					<NotifyNudge
						onEnable={() => {
							setNudge(false);
							dismissNudge(me.username);
							void enableNotifications().catch(() => undefined);
						}}
						onDismiss={() => {
							setNudge(false);
							dismissNudge(me.username);
						}}
					/>
				)}

				{current?.kind === 'voice' ? (
					<Call
						view={view}
						peers={rooms[current.id] ?? []}
						meUsername={me.username}
						onMenuPerson={(key) => setMenu('chamada:' + key)}
						menuFor={(key, username, anchor) => drawMenu('chamada:' + key, username, undefined, anchor)}
						onInvite={() => setInviting(true)}
						onOpenPanel={setPanel}
						here={view.channelID === current.id && view.state !== 'off'}
						onJoin={() => void voice.join(current.id)}
					/>
				) : page && current ? (
					<>
						<Messages
							key={current.id}
							messages={page.messages}
							me={me}
							canModerate={!!me.is_admin || (current.kind === 'text' && canModerateMessages)}
							nameColor={nameColor}
							hasMore={page.hasMore}
							hasNewer={page.hasNewer}
							jump={jump?.channel === current.id ? jump : null}
							onJumped={(serial) => setJump((cur) => cur?.serial === serial ? null : cur)}
							onLoadNewer={() => void loadMore(true)}
							onGoRecent={() => void goRecent()}
							loadingMore={loadingMore}
							onLoadMore={() => void loadMore()}
							// Only ever this conversation's own mark: during the frame
							// between changing channel and the photograph being taken,
							// it belongs to the previous one and draws nothing.
							lastReadID={opened.channel === current.id ? opened.at : 0}
							onEdit={async (id, body) => {
								await api.edit(id, body);
							}}
							onRemove={async (id) => {
								await api.remove(id);
							}}
							onReply={(m) => setReplies((cur) => ({ ...cur, [current.id]: m }))}
							people={members}
							onPerson={(username) => setProfile(username)}
							// Nothing is drawn here and then confirmed: the press goes to
							// the server and the room hears about it on the socket, this
							// page included. That is one path instead of two, and it is
							// what makes a refusal — a seventh reaction, say — show the
							// truth rather than something that has to be taken back.
							onReact={async (id, emoji, on) => {
								try {
									await api.react(id, emoji, on);
								} catch (err) {
									setError(message(err));
								}
							}}
							canPin={current.kind === 'dm' || (current.kind === 'text' && canModerateMessages)}
							// Like a reaction: the room, this page included, hears
							// about it on the socket.
							onPin={async (id, on) => {
								try {
									await api.pin(id, on);
								} catch (err) {
									setError(message(err));
								}
							}}
						/>
						<Typing
							names={Object.entries(typing)
								.filter(([k]) => k.startsWith(current.id + '/'))
								.map(([, v]) => v.name)}
						/>
						{current.kind === 'dm' && current.peer && isBlocked(current.peer.username) ? <BlockedConversation username={current.peer.username} /> : <Composer
							// The conversation's identity, given to the box.
							//
							// A different conversation is a different box, built
							// with the right draft already in it — rather than one
							// box emptied and refilled after the fact, which is
							// what let a fast typist lose a sentence (D4l).
							key={current.id}
							channelID={current.id}
							draft={drafts.current[current.id] ?? ''}
							onDraft={(id, text) => {
								if (text === '') delete drafts.current[id];
								else drafts.current[id] = text;
							}}
							members={members}
							placeholder={
								current.kind === 'dm'
									? `Conversar com ${channelLabel(current)}`
									: `Escrever em ${channelLabel(current)}`
							}
							where={
								current.kind === 'dm'
									? `para ${channelLabel(current)}`
									: `em ${channelLabel(current)}`
							}
							replyTo={replies[current.id] ?? null}
							onCancelReply={() => setReplies((cur) => without(cur, current.id))}
							onSent={() => setReplies((cur) => without(cur, current.id))}
						/>}
					</>
				) : (
					<div class="grid flex-1 place-items-center text-foreground-muted">Carregando…</div>
				)}
				</div>
			</main>

			{people && (
				<>
					{/* The backdrop, on a telephone only, and it stops where the
					    panel begins (`right-60` is the panel's own `w-60`). A
					    backdrop that ran underneath it would be a thing you cannot
					    press in the middle of the screen — invisible, and exactly
					    where a thumb lands. The two have to be changed together,
					    and they were: the panel went from 208 to 240 when the
					    names in it went from 13px to 15. */}
					<button
						class="absolute inset-y-0 left-0 right-60 z-10 bg-veil md:hidden"
						onClick={() => setPeople(false)}
						aria-label="fechar"
					/>
				<aside
					class={
						// On a monitor it is a column of the layout. On a telephone
						// it is a panel over the conversation with the same dark
						// backdrop the channel menu has, because two menus in one
						// app that close in two different ways is one of them being
						// wrong.
						//
						// On a monitor it is one big island (23/09/2026), the
						// whole height of the window less 8px at each end.
						'scroller w-60 shrink-0 bg-surface p-2 ' +
						'absolute inset-y-0 right-0 z-10 md:static md:z-auto md:my-2 md:mr-2 md:rounded-tile'
					}
					data-island="pessoas"
				>
					<p class="px-2 pb-1 pt-2 text-label uppercase text-foreground-muted">
						Pessoas · {members.length}
					</p>
					{groups.map(
						(g) =>
							g.who.length > 0 && (
								<div key={g.role?.id ?? g.label} class="space-y-0.5" data-group={g.role ? 'role' : g.label}>
									<p class="px-2 pb-0.5 pt-4 text-label uppercase text-foreground-muted">
										{g.label} · {g.who.length}
									</p>
									{g.who.map((m) => {
										const state = personState(m.username);
										return (
											<div key={m.username} class="relative">
												<button
													class={
														'flex h-12 w-full items-center gap-1.5 rounded-control px-2 text-left text-item ' +
														(state === 'offline'
															? 'text-foreground-muted hover:bg-surface-2'
															: 'text-foreground hover:bg-surface-2')
													}
													onClick={() => setProfile(m.username)}
													onContextMenu={(ev) => {
														ev.preventDefault();
														setMenu('pessoas:' + m.username);
													}}
													data-person={m.username}
													data-state={state}
													title={stateLabel(state)}
												>
													<Avatar
															username={m.username}
															avatar={m.avatar}
														name={nameOf(m.username, m.display_name)}
														state={state}
													/>
													<span class={'min-w-0 flex-1 truncate ' + nameColor(m.username)} data-name-color={colorOf(m.username, roles) || undefined}>
														{nameOf(m.username, m.display_name)}
													</span>
													{/* No "dono" here. The mockup does not
														draw one, and the owner asked for it
														to go on 20/09/2026 - it was a word
														in every row of the list answering a
														question nobody in a server of twenty
														friends is asking. Being the owner is
														still shown where it is useful: on the
														profile, and by the things only he can
														do. */}
													{/* And in a call is green, because
														that is the colour of somebody
														being heard - a handset, not the
														speaking ring, which belongs to
														the face and to this second. */}
													{state === 'in_call' && (
														<span class="shrink-0 text-success" title="Em chamada">
															<Icon name="handset" size={14} />
														</span>
													)}
												</button>
												{drawMenu('pessoas:' + m.username, m.username, m.display_name)}
											</div>
										);
									})}
								</div>
							),
					)}
				</aside>
				</>
			)}

			{profile && (() => {
				const at = whereIs(profile);
				// Somebody who joined the server after this page loaded is not
				// in the list yet. The room knows their name, which is enough
				// to draw them while the list catches up.
				const person: User | undefined =
					members.find((m) => m.username === profile) ??
					(at ? { username: at.peer.username, display_name: at.peer.display_name, avatar: at.peer.avatar } : undefined);
				if (!person) return null;
				const sharing = (at?.peer.publishing ?? []).includes('screen');
				const mine = view.channelID && at?.channel.id === view.channelID;
				return (
					<Profile
						user={person}
						roles={rolesOf(person.username, roles)}
						allRoles={roles}
						me={me}
						onCall={person.username === me.username ? null : () => { setProfile(''); void direct.ring(person as User); }}
						onAvatar={(version) => updateAvatar(person.username, version)}
						peer={at?.peer ?? null}
						state={personState(person.username)}
						onRing={
							// Calling somebody is only possible from inside a room:
							// you invite people to where you are. The server checks
							// this as well, and answers 409 if it is not true.
							canCall(person.username)
								? async () => {
										setProfile('');
										await callInto(person.username);
									}
								: null
						}
						isMe={person.username === me.username}
						onNickname={
							person.username === me.username
								? null
								: async (name: string) => {
										try {
											await api.nickname(person.username, name);
											// The list is where the names come from, so
											// it is the list that has to be read again.
											setMembers(remember((await api.members()).members));
										} catch (err) {
											setError(message(err));
										}
									}
						}
						inMyRoom={!!mine && person.username !== me.username}
						roomName={at?.channel.name ?? ''}
						canJoinRoom={!!at && !mine}
						onJoinRoom={() => {
							if (!at) return;
							setActive(at.channel.id);
							void voice.join(at.channel.id);
							setProfile('');
						}}
						onWatch={
							sharing && at
								? () => {
										setActive(at.channel.id);
										if (!mine) void voice.join(at.channel.id);
										voice.watch(person.username);
										setProfile('');
									}
								: null
						}
						onMessage={
							// A conversation with yourself is not a thing this server
							// has — asking for one answers the same 404 as asking for
							// one with a name that does not exist, which is on purpose
							// (it is how nobody can find out who has an account). So
							// the button is not drawn on your own profile, instead of
							// being drawn and leading to "Não encontrado".
							person.username === me.username
								? null
								: () => {
										setProfile('');
										void openDM(person.username);
									}
						}
						onClose={() => setProfile('')}
					/>
				);
			})()}

			{deleting && (
				<Confirm
					question={'Apagar ' + channelLabel(deleting) + '?'}
					description={
						deleting.kind === 'voice'
							? 'A sala some para todo mundo.'
							: 'O canal some para todo mundo, com as mensagens. Elas não são apagadas do servidor: se foi engano, dá para trazer de volta.'
					}
					actionLabel="Apagar"
					actionIcon="trash-bin"
					onConfirm={() => void deleteChannel(deleting)}
					onCancel={() => setDeleting(null)}
				>
					<div class={'flex items-center text-item ' + (deleting.kind === 'text' ? 'gap-1' : 'gap-2')} data-deleting={deleting.id}>
						<span class={MARK}>
							<Icon name={deleting.kind === 'voice' ? 'headphones' : 'hashtag'} size={18} />
						</span>
						<span class="min-w-0 truncate">{deleting.name}</span>
					</div>
				</Confirm>
			)}

			{inviting && view.channelID && view.state !== 'off' && (
				<Invite
					members={members}
					peers={rooms[view.channelID] ?? []}
					meUsername={me.username}
					onRing={callInto}
					onClose={() => setInviting(false)}
				/>
			)}

			{ring && (
				<RingBox
					ring={ring}
					onJoin={() => {
						setActive(ring.channel_id);
						setDrawer(false);
						void voice.join(ring.channel_id);
						setRing(null);
					}}
					onDismiss={() => setRing(null)}
				/>
			)}

			{panel === 'screen' && <ScreenPanel view={view} onClose={() => setPanel('')} />}
			{panel === 'camera' && <CameraPanel view={view} onClose={() => setPanel('')} />}

			{/* Drawn unconditionally: it decides for itself whether there is a
			    call to show, and it is the only thing in the app that may cover
			    everything else. */}
			<DirectCall me={me} />

			{settings && me && <Settings view={view} me={me} roles={roles} members={members} onClose={() => setSettings(false)} />}
			{creating && (
				<NewChannel
					kind={creating}
					onCreate={(name) => createChannel(creating, name)}
					onClose={() => setCreating(null)}
				/>
			)}

			{naming && (
				<NewChannel
					kind={naming.kind}
					title={
						naming.kind === 'folder'
							? naming.id
								? 'Renomear categoria'
								: 'Nova categoria'
							: 'Renomear canal'
					}
					initial={naming.name}
					action={naming.id ? 'Salvar' : 'Criar'}
					onCreate={(name) => saveName(naming, name)}
					onClose={() => setNaming(null)}
				/>
			)}

			{pinsOpen && current && current.kind !== 'voice' && (
				<Pins
					channelID={current.id}
					canPin={current.kind === 'dm' || (current.kind === 'text' && canModerateMessages)}
					serial={pinSerial}
					onOpen={(messageID) => void jumpToMessage(current.id, messageID)}
					onClose={() => setPinsOpen(false)}
				/>
			)}

			{searching && (
				<Search
					onOpen={(id, messageID) => void jumpToMessage(id, messageID)}
					onClose={() => setSearching(false)}
				/>
			)}
		</div>
	);
}
