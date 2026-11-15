// Looking for something somebody said.
//
// Pressing an answer opens its conversation at that exact message. The history
// pane loads a window around it when it is not already on the page.
//
// And it does not search while somebody is still typing the first letter. The
// box waits a quarter of a second after the last key, which is the difference
// between one query and eight for the same word.

import { useEffect, useRef, useState } from 'preact/hooks';
import { api, message, type Hit } from './api';
import { Avatar } from './Avatar';
import { Icon } from './Icon';
import { useDialogFocus } from './Confirm';
import { nameOf } from './names';
import { withEmoji } from './Twemoji';
import { isBlocked, usePreferences } from './blocks';

function whereItWas(hit: Hit): string {
	const ch = hit.channel;
	if (!ch) return '';
	if (ch.kind === 'dm' && ch.peer) return nameOf(ch.peer.username, ch.peer.display_name);
	return '#' + ch.name;
}

function when(iso: string): string {
	return new Date(iso).toLocaleDateString('pt-BR', { day: '2-digit', month: '2-digit', year: '2-digit' });
}

export function Search({ onOpen, onClose }: { onOpen: (channelID: string, messageID: number) => void; onClose: () => void }) {
	usePreferences();
	const card = useRef<HTMLDivElement>(null);
	const drag = useRef<number | null>(null);
	useDialogFocus(card, onClose);
	const [query, setQuery] = useState('');
	const [hits, setHits] = useState<Hit[]>([]);
	const [state, setState] = useState<'idle' | 'looking' | 'done'>('idle');
	const [error, setError] = useState('');
	// Which search the answers on the screen belong to. Answers to a word that
	// has already been typed over are thrown away: they arrive out of order over
	// a slow connection, and the older one would win.
	const asked = useRef(0);

	useEffect(() => {
		const text = query.trim();
		// Every change moves the count on, the empty box included: an answer
		// already on its way for the word that was just erased would otherwise
		// still match, and land under a box with nothing in it.
		const mine = ++asked.current;
		setError('');
		setHits([]);
		if (text.length < 2) {
			setState('idle');
			return;
		}
		setState('looking');
		const timer = setTimeout(() => {
			api
				.search(text)
				.then((res) => {
					if (asked.current !== mine) return;
					setHits(res.messages);
					setError('');
					setState('done');
				})
				.catch((err) => {
					if (asked.current !== mine) return;
					setError(message(err));
					setState('done');
				});
		}, 250);
		return () => { clearTimeout(timer); ++asked.current; };
	}, [query]);

	return (
		<div class="search-veil fixed inset-0 z-50 grid place-items-center bg-veil p-4" onClick={(e) => { if (e.target === e.currentTarget) onClose(); }}>
		<div ref={card} role="dialog" aria-modal="true" aria-label="procurar" tabIndex={-1} data-search-panel class="search-panel flex w-full max-w-lg flex-col rounded-panel bg-surface p-6 text-foreground">
			<div class="search-handle grid h-6 shrink-0 place-items-center" onPointerDown={(e) => { drag.current = e.clientY; e.currentTarget.setPointerCapture(e.pointerId); }} onPointerUp={(e) => { if (drag.current !== null && e.clientY - drag.current > 60) onClose(); drag.current = null; }} onPointerCancel={() => { drag.current = null; }}><span class="h-1 w-9 rounded-control bg-surface-3" /></div>
			<div class="flex items-center gap-2 pb-3"><h2 class="flex-1 text-title">Procurar</h2><button class="grid h-8 w-8 shrink-0 place-items-center rounded-control bg-default text-foreground hover:bg-default-hover" title="Fechar" aria-label="fechar" onClick={onClose}><Icon name="xmark" /></button></div>
			<label class="flex h-12 shrink-0 items-center gap-3 rounded-control bg-surface-2 px-3"><Icon name="magnifier" size={20} />
			<input
				class="min-w-0 flex-1 bg-transparent text-body text-foreground outline-none placeholder:text-foreground-muted"
				placeholder="Uma palavra que estava na mensagem"
				value={query}
				data-search-box
				aria-label="procurar mensagens"
				autofocus
				onInput={(ev) => setQuery((ev.target as HTMLInputElement).value)}
			/>
			</label>
			<div role="status" class="search-status">
			{state === 'idle' && <p class="py-3 text-meta text-foreground-muted">Digite pelo menos duas letras.</p>}
			{state === 'looking' && <p class="py-3 text-meta text-foreground-muted">Procurando…</p>}
			{error && <p class="py-3 text-meta text-danger-soft-fg">{error}</p>}
			{state === 'done' && hits.length === 0 && !error && (
				<p class="py-3 text-item text-foreground-muted">Nada com essa palavra.</p>
			)}
			</div>
			{/* A 12px strip on the right (style.css): the bar sits beside the cards, not on
			    their edge (the owner, 24/09/2026). */}
			<div class="scroller search-results -mr-3 mt-2 min-h-0 space-y-1" data-search-results>
				{hits.map((hit) => (
					<button
						key={hit.id}
						class="block min-h-14 w-full rounded-control bg-surface-2 p-3 text-left hover:bg-default"
						data-hit={hit.id}
						onClick={() => {
							onOpen(hit.channel_id, hit.id);
							onClose();
						}}
					>
						<span class="flex items-center gap-1">
							<Avatar username={hit.author.username} avatar={hit.author.avatar} name={nameOf(hit.author.username, hit.author.display_name)} size={28} />
							<span class="min-w-0 flex-1">
								<span class="flex items-baseline gap-2 text-meta text-foreground-muted">
									<span class="truncate font-semibold text-foreground">{whereItWas(hit)}</span>
									<span class="truncate">
										{nameOf(hit.author.username, hit.author.display_name)}
							</span>
							<span class="ml-auto shrink-0">{when(hit.created_at)}</span>
						</span>
						<span class="block whitespace-pre-wrap break-words pt-0.5 text-body">{isBlocked(hit.author.username) ? 'Mensagem de pessoa bloqueada' : withEmoji(hit.body)}</span>
							</span>
						</span>
					</button>
				))}
			</div>
		</div>
		</div>
	);
}
