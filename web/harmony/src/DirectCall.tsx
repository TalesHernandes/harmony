// What a direct call looks like: the box that asks, and the screen it becomes.
//
// Everything that is a stream lives in direct.ts, behind a subscription, so
// that a redraw here can never interrupt anybody's audio or blink anybody's
// picture (CLAUDE.md). This file holds names, flags and buttons.

import { useEffect, useRef, useState } from 'preact/hooks';
import { direct, SLOT_CAM, SLOT_MIC, SLOT_SCREEN, SLOT_SCREEN_AUDIO, type DirectView } from './direct';
import { FaceFill } from './Avatar';
import { nameOf } from './names';
import { Icon } from './Icon';
import { BAR, CHIP, CHIP_BIG, Deafened, IDLE, LEAVE, MUTED, OFF, ON } from './CallKit';
import { BIG, CIRCLE_BIG, CIRCLE_SMALL, GRID, STRIP } from './Call';
import type { User } from './api';
import { startRing, stopRing } from './sounds';

// useDirect re-renders when the call changes, and nothing more.
export function useDirect(): DirectView {
	const [view, setView] = useState(direct.snapshot());
	useEffect(() => direct.subscribe(() => setView(direct.snapshot())), []);
	return view;
}

// Video is one picture in the call. It is its own component so that a change
// anywhere else in the screen cannot make the element be recreated — a
// recreated <video> is a black flash.
function Video({ slot, mine, cover }: { slot: number; mine?: boolean; cover?: boolean }) {
	const el = useRef<HTMLVideoElement | null>(null);
	useEffect(() => {
		direct.mount(el.current, slot, mine);
	});
	return (
		<video
			ref={el}
			autoPlay
			playsInline
			muted={mine}
			class={'h-full w-full ' + (cover ? 'object-cover' : 'object-contain')}
		/>
	);
}

// Sound has no picture, and it still needs an element: this is where the other
// person's voice actually comes out.
function Sound({ slot }: { slot: number }) {
	const el = useRef<HTMLAudioElement | null>(null);
	useEffect(() => {
		direct.mount(el.current, slot);
	});
	return <audio ref={el} autoPlay />;
}

function clock(since: number): string {
	if (!since) return '';
	const total = Math.floor((Date.now() - since) / 1000);
	const mm = String(Math.floor(total / 60)).padStart(2, '0');
	const ss = String(total % 60).padStart(2, '0');
	return `${mm}:${ss}`;
}

// IncomingCall is the box that asks. On a monitor it is a card at the top of
// the window — the rest of the app stays usable under it — and on a telephone
// it is the whole screen, the way a telephone rings (Etapa 3 §03): the face,
// the name, and two wide buttons where a thumb lands. Answering is accent
// because it is your choice; refusing is danger. No green: nobody is talking
// yet.
function IncomingCall({ view }: { view: DirectView }) {
	// A call nobody answers stops asking, and it stops asking here at the same
	// moment the server forgets it (api/call.go ringFor) and the push expires.
	// Through a ref, because the caller redraws for reasons of its own and a
	// restarted timer would ring for ever.
	const id = view.id;
	useEffect(() => {
		const timer = setTimeout(() => void direct.hangup('missed'), 45_000);
		return () => clearTimeout(timer);
	}, [id]);

	const name = view.peer ? nameOf(view.peer.username, view.peer.display_name) : 'Alguém';
	const wide =
		'flex h-11 flex-1 items-center justify-center gap-2 rounded-control text-[14px] font-medium ' +
		'max-sm:h-14 max-sm:flex-none max-sm:text-body';
	return (
		<div
			class="safe-fixed-top fixed inset-x-0 top-0 z-40 flex justify-center p-3 max-sm:bottom-0 max-sm:bg-background max-sm:px-6 max-sm:pb-6"
			data-incoming-call
		>
			<div class="flex w-full max-w-sm flex-col gap-4 rounded-panel bg-surface-2 p-4 max-sm:max-w-none max-sm:bg-transparent max-sm:p-0">
				<div class="flex min-w-0 items-center gap-3 max-sm:flex-1 max-sm:flex-col max-sm:items-start max-sm:justify-center max-sm:gap-4">
					<span data-face class="grid h-12 w-12 shrink-0 place-items-center rounded-full bg-surface-3 text-[18px] font-semibold max-sm:h-24 max-sm:w-24 max-sm:text-[36px]">
						<FaceFill name={name} username={view.peer?.username ?? ''} avatar={view.peer?.avatar} />
					</span>
					<span class="min-w-0">
						<span class="block truncate text-item max-sm:text-[20px] max-sm:leading-7 max-sm:font-semibold">
							{name}
						</span>
						<span class="block text-meta text-foreground-muted max-sm:text-body">
							está te ligando
						</span>
					</span>
				</div>
				<div class="flex gap-2 max-sm:flex-col">
					<button
						class={wide + ' bg-accent text-foreground hover:bg-accent-hover'}
						data-call-accept
						onClick={() => void direct.accept()}
					>
						<Icon name="handset-arrow-in" size={20} />
						Atender
					</button>
					<button
						class={"danger-action " + (wide + ' bg-danger-soft text-danger-soft-fg hover:bg-danger hover:text-foreground')}
						data-call-reject
						onClick={() => void direct.hangup('rejected')}
					>
						<Icon name="xmark" size={20} />
						Recusar
					</button>
				</div>
			</div>
		</div>
	);
}

// The green ring of whoever is talking, as in a room (Call.tsx Speaking): a
// border on a span over the block, never a shadow on a video.
function Talking({ who }: { who: 'me' | 'peer' }) {
	const [on, setOn] = useState(false);
	useEffect(() => direct.watchSpeaking(who, setOn), [who]);
	if (!on) return null;
	return <span class="pointer-events-none absolute inset-0 rounded-panel border-2 border-success" data-speaking={who} />;
}

// One of the two people, drawn as a seat of the room (Call.tsx Seat): the
// camera when it is on, the face otherwise, the name in a chip in the corner
// with the microphone or hearing mark, and the ring when they talk.
function Person({
	who,
	name,
	username,
	avatar,
	camera,
	muted,
	deafened,
	big,
	size,
}: {
	who: 'me' | 'peer';
	name: string;
	username: string;
	avatar?: string;
	camera: boolean;
	muted: boolean;
	deafened?: boolean;
	big: boolean;
	size: string;
}) {
	const chip = big ? CHIP_BIG : CHIP;
	return (
		<div class={'relative shrink-0 ' + size} data-person-tile={who}>
			<div class={'absolute inset-0 overflow-hidden rounded-panel ' + (camera ? 'bg-surface-2' : 'bg-surface')}>
				{camera ? (
					<Video slot={SLOT_CAM} mine={who === 'me'} cover={who === 'me'} />
				) : (
					<span class="absolute inset-0 grid place-items-center">
						<span
							data-face
							class={'grid place-items-center rounded-full bg-surface-3 font-semibold text-foreground ' + (big ? CIRCLE_BIG : CIRCLE_SMALL)}
						>
							<FaceFill name={name} username={username} avatar={avatar} />
						</span>
					</span>
				)}
				<span class={chip + ' absolute bottom-2 left-2 max-w-[calc(100%-16px)]'} data-nome>
					{deafened ? (
						<span class="shrink-0 text-danger-soft-fg" title="Não está ouvindo">
							<Deafened size={12} />
						</span>
					) : muted ? (
						<span class="shrink-0 text-danger-soft-fg" title="Microfone desligado">
							<Icon name="microphone-slash" size={12} />
						</span>
					) : null}
					<span class="truncate">{who === 'me' ? `${name} (você)` : name}</span>
				</span>
				<Talking who={who} />
			</div>
		</div>
	);
}

// A screen being shown, by either of the two: the big block, as a screen
// watched in a room.
function Shown({ mine, label }: { mine: boolean; label: string }) {
	return (
		<div class={'relative shrink-0 ' + BIG} data-shown={mine ? 'mine' : 'peer'}>
			<div class="absolute inset-0 overflow-hidden rounded-panel bg-surface-2">
				<Video slot={SLOT_SCREEN} mine={mine} />
				<span class={CHIP_BIG + ' absolute bottom-2 left-2 max-w-[calc(100%-16px)]'}>
					<span class="shrink-0">
						<Icon name="display-pulse" size={12} />
					</span>
					<span class="truncate">{label}</span>
				</span>
			</div>
		</div>
	);
}

// CallScreen is the call itself, over the app rather than inside it: while it
// is up it is the only thing anybody is doing, and putting it in the
// conversation pane would mean the layout of every other screen has to know
// about calls.
//
// Drawn as a room is (the owner, 18/10/2026: "parecer mais com a interface de
// uma chamada normal em um canal"): the two people as two equal blocks, the
// green ring on whoever talks, a shared screen big with the two of them in a
// strip under it, and the room's bar — microphone, hearing, camera, screen,
// and the red way out.
function CallScreen({ view, me }: { view: DirectView; me: User | null }) {
	// The clock ticks on its own, once a second, and only while the call is up.
	const [, tick] = useState(0);
	useEffect(() => {
		if (view.state !== 'live') return;
		const timer = setInterval(() => tick((n) => n + 1), 1000);
		return () => clearInterval(timer);
	}, [view.state]);

	const name = view.peer ? nameOf(view.peer.username, view.peer.display_name) : '';
	const myName = me ? nameOf(me.username, me.display_name) : 'Você';
	const status =
		view.state === 'calling'
			? 'Chamando…'
			: view.state === 'connecting'
				? // Accepted, and the two browsers are finding a route to each
					// other: that is the wait, so that is what it says.
					'Ligando o áudio…'
				: clock(view.since);
	// getDisplayMedia does not exist on a telephone, and a button that opens a
	// dialog saying "not supported" is worse than no button.
	const canShare = typeof navigator.mediaDevices?.getDisplayMedia === 'function';
	// "Reconectando…" is the line getting worse and still working, which is
	// warning; everything else this says is something that failed.
	const worse = view.notice === 'Reconectando…';
	// Theirs wins over yours: it is the thing being pointed at.
	const shown = view.peerScreen ? 'peer' : view.screen ? 'mine' : '';
	const people = (size: string, big: boolean) => [
		<Person
			key="peer"
			who="peer"
			name={name}
			username={view.peer?.username ?? ''}
			avatar={view.peer?.avatar}
			camera={view.peerCam}
			muted={view.peerMuted}
			big={big}
			size={size}
		/>,
		<Person
			key="me"
			who="me"
			name={myName}
			username={me?.username ?? ''}
			avatar={me?.avatar}
			camera={view.cam}
			muted={view.muted}
			deafened={view.deafened}
			big={big}
			size={size}
		/>,
	];

	return (
		<div class="safe-fixed fixed inset-0 z-40 flex flex-col gap-2 bg-background p-2 max-sm:p-2" data-call-screen>
			{/* The room's header, with the person where the channel's name goes. */}
			<div class="flex h-12 shrink-0 items-center gap-2 rounded-panel bg-surface px-2">
				{/* Out of the way, not hung up. It says so in words: a bare chevron
				    beside a hang-up button reads just as easily as "end this". */}
				<button
					class="flex h-9 shrink-0 items-center gap-2 rounded-control bg-default px-3 text-meta font-medium text-foreground hover:bg-default-hover"
					title="A chamada continua — você volta por cima da conversa"
					aria-label="voltar ao app sem desligar"
					data-call-shrink
					onClick={() => direct.shrink(true)}
				>
					<Icon name="chevron-down" size={16} />
					<span class="max-sm:hidden">Voltar ao app</span>
				</button>
				<span class="flex min-w-0 items-center gap-2 px-1">
					<span class="shrink-0 text-foreground-muted">
						<Icon name="handset" size={18} />
					</span>
					<span class="truncate text-item font-semibold">{name}</span>
					<span class="shrink-0 tabular-nums text-meta text-foreground-muted" data-call-status>
						· {status}
					</span>
				</span>
			</div>

			{view.notice && (
				<p
					class={
						'shrink-0 rounded-panel px-3 py-2 text-meta text-foreground ' + (worse ? 'bg-warning-soft' : 'bg-danger-soft')
					}
				>
					{view.notice}
				</p>
			)}

			{shown ? (
				<div class="flex min-h-0 flex-1 flex-col gap-2">
					<div class="grid min-h-0 flex-1 place-items-center [container-type:size]" data-grande>
						<Shown mine={shown === 'mine'} label={shown === 'mine' ? 'Sua tela' : `Tela de ${name}`} />
					</div>
					<div class="flex h-28 w-full shrink-0 justify-center gap-2 max-sm:h-20" data-tira>
						{people(STRIP, false)}
					</div>
				</div>
			) : (
				<div class="flex min-h-0 flex-1 flex-wrap content-center justify-center gap-2 [container-type:size]" data-grade>
					{people(GRID[2], true)}
				</div>
			)}

			<div class={BAR} data-call-controls>
				<button
					class={view.muted || view.deafened ? MUTED : IDLE}
					title={view.deafened ? 'Volte a ouvir para ligar o microfone' : view.muted ? 'Ligar o microfone' : 'Desligar o microfone'}
					aria-label="microfone"
					aria-pressed={view.muted}
					disabled={view.deafened}
					data-call-mute
					onClick={() => direct.toggleMute()}
				>
					{view.muted || view.deafened ? <Icon name="microphone-slash" size={20} /> : <Icon name="microphone" size={20} />}
				</button>
				<button
					class={view.deafened ? MUTED : IDLE}
					title={view.deafened ? 'Voltar a ouvir' : 'Não ouvir'}
					aria-label={view.deafened ? 'voltar a ouvir' : 'não ouvir'}
					aria-pressed={view.deafened}
					data-call-deafen
					onClick={() => direct.toggleDeafen()}
				>
					{view.deafened ? <Deafened size={20} /> : <Icon name="headphones" size={20} />}
				</button>
				<button
					class={view.cam ? ON : OFF}
					title={view.cam ? 'Desligar a câmera' : 'Ligar a câmera'}
					aria-label="câmera"
					aria-pressed={view.cam}
					data-call-cam
					onClick={() => void direct.toggleCam()}
				>
					<Icon name="camera" size={20} />
				</button>
				{canShare && (
					<button
						class={view.screen ? ON : OFF}
						title={view.screen ? 'Parar de compartilhar' : 'Compartilhar a tela'}
						aria-label="tela"
						aria-pressed={view.screen}
						data-call-share
						onClick={() => void (view.screen ? direct.stopScreen() : direct.startScreen())}
					>
						<Icon name={view.screen ? 'display-pulse' : 'display'} size={20} />
					</button>
				)}
				<span class="min-w-2 flex-1" />
				<button
					class={LEAVE}
					title="Desligar"
					aria-label="desligar"
					data-call-hangup
					onClick={() => void direct.hangup()}
				>
					<Icon name="handset-arrow-out" size={20} />
					<span class="max-sm:hidden">Desligar</span>
				</button>
			</div>
		</div>
	);
}

// CallStrip is the call while somebody is using the rest of the app: who and
// for how long, the two buttons worth having at hand, and a way back to the
// whole thing (Componentes §10, "faixa de chamada minimizada").
//
// It is drawn *in* the layout rather than floating over it, under the header of
// the conversation, so it covers nothing — a floating pill would sit on top of
// the box you type in, on a telephone, which is the one thing that must never
// be covered.
export function CallStrip() {
	const view = useDirect();
	const [, tick] = useState(0);
	useEffect(() => {
		if (view.state !== 'live' || !view.small) return;
		const timer = setInterval(() => tick((n) => n + 1), 1000);
		return () => clearInterval(timer);
	}, [view.state, view.small]);

	if (!view.small || view.state === 'idle' || view.state === 'ringing') return null;
	const name = view.peer ? nameOf(view.peer.username, view.peer.display_name) : '';
	const small = 'grid h-9 w-9 shrink-0 place-items-center rounded-control max-sm:h-12 max-sm:w-12';
	return (
		<div class="shrink-0 px-4 pt-2 max-sm:px-3">
			<div class="flex items-center gap-3 rounded-control bg-surface px-3 py-2" data-call-strip>
				{/* The call is up. */}
				<span class="h-2 w-2 shrink-0 rounded-full bg-success" />
				<span class="flex min-w-0 flex-1 flex-col">
					<span class="truncate text-meta font-medium">{name}</span>
					<span class="text-meta tabular-nums text-foreground-muted">
						{view.state === 'live' ? clock(view.since) : 'Chamando…'}
					</span>
				</span>
				{/* The way back. Same rule as the button that got you here: say
				    where it goes, not which direction it moves. */}
				<button
					class="h-9 shrink-0 rounded-control bg-default px-3 text-meta font-medium text-foreground hover:bg-default-hover max-sm:h-12"
					title="Voltar para a tela da chamada"
					aria-label="voltar para a chamada"
					data-call-open
					onClick={() => direct.shrink(false)}
				>
					Ver chamada
				</button>
				<button
					class={
						small +
						' bg-default hover:bg-default-hover ' +
						(view.muted ? 'text-danger-soft-fg' : 'text-foreground')
					}
					title={view.muted ? 'Ligar o microfone' : 'Desligar o microfone'}
					aria-label="microfone"
					aria-pressed={view.muted}
					data-strip-mute
					onClick={() => direct.toggleMute()}
				>
					{view.muted ? <Icon name="microphone-slash" /> : <Icon name="microphone" />}
				</button>
				<button
					class={small + ' bg-danger-soft text-danger-soft-fg hover:bg-danger hover:text-foreground'}
					title="Desligar"
					aria-label="desligar"
					data-strip-hangup
					onClick={() => void direct.hangup()}
				>
					<Icon name="handset-arrow-out" />
				</button>
			</div>
		</div>
	);
}

// DirectCall is the whole of it, in one place the app can draw unconditionally.
//
// The two sound elements are here rather than inside the call screen, and that
// is not tidiness: putting the call away unmounts the screen, and an unmounted
// <audio> is a call you can no longer hear. What is drawn changes; what is
// playing does not.
export function DirectCall({ me = null }: { me?: User | null }) {
	const view = useDirect();

	// The ring sounds while somebody is asking, on both sides: the person being
	// called hears the bell, and the person calling hears that it is ringing,
	// quieter (sounds.ts).
	useEffect(() => {
		if (view.state === 'ringing' || view.state === 'calling') startRing(view.state === 'calling');
		else stopRing();
		return stopRing;
	}, [view.state]);

	if (view.state === 'idle') return null;
	if (view.state === 'ringing') return <IncomingCall view={view} />;
	return (
		<>
			<Sound slot={SLOT_MIC} />
			<Sound slot={SLOT_SCREEN_AUDIO} />
			{!view.small && <CallScreen view={view} me={me} />}
		</>
	);
}
