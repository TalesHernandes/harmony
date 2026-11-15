import { isBlocked, usePreferences } from './blocks';
// The history pane: one page of messages, grouped the way a chat window is
// read, plus scroll-back.
//
// Nothing here interprets a message body as markup. The body is text nodes and
// only text nodes, so a message that contains angle brackets shows angle
// brackets. The only things pulled out of it are links and mentions, and both
// are matched, not parsed: a link is an <a> to an http(s) address and nothing
// else.

import { useEffect, useLayoutEffect, useRef, useState } from 'preact/hooks';
import { message, type Attachment, type Embed, type Message, type User } from './api';
import { nameOf } from './names';
import { Icon } from './Icon';
import { Avatar } from './Avatar';
import { Picker, Reactions } from './Reactions';
import { Confirm, useDialogFocus } from './Confirm';
import { withEmoji } from './Twemoji';

const MENTION = /@([a-z0-9][a-z0-9._-]{1,30}[a-z0-9])/gi;

const LINK = /https?:\/\/[^\s<>]+/gi;

// Where a link ends, the way chat programs read it (and api/preview.go, so the
// card and the blue text agree): closing punctuation belongs to the sentence,
// and a ")" to the link only when the link opened one.
export function trimLink(url: string): string {
	let end = url.length;
	while (end > 0) {
		const c = url[end - 1];
		if ('.,;:!?\'"'.includes(c)) {
			end--;
			continue;
		}
		if (c === ')') {
			const s = url.slice(0, end);
			if (s.split('(').length < s.split(')').length) {
				end--;
				continue;
			}
		}
		break;
	}
	return url.slice(0, end);
}

// Links first, so an "@" inside an address is not taken for a person.
function Body({ body, me }: { body: string; me: string }) {
	const parts: any[] = [];
	let last = 0;
	LINK.lastIndex = 0;
	for (let m = LINK.exec(body); m; m = LINK.exec(body)) {
		const url = trimLink(m[0]);
		if (!url.includes('://') || url.endsWith('://')) continue;
		if (m.index > last) parts.push(...Mentions(body.slice(last, m.index), me));
		parts.push(
			<a
				href={url}
				target="_blank"
				rel="noopener noreferrer"
				class="break-all text-accent-soft-fg hover:underline"
			>
				{url}
			</a>,
		);
		last = m.index + url.length;
		LINK.lastIndex = last;
	}
	if (last < body.length) parts.push(...Mentions(body.slice(last), me));
	return <span class="body-text">{parts}</span>;
}

function Mentions(body: string, me: string): any[] {
	const parts: any[] = [];
	let last = 0;
	MENTION.lastIndex = 0;
	for (let m = MENTION.exec(body); m; m = MENTION.exec(body)) {
		if (m.index > last) parts.push(...withEmoji(body.slice(last, m.index)));
		// "@todos" is everybody's name, so it lights up for everybody — the same
		// word the server treats as every member of this channel.
		const mine = m[1].toLowerCase() === me || m[1].toLowerCase() === 'todos';
		parts.push(
			<span
				class={
					'-mx-1 rounded-control px-1 py-px font-medium bg-accent-soft ' +
					(mine ? 'text-foreground' : 'text-accent-soft-fg')
				}
			>
				@{m[1]}
			</span>,
		);
		last = m.index + m[0].length;
	}
	if (last < body.length) parts.push(...withEmoji(body.slice(last)));
	return parts;
}

// The stamp on a message. Until now it was the hour alone, which is fine while
// you are reading the bottom of a conversation and useless the moment you
// scroll up — "14:32" of which day? The separator above says the day, but it
// can be a screen and a half away.
//
// So: the hour for today, "ontem" for yesterday, and the date for anything
// older, with the whole thing — seconds and all — always in the tooltip.
function clock(iso: string): string {
	const d = new Date(iso);
	const time = d.toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit' });
	const today = new Date();
	const sameDay = (a: Date, b: Date) => a.toDateString() === b.toDateString();
	if (sameDay(d, today)) return time;
	const yesterday = new Date(today.getTime() - 86400_000);
	if (sameDay(d, yesterday)) return 'Ontem ' + time;
	return d.toLocaleDateString('pt-BR', { day: '2-digit', month: '2-digit' }) + ' ' + time;
}

function fullStamp(iso: string): string {
	return new Date(iso).toLocaleString('pt-BR', { dateStyle: 'full', timeStyle: 'medium' });
}

function day(iso: string): string {
	return new Date(iso).toLocaleDateString('pt-BR', {
		weekday: 'long',
		day: '2-digit',
		month: 'long',
	});
}

// Images is the row of pictures under a message.
//
// Two things changed here and they are the same thing twice. The box used to be
// laid out from the picture's own width and height and the picture then drawn
// *inside* it with object-contain, which is a letterbox: a tall photograph got
// black bars beside it and a wide one got them above, and the message sat in a
// box bigger than what was in it. Now the box is the picture — w-auto, h-auto,
// capped — and there is nothing left over to letterbox.
//
// The width and height attributes stay, and they are not decoration: they are
// what lets the browser hold the right amount of room before the bytes arrive,
// so a conversation does not jump as pictures land in it.
function Images({ files, onOpen }: { files: Attachment[]; onOpen: (a: Attachment) => void }) {
	if (files.length === 0) return null;
	// One picture and several pictures are two different things, and the design
	// draws the second one: a row of squares, 4px apart, 400px across.
	//
	// The first one is left alone on purpose, and this is the one place the
	// design was not followed to the letter. A single picture is nearly always a
	// screenshot somebody wants *read*, and squaring it either crops the half
	// that matters or letterboxes it — which is the exact complaint that was
	// fixed in D4c ("fica muito ruim, cria um scroll"). So: alone, the box is
	// the picture; in company, the design's grid, cropped, because a wall of
	// thumbnails with black bars between them is worse than a wall of crops.
	const alone = files.length === 1;
	if (alone) {
		const f = files[0];
		return (
			<div class="pt-1">
				<button
					class="block cursor-zoom-in overflow-hidden rounded-control"
					onClick={() => onOpen(f)}
					title={f.name}
					data-image={f.id}
				>
					<img
						src={f.url}
						alt={f.name}
						width={f.width || undefined}
						height={f.height || undefined}
						loading="lazy"
						class="block h-auto max-h-96 w-auto max-w-full"
					/>
				</button>
			</div>
		);
	}
	return (
		<div class="flex max-w-[400px] gap-1 pt-1">
			{files.map((f) => (
				<button
					key={f.id}
					class="aspect-square min-w-0 flex-1 cursor-zoom-in overflow-hidden rounded-control bg-surface-2"
					onClick={() => onOpen(f)}
					title={f.name}
					data-image={f.id}
				>
					<img
						src={f.url}
						alt={f.name}
						loading="lazy"
						class="block h-full w-full object-cover"
					/>
				</button>
			))}
		</div>
	);
}

// Gone is what stands in for an attachment whose file no longer exists. Nine
// of them were lost in the move off the Contabo (18/09/2026), and before this
// they showed as a file name with nothing under it — which read as "still
// loading" forever. Saying it is gone is the only true thing left to say.
function Gone({ files }: { files: Attachment[] }) {
	if (files.length === 0) return null;
	return (
		<div class="flex flex-col gap-1 pt-1">
			{files.map((f) => (
				<div
					key={f.id}
					class="flex w-full max-w-[320px] items-center gap-3 rounded-control bg-surface-2 p-3"
					data-gone={f.id}
				>
					<span class="shrink-0 text-foreground-muted">
						<Icon name="file" size={24} />
					</span>
					<span class="min-w-0">
						<span class="block truncate text-meta text-foreground-muted" title={f.name}>
							{f.name}
						</span>
						<span class="block text-meta text-foreground-muted">
							Este arquivo não está mais no servidor.
						</span>
					</span>
				</div>
			))}
		</div>
	);
}

// Embeds are the cards the server made from the links in a message (api.ts
// Embed). A video, or a page with a wide picture, puts the picture under the
// text at the card's width; anything else gets a small square at the side,
// the way the Discord the friends came from draws them. The whole card is one
// link to the page; nothing plays here.
function Embeds({ list }: { list?: Embed[] }) {
	if (!list?.length) return null;
	return (
		<div class="flex flex-col gap-1 pt-1">
			{list.map((e) => {
				const wide = !!e.image && (e.kind === 'video' || (e.width ?? 0) >= 400);
				return (
					<a
						key={e.url}
						href={e.url}
						target="_blank"
						rel="noopener noreferrer"
						title={e.url}
						class="group flex w-full max-w-[432px] gap-3 rounded-control bg-surface-2 p-3 hover:bg-surface-3"
						data-embed={e.kind}
					>
						<span class="flex min-w-0 flex-1 flex-col gap-1">
							{e.site && <span class="truncate text-meta text-foreground-muted">{e.site}</span>}
							{e.title && (
								<span class="line-clamp-2 font-semibold text-accent-soft-fg group-hover:underline">
									{e.title}
								</span>
							)}
							{e.description && (
								<span class="line-clamp-3 text-meta text-foreground-muted">{e.description}</span>
							)}
							{wide && (
								<span class="relative mt-1 block overflow-hidden rounded-control">
									<img
										src={e.image}
										alt=""
										width={e.width || undefined}
										height={e.height || undefined}
										loading="lazy"
										class="block h-auto max-h-72 w-full object-cover"
									/>
									{e.kind === 'video' && (
										<span class="absolute inset-0 flex items-center justify-center">
											<svg width="56" height="56" viewBox="0 0 56 56" aria-hidden="true">
												<circle cx="28" cy="28" r="28" fill="rgba(0,0,0,0.6)" />
												<path d="M23 18 L39 28 L23 38 Z" fill="#fff" />
											</svg>
										</span>
									)}
								</span>
							)}
						</span>
						{!wide && e.image && (
							<img
								src={e.image}
								alt=""
								loading="lazy"
								class="size-20 shrink-0 rounded-control object-cover"
							/>
						)}
					</a>
				);
			})}
		</div>
	);
}

// EditBox is a message being rewritten in place. It takes the cursor the
// moment it opens — pressing "editar" and then having to click into the box
// was one step too many (the owner, 24/09/2026) — with the caret at the end,
// where somebody fixing a typo usually is. Once, on opening, and not on every
// redraw: a box that grabs focus back whenever the list redraws would fight
// anybody who clicked away. And the two buttons, because the keys are only
// news to somebody who reads the line under the box.
function EditBox({
	draft,
	error,
	onInput,
	onSave,
	onCancel,
}: {
	draft: string;
	error: string;
	onInput: (text: string) => void;
	onSave: () => void;
	onCancel: () => void;
}) {
	const box = useRef<HTMLTextAreaElement | null>(null);
	useEffect(() => {
		const el = box.current;
		if (!el) return;
		el.focus();
		el.setSelectionRange(el.value.length, el.value.length);
	}, []);
	return (
		<div class="flex flex-col gap-2 py-1" data-editing>
			<textarea
				ref={box}
				class="field w-full resize-none rounded-control bg-surface-2 px-3 py-2 text-message text-foreground"
				rows={2}
				value={draft}
				onInput={(e) => onInput((e.target as HTMLTextAreaElement).value)}
				onKeyDown={(e) => {
					if (e.key === 'Enter' && !e.shiftKey) {
						e.preventDefault();
						onSave();
					}
					if (e.key === 'Escape') onCancel();
				}}
			/>
			{error && <p class="text-meta text-danger-soft-fg">Não salvou: {error}</p>}
			<div class="flex items-center gap-2">
				<button
					class="h-8 rounded-control bg-accent px-3 text-meta font-medium text-foreground hover:bg-accent-hover"
					data-edit-save
					onClick={onSave}
				>
					Salvar
				</button>
				<button
					class="danger-action h-8 rounded-control bg-default px-3 text-meta font-medium text-foreground hover:bg-default-hover"
					data-edit-cancel
					onClick={onCancel}
				>
					Cancelar
				</button>
				<span class="text-meta text-foreground-muted">Enter salva, Esc cancela.</span>
			</div>
		</div>
	);
}

// The row of things you can do to a message, which only exists while somebody
// is pointing at one.
//
// Until 18/09/2026 it was four words in a thin box, the same colour as each
// other, and the owner was right about it: "muito sem sal e sem açúcar". Four
// words in a row is a *menu*, not a set of controls — and worse, it made the one
// action with no undo look exactly as harmless as the one that can be undone.
//
// So each one is a target of its own, with its own ground under it when it is
// pointed at, and the destructive one answers in red and stands behind a
// hairline. Twenty-eight pixels square, which is the size the design asked
// for.
//
// **They were words until 20/09/2026, and that was deliberate.** The kit file
// we could read had no pencil and no bin, geometry does not get drawn by hand
// here (PLANO-DESIGN §11.1), and four buttons where two are icons and two are
// words is worse than four words. The two glyphs arrived with the D4t, and
// this became what the plan said it would: four icons, and nothing else
// moved.
const action =
	'grid h-8 w-8 place-items-center rounded-control text-foreground-muted hover:bg-surface-3 hover:text-foreground';
// Red at rest as well as under the pointer (the owner, 23/09/2026): the one
// thing in the row that cannot be taken back says so before you reach it.
const destructive =
	'grid h-8 w-8 place-items-center rounded-control text-danger-soft-fg hover:bg-danger-soft';

// The date a conversation crosses, and the line where somebody stopped reading.
// They are components because the call notice below needs them too, and two
// copies of a divider is two chances to change one of them.
function DayLine({ day }: { day: string }) {
	return (
		<div class="flex items-center gap-3 py-1.5" data-day-line>
			<span class="h-px flex-1 bg-border" />
			<span class="text-label uppercase tracking-[0.06em] text-foreground">{day}</span>
			<span class="h-px flex-1 bg-border" />
		</div>
	);
}

// The unread line is deliberately not the day line in another colour. It has a
// rule on one side only, with the words at the right-hand end, so that the eye
// lands on the words and then on the first message under them — the design's
// own arrangement, and the difference is what keeps two horizontal rules three
// lines apart from reading as the same kind of thing.
function UnreadLine() {
	return (
		<div class="flex items-center gap-3 pb-1 pt-3">
			<span class="h-px flex-1 bg-danger" />
			<span class="text-label uppercase tracking-[0.06em] text-danger-soft-fg">não lidas</span>
		</div>
	);
}

// prettySize is a number somebody can read. Nobody wants "13421772 bytes".
function prettySize(bytes: number): string {
	if (bytes >= 1 << 20) return (bytes / (1 << 20)).toFixed(1).replace('.', ',') + ' MB';
	if (bytes >= 1 << 10) return Math.round(bytes / (1 << 10)) + ' KB';
	return bytes + ' B';
}

// Files is everything attached to a message that is not a picture: sound to
// listen to, video to watch, and everything else as a card with its name, its
// size and a way to keep it.
//
// The three shapes are not decoration. A voice note that has to be downloaded
// before it can be heard is a voice note nobody listens to, and a PDF that
// tries to play is a broken video — what a file *is* decides what it is worth
// showing, and that comes from the type the server decided at upload time
// (api/files.go), never from the name.
function Files({ files }: { files: Attachment[] }) {
	if (files.length === 0) return null;
	return (
		<div class="flex flex-col gap-1 pt-1">
			{files.map((f) => {
				if (f.mime.startsWith('audio/')) {
					return (
						<div key={f.id} class="max-w-[320px]" data-file={f.id}>
							<p class="truncate text-meta text-foreground-muted">{f.name}</p>
							<audio src={f.url} controls preload="metadata" class="w-full" />
						</div>
					);
				}
				if (f.mime.startsWith('video/')) {
					return (
						<div key={f.id} class="max-w-md" data-file={f.id}>
							<video
								src={f.url}
								controls
								preload="metadata"
								class="max-h-80 w-full rounded-control"
							/>
							<p class="truncate text-meta text-foreground-muted">
								{f.name} · {prettySize(f.size)}
							</p>
						</div>
					);
				}
				// The card the design draws: 320 across, the kind of file on the
				// left at 24, and its name over its weight.
				//
				// The design also puts a 36px button on the right with a
				// down-arrow-on-a-page in it, and that glyph is not in the
				// twenty-four the kit handed over (docs/PLANO-DESIGN.md §11.1
				// — it is the fourth one missing, and it goes in the same
				// request as the other three). Rather than draw an arrow by
				// hand or borrow a glyph that means something else, the word
				// stays where it already was: the whole card is the link, and
				// the second line says what pressing it does.
				return (
					<a
						key={f.id}
						href={f.url}
						download={f.name}
						data-file={f.id}
						title={'Baixar ' + f.name}
						class="flex w-full max-w-[320px] items-center gap-3 rounded-control bg-surface p-3 hover:bg-surface-2"
					>
						<span class="flex shrink-0 text-foreground-muted" aria-hidden="true">
							<Icon
								name={
									f.mime === 'application/pdf'
										? 'file-text'
										: f.mime === 'application/zip'
											? 'file-zipper'
											: 'file'
								}
								size={28}
							/>
						</span>
						<span class="flex min-w-0 flex-1 flex-col">
							<span class="truncate text-item">{f.name}</span>
							<span class="text-meta text-foreground-muted">{prettySize(f.size)}</span>
						</span>
						{/* The arrow the card was waiting for. The whole card
						    is still the link, and this is not a second button:
						    it is what pressing the card does, drawn where the
						    eye ends up. */}
						<span
							class="grid h-10 w-10 shrink-0 place-items-center rounded-control bg-default text-foreground"
							aria-hidden="true"
						>
							<Icon name="file-arrow-down" />
						</span>
					</a>
				);
			})}
		</div>
	);
}

// CallNotice is the line a call that nobody answered leaves behind (D4i).
//
// It is the same row at both ends of the same call and it says different things
// there, which is the whole reason it is drawn here and not written as a
// sentence by the server: the person who rang reads "ninguém atendeu", and the
// person who was rung reads "chamada perdida de fulano" — the second one being
// the sentence the owner said was missing entirely.
function CallNotice({ m, me }: { m: Message; me: string }) {
	const mine = m.author.username === me;
	const who = nameOf(m.author.username, m.author.display_name);
	const rejected = m.body === 'rejected';
	const text = rejected
		? mine
			? `${who} recusou a chamada`
			: 'Você recusou a chamada'
		: mine
			? 'Você ligou — ninguém atendeu'
			: `Chamada perdida de ${who}`;
	return (
		<div
			class="grid grid-cols-[48px_minmax(0,1fr)] py-1 text-foreground-muted"
			data-call-note={m.id}
		>
			<span class="grid h-6 place-items-center">
				{rejected ? <Icon name="ban" size={20} /> : <Icon name="handset" size={20} />}
			</span>
			<div class="min-w-0 text-message">
				<span data-call-note-text class={rejected ? '' : 'text-danger-soft-fg'}>{text}</span>
				<span class="ml-2 whitespace-nowrap text-meta text-foreground-muted" title={fullStamp(m.created_at)}>{clock(m.created_at)}</span>
			</div>
		</div>
	);
}

// PinNotice is the line a new pin leaves (30/10/2026, as Discord does). The
// words are drawn here from the kind; the body is only for apps that do not know
// it. "mensagem" jumps to the pinned line when it is on this page.
function PinNotice({ m, me }: { m: Message; me: string }) {
	const who = m.author.username === me ? 'Você' : nameOf(m.author.username, m.author.display_name);
	const target = m.reply_to?.id;
	return (
		<div class="grid grid-cols-[48px_minmax(0,1fr)] py-1 text-foreground-muted" data-pin-note={m.id}>
			<span class="grid h-6 place-items-center">
				<Icon name="pin" size={20} />
			</span>
			<div class="min-w-0 text-message">
				<span data-pin-note-text>
					{who} fixou{' '}
					{target ? (
						<button
							class="font-semibold text-foreground hover:underline"
							onClick={() => document.getElementById('msg-' + target)?.scrollIntoView({ block: 'center' })}
						>
							uma mensagem
						</button>
					) : (
						'uma mensagem'
					)}
					.
				</span>
				<span class="ml-2 whitespace-nowrap text-meta text-foreground-muted" title={fullStamp(m.created_at)}>{clock(m.created_at)}</span>
			</div>
		</div>
	);
}

// Lightbox is the picture on its own, over the app rather than in a tab of its
// own — the owner's request, and the thing every service like this one does.
//
// A new tab was never wrong, it was just a one-way door: it loses the
// conversation, and on a telephone it loses the app. Here Escape, the ✕ and
// anywhere outside the picture all come back, which are the three things people
// try. Download keeps the file without leaving the conversation.
export function Lightbox({ file, onClose, author, createdAt, photo }: { photo?: boolean; file: Attachment; onClose: () => void; author?: string; createdAt?: string }) {
	const card = useRef<HTMLDivElement>(null);
	useDialogFocus(card, onClose);
	const imageGesture = useRef(false);
	const picture = useRef<HTMLImageElement>(null);
	const [view, setView] = useState({ scale: 1, x: 0, y: 0 });
	const drag = useRef<{ x: number; y: number } | null>(null);
	const fit = () => setView({ scale: 1, x: 0, y: 0 });
	const zoom = (scale: number, x = 0, y = 0) => setView(old => {
		const image = picture.current;
		const limit = image && image.width ? Math.max(16, image.naturalWidth / image.width) : 16;
		const next = Math.max(0.25, Math.min(limit, scale));
		return { scale: next, x: x - (x - old.x) * next / old.scale, y: y - (y - old.y) * next / old.scale };
	});
	const actual = () => {
		const image = picture.current;
		if (view.scale !== 1) fit();
		else if (image) zoom(image.naturalWidth / image.width);
	};
	return (
		<div ref={card} class="conversation-lightbox fixed inset-0 z-50 flex flex-col bg-background p-8 text-foreground" role="dialog" aria-modal="true" aria-label={'imagem ' + file.name} tabIndex={-1} data-lightbox data-photo-lightbox={photo || undefined} onClick={e => { if(e.target === e.currentTarget) onClose(); }}>
			<div class="lightbox-hint absolute left-4 top-4 flex h-8 items-center gap-2 rounded-control bg-surface px-3 text-meta text-foreground-muted"><kbd class="rounded-control bg-surface-2 px-1.5">Esc</kbd> ou um clique no fundo</div>
			<div class="absolute right-6 top-6 z-10 flex gap-2">
				<a class="flex h-11 items-center gap-2 rounded-control bg-surface px-4 text-item" href={file.url} download={file.name}><Icon name="arrow-down-to-line" size={16} />Baixar</a>
				<button type="button" class="grid h-11 w-11 place-items-center rounded-control bg-surface" aria-label="fechar" onClick={onClose}><Icon name="xmark" size={20} /></button>
			</div>
			<div class="image-viewport" onWheel={e => {
				e.preventDefault(); const rect = e.currentTarget.getBoundingClientRect();
				zoom(view.scale * Math.exp(-e.deltaY * 0.002), e.clientX - rect.left - rect.width / 2, e.clientY - rect.top - rect.height / 2);
			}} onDblClick={actual} onPointerDown={e => {
				if(e.button !== 0 || e.target !== picture.current) return;
				imageGesture.current = true; drag.current = { x: e.clientX, y: e.clientY }; e.currentTarget.setPointerCapture(e.pointerId); e.preventDefault();
			}} onPointerMove={e => {
				const d = drag.current; if (!d) return;
				const x = e.clientX - d.x, y = e.clientY - d.y;
				if(view.scale > 1) setView(old => ({ ...old, x: old.x + x, y: old.y + y }));
				d.x = e.clientX; d.y = e.clientY;
			}} onPointerUp={e => { if(e.currentTarget.hasPointerCapture(e.pointerId)) e.currentTarget.releasePointerCapture(e.pointerId); drag.current = null; }} onPointerCancel={() => { drag.current = null; imageGesture.current = false; }} onLostPointerCapture={() => { drag.current = null; }} onClick={e => { if(imageGesture.current) { imageGesture.current = false; return; } if(e.target === e.currentTarget) onClose(); }}>
				<img ref={picture} src={file.url} alt={file.name} draggable={false} style={{ transform: `translate(${view.x}px, ${view.y}px) scale(${view.scale})`, cursor: view.scale > 1 ? 'grab' : 'default' }} />
			</div>
			<div class="image-zoom-controls">
				<button type="button" aria-label="Diminuir zoom" onClick={() => zoom(view.scale / 1.25)}>&minus;</button>
				<button type="button" aria-label="Tamanho ajustado" onClick={fit}>Ajustar</button>
				<button type="button" aria-label="Aumentar zoom" onClick={() => zoom(view.scale * 1.25)}>+</button>
			</div>
			<p class="absolute bottom-4 left-8 right-8 text-center text-meta text-foreground-muted">{[author, createdAt ? clock(createdAt) : '', file.name, prettySize(file.size)].filter(Boolean).join(' · ')}</p>
		</div>
	);
}

// Where each conversation was left, so that coming back to it is coming back
// rather than starting again.
//
// Two things are remembered and the *first* is what makes this work: whether
// the reader was at the bottom. Almost always they were, and "the bottom" is
// the only description of a position that survives new messages arriving — a
// pixel offset would leave somebody one screen above the newest line for no
// reason. Only when they were genuinely reading further up is the offset used,
// and it is safe there because the page of history above it is the same one.
//
// It is kept per person and per device, in the same way and for the same
// reasons as the voice settings: the number is about this screen.
const PLACES = 'harmony.scroll';
const MAX_PLACES = 30;

type Place = { top: number; bottom: boolean };

function places(): Record<string, Place> {
	try {
		const raw = localStorage.getItem(PLACES);
		return raw ? (JSON.parse(raw) as Record<string, Place>) : {};
	} catch {
		return {};
	}
}

function rememberPlace(channel: string, place: Place): void {
	if (!channel) return;
	try {
		const all = places();
		all[channel] = place;
		// A server with twenty channels never reaches this; a browser that has
		// been used for a year should not carry every conversation it ever saw.
		const keys = Object.keys(all);
		if (keys.length > MAX_PLACES) delete all[keys[0]];
		localStorage.setItem(PLACES, JSON.stringify(all));
	} catch {
		// Storage switched off: this visit remembers, the next one does not.
	}
}

interface Props {
	people: User[];
	messages: Message[];
	me: User;
	canModerate: boolean;
	hasMore: boolean;
	hasNewer: boolean;
	jump: { id: number; serial: number } | null;
	onJumped: (serial: number) => void;
	onLoadNewer: () => void;
	onGoRecent: () => void;
	loadingMore: boolean;
	onLoadMore: () => void;
	onEdit: (id: number, body: string) => Promise<void>;
	onRemove: (id: number) => Promise<void>;
	onReply: (m: Message) => void;
	// One emoji on or off. The row of buttons under a message is a row of
	// switches, so this is the same call whichever way it is going.
	onReact: (id: number, emoji: string, on: boolean) => Promise<void>;
	// Pinning (D12a): in a channel the owner's, in a conversation of two
	// either person's. The server decides; this only hides a button that
	// would be refused.
	canPin: boolean;
	onPin: (id: number, on: boolean) => Promise<void>;
	// Opening somebody from their name over a message.
	onPerson: (username: string) => void;
	// The class that paints somebody's name in their role's colour (D12c),
	// '' for none.
	nameColor: (username: string) => string;
	lastReadID: number;
}

export function Messages(props: Props) {
	usePreferences();
	const [shown, setShown] = useState<Set<number>>(() => new Set());
	const { messages, me } = props;
	const scroller = useRef<HTMLDivElement>(null);
	const [editing, setEditing] = useState<number | null>(null);
	const [draft, setDraft] = useState('');
	// Why the last save did not take, shown under the box that is still open.
	const [editError, setEditError] = useState('');
	const [looking, setLooking] = useState<{ file: Attachment; message: Message } | null>(null);
	const [removing, setRemoving] = useState<Message | null>(null);
	// Which message the emoji panel is open for, if any.
	const [reacting, setReacting] = useState<Message | null>(null);
	const [highlight, setHighlight] = useState<{ id: number; serial: number } | null>(null);
	const lastJump = useRef<number | null>(null);
	const [touchMenu, setTouchMenu] = useState<number | null>(null);
	const hold = useRef<{ timer: ReturnType<typeof setTimeout>; x: number; y: number } | null>(null);
	const touchInput = useRef(false);
	const suppressClick = useRef(false);

	function cancelHold() {
		if (hold.current) clearTimeout(hold.current.timer);
		hold.current = null;
	}

	useEffect(() => {
		const outside = (ev: PointerEvent) => {
			suppressClick.current = false;
			if (touchMenu !== null && !(ev.target as Element).closest(`[data-message="${touchMenu}"]`)) {
				setTouchMenu(null);
				setReacting(null);
			}
		};
		document.addEventListener('pointerdown', outside, true);
		return () => {
			document.removeEventListener('pointerdown', outside, true);
		};
	}, [touchMenu]);
	useEffect(() => () => cancelHold(), []);

	useEffect(() => {
		if (highlight === null) return;
		const timer = setTimeout(() => setHighlight(null), 3000);
		return () => clearTimeout(timer);
	}, [highlight]);
	// Remember where the bottom was before the render so that a page of older
	// messages arriving above does not move the reader.
	const anchor = useRef<{ height: number; top: number } | null>(null);
	const stick = useRef(true);
	// Which conversation these messages belong to, as the last render saw it.
	const channel = useRef('');
	// The place to go back to, and how long it is still worth going back to it.
	//
	// The window exists because of pictures. A conversation with images in it
	// is laid out twice — once with the boxes empty and again as each one
	// arrives — and every arrival makes everything below it move. Restoring
	// once, on the first layout, puts somebody at a position that stops meaning
	// what it meant a quarter of a second later. So the position is re-applied
	// while the content is still settling, and the moment the reader touches
	// the wheel it is abandoned: it is their scroll now, not ours.
	const going = useRef<{ place: Place; until: number } | null>(null);

	function goTo(el: HTMLDivElement, place: Place) {
		el.scrollTop = place.bottom ? el.scrollHeight : Math.min(place.top, el.scrollHeight);
	}

	useLayoutEffect(() => {
		const el = scroller.current;
		if (!el) return;
		const now = messages.length ? messages[0].channel_id : '';
		if (props.jump && lastJump.current !== props.jump.serial) {
			const target = props.jump.id ? el.querySelector<HTMLElement>(`[data-message="${props.jump.id}"]`) : null;
			if (props.jump.id && !target) return;
			lastJump.current = props.jump.serial;
			channel.current = now;
			going.current = null;
			anchor.current = null;
			stick.current = !props.jump.id;
			if (target) {
				el.scrollTop += target.getBoundingClientRect().top - el.getBoundingClientRect().top -
					(el.clientHeight - target.offsetHeight) / 2;
				setHighlight({ id: props.jump.id, serial: props.jump.serial });
			} else {
				el.scrollTop = el.scrollHeight;
				setHighlight(null);
			}
			props.onJumped(props.jump.serial);
			return;
		}

		// A different conversation: go where it was left, and keep going there
		// for a moment while pictures land.
		if (now && now !== channel.current) {
			channel.current = now;
			const place = places()[now] ?? { top: 0, bottom: true };
			stick.current = place.bottom;
			going.current = { place, until: Date.now() + 1500 };
			goTo(el, place);
			return;
		}

		if (anchor.current) {
			el.scrollTop = el.scrollHeight - anchor.current.height + anchor.current.top;
			anchor.current = null;
			return;
		}
		if (going.current && Date.now() < going.current.until) {
			goTo(el, going.current.place);
			return;
		}
		if (stick.current) el.scrollTop = el.scrollHeight;
	}, [messages, props.jump?.serial]);

	// Pictures do not arrive on a render, so the layout effect above cannot see
	// them land. This does: it watches the height of the content and puts the
	// reader back while the window is still open.
	useEffect(() => {
		const el = scroller.current;
		if (!el || typeof ResizeObserver === 'undefined') return;
		const watch = new ResizeObserver(() => {
			const place = going.current;
			if (place && Date.now() < place.until) goTo(el, place.place);
			else if (stick.current) el.scrollTop = el.scrollHeight;
		});
		for (const child of Array.from(el.children)) watch.observe(child);
		return () => watch.disconnect();
	}, [messages.length]);

	function onScroll() {
		const el = scroller.current;
		if (!el) return;
		// Somebody moved it themselves, so it is theirs now.
		going.current = null;
		stick.current = !props.hasNewer && el.scrollHeight - el.scrollTop - el.clientHeight < 80;
		rememberPlace(channel.current, { top: el.scrollTop, bottom: stick.current });
		if (el.scrollTop < 120 && props.hasMore && !props.loadingMore) {
			cancelHold();
			anchor.current = { height: el.scrollHeight, top: el.scrollTop };
			props.onLoadMore();
		} else if (el.scrollHeight - el.scrollTop - el.clientHeight < 120 && props.hasNewer && !props.loadingMore) {
			cancelHold();
			// Appending a forward page keeps the old bottom in view.
			stick.current = false;
			props.onLoadNewer();
		}
	}

	// The box closes when the server has the edit, not before. Closing it first
	// turned a refused save into a silent one: the change was gone, the message
	// still said what it said, and opening the editor again put the old text
	// back over what had been typed (Codex, 23/09/2026).
	async function commit(id: number) {
		const text = draft.trim();
		if (!text) {
			setEditing(null);
			return;
		}
		try {
			await props.onEdit(id, text);
			setEditError('');
			setEditing((cur) => (cur === id ? null : cur));
		} catch (err) {
			setEditError(message(err));
		}
	}

	// Where the "não lidas" line goes: above the first message that arrived
	// after the mark **and that somebody else wrote**.
	//
	// The second half is the whole of this. The mark is taken when the
	// conversation is opened and does not move while it is on the screen, so
	// everything written after that is "new" — including your own sentence, and
	// a line announcing that you have not read what you just typed is nonsense.
	// It has to be worked out over the whole page rather than per message: your
	// own line and a reply under it are both newer than the mark, and testing
	// each message on its own would find the first candidate, reject it for
	// being yours, and then reject the reply too because the message before it
	// was already past the mark.
	const unreadAt =
		props.lastReadID > 0
			? (messages.find((m) => m.id > props.lastReadID && m.author.username !== me.username)?.id ?? 0)
			: 0;

	let previous: Message | undefined;
	let previousDay = '';

	return (
		<div data-message-viewport class="relative mb-2 flex min-h-0 flex-1 flex-col">
		<div ref={scroller} onScroll={onScroll} data-history class="scroller flex flex-1 flex-col pl-4 pr-6">
			{/* Everything in the list is pushed to the bottom, which is where a
			    conversation is read from. Until now a channel with four messages
			    in it drew them at the top with a field of black underneath, as
			    far from the box you type in as the screen allows.

			    It is `mt-auto` on a wrapper and not `justify-end` on the
			    scroller itself, and the difference is not cosmetic: a flex
			    column that justifies to the end puts its overflow *above* the
			    scroll origin, and the oldest messages become unreachable — you
			    scroll up and nothing happens. The automatic margin gives the
			    same picture with the overflow on the side the scrollbar can
			    reach. */}
			<div class="mt-auto">
			{props.hasMore && props.loadingMore && (
				<p class="py-2 text-center text-meta text-foreground-muted">Carregando…</p>
			)}
			{!props.hasMore && messages.length > 0 && (
				<p class="py-4 text-center text-meta text-foreground-muted">Começo da conversa.</p>
			)}
			{messages.map((m) => {
				if (isBlocked(m.author.username) && !shown.has(m.id)) return <div key={m.id} id={'msg-'+m.id} class="flex items-center gap-3 px-4 py-3 text-meta text-foreground-muted" data-blocked-message={m.id}>Mensagem de pessoa bloqueada<button class="h-9 rounded-control bg-default px-3 text-foreground hover:bg-default-hover" onClick={() => setShown(new Set([...shown,m.id]))}>Mostrar</button></div>;
				const d = day(m.created_at);
				const newDay = d !== previousDay;
				previousDay = d;
				// Same person, within five minutes, no day break: one block.
				const grouped =
					!newDay &&
					previous !== undefined &&
					!previous.kind &&
					previous.author.username === m.author.username &&
					new Date(m.created_at).getTime() - new Date(previous.created_at).getTime() < 5 * 60_000;
				const firstUnread = unreadAt > 0 && m.id === unreadAt;
				previous = m;
				const mentioned = m.mentions.includes(me.username);
				// A line the server wrote, not a person: it has no author to press on,
				// nothing to reply to and nothing to react to.
				if (m.kind === 'call' || m.kind === 'pin') {
					return (
						<div key={m.id}>
							{newDay && <DayLine day={d} />}
							{firstUnread && <UnreadLine />}
							{m.kind === 'call' ? <CallNotice m={m} me={me.username} /> : <PinNotice m={m} me={me.username} />}
						</div>
					);
				}
				return (
					<div key={m.id}>
						{newDay && <DayLine day={d} />}
						{firstUnread && <UnreadLine />}
						{/* Two columns, and the left one is 48 wide whether or not
						    anything is drawn in it. That empty column is what the
						    design buys: every line of every message starts at the
						    same place, so a run of messages from one person reads
						    as one paragraph with a face beside it instead of as
						    five rows that happen to share a name.

						    The row bleeds past the list's own padding (-mx-4 px-4)
						    so that the tint on a message naming you, and the tint
						    under the pointer, reach the edges of the conversation
						    instead of stopping in a stripe with a gutter round
						    it. */}
						<div
							id={'msg-' + m.id}
							data-message={m.id}
							onPointerDown={(ev) => {
								touchInput.current = ev.pointerType === 'touch';
								cancelHold();
								if (!touchInput.current || m.deleted || editing === m.id ||
									(ev.target as Element).closest('input, textarea, audio, video, [data-actions]')) return;
								hold.current = { x: ev.clientX, y: ev.clientY, timer: setTimeout(() => {
									hold.current = null;
									suppressClick.current = true;
									setTouchMenu(m.id);
								}, 500) };
							}}
							onPointerMove={(ev) => {
								const start = hold.current;
								if (start && Math.hypot(ev.clientX - start.x, ev.clientY - start.y) > 10) cancelHold();
							}}
							onPointerUp={cancelHold}
							onPointerCancel={cancelHold}
							onContextMenu={(ev) => { if (touchInput.current) ev.preventDefault(); }}
							onClickCapture={(ev) => {
								if (suppressClick.current) {
									ev.preventDefault();
									ev.stopPropagation();
									suppressClick.current = false;
								}
							}}
							class={
								// Padding above **and** below, and the gap between
								// blocks is a margin rather than more padding. That
								// split is the whole of it: padding is tinted and
								// margin is not, so the band that appears under the
								// pointer is centred on what was written instead of
								// sitting 12px low. The first version put the block gap
								// in the padding, and the owner saw it immediately —
								// "dá pra ver que a mensagem tá bem descentralizada".
								//
								// Rounded, and set 8px in from the edges so the
								// corners can be seen, with a tint of its own: under
								// the pointer it was bg-surface, the very colour of
								// the columns, and a message lit up as a piece of the
								// column (the owner, 23/09/2026). And 8px of air
								// above and below instead of 4 - "aumente o tamanho
								// das div das mensagens".
								//
								// A line that continues a block gets 2px instead of 8
								// (the owner, 24/09/2026: "diminua o espaçamento apenas
								// entre mensagens consecutivas"): eight lines of "teste"
								// from one person read as eight messages, not one block.
								// Every line has the same room above and below, so the band
								// under the pointer stays centred on its text (lista.spec),
								// and the room between two blocks lives in the margin.
								// Then "o highlight ta pequeno mas o espaco entre as
								// mensagens ta grande" (same day), twice. The second
								// time the two were pulled apart, because padding grows
								// both at once: the band under the pointer is its own
								// layer (.msg-hover in style.css) reaching 4px past the
								// row above and below, so it is 8px around the text,
								// while the rows keep 4 and the gap between two blocks
								// is 4 + 12 + 4 = 20.
								'group relative -mx-2 grid grid-cols-[48px_minmax(0,1fr)] rounded-control px-2 py-1 ' +
								(highlight?.id === m.id ? 'message-highlight ' : '') +
								// Raise the whole isolated row, not just its panel, above siblings.
								(reacting?.id === m.id || touchMenu === m.id ? 'message-actions-open ' : '') +
								(mentioned ? 'mentioned ' : 'msg-hover ') +
								// The date supplies 6px; row padding supplies 4px.
								// Do not add a block margin under the day label.
								(grouped && !m.reply_to || newDay ? '' : 'mt-3 ')
							}
						>
							<span class="message-highlight-layer" aria-hidden="true" />
							{/* The line being answered, above the answer, and on a grid
							    row of its own rather than inside the column of text.
							    That is what keeps the face beside the *name*: as the
							    first thing in the text column it pushed the name down a
							    line and left the avatar level with a quotation, which
							    is the staircase this column exists to prevent.

							    Pressing it jumps to the original if it is on this page —
							    and when it is not, it is at least still readable here,
							    which is the whole point of carrying the quotation. */}
							{m.reply_to && (
								<button
									class="col-start-2 flex min-w-0 items-center gap-2 pb-0.5 text-left text-item text-foreground-muted hover:text-foreground"
									onClick={() => {
										document
											.getElementById('msg-' + m.reply_to!.id)
											?.scrollIntoView({ block: 'center' });
									}}
									data-quote={m.reply_to.id}
								>
									<span class="flex shrink-0">
										<Icon name="arrow-shape-turn-up-left" size={14} />
									</span>
									<Avatar username={m.reply_to.author.username} avatar={props.people.find(person => person.username === m.reply_to!.author.username)?.avatar ?? m.reply_to.author.avatar} name={nameOf(m.reply_to.author.username, m.reply_to.author.display_name)} size={28} />
									<span class="shrink-0 font-semibold">
										{nameOf(m.reply_to.author.username, m.reply_to.author.display_name)}
									</span>
									<span class="min-w-0 truncate">
										{isBlocked(m.reply_to.author.username) ? 'Mensagem de pessoa bloqueada' : m.reply_to.deleted ? (
											<em>Mensagem removida</em>
										) : (
											m.reply_to.body ? withEmoji(m.reply_to.body) : 'Imagem'
										)}
									</span>
								</button>
							)}
							{/* The face begins at the author line, including replies. */}
							{grouped && !m.reply_to ? (
								<span />
							) : (
								// 36px face against a 24px name line plus a 24px first line of
								// text: 6px down centres it on that pair instead of riding high.
								<button class="mt-1.5 self-start text-left" data-message-person={m.author.username} aria-label={'Ver perfil de ' + nameOf(m.author.username, m.author.display_name)} onClick={() => props.onPerson(m.author.username)}>
									<Avatar username={m.author.username} avatar={props.people.find(person => person.username === m.author.username)?.avatar ?? m.author.avatar} name={nameOf(m.author.username, m.author.display_name)} size={36} />
								</button>
							)}
							<div class="flex min-w-0 flex-col">
							{(!grouped || m.reply_to) && (
								<div class="flex h-6 items-baseline gap-2">
									{/* Pressing the name is how you get to somebody, and it
									    is the most natural place in the app to do it. The
									    handle that used to sit beside it is gone: with a
									    face in the gutter and a name in bold, "@fulano" on
									    every single line was the app saying the same thing
									    three times. It is one hover away (the title), one
									    press away (the profile), and always in the column
									    on the right. */}
									<button
										class={'text-left text-name hover:underline ' + props.nameColor(m.author.username)}
										data-author={m.author.username}
										title={'Ver @' + m.author.username}
										onClick={() => props.onPerson(m.author.username)}
									>
										{nameOf(m.author.username, m.author.display_name)}
									</button>
									<span
										class="text-meta text-foreground-muted"
										title={fullStamp(m.created_at)}
									>
										{clock(m.created_at)}
									</span>
								</div>
							)}
							{m.deleted ? (
								<p class="text-message italic text-foreground-muted">
									Mensagem removida{m.deleted_by ? ` por @${m.deleted_by}` : ''}
								</p>
							) : editing === m.id ? (
								<EditBox
									draft={draft}
									error={editError}
									onInput={setDraft}
									onSave={() => void commit(m.id)}
									onCancel={() => {
										setEditError('');
										setEditing(null);
									}}
								/>
							) : (
								<>
									<div class="text-message">
										<Body body={m.body} me={me.username} />
										{m.edited_at && (
											<span class="ml-1 text-meta text-foreground-muted">(editada)</span>
										)}
									</div>
									<Images
										files={m.attachments.filter((f) => !f.missing && f.mime.startsWith('image/'))}
										onOpen={(file) => setLooking({ file, message: m })}
									/>
									<Files
										files={m.attachments.filter((f) => !f.missing && !f.mime.startsWith('image/'))}
									/>
									<Gone files={m.attachments.filter((f) => f.missing)} />
									<Embeds list={m.deleted ? undefined : m.embeds} />
									{/* Under the message and in the flow, unlike the row of actions
										above: these are not a hover state, they are part of what the
										message now says, and a message nobody is pointing at has to
										still show them. */}
									<Reactions
										list={m.reactions ?? []}
										me={me.username}
										onToggle={(emoji, on) => void props.onReact(m.id, emoji, on)}
									/>
								</>
							)}
							{/* Floated over the message, not added under it. In the flow
							    it made every message grow as the mouse crossed it, which
							    pushed the whole conversation down and moved the scroll
							    under the reader — and on the last message the buttons
							    appeared below the bottom edge, where they had to be
							    scrolled to. Over it, the layout never moves. */}
							{!m.deleted && editing !== m.id && (
								<div
									data-actions
									data-actions-open={touchMenu === m.id || reacting?.id === m.id || undefined}
									class={"absolute -top-4 right-2 items-center gap-0.5 rounded-control bg-surface-3 p-1 text-meta " + (reacting?.id === m.id ? "z-30 flex" : touchMenu === m.id ? "z-10 flex" : "z-10 hidden group-hover:flex group-focus-within:flex")}
								>
									{reacting?.id === m.id && (
										<Picker fromMessage
											onClose={() => setReacting(null)}
											onPick={(emoji) => {
												// Pressing one from the panel that is already yours takes it
												// back, the same as pressing the button under the message:
												// two ways in, one meaning.
												const mine = (reacting.reactions ?? []).some(
													(r) => r.emoji === emoji && r.users.includes(me.username),
												);
												void props.onReact(reacting.id, emoji, !mine);
											}}
										/>
									)}
									<button
										class={action}
										data-react={m.id}
										title="Reagir a esta mensagem"
										aria-label="reagir"
										onClick={() => setReacting(m)}
									>
										<Icon name="face-smile" />
									</button>
									<button
										class={action}
										title="Responder a esta mensagem"
										aria-label="responder"
										onClick={() => { setTouchMenu(null); props.onReply(m); }}
									>
										<Icon name="arrow-shape-turn-up-left" />
									</button>
									{props.canPin && !m.kind && (
										<button
											class={action + (m.pinned ? ' text-accent' : '')}
											data-pin={m.id}
											title={m.pinned ? 'Desafixar esta mensagem' : 'Fixar esta mensagem'}
											aria-label={m.pinned ? 'desafixar' : 'fixar'}
											aria-pressed={!!m.pinned}
											onClick={() => {
												setTouchMenu(null);
												void props.onPin(m.id, !m.pinned);
											}}
										>
											<Icon name="pin" />
										</button>
									)}
									{m.author.username === me.username && (
										<button
											class={action}
											title="Editar esta mensagem"
											aria-label="editar"
											onClick={() => {
												setTouchMenu(null);
												setEditError('');
												setEditing(m.id);
												setDraft(m.body);
											}}
										>
											<Icon name="pencil" />
										</button>
									)}
									{(m.author.username === me.username || props.canModerate) && (
										<>
											{/* A hair of a rule before the one that has no
											    undo. Four controls in a row, all the same
											    colour, made "remover" look exactly as
											    harmless as "editar" — and that is still
											    true now that they are glyphs. */}
											<button
												class={destructive}
												data-remove={m.id}
												title="Remover esta mensagem"
												aria-label="remover"
												onClick={(ev) => {
													setTouchMenu(null);
													if (ev.shiftKey) void props.onRemove(m.id);
													else setRemoving(m);
												}}
											>
												<Icon name="trash-bin" />
											</button>
										</>
									)}
								</div>
							)}
							</div>
						</div>
					</div>
				);
			})}
			{messages.length === 0 && (
				<p class="grid h-full place-items-center text-body text-foreground-muted">
					Ninguém falou nada aqui ainda.
				</p>
			)}
			</div>
			{looking && <Lightbox file={looking.file} author={nameOf(looking.message.author.username, looking.message.author.display_name)} createdAt={looking.message.created_at} onClose={() => setLooking(null)} />}
			{/* The pause before a message goes. Removing one is not undoable,
			    and the button sits a few pixels from "responder" under a mouse
			    that is already moving — so the box shows the message itself:
			    "are you sure" is unanswerable without it, because the mistake
			    being guarded against is the wrong message under the pointer.
			    Shift skips it, for an evening of tidying up ten lines. */}
			{removing && (
				<Confirm
					question="Apagar esta mensagem?"
					description="Ela sai para todo mundo nesta conversa, e isso não se desfaz. Fica no lugar a linha de mensagem removida."
					actionLabel="Apagar" actionIcon="trash-bin"
					onCancel={() => setRemoving(null)}
					onConfirm={() => {
						const id = removing.id;
						setRemoving(null);
						void props.onRemove(id);
					}}
				>
					<div class="flex items-baseline gap-2"><span class="text-meta font-semibold">{nameOf(removing.author.username, removing.author.display_name)}</span><span class="text-meta text-foreground-muted">{clock(removing.created_at)}</span></div>
					<p class="body-text text-item">{removing.body || removing.attachments.map((f) => f.name).join(', ')}</p>
				</Confirm>
			)}
		</div>
		{props.hasNewer && (
			<button type="button" data-go-recent class="absolute bottom-2 left-1/2 -translate-x-1/2 rounded-control bg-accent px-4 py-2 text-item text-foreground" onClick={props.onGoRecent}>
				Ir para as recentes
			</button>
		)}
		</div>
	);
}
