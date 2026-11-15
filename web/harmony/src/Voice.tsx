import { isMuted } from './blocks';
// The voice channels in the column, and the strip at the bottom with the
// controls.
//
// A voice channel is a room, not a conversation: pressing it puts you in it and
// shows the call. It has no chat of its own — the owner's call, and the right
// one, because a room people are talking in is not a place anybody was going to
// read later.
//
// This file holds names, flags and numbers. Everything that is a stream lives in
// media.ts, behind a subscription, so a redraw here cannot interrupt anybody's
// audio or blink anybody's screen.

import type { ComponentChildren } from 'preact';
import { useEffect, useState } from 'preact/hooks';
import type { Channel, VoicePeer } from './api';
import { voice, SCREEN, CAMERA, type VoiceView } from './media';
import { useSpeaking } from './Call';
import { Deafened, Quality, ScreenButton } from './CallKit';
import { Avatar } from './Avatar';
import { nameOf } from './names';
import { Icon, MARK } from './Icon';

// useVoice re-renders when the session changes, and nothing more. What comes
// back is plain data.
export function useVoice(): VoiceView {
	const [view, setView] = useState(voice.snapshot());
	useEffect(() => voice.subscribe(() => setView(voice.snapshot())), []);
	return view;
}

// Person is one line under a voice channel: who, whether they can be heard, and
// whether they have a screen or a camera on. Pressing it opens who they are.
function Person({
	peer,
	me,
	onOpen,
	onMenu,
}: {
	peer: VoicePeer;
	me: boolean;
	onOpen: () => void;
	// The menu on a person. It was on the people list and on the conversations
	// and not here, which is the one place somebody is most likely to want it:
	// this is the list you are looking at while you are in a call with them.
	onMenu: () => void;
}) {
	const sharing = peer.publishing ?? [];
	// The same green as the ring in the call, in the place people are actually
	// looking when nobody has a camera on: the list in the column. The owner
	// asked for both, and the column is the one that matters more — it is on
	// screen while you are reading a channel, and the call is not.
	//
	// A ring drawn *inside* the face rather than a border around it, so a person
	// lighting up does not move the two below them by a pixel. It lives in
	// src/Avatar.tsx now, with the rest of what a face is.
	const talking = useSpeaking(peer.username);
	return (
		// Thirty-six pixels like every other row, and indented by exactly the
		// room's own padding plus its mark plus its gap - 8 + 18 + 6 = 32, which
		// is pl-8 - so a face starts precisely where the room's name starts and
		// the block reads as one thing hanging off it. Those three numbers have
		// all moved at least once; if one moves again this has to move with it.
		//
		// No presence dot on this face. Being in the room *is* the presence,
		// and a green dot beside a green ring would be the same news twice.
		<button
			class={
				'flex h-9 w-full items-center gap-1.5 rounded-control py-0 pl-8 pr-2 text-left text-item ' +
				(talking
					? 'text-foreground hover:bg-surface-2'
					: 'text-foreground-muted hover:bg-surface-2 hover:text-foreground')
			}
			onClick={onOpen}
			onContextMenu={(ev) => {
				ev.preventDefault();
				onMenu();
			}}
			title="Clique para ver o perfil, botão direito para as opções"
			data-person={peer.username}
			data-speaking={talking ? '1' : ''}
		>
			<Avatar username={peer.username} avatar={peer.avatar} name={peer.display_name || peer.username} size={28} speaking={talking} />
			<span class="min-w-0 flex-1 truncate">
				{nameOf(peer.username, peer.display_name)}
				{me && <span class="text-foreground-muted"> (você)</span>}
			</span>
			<span class="flex shrink-0 items-center gap-1">
				{isMuted(peer.username) && <span title="Silenciado por você" aria-label="Silenciado por você" data-person-muted><Icon name="microphone-slash" size={14} /></span>}
				{sharing.includes(SCREEN) && <span class="flex items-center" title="Está com a tela ligada"><Icon name="display-pulse" size={14} /></span>}
				{sharing.includes(CAMERA) && <span class="flex items-center" title="Está com a câmera ligada"><Icon name="camera" size={14} /></span>}
				{peer.deafened ? (
					<span class="flex items-center text-danger-soft-fg" title="Não está ouvindo"><Deafened size={14} /></span>
				) : peer.muted ? (
					<span class="flex items-center text-danger-soft-fg" title="Microfone desligado"><Icon name="microphone-slash" size={14} /></span>
				) : null}
			</span>
		</button>
	);
}

// VoiceChannels is the "CANAIS DE VOZ" block in the channel column.
export function VoiceChannels({
	channels,
	rooms,
	meUsername,
	canManage,
	onCreate,
	onOpen,
	onOpenPerson,
	onMenuPerson,
	menuFor,
	onMenuRoom,
	roomMenu,
	onInvite,
	activeID,
	bare = false,
	rowProps,
}: {
	channels: Channel[];
	rooms: Record<string, VoicePeer[]>;
	meUsername: string;
	canManage: boolean;
	onCreate: () => void;
	onOpen: (id: string) => void;
	onOpenPerson: (username: string) => void;
	// Opening the menu on somebody, and drawing it. The list is here and the
	// menu belongs to the app, because the same menu is opened from three
	// lists now.
	onMenuPerson: (username: string) => void;
	menuFor: (username: string) => ComponentChildren;
	// The room's own menu (the owner's "Apagar canal"), opened with a right
	// click on the room's name and drawn by the app like every other menu.
	onMenuRoom: (c: Channel) => void;
	roomMenu: (c: Channel) => ComponentChildren;
	// Calling somebody into the room you are in. Only ever drawn on that room:
	// you invite people to where you are, and the server refuses anything else
	// (api/ring.go).
	onInvite: () => void;
	activeID: string;
	// Inside a folder (D12a): the rooms only, under the folder's own heading.
	bare?: boolean;
	// What the column's drag needs on each room's row (App.tsx): the drop
	// target's data attribute, the pointer handler, and the class that draws
	// where a drop would land.
	rowProps?: (c: Channel) => Record<string, unknown>;
}) {
	const view = useVoice();
	// The heading stays for the owner with no room under it: it carries the
	// + that makes one, and every room may have been moved into folders.
	if (channels.length === 0 && (bare || !canManage)) return null;

	return (
		<div class={bare ? 'space-y-0.5' : 'mt-2 space-y-0.5'}>
			{!bare && <div data-drop-folder="" class="flex items-center pb-1 pl-2 pt-1 text-label uppercase text-foreground-muted">
				<span>Voz</span>
				{canManage && (
					<button
						class="ml-auto -my-1 grid h-8 w-8 place-items-center rounded-control hover:bg-surface-2 hover:text-foreground"
						title="Novo canal de voz"
						aria-label="novo canal de voz"
						onClick={onCreate}
					>
						<Icon name="plus" size={12} />
					</button>
				)}
			</div>}
			{channels.map((c) => {
				const people = rooms[c.id] ?? [];
				const here = view.channelID === c.id && view.state !== 'off';
				return (
					<div key={c.id} {...(rowProps?.(c) ?? {})}>
						<div
							class={
								'group relative flex h-9 items-center rounded-control text-item ' +
								(c.id === activeID
									? 'bg-default font-semibold text-foreground'
									: 'text-foreground-muted hover:bg-surface-2')
							}
						>
							{c.id === activeID && (
								<span class="absolute inset-y-0 -left-2 w-[3px] rounded-r-full bg-accent" />
							)}
							<button
								class="flex min-w-0 flex-1 items-center gap-1.5 px-2 text-left hover:text-foreground"
								onClick={() => {
									onOpen(c.id);
									if (!here) void voice.join(c.id);
								}}
								onContextMenu={(ev) => {
									ev.preventDefault();
									onMenuRoom(c);
								}}
								title="Entrar na sala"
							>
								{/* Being *in* the room and *looking at* it are two
								    different facts and they were sharing one mark. The
								    surface and the accent bar say which room you have
								    open; the green mark says which one your voice is
								    in - the colour of somebody being heard, which is what
								    it is (docs/PLANO-DESIGN.md §3).

								    Headphones and not the speaker, chosen by the owner
								    on 20/09/2026 out of the six the kit allows: *"nao
								    gostei muito do icone de volume dos canais de voz...
								    talvez ele seja muito redondo"*. The same mark now
								    stands for a voice room everywhere - here, in the
								    conversation header, in the incoming ring and on the
								    profile's join button - because a room that is drawn
								    two ways is two things. */}
								<span class={MARK + (here ? ' text-success' : '')}>
									<Icon name="headphones" size={18} />
								</span>
								{/* No head count: the owner took it out on
								    23/09/2026. The people in the room are listed
								    right under its name, so the number said
								    again what the list already shows. */}
								<span class="min-w-0 flex-1 truncate">{c.name}</span>
							</button>
							{/* The owner's second look at the invite button: "ele
							    seria no menu da esquerda, um botãozinho de +
							    apenas". It sits on the room you are *in*, which is
							    the only room you can call anybody into — so it is
							    never a question of which room it means.

							    And it only shows under the pointer, which he asked
							    for on 20/09/2026: a button parked permanently on one
							    row was one more thing in a column he had just called
							    cluttered. `revela` (style.css) keeps it on a
							    telephone, where there is no pointer to wait for. */}
							{here && (
								<button
									class="revela h-9 w-9 shrink-0 place-items-center rounded-control text-foreground-muted hover:bg-surface-3 hover:text-foreground"
									title="Chamar alguém para esta sala"
									aria-label="chamar alguém para esta sala"
									data-open="invite"
									onClick={onInvite}
								>
									<Icon name="person-plus" size={16} />
								</button>
							)}
							{roomMenu(c)}
						</div>
						{people.length > 0 && (
							<div class="space-y-0.5 pt-0.5">
								{people.map((p) => (
									<div key={p.session_id} class="relative">
										<Person
											peer={p}
											me={p.username === meUsername}
											onOpen={() => onOpenPerson(p.username)}
											onMenu={() => onMenuPerson(p.username)}
										/>
										{menuFor(p.username)}
									</div>
								))}
							</div>
						)}
					</div>
				);
			})}
		</div>
	);
}

// VoiceBar is the strip above your own name: what room you are in, and the
// controls. It draws nothing at all when you are not in one.
//
// Two things since 23/09/2026 (the owner's option C): where you are, and the
// way out, plus the same screen selector as the call bar. The microphone
// and the hearing moved to your account's line (VoiceToggles, below).
function voiceStep(step: VoiceView['step']): string {
	if (step === 'mic') return 'Abrindo o microfone…';
	if (step === 'server') return 'Entrando na sala…';
	if (step === 'route') return 'Ligando o áudio…';
	return 'Conectando…';
}

export function VoiceBar({
	channels,
	onOpenCall,
	onOpenScreen,
}: {
	channels: Channel[];
	onOpenCall: (id: string) => void;
	onOpenScreen: () => void;
}) {
	const view = useVoice();
	if (view.state === 'off' && !view.error) return null;

	const room = channels.find((c) => c.id === view.channelID);

	// "Voz conectada" is plain text now. It was accent, and accent means "this
	// is what you picked" — the strip is about a room you are in, which the dot
	// beside it already says.
	// Not one "Conectando…" for everything (the owner, 24/09/2026): each of
	// the waits in media.ts join() gets its own words, and a try at coming
	// back after a drop says which try it is.
	let status = voiceStep(view.step);
	let tone = 'text-foreground-muted';
	if (view.state === 'joining' && view.attempt > 0) {
		status = `Reconectando (${view.attempt} de 3)…`;
	}
	if (view.state === 'live') {
		status = 'Voz conectada';
		tone = 'text-foreground';
	} else if (view.state === 'lost') {
		status = 'A conexão caiu, voltando…';
		tone = 'text-danger-soft-fg';
	}
	if (view.error === 'mic') {
		status = 'Sem microfone';
		tone = 'text-danger-soft-fg';
	} else if (view.error === 'camera') {
		status = 'Sem câmera';
		tone = 'text-danger-soft-fg';
	} else if (view.error === 'join') {
		status = 'Não deu para entrar';
		tone = 'text-danger-soft-fg';
	}

	return (
		// Like the account's line under it: what you read starts 16 in, and the
		// leave button ends 8 from the edge, on the line the + of each group,
		// the gear and the channel rows' pills end on (8 since 24/09/2026).
		<div class="shrink-0 pb-2 pl-4 pr-2">
			<div class="flex items-center gap-2">
				<button
					class="flex min-w-0 flex-1 items-center gap-2 text-left"
					disabled={!room}
					onClick={() => room && onOpenCall(room.id)}
					title={room ? 'Voltar para a chamada' : ''}
				>
					{/* One line (the owner, 23/09/2026): the status, the room,
					    the bars. When it is short of room the room's name gives
					    way first; the status and the bars never do. */}
					<span class="flex min-w-0 items-center gap-1.5 text-meta">
						<span class={'shrink-0 font-medium ' + tone}>{status}</span>
						<span class="min-w-0 truncate text-foreground-muted">{room ? room.name : '—'}</span>
						{view.state === 'live' && <Quality />}
					</span>
				</button>
				<ScreenButton sharing={view.sharingScreen} onOpen={onOpenScreen} compact />
				{/* The room's own "Sair": solid red, white handset (the owner,
				    23/09/2026: "o mesmo formato de cor e ícone"), in the same
				    square as the island's other buttons. */}
				<button
					class="grid h-8 w-8 shrink-0 place-items-center rounded-control bg-danger text-foreground hover:bg-danger-hover"
					title="Sair da sala"
					aria-label="sair da sala"
					onClick={() => void voice.leave()}
				>
					<Icon name="handset-arrow-out" size={20} />
				</button>
			</div>

			{view.notice && (
				<p class="pt-1 text-meta text-danger-soft-fg" onClick={() => voice.dismiss()}>
					{view.notice}
				</p>
			)}
			{view.error === 'mic' && (
				<p class="pt-1 text-meta text-foreground-muted">
					O navegador não deu acesso ao microfone. Libere na barra de endereço e entre de novo.
				</p>
			)}
			{view.error === 'camera' && (
				<p class="pt-1 text-meta text-foreground-muted">
					O navegador não deu acesso à câmera. Libere na barra de endereço e tente de novo.
				</p>
			)}
		</div>
	);
}

// Your microphone and your hearing, on your account's line at the bottom of
// the column, while you are in a room — the owner's option C (23/09/2026),
// "com o highlightzinho" they had: the raised ground of the call's buttons, at
// the island's 32px, and red when you switched them off. Outside a room they
// are not drawn: muting before joining has never been a thing this app did.
export function VoiceToggles() {
	const view = useVoice();
	if (view.state === 'off') return null;
	const button = 'grid h-8 w-8 shrink-0 place-items-center rounded-control bg-surface-2 hover:bg-surface-3';
	// Deafened turns the microphone off as well, and it *looks* off: not
	// hearing anybody means not talking to them either, which is the point.
	return (
		<>
			<button
				class={button + (view.muted || view.deafened ? ' text-danger-soft-fg' : ' text-foreground')}
				title={
					view.deafened
						? 'Seu microfone está desligado porque você não está ouvindo'
						: view.muted
							? 'Ligar o microfone'
							: 'Desligar o microfone'
				}
				aria-pressed={view.muted || view.deafened}
				disabled={view.deafened}
				data-toggle="mic"
				onClick={() => voice.setMuted(!view.muted)}
			>
				{view.muted || view.deafened ? (
					<Icon name="microphone-slash" size={20} />
				) : (
					<Icon name="microphone" size={20} />
				)}
			</button>
			<button
				class={button + (view.deafened ? ' text-danger-soft-fg' : ' text-foreground')}
				title={view.deafened ? 'Voltar a ouvir' : 'Não ouvir ninguém'}
				aria-pressed={view.deafened}
				data-toggle="deafen"
				onClick={() => voice.setDeafened(!view.deafened)}
			>
				{view.deafened ? <Deafened size={20} /> : <Icon name="headphones" size={20} />}
			</button>
		</>
	);
}
