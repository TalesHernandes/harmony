// Reacting to a message: the row of buttons under it, and the panel for
// choosing one.
//
// A reaction is the cheapest thing anybody does in a chat — it is the answer
// for when a message deserves *something* and not a sentence — so the whole of
// this file is about it staying cheap. Nothing here notifies anybody, nothing
// here is worth a line of its own in the conversation, and a press that is a
// mistake is undone by pressing again.

import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'preact/hooks';
import type { JSX } from 'preact';
import type { Reaction } from './api';
import { Icon } from './Icon';
import { nameOf } from './names';
import { GROUPS, search } from './emoji';
import { Emoji } from './Twemoji';

// The last few that were used here, so that the second time somebody wants 👍
// it is the first thing on the panel. It is per device, in the browser's own
// storage, for the same reason the volumes are: it is a fact about this
// keyboard and this pair of hands, not about the person.
const RECENT = 'harmony.emoji';
const MAX_RECENT = 12;

function recent(): string[] {
	try {
		const raw = localStorage.getItem(RECENT);
		const list = raw ? (JSON.parse(raw) as string[]) : [];
		return Array.isArray(list) ? list.slice(0, MAX_RECENT) : [];
	} catch {
		return [];
	}
}

function rememberEmoji(emoji: string): void {
	try {
		const list = [emoji, ...recent().filter((e) => e !== emoji)].slice(0, MAX_RECENT);
		localStorage.setItem(RECENT, JSON.stringify(list));
	} catch {
		// Storage switched off: this visit remembers, the next one does not.
	}
}

// The panel belongs to its opener. Direction and available room become classes,
// never inline coordinates; focusing it must not move the conversation.
export function Picker({ onPick, onClose, fromMessage = false }: {
	onPick: (emoji: string) => void; onClose: () => void; fromMessage?: boolean;
}) {
	const [used] = useState(recent);
	const [query, setQuery] = useState('');
	// Which section is at the top of the list, lit on the rail.
	const [shown, setShown] = useState(used.length ? 'recent' : '0');
	const list = useRef<HTMLDivElement>(null);
	// The groups near enough to the visible part of the list to fetch their
	// pictures (Twemoji.tsx, `hold`). Measured against the list itself, 400px
	// ahead, so scrolling finds them already drawn.
	const [near, setNear] = useState<Set<string>>(() => new Set());
	useEffect(() => {
		const box = list.current;
		if (!box) return;
		const seen = new IntersectionObserver(
			(entries) => {
				const add = entries.filter((e) => e.isIntersecting).map((e) => (e.target as HTMLElement).dataset.section!);
				if (add.length) setNear((old) => (add.every((k) => old.has(k)) ? old : new Set([...old, ...add])));
			},
			{ root: box, rootMargin: '400px 0px' },
		);
		box.querySelectorAll('[data-section]').forEach((el) => seen.observe(el));
		return () => seen.disconnect();
	}, [query]);
	const [place, setPlace] = useState('emoji-pending');
	const card = useRef<HTMLDivElement>(null);
	const close = useRef(onClose);
	close.current = onClose;
	useEffect(() => {
		const opener = document.activeElement as HTMLElement | null;
		const el = card.current;
		if (!el || place === 'emoji-pending') return;
		const controls = () => Array.from(el.querySelectorAll<HTMLElement>('button:not(:disabled), input'));
		// Both native autofocus and the shared dialog hook scroll ancestors.
		// Keep the same keyboard trap, including return focus, without scrolling.
		el.querySelector<HTMLElement>('[data-emoji-search]')?.focus({ preventScroll: true });
		const key = (ev: KeyboardEvent) => {
			if (ev.key === 'Escape') {
				ev.preventDefault();
				ev.stopPropagation();
				close.current();
			} else if (ev.key === 'Tab') {
				const all = controls(), first = all[0] ?? el, last = all[all.length - 1] ?? el;
				if (ev.shiftKey && document.activeElement === first) { ev.preventDefault(); last.focus({ preventScroll: true }); }
				else if (!ev.shiftKey && document.activeElement === last) { ev.preventDefault(); first.focus({ preventScroll: true }); }
			}
		};
		document.addEventListener('keydown', key, true);
		return () => {
			document.removeEventListener('keydown', key, true);
			if (opener?.isConnected) opener.focus({ preventScroll: true });
		};
	}, [place === 'emoji-pending']);
	useLayoutEffect(() => {
		const el = card.current;
		if (!el) return;
		const pane = el.closest<HTMLElement>('[data-history]');
		const position = () => {
			const anchor = el.parentElement!.getBoundingClientRect();
			const bounds = pane?.getBoundingClientRect();
			const top = Math.max(8, bounds ? bounds.top + pane!.clientTop : 8);
			const bottom = Math.min(window.innerHeight - 8, bounds ? bounds.top + pane!.clientTop + pane!.clientHeight : window.innerHeight - 8);
			const above = Math.max(0, anchor.top - top - 8);
			const below = Math.max(0, bottom - anchor.bottom - 8);
			// Prefer down when the full 420px panel fits. Otherwise use the
			// larger side, with an exact cap rather than a size that may overflow.
			const up = !fromMessage || (below < 420 && above > below);
			const height = Math.floor(Math.min(420, up ? above : below));
			const left = Math.max(8, bounds?.left ?? 8);
			const width = Math.floor(Math.max(0, Math.min(384, fromMessage ? anchor.right - left - 8 : window.innerWidth - 32)));
			// Binary classes encode pixel caps without CSP-forbidden inline styles.
			const size = (axis: string, pixels: number) => Array.from({ length: 9 }, (_, i) => 1 << i)
				.filter((bit) => pixels & bit).map((bit) => ` emoji-${axis}-${bit}`).join('');
			setPlace((up ? 'emoji-up' : 'emoji-down') + ' emoji-sized' + size('height', height) + size('width', width));
		};
		position();
		// The list moves under the panel on its own too — it follows the bottom
		// when somebody writes — so a scroll re-measures instead of closing:
		// closing would take the panel away mid-choice because of somebody else.
		const scrolled = (ev: Event) => { if (ev.target === pane) position(); };
		pane?.addEventListener('scroll', scrolled);
		const watch = new ResizeObserver(position);
		if (pane) watch.observe(pane);
		watch.observe(el.parentElement!);
		window.addEventListener('resize', position);
		return () => {
			watch.disconnect();
			pane?.removeEventListener('scroll', scrolled);
			window.removeEventListener('resize', position);
		};
	}, [fromMessage]);
	useEffect(() => {
		const away = (ev: PointerEvent) => {
			if (!card.current?.contains(ev.target as Node)) close.current();
		};
		document.addEventListener('pointerdown', away);
		return () => document.removeEventListener('pointerdown', away);
	}, []);
	const found = useMemo(() => search(query), [query]);
	const choose = (emoji: string) => { rememberEmoji(emoji); onClose(); onPick(emoji); };
	// Every group in one list that scrolls down, with a rail beside it that
	// jumps to a group and lights the one being looked at (the owner,
	// 24/09/2026: no sideways scrolling in the emoji panel). It was one group at
	// a time under a row of tabs that scrolled to the side: past the ninth tab
	// the rest were off the edge, and a group could not be read into the next.
	const sections: { key: string; name: string; list: string[]; mark: JSX.Element }[] = [
		...(used.length ? [{ key: 'recent', name: 'usados aqui', list: used, mark: <Icon name="clock" size={18} /> }] : []),
		...GROUPS.map((g, i) => ({
			key: String(i),
			name: g.name,
			list: g.emoji.map(([e]) => e),
			mark: <Emoji char={g.emoji[0][0]} class="emoji-tab" />,
		})),
	];
	const jump = (key: string) => {
		const box = list.current;
		const to = box?.querySelector<HTMLElement>('[data-section="' + key + '"]');
		// The list's own scroll, set directly: scrollIntoView would also move
		// every scrolling thing around it, the conversation included.
		if (box && to) box.scrollTop = to.offsetTop;
		setShown(key);
	};
	const follow = () => {
		const box = list.current;
		if (!box) return;
		let top = sections[0]?.key ?? '0';
		for (const el of box.querySelectorAll<HTMLElement>('[data-section]')) {
			if (el.offsetTop <= box.scrollTop + 8) top = el.dataset.section!;
		}
		setShown(top);
	};
	const grid = (key: string, name: string, emoji: string[]) => (
		<section key={key} data-section={key}>
			<h3 class="sticky top-0 z-10 bg-surface pb-1 pt-2 text-label uppercase text-foreground-muted">{name}</h3>
			<div class="emoji-grid">{emoji.map((e) => (
				<button type="button" key={key + e} class="emoji-cell rounded-control hover:bg-default" data-emoji={e} aria-label={e} onClick={() => choose(e)}><Emoji char={e} hold={!near.has(key)} /></button>
			))}</div>
		</section>
	);
	return (
		<div ref={card} role="dialog" aria-label="reagir" tabIndex={-1} data-emoji-panel
			class={'emoji-panel absolute z-30 flex flex-col gap-2 rounded-panel bg-surface p-2 text-foreground ' + place + (fromMessage ? ' right-0' : ' left-2')}>
			<div class="flex h-11 shrink-0 items-center gap-2 rounded-control bg-surface-2 px-3">
				<Icon name="magnifier" size={16} />
				<input class="min-w-0 flex-1 bg-transparent text-item outline-none" placeholder="Fogo, coracao, risada" value={query} data-emoji-search aria-label="procurar emoji" onInput={(ev) => setQuery(ev.currentTarget.value)} />
				{query && <button class="grid h-8 w-8 place-items-center rounded-control bg-default" aria-label="limpar busca de emoji" onClick={() => setQuery('')}><Icon name="xmark" size={12} /></button>}
			</div>
			<div class="flex min-h-0 flex-1 gap-2">
				<div class="emoji-rail flex shrink-0 flex-col gap-1" aria-label="grupos de emoji" data-emoji-rail>
					{sections.map((g) => {
						const on = g.key === shown && !query;
						return (
							<button type="button" key={g.key} class={'grid h-8 w-8 shrink-0 place-items-center rounded-control ' + (on ? 'bg-default text-foreground' : 'text-foreground-muted hover:bg-default')} aria-label={g.name} title={g.name} aria-pressed={on} data-group={g.key} onClick={() => { setQuery(''); requestAnimationFrame(() => jump(g.key)); }}>
								<span aria-hidden="true" class="grid place-items-center">{g.mark}</span>
							</button>
						);
					})}
				</div>
				<div ref={list} class="scroller emoji-list relative min-h-0 flex-1 pb-2" onScroll={follow} data-emoji-list>
					{query.trim() ? (found.length ? grid('found', 'encontrados', found) : <div class="py-4" role="status"><p class="text-item">Nenhum emoji com esse nome</p><p class="text-meta text-foreground-muted">Tente uma palavra só, como abacaxi ou pizza.</p></div>) : sections.map((g) => grid(g.key, g.name, g.list))}
				</div>
			</div>
		</div>
	);
}

// Reactions is the row under a message: one button per emoji, each one a
// switch. Yours are the lit ones — worked out by looking for your own name in
// the list rather than asked of the server, because the same message goes to
// the whole room at once and "yours" would have to be a different answer for
// every reader.
export function Reactions({
	list,
	me,
	onToggle,
}: {
	list: Reaction[];
	me: string;
	onToggle: (emoji: string, on: boolean) => void;
}) {
	if (list.length === 0) return null;
	return (
		<div class="flex flex-wrap gap-1 pt-1" data-reactions>
			{list.map((r) => {
				const mine = r.users.includes(me);
				return (
					<button
						key={r.emoji}
						data-reaction={r.emoji}
						// Who pressed it, which is most of why anybody wants this.
						// Their names as *you* call them: a nickname that stopped at
						// the edge of this row would be a second name for the same
						// person on the same screen.
						title={r.users.map((u) => nameOf(u)).join(', ')}
						// 24 tall, and the difference between yours and somebody
						// else's is a ring rather than a different shape: the row
						// has to read as one row of the same thing, with some of
						// them lit.
						class={
							'flex h-6 items-center gap-1 rounded-control px-2 text-meta ' +
							(mine
								? 'bg-accent-soft text-accent-soft-fg ring-1 ring-accent ring-inset'
								: 'bg-surface-2 text-foreground-muted hover:bg-surface-3')
						}
						onClick={() => onToggle(r.emoji, !mine)}
					>
						<span class="leading-none"><Emoji char={r.emoji} class="emoji-chip" /></span>
						<span class="font-semibold tabular-nums">{r.users.length}</span>
					</button>
				);
			})}
		</div>
	);
}
