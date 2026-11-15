// The three places notifications touch the screen: the panel in Ajustes where
// they are switched on, the one line that offers to switch them on after
// something happened, and the box that appears when somebody calls you while
// you are looking at the app.

import { useEffect, useRef, useState } from 'preact/hooks';
import {
	blip,
	disable,
	enable,
	explain,
	setSoundOn,
	soundOn,
	status,
	type NotifyStatus,
} from './notify';
import { Icon } from './Icon';

// Inside the settings' 16px surface: raised 8px cards, and solid buttons —
// accent for the one that turns something on, default for the rest.
const card = 'rounded-control bg-surface-2 p-4';
const small = 'h-9 rounded-control bg-default px-3 text-meta font-medium text-foreground hover:bg-default-hover';
const go = 'h-9 rounded-control bg-accent px-3 text-meta font-medium text-foreground hover:bg-accent-hover';

// NotifySettings is the honest version of an on/off switch: it says what the
// *server* holds, not what this browser believes, because the two disagreeing
// is exactly how a doorbell ends up off while looking fine.
export function NotifySettings() {
	const [state, setState] = useState<NotifyStatus | null>(null);
	const [busy, setBusy] = useState(false);
	// The server could not be asked. Without this the screen said
	// "Verificando…" for ever, with no switch to press (Codex, 23/09/2026).
	const [failed, setFailed] = useState(false);

	function check() {
		setFailed(false);
		status()
			.then(setState)
			.catch(() => setFailed(true));
	}
	useEffect(check, []);

	async function toggle(on: boolean) {
		setBusy(true);
		try {
			setState(on ? await enable() : await disable());
		} catch {
			setState(await status().catch(() => null));
		} finally {
			setBusy(false);
		}
	}

	return (
		<section class="space-y-2">
			<h3 class="text-label uppercase text-foreground-muted">Notificações</h3>
			<div class={card}>
				<p class="text-item">
					{state ? explain(state.state) : failed ? 'Não consegui perguntar ao servidor.' : 'Verificando…'}
				</p>
				{!state && failed && (
					<button class={small + ' mt-3'} onClick={check}>
						Tentar de novo
					</button>
				)}
				{state && (state.state === 'off' || state.state === 'permitted') && (
					<button
						class={go + ' mt-3'}
						disabled={busy}
						onClick={() => void toggle(true)}
					>
						Ligar neste aparelho
					</button>
				)}
				{state?.state === 'on' && (
					<button class={small + ' mt-3'} disabled={busy} onClick={() => void toggle(false)}>
						Desligar neste aparelho
					</button>
				)}
				{state && state.devices > 0 && (
					<p class="mt-3 text-meta text-foreground-muted">
						{state.devices} aparelho{state.devices > 1 ? 's' : ''} avisado
						{state.devices > 1 ? 's' : ''}.
					</p>
				)}
			</div>
			{/* This one stays. It is not a description of what is on the screen:
			    it is the one fact about a notification that cannot be seen from
			    the outside, and it is what somebody is deciding about here. */}
			<p class="text-meta text-foreground-muted">O aviso diz quem falou e onde, nunca o que foi escrito.</p>

			<MessageSound />
		</section>
	);
}

// MessageSound is the blip with the app open, which is a different question
// from the notification above: that one is about a telephone in a pocket, this
// one is about the room you are sitting in. It is remembered per device for the
// same reason the volumes are.
function MessageSound() {
	const [on, setOn] = useState(soundOn);
	return (
		<div class={card}>
			<p class="text-item">Som quando chega mensagem</p>
			<p class="pt-1 text-meta text-foreground-muted">
				Toca só para o que você não está olhando — outra conversa, outra janela, ou seu nome.
			</p>
			<div class="flex gap-2 pt-3">
				<button
					class={on ? go : small}
					aria-pressed={on}
					data-sound-on
					onClick={() => {
						setSoundOn(true);
						setOn(true);
						// So the choice is heard and not just read.
						blip();
					}}
				>
					Ligado
				</button>
				<button
					class={on ? small : go}
					aria-pressed={!on}
					data-sound-off
					onClick={() => {
						setSoundOn(false);
						setOn(false);
					}}
				>
					Desligado
				</button>
			</div>
		</div>
	);
}

// NotifyNudge is the only time this app asks for permission by itself, and it
// is not the asking: it is one line offering to ask, shown after a mention or a
// private message has already arrived. Pressing "agora não" is final.
export function NotifyNudge({ onEnable, onDismiss }: { onEnable: () => void; onDismiss: () => void }) {
	return (
		<div class="m-2 flex items-center gap-2 rounded-control bg-surface px-3 py-2 text-meta">
			<span class="min-w-0 flex-1 text-foreground-muted">
				Quer ser avisado no celular quando te chamarem, mesmo com o app fechado?
			</span>
			<button class={go} onClick={onEnable}>
				Ligar
			</button>
			<button class={small} onClick={onDismiss}>
				Agora não
			</button>
		</div>
	);
}

export interface IncomingRing {
	channel_id: string;
	channel_name: string;
	from: { username: string; display_name: string };
}

// RingBox is what a call looks like when the app is already open. With the app
// closed the same thing arrives as a notification from the service worker; this
// is the case the push deliberately does not cover, because the server does not
// wake a phone whose owner is looking at the screen.
export function RingBox({
	ring,
	onJoin,
	onDismiss,
}: {
	ring: IncomingRing;
	onJoin: () => void;
	onDismiss: () => void;
}) {
	// A call that nobody answered stops asking. Forty-five seconds is the same
	// life the push itself is given (push.RingTTL), so the two agree.
	//
	// onDismiss goes through a ref rather than into the dependency list: it is
	// written inline by the caller, so it is a different function on every
	// render, and a presence dot changing colour somewhere else on the screen
	// would otherwise restart this timer for ever.
	const dismiss = useRef(onDismiss);
	dismiss.current = onDismiss;
	useEffect(() => {
		const timer = setTimeout(() => dismiss.current(), 45_000);
		return () => clearTimeout(timer);
	}, [ring.channel_id, ring.from.username]);

	return (
		<div class="safe-fixed-top fixed inset-x-0 top-0 z-30 flex justify-center p-3">
			<div class="w-full max-w-sm rounded-panel bg-surface p-4">
				<p class="text-item text-foreground-muted">
					<span class="font-semibold text-foreground">{ring.from.display_name}</span> está te chamando
				</p>
				<p class="flex items-center gap-1 pt-1 text-meta text-foreground-muted">
					em <Icon name="headphones" size={12} /> {ring.channel_name}
				</p>
				<div class="flex gap-2 pt-4">
					<button
						class="h-11 flex-1 rounded-control bg-accent px-4 text-[14px] font-medium text-foreground hover:bg-accent-hover"
						onClick={onJoin}
					>
						Entrar
					</button>
					<button
						class="h-11 rounded-control bg-default px-4 text-[14px] font-medium text-foreground hover:bg-default-hover"
						onClick={onDismiss}
					>
						Agora não
					</button>
				</div>
			</div>
		</div>
	);
}
