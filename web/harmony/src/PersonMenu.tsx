// The menu that opens on a person.
//
// Everything in it already existed, in the profile panel. What did not exist
// was the gesture: pressing a name to open a panel to press a button is three
// steps for "chama o fulano", and the owner asked for the one step everybody
// else's service has — right-click the person, pick the thing.
//
// It offers nothing the profile does not offer, and nothing the server would
// not have checked anyway. A menu item that is not drawn is a courtesy to the
// reader, never a control: the request behind each one is refused on its own
// merits (api/ring.go wants the caller inside the room and the target a member;
// a conversation with yourself answers 404 like any name that does not exist).
//
// **It is anchored to the row, not placed at the pointer**, and that is not a
// shortcut — it is the rule. Nothing in this client may carry a style, and a
// menu at the pointer is a pair of numbers that can only reach CSS through one
// (internal/harmony/api/security_test.go checks the source for it). Hung off
// the row it needs no numbers at all: the only decision left is whether it
// opens downwards or upwards, and that is a class.

import { useEffect, useLayoutEffect, useRef, useState } from 'preact/hooks';

export interface PersonMenuItem {
	label: string;
	onPick: () => void;
	// Something that cannot be taken back from the menu itself (deleting a
	// channel): the red of every other removal in the app.
	danger?: boolean;
}

export function PersonMenu({
	title,
	items,
	onClose,
	anchor = 'row',
}: {
	title: string;
	items: PersonMenuItem[];
	onClose: () => void;
	// Where it hangs. 'row' is under (or over) the row of a list, as wide as
	// it. In the call there is no row: 'inside' opens it in the top left of
	// the block that was pressed, 'above' over a block too small to hold it —
	// the strip under the big picture. Still no coordinates either way.
	anchor?: 'row' | 'inside' | 'above';
}) {
	const card = useRef<HTMLDivElement | null>(null);
	// Through a ref, for the reason spelled out in Profile.tsx: the caller
	// writes onClose inline, so it changes on every render of the app, and a
	// listener that is taken off and put back on every render has a gap in it.
	const close = useRef(onClose);
	close.current = onClose;
	// Which way it opens, and whether it has been worked out yet. Drawn before
	// it is known, it would appear once in the wrong place and jump — a flash
	// this app has already paid for once, with the "não lidas" line.
	const [place, setPlace] = useState<'' | 'down' | 'up'>('');

	useLayoutEffect(() => {
		const el = card.current;
		if (!el) return;
		const box = el.getBoundingClientRect();
		// Opened on the last name in the list, downwards would run off the
		// bottom of the window — which on a telephone means it is simply not
		// there. Upwards, from the same row, always fits when downwards does
		// not: the row it hangs from is on the screen by definition.
		setPlace(anchor === 'above' ? 'up' : box.bottom > window.innerHeight - 8 ? 'up' : 'down');
	}, [items.length]);

	// Anywhere else closes it, and so does Escape and the next right-click.
	//
	// **A scroll does not**, and that was a real defect once the list of people
	// grew past a screenful (D4i). The menu hangs off the row it belongs to, so
	// it travels with that row: there is nothing for a scroll to leave behind.
	// Closing on one meant that opening the menu on the last person in the list
	// — the case where part of the menu is below the edge of the column, so the
	// first thing anybody does is scroll a little to see it — made it vanish
	// under the finger. Closing on scroll is the rule for a menu placed at the
	// pointer, and this one deliberately is not (see the note above).
	useEffect(() => {
		// Anywhere *else*. The "else" is the whole of it: this listens on
		// mousedown, and a press on one of the menu's own items is a mousedown
		// too — so without this the menu closed itself under the finger and the
		// click landed on nothing. Every item in it was unreachable, which is
		// not a thing a test that only checks Escape was ever going to notice.
		const away = (ev: Event) => {
			if (card.current && card.current.contains(ev.target as Node)) return;
			close.current();
		};
		const key = (ev: KeyboardEvent) => {
			if (ev.key === 'Escape') close.current();
		};
		// Next tick, or the very press that opened this closes it again.
		const timer = setTimeout(() => {
			document.addEventListener('mousedown', away);
			document.addEventListener('contextmenu', away);
		}, 0);
		document.addEventListener('keydown', key);
		window.addEventListener('resize', away);
		return () => {
			clearTimeout(timer);
			document.removeEventListener('mousedown', away);
			document.removeEventListener('contextmenu', away);
			document.removeEventListener('keydown', key);
			window.removeEventListener('resize', away);
		};
	}, []);

	return (
		<div
			ref={card}
			class={
				// As wide as the row it hangs from, never wider: the lists it
				// opens in are columns, and a menu that sticks out of one is a
				// menu the column's own scrolling cuts in half.
				(anchor === 'row'
					? 'absolute right-0 z-30 w-full min-w-40 overflow-hidden rounded-panel bg-surface-3 p-1 ' +
						(place === 'up' ? 'bottom-full mb-1 ' : 'top-full mt-1 ')
					: anchor === 'inside'
						? 'absolute left-2 top-2 z-30 w-56 max-w-[calc(100%-16px)] overflow-hidden rounded-panel bg-surface-3 p-1 '
						: 'absolute bottom-full left-0 z-30 mb-2 w-56 overflow-hidden rounded-panel bg-surface-3 p-1 ') +
				(place === '' ? 'invisible' : 'visible')
			}
			role="menu"
			aria-label={'opções de ' + title}
			data-menu={title}
			// The menu is inside the row it belongs to, and the row is a button.
			// Without this, picking anything from the menu would also open that
			// person's profile on the way past.
			onClick={(ev) => ev.stopPropagation()}
		>
			<p class="truncate px-2 py-1 text-label uppercase text-foreground-muted">{title}</p>
			{/* 4 between the items: back to back, a highlighted "Apagar" touched
			    the row above it and read as one block (the owner, 30/10/2026). */}
			<div class="flex flex-col gap-1">
				{items.map((it) => (
					<button
						key={it.label}
						role="menuitem"
						class={
							'block h-8 w-full rounded-control px-2 text-left text-meta hover:bg-default-hover ' +
							(it.danger ? 'text-danger-soft-fg' : 'text-foreground')
						}
						data-danger={it.danger ? '' : undefined}
						onClick={() => {
							close.current();
							it.onPick();
						}}
					>
						{it.label}
					</button>
				))}
			</div>
		</div>
	);
}
