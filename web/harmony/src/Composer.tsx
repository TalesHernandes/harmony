// The box you type in: text, @ completion, and files.
//
// A file is uploaded the moment it is chosen — dropped, pasted or picked — before
// the message exists, and what the composer keeps is the id the server gave back.
// That is why the CSP needs no blob: the preview is the real picture, served from
// this server, through the same membership check as everything else. It is also
// why something still on its way up shows a name and a bar rather than the
// picture: the picture does not exist anywhere this page may read from until the
// server has it.

import { useEffect, useRef, useState } from 'preact/hooks';
import {
	api,
	ApiError,
	message,
	MAX_UPLOAD_BYTES,
	type Attachment,
	type Message,
	type User,
} from './api';
import { log } from './diag';
import { Picker } from './Reactions';
import { Icon } from './Icon';
import { Lightbox } from './Messages';

// One thing on its way up, or already there. The tile is drawn from the moment
// the file is chosen to the moment the message is sent, and only its middle
// changes, so everything the tile needs lives here.
interface Item {
	// Ours, not the server's: the server has no name for this file until it has
	// the file, and the tile has to be drawn before that.
	key: string;
	name: string;
	// How much of it has left this machine, 0 to 1. It is not the whole story —
	// see the tile.
	sent: number;
	// The answer, once there is one. Until then this is the difference between
	// "going up" and "ready to send".
	stored: Attachment | null;
	stop: AbortController;
}

interface Props {
	channelID: string;
	placeholder: string;
	// Where a dropped file is about to land, already worded: "em #geral", "para
	// Ana". The placeholder cannot be reused for it — "Escrever em #geral" reads
	// wrong under "Solte aqui para mandar".
	where: string;
	// What was being written in this conversation when it was last left. Read
	// once, when this box is built, because from then on the box is the one
	// holding it — see the key in App.tsx.
	draft: string;
	// Reported on every keystroke, and it costs nothing: the app writes it into
	// a box of its own that nothing renders, so typing still redraws this
	// component and not the conversation above it.
	onDraft: (channelID: string, text: string) => void;
	members: User[];
	// The message being answered, if any. It is held by the app rather than
	// here because it is chosen up in the conversation and used down here, and
	// because changing channel has to drop it — which is the same rule as the
	// draft, one line below.
	replyTo: Message | null;
	onCancelReply: () => void;
	onSent: () => void;
}

// Attach and emoji: grey icons inside the island, 36px, with the surface of a
// control only under the pointer.
const TOOL =
	'grid h-9 w-9 shrink-0 place-items-center rounded-control text-foreground-muted hover:bg-default hover:text-foreground';

export function Composer(props: Props) {
	// Seeded from the draft, and only ever seeded: a different conversation is a
	// different box entirely (App.tsx gives this component the conversation's own
	// identity), so there is no case where this has to be reset in place.
	const [body, setBody] = useState(props.draft);
	const [items, setItems] = useState<Item[]>([]);
	const [preview, setPreview] = useState<Attachment | null>(null);
	const [busy, setBusy] = useState(false);
	// Whether a file is being dragged over the window right now.
	const [over, setOver] = useState(false);
	const [error, setError] = useState('');
	const [pick, setPick] = useState<{ prefix: string; at: number } | null>(null);
	// Whether the emoji panel is open. It is the same panel used for reacting to
	// a message: one list, one search box, one place to add to.
	const [emoji, setEmoji] = useState(false);
	const [highlight, setHighlight] = useState(0);
	const box = useRef<HTMLTextAreaElement>(null);
	// When this device last said it was typing.
	//
	// Once every three seconds, not once per key: the point is a light that
	// stays on while somebody writes, and a request per keystroke would be a
	// hundred requests for one sentence to say a thing that was already true.
	// The other end keeps it lit for five seconds, so three leaves room for one
	// to go missing without the light blinking.
	const said = useRef(0);
	// A number whose only job is to be different from the last one.
	const seq = useRef(0);
	// What is on screen, readable from a listener that was registered once. A
	// closure over `items` goes stale the first time one of them lands.
	const live = useRef<Item[]>([]);
	live.current = items;
	// How many elements deep the dragged file is. It is not bookkeeping for its
	// own sake: dragenter and dragleave fire for every element the pointer
	// crosses on the way in, so a plain boolean blinks off each time the file
	// passes over a message. The depth only reaches zero when the file has
	// really left the window.
	const depth = useRef(0);

	// Anything still on its way up belonged to the conversation being left, so it
	// is stopped rather than landed in the next one.
	//
	// Clearing the rest of the state is not here, and that is the point. This
	// used to be an effect that emptied the box whenever the conversation
	// changed, and it was the D4l bug: an effect runs *after* the paint and runs
	// on the first render too, so between the box being typeable on screen and
	// the line that empties it there was a gap, and anything typed inside the
	// gap was wiped by a tidy-up with nothing to tidy. It is now impossible
	// rather than guarded against — App.tsx gives this component the
	// conversation's own identity, so a different conversation builds a
	// different box, with the state it should have from the first frame.
	useEffect(() => {
		return () => {
			for (const it of live.current) it.stop.abort();
		};
	}, []);

	// "@todos" is offered like a person, at the top, because that is where
	// somebody types it — and it is a real name to the server too: every member
	// of *this* conversation gets the mention, and nobody outside it does
	// (store.insertMentions).
	const EVERYONE = { username: 'todos', display_name: 'Todo mundo daqui' } as User;
	const matches = pick
		? [EVERYONE, ...props.members]
				.filter((m) => (m.username + ' ' + m.display_name).toLowerCase().includes(pick.prefix))
				.slice(0, 6)
		: [];

	function grow() {
		const el = box.current;
		if (!el) return;
		el.rows = Math.min(6, body.split('\n').length);
	}

	function onInput(e: Event) {
		const el = e.target as HTMLTextAreaElement;
		setBody(el.value);
		props.onDraft(props.channelID, el.value);
		const now = Date.now();
		if (el.value.trim() !== '' && now - said.current > 3000) {
			said.current = now;
			void api.typing(props.channelID).catch(() => undefined);
		}
		// Look back from the caret for an @ that is still being typed.
		const upto = el.value.slice(0, el.selectionStart ?? el.value.length);
		const m = /@([a-z0-9._-]*)$/i.exec(upto);
		setPick(m ? { prefix: m[1].toLowerCase(), at: upto.length - m[0].length } : null);
		setHighlight(0);
	}

	function complete(username: string) {
		if (!pick) return;
		const el = box.current;
		const caret = el?.selectionStart ?? body.length;
		const next = body.slice(0, pick.at) + '@' + username + ' ' + body.slice(caret);
		setBody(next);
		props.onDraft(props.channelID, next);
		setPick(null);
		el?.focus();
	}

	// Four is the server's rule as well (store.MaxAttachmentsPerMessage), so this
	// is not a screen being fussy: it is the same number, said earlier.
	const MAX_FILES = 4;

	function attach(list: FileList | File[] | null) {
		if (!list) return;
		const chosen = Array.from(list);
		if (chosen.length === 0) return;
		const room = MAX_FILES - items.length;
		if (room <= 0) {
			setError(`Uma mensagem leva ${MAX_FILES} arquivos, não mais.`);
			return;
		}
		setError('');
		// All of them start at once. They are four at most, and four bars that
		// all move is the honest picture of four uploads.
		for (const file of chosen.slice(0, room)) void put(file);
		if (chosen.length > room) setError(`Uma mensagem leva ${MAX_FILES} arquivos, não mais.`);
	}

	async function put(file: File) {
		// Too big is answered here rather than thirty-two megabytes up the wire.
		// The server would say exactly this (api/files.go) and say it last, which
		// on a telephone is a minute of a bar that was never going to arrive.
		if (file.size > MAX_UPLOAD_BYTES) {
			setError(`${file.name}: ${message(new ApiError(413, 'too_large'))}`);
			return;
		}
		const key = String(++seq.current);
		const stop = new AbortController();
		setItems((cur) => [...cur, { key, name: file.name, sent: 0, stored: null, stop }]);
		const patch = (f: (it: Item) => Item) =>
			setItems((cur) => cur.map((x) => (x.key === key ? f(x) : x)));
		try {
			const a = await api.upload(
				props.channelID,
				file,
				(sent, total) => patch((x) => ({ ...x, sent: total > 0 ? sent / total : 0 })),
				stop.signal,
			);
			patch((x) => ({ ...x, sent: 1, stored: a }));
		} catch (err) {
			// Changing your mind is not a failure to report: the tile simply goes.
			if (!(err instanceof ApiError && err.code === 'canceled')) {
				setError(`${file.name}: ${message(err)}`);
			}
			setItems((cur) => cur.filter((x) => x.key !== key));
		}
	}

	// Taking one off. While it is still going up this is a real cancellation —
	// the request is aborted and the bytes stop leaving the machine — which is
	// why the × is there during the upload and not only after it.
	function forget(key: string) {
		live.current.find((x) => x.key === key)?.stop.abort();
		setItems((cur) => cur.filter((x) => x.key !== key));
	}

	// A file dropped anywhere on the page lands in the box you were typing in.
	//
	// The listeners are on the window and not on this element, because what
	// somebody aims at is the conversation — which is most of the screen, and
	// none of it is this component. They are registered once and read the newest
	// `attach` through a ref: a listener that closed over the first render would
	// still be filing files under the first channel.
	const dropHere = useRef(attach);
	dropHere.current = attach;

	useEffect(() => {
		// Only files. Dragging selected text across the page is not an upload.
		const carries = (e: DragEvent) => Array.from(e.dataTransfer?.types ?? []).includes('Files');
		const enter = (e: DragEvent) => {
			if (!carries(e)) return;
			depth.current++;
			setOver(true);
		};
		const move = (e: DragEvent) => {
			if (!carries(e)) return;
			// Without this the drop never fires and the browser opens the file
			// instead, throwing the whole app away mid-sentence.
			e.preventDefault();
			if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy';
		};
		const leave = (e: DragEvent) => {
			if (!carries(e)) return;
			depth.current = Math.max(0, depth.current - 1);
			if (depth.current === 0) setOver(false);
		};
		const land = (e: DragEvent) => {
			if (!carries(e)) return;
			e.preventDefault();
			depth.current = 0;
			setOver(false);
			dropHere.current(e.dataTransfer?.files ?? null);
		};
		window.addEventListener('dragenter', enter);
		window.addEventListener('dragover', move);
		window.addEventListener('dragleave', leave);
		window.addEventListener('drop', land);
		return () => {
			window.removeEventListener('dragenter', enter);
			window.removeEventListener('dragover', move);
			window.removeEventListener('dragleave', leave);
			window.removeEventListener('drop', land);
			depth.current = 0;
		};
	}, []);

	// What can go, and whether anything is still coming. Nothing is sent while
	// something is on its way up: a message that left without its picture cannot
	// be mended afterwards.
	const ready = items.filter((x): x is Item & { stored: Attachment } => x.stored !== null);
	const climbing = items.length !== ready.length;

	// One send at a time, whichever way it was asked for. `busy` only greyed
	// the button, and Enter went straight past it: pressing it twice while the
	// first one was on its way sent the same message twice (Codex, 23/09/2026).
	// A ref, because two key presses can land before the first re-render.
	const sending = useRef(false);
	// What the box holds right now, for after the wait: the sentence as it was
	// when the send started is in `body` below, and it is stale by then.
	const bodyNow = useRef(body);
	bodyNow.current = body;

	async function send() {
		const text = body.trim();
		if (sending.current) return;
		if (climbing) return;
		if (!text && ready.length === 0) return;
		sending.current = true;
		setBusy(true);
		const sentBody = body;
		const sentItems = new Set<Item>(ready);
		try {
			await api.send(
				props.channelID,
				text,
				ready.map((f) => f.stored.id),
				props.replyTo?.id ?? 0,
			);
			// Only what went. The box stays usable while the message is on its
			// way, and whatever was written or attached meanwhile is the next
			// message, not this one: clearing the lot erased it.
			if (bodyNow.current === sentBody) {
				setBody('');
				props.onDraft(props.channelID, '');
			}
			setItems((cur) => cur.filter((x) => !sentItems.has(x)));
			setError('');
			props.onSent();
			log('mensagem enviada');
		} catch (err) {
			setError(message(err));
		} finally {
			sending.current = false;
			setBusy(false);
			box.current?.focus();
		}
	}

	function onKeyDown(e: KeyboardEvent) {
		if (pick && e.key === 'Escape') {
			e.preventDefault();
			setPick(null);
			return;
		}
		if (matches.length > 0) {
			if (e.key === 'ArrowDown') {
				e.preventDefault();
				setHighlight((h) => (h + 1) % matches.length);
				return;
			}
			if (e.key === 'ArrowUp') {
				e.preventDefault();
				setHighlight((h) => (h - 1 + matches.length) % matches.length);
				return;
			}
			if (e.key === 'Enter' || e.key === 'Tab') {
				e.preventDefault();
				complete(matches[highlight].username);
				return;
			}
			if (e.key === 'Escape') {
				setPick(null);
				return;
			}
		}
		if (e.key === 'Escape' && props.replyTo) {
			e.preventDefault();
			props.onCancelReply();
			return;
		}
		if (e.key === 'Enter' && !e.shiftKey) {
			e.preventDefault();
			void send();
		}
	}

	function onPaste(e: ClipboardEvent) {
		const parts = Array.from(e.clipboardData?.items ?? []);
		const images = parts.filter((i) => i.kind === 'file' && i.type.startsWith('image/'));
		if (images.length === 0) return;
		e.preventDefault();
		void attach(images.map((i) => i.getAsFile()).filter((f): f is File => f !== null));
	}

	grow();

	// putEmoji drops one into the sentence where the cursor was, not at the end:
	// somebody who went back to fix a word is still standing there.
	const putEmoji = (e: string): void => {
		const el = box.current;
		const at = el ? (el.selectionStart ?? body.length) : body.length;
		const next = body.slice(0, at) + e + body.slice(el ? (el.selectionEnd ?? at) : at);
		setBody(next);
		props.onDraft(props.channelID, next);
		// After the redraw, or the cursor is put back where the old text ended.
		setTimeout(() => {
			if (!el) return;
			el.focus();
			const where = at + e.length;
			el.setSelectionRange(where, where);
		}, 0);
	};

	return (
		// The typing box in the new drawing (design item 7, 24/09/2026): one
		// island of surface with everything in it - attach, the text, emoji and
		// Enviar - and no rule above it. It was the last screen still in the
		// old drawing: a line across the conversation, outlined buttons, a
		// 12px corner and a faded blue Enviar.
		// No side padding: the island's edges are the header's edges above it,
		// and its foot is 8 off the window like the account bar's beside it.
		// A telephone's header is a band from edge to edge, so there the island
		// keeps the 16 it had rather than touching the glass.
		<div data-composer class="relative bg-background pb-2 max-md:px-4 max-md:pb-4">
			{pick && (
				<div data-mentions class="mention-panel absolute bottom-full left-2 z-20 mb-2 rounded-panel bg-surface p-2 text-foreground">
					<p class="px-2 pb-2 text-label uppercase text-foreground-muted">Pessoas nesta conversa</p>
					<ul id="mention-options" role="listbox" aria-label="pessoas nesta conversa">
						{matches.map((m, i) => (
							<li key={m.username} role="option" id={'mention-' + m.username} aria-selected={i === highlight}>
								<button type="button" tabIndex={-1} class={'mention-option relative flex h-11 w-full items-center gap-3 rounded-control px-2 text-left ' + (i === highlight ? 'bg-default is-selected' : 'hover:bg-default')}
									onMouseDown={(e) => e.preventDefault()} onClick={() => complete(m.username)}>
									<span data-face class="grid h-7 w-7 shrink-0 place-items-center rounded-full bg-surface-3 text-meta">{m.username === 'todos' ? '@' : m.display_name.slice(0, 2).toUpperCase()}</span>
									<span class="min-w-0 flex-1 truncate text-item">{m.display_name}</span><span class="truncate text-meta text-foreground-muted">@{m.username}</span>
								</button>
							</li>
						))}
					</ul>
					{matches.length === 0 && <p class="px-2 py-3 text-item text-foreground-muted" role="status">Ninguém com esse nome.</p>}
				</div>
			)}

			{/* Who is being answered, right above where the answer is typed, so
			    that it cannot be forgotten about — which is exactly what happens
			    when the only sign of it is a highlight somewhere up the page. */}
			{props.replyTo && (
				<div
					class="mb-2 flex h-10 items-center gap-2 rounded-control bg-surface pl-3 pr-1 text-meta text-foreground-muted"
					data-replying={props.replyTo.id}
				>
					<Icon name="arrow-shape-turn-up-left" size={16} />
					<span class="shrink-0">Respondendo a</span>
					<span class="shrink-0 font-semibold text-foreground">{props.replyTo.author.display_name}</span>
					<span class="min-w-0 flex-1 truncate">
						{props.replyTo.deleted ? '' : props.replyTo.body || 'Imagem'}
					</span>
					<button
						class="grid h-8 w-8 shrink-0 place-items-center rounded-control hover:bg-default hover:text-foreground"
						aria-label="não responder"
						title="Não responder"
						onClick={props.onCancelReply}
					>
						<Icon name="xmark" size={12} />
					</button>
				</div>
			)}

			{preview && <Lightbox file={preview} onClose={() => setPreview(null)} />}
			{items.length > 0 && (
				<div class="mb-2 flex flex-wrap gap-2">
					{items.map((f) => (
						<div key={f.key} class="rise relative" data-upload={f.name}>
							{f.stored && f.stored.mime.startsWith('image/') ? (
								<button type="button" aria-label={"Ampliar " + f.name} onClick={() => setPreview(f.stored)}><img
									src={f.stored.url}
									alt={f.stored.name}
									class="h-16 w-16 rounded-control bg-surface object-cover"
								/></button>
							) : (
								<span
									class="flex h-16 w-40 flex-col justify-center gap-1 rounded-control bg-surface px-3 text-meta text-foreground-muted"
									title={f.name}
								>
									<span class="truncate text-foreground">{f.name}</span>
									{f.stored ? (
										<span>Pronto para enviar</span>
									) : (
										<>
											{/* A <progress> and not a coloured box: a box that
											    grows needs its width written onto the element,
											    and nothing on this page carries an inline style
											    (the CSP forbids it, and security_test.go
											    checks). Here the number is an attribute, the
											    browser draws the bar, and a screen reader reads
											    it out loud. */}
											<progress class="upload-bar" value={Math.round(f.sent * 100)} max={100} />
											{/* The pause at the end of every bar there ever was:
											    the bytes are up, and the server is still reading
											    them. Saying so beats a full bar that sits there
											    looking stuck. */}
											<span>{f.sent < 1 ? `${Math.round(f.sent * 100)}%` : 'Guardando…'}</span>
										</>
									)}
								</span>
							)}
							<button
								class="absolute -right-1.5 -top-1.5 grid h-6 w-6 place-items-center rounded-control bg-surface-3 text-foreground-muted hover:bg-danger hover:text-foreground"
								title={f.stored ? 'Tirar' : 'Cancelar'}
								aria-label={f.stored ? 'tirar' : 'cancelar'}
								onClick={() => forget(f.key)}
							>
								<Icon name="xmark" size={12} />
							</button>
						</div>
					))}
				</div>
			)}

			{error && <p class="mb-2 px-1 text-meta text-danger-soft-fg">{error}</p>}

			<div class="composer-island flex items-end gap-1 rounded-control bg-surface p-1.5" data-composer-island>
				<label
					class={TOOL + ' cursor-pointer'}
					title="Mandar arquivo: imagem, PDF, som, vídeo ou zip — ou arraste para a conversa"
					aria-label="mandar arquivo"
				>
					<Icon name="file" />
					<input
						type="file"
						// A hint for the file chooser and nothing more: what is
						// actually allowed is decided from the bytes, on the server.
						accept="image/png,image/jpeg,image/gif,image/webp,application/pdf,text/plain,audio/*,video/mp4,video/webm,application/zip"
						multiple
						class="hidden"
						onChange={(e) => {
							const el = e.target as HTMLInputElement;
							void attach(el.files);
							el.value = '';
						}}
					/>
				</label>
				<button
					class={TOOL}
					title="Emoji"
					aria-label="emoji"
					data-emoji-open
					onClick={() => { setPick(null); setEmoji(true); }}
				>
					<Icon name="face-smile" />
				</button>
				<textarea
					ref={box}
					aria-autocomplete="list"
					aria-controls={pick ? "mention-options" : undefined}
					aria-activedescendant={pick && matches[highlight] ? "mention-" + matches[highlight].username : undefined}
					rows={1}
					class="scroller max-h-40 min-h-9 flex-1 resize-none self-center bg-transparent px-1 py-1.5 text-message text-foreground outline-none placeholder:text-foreground-muted"
					placeholder={props.placeholder}
					value={body}
					onInput={onInput}
					onKeyDown={onKeyDown}
					onPaste={onPaste}
				/>
				<button
					// Blue when there is something to send, the flat grey of a
					// control with nothing to do when there is not - and not a
					// faded blue, which read as broken rather than as waiting.
					class="h-9 shrink-0 rounded-control bg-accent px-4 text-meta font-semibold text-foreground hover:bg-accent-hover disabled:cursor-default disabled:bg-default disabled:text-foreground-muted"
					disabled={busy || climbing || (!body.trim() && ready.length === 0)}
					title={climbing ? 'Esperando o arquivo subir' : undefined}
					onClick={() => void send()}
				>
					Enviar
				</button>
			</div>

			{emoji && <Picker onPick={putEmoji} onClose={() => setEmoji(false)} />}

			{/* The whole window, because that is what somebody aims at. It takes no
			    pointer events of its own: appearing under the cursor with them on
			    would fire a dragleave at itself and count the file back out again,
			    which is the classic way this breaks. Under the call screen (z-40)
			    on purpose — a file dropped during a call is not for the call. */}
			{over && (
				<div
					class="pointer-events-none fixed inset-0 z-30 flex items-center justify-center bg-veil p-6"
					data-drop
				>
					<div class="rise flex flex-col items-center gap-3 rounded-panel border-2 border-dashed border-accent bg-surface px-10 py-8 text-center">
						<span class="text-accent">
							<Icon name="file" size={32} />
						</span>
						<p class="text-title text-foreground">Solte aqui para mandar {props.where}</p>
						<p class="text-meta text-foreground-muted">Até {MAX_FILES} por mensagem, 32 MB cada.</p>
					</div>
				</div>
			)}
		</div>
	);
}
