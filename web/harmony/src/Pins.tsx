// The pinned messages of one conversation (D12a), most recently pinned first.
// The same card as the search (Search.tsx): a press on one opens the
// conversation at that message, and whoever may pin there may also take a pin
// off from here.

import { useEffect, useRef, useState } from 'preact/hooks';
import { api, message, type Message } from './api';
import { Avatar } from './Avatar';
import { Icon } from './Icon';
import { useDialogFocus } from './Confirm';
import { nameOf } from './names';
import { withEmoji } from './Twemoji';
import { isBlocked, usePreferences } from './blocks';

function when(iso: string): string {
	const d = new Date(iso);
	return d.toLocaleDateString('pt-BR', { day: '2-digit', month: '2-digit' }) + ' ' + d.toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit' });
}

export function Pins({
	channelID,
	canPin,
	serial,
	onOpen,
	onClose,
}: {
	channelID: string;
	canPin: boolean;
	// Moves on when a message of this conversation changes, so a pin made on
	// another device shows up here without closing the card.
	serial: number;
	onOpen: (messageID: number) => void;
	onClose: () => void;
}) {
	usePreferences();
	const card = useRef<HTMLDivElement>(null);
	useDialogFocus(card, onClose);
	const [list, setList] = useState<Message[] | null>(null);
	const [error, setError] = useState('');

	useEffect(() => {
		let gone = false;
		api
			.pins(channelID)
			.then((res) => {
				if (!gone) setList(res.messages);
			})
			.catch((err) => {
				if (!gone) setError(message(err));
			});
		return () => {
			gone = true;
		};
	}, [channelID, serial]);

	return (
		<div class="search-veil fixed inset-0 z-50 grid place-items-center bg-veil p-4" onClick={(e) => { if (e.target === e.currentTarget) onClose(); }}>
			<div ref={card} role="dialog" aria-modal="true" aria-label="mensagens fixadas" tabIndex={-1} data-pins-panel class="search-panel flex w-full max-w-lg flex-col rounded-panel bg-surface p-6 text-foreground">
				<div class="flex items-center gap-2 pb-3">
					<h2 class="flex-1 text-title">Fixadas</h2>
					<button class="grid h-8 w-8 shrink-0 place-items-center rounded-control bg-default text-foreground hover:bg-default-hover" title="Fechar" aria-label="fechar" onClick={onClose}>
						<Icon name="xmark" />
					</button>
				</div>
				<div role="status" class="search-status">
					{list === null && !error && <p class="py-3 text-meta text-foreground-muted">Carregando…</p>}
					{error && <p class="py-3 text-meta text-danger-soft-fg">{error}</p>}
					{list?.length === 0 && (
						<p class="py-3 text-item text-foreground-muted">
							Nenhuma mensagem fixada aqui. {canPin ? 'Passe o mouse numa mensagem e use o alfinete.' : ''}
						</p>
					)}
				</div>
				<div class="scroller search-results -mr-3 mt-2 min-h-0 space-y-1" data-pins-list>
					{(list ?? []).map((m) => (
						<div key={m.id} class="relative">
							<button
								class="block min-h-14 w-full rounded-control bg-surface-2 p-3 pr-12 text-left hover:bg-default"
								data-pinned={m.id}
								onClick={() => {
									onOpen(m.id);
									onClose();
								}}
							>
								<span class="flex items-center gap-1">
									<Avatar username={m.author.username} avatar={m.author.avatar} name={nameOf(m.author.username, m.author.display_name)} size={28} />
									<span class="min-w-0 flex-1">
										<span class="flex items-baseline gap-2 text-meta text-foreground-muted">
											<span class="truncate font-semibold text-foreground">{nameOf(m.author.username, m.author.display_name)}</span>
											<span class="ml-auto shrink-0">{when(m.created_at)}</span>
										</span>
										<span class="block whitespace-pre-wrap break-words pt-0.5 text-body">
											{isBlocked(m.author.username) ? 'Mensagem de pessoa bloqueada' : m.body ? withEmoji(m.body) : m.attachments.map((f) => f.name).join(', ')}
										</span>
									</span>
								</span>
							</button>
							{canPin && (
								<button
									class="absolute right-2 top-2 grid h-8 w-8 place-items-center rounded-control text-foreground-muted hover:bg-surface-3 hover:text-foreground"
									title="Desafixar"
									aria-label="desafixar"
									data-unpin={m.id}
									onClick={async () => {
										try {
											await api.pin(m.id, false);
											setList((cur) => (cur ?? []).filter((x) => x.id !== m.id));
										} catch (err) {
											setError(message(err));
										}
									}}
								>
									<Icon name="xmark" size={14} />
								</button>
							)}
						</div>
					))}
				</div>
			</div>
		</div>
	);
}
