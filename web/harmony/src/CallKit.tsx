// The pieces every call is drawn with: the round buttons, the chip a name sits
// in, and the reading of the line. There are three places a call is controlled
// from — the room, the direct call, and the strip it shrinks to — and before
// this file each of them had its own idea of what "microphone off" looks like:
// red in one, amber in another, a red outline in the third.
//
// The design settles most of it in one sentence (Componentes §10): something
// on by your choice is accent; off is the same button with its icon gone grey;
// leaving is danger. With one exception the owner made on 23/09/2026, the
// night D4u went up: **your microphone and your hearing, switched off, are
// red** — "o microfone e o fone devem ficar vermelhos ao mutar". They are the
// two states somebody forgets they left on and then talks into for a minute,
// and red is the colour that gets noticed. A camera or a screen that is off is
// still grey: nobody is embarrassed by a camera they forgot to turn on.

import { useEffect, useState } from 'preact/hooks';
import { Icon } from './Icon';
import { voice, type LinkStats } from './media';
import { log } from './diag';

// 44 on a monitor, 48 on a telephone: the design's two targets, the second one
// being the thumb.
const ROUND = 'grid h-11 w-11 shrink-0 place-items-center rounded-control max-sm:h-12 max-sm:w-12';
// The resting state of something that is on by default — the microphone, the
// sound. Full-strength icon.
export const IDLE = ROUND + ' bg-default text-foreground hover:bg-default-hover';
// The same button, off. Only the icon changes (and it changes shape as well as
// tone: microphone-slash, not a grey microphone).
export const OFF = ROUND + ' bg-default text-foreground-muted hover:bg-default-hover hover:text-foreground';
// Your microphone or your hearing, off. The icon red, the button unchanged.
export const MUTED = ROUND + ' bg-default text-danger-soft-fg hover:bg-default-hover';
// On, because you turned it on: a camera, a screen.
export const ON = ROUND + ' bg-accent text-foreground hover:bg-accent-hover';
// The way out, and the only red on the bar. It carries its word on a monitor
// and loses it on a telephone, where the row has no room for a pill.
export const LEAVE =
	'flex h-11 shrink-0 items-center justify-center gap-2 rounded-control bg-danger px-5 text-[14px] font-medium ' +
	'text-foreground hover:bg-danger-hover max-sm:h-12 max-sm:w-12 max-sm:px-0';
// Both room entry points open the same screen panel, including its Stop action.
export function ScreenButton({ sharing, onOpen, compact = false }: { sharing: boolean; onOpen: () => void; compact?: boolean }) {
  const size = compact ? 'grid h-8 w-8 shrink-0 place-items-center rounded-control' : ROUND;
  return <button class={size + (sharing ? ' bg-accent text-foreground hover:bg-accent-hover' : ' bg-default text-foreground-muted hover:bg-default-hover hover:text-foreground')}
    title={sharing ? 'Sua tela' : 'Compartilhar a tela'} aria-label={sharing ? 'sua tela' : 'compartilhar a tela'}
    aria-pressed={sharing} data-open="screen" onClick={onOpen}>
    <Icon name={sharing ? 'display-pulse' : 'display'} size={20} />
  </button>;
}
// The bar the buttons sit on: a surface panel, not a row floating on the call.
// A 16px piece with its buttons 8 in, so their 8px corners follow its own.
export const BAR = 'flex shrink-0 items-center gap-2 rounded-tile bg-surface p-2';

// The Android hearing-off mark: the kit's speaker with an X (Kit.kt).
export function Deafened({ size = 16 }: { size?: number }) {
	return <Icon name="volume-xmark" size={size} />;
}

// A name on a picture. Always a solid chip and never text straight on the
// video: there is no blur to lift it off a bright frame (Componentes §09), and
// white letters on a white slide are no letters at all.
export const CHIP =
	'flex h-6 min-w-0 items-center gap-1.5 rounded-control bg-background px-2 text-meta font-medium text-foreground';
// The bigger one, on the big picture.
export const CHIP_BIG =
	'flex h-7 min-w-0 items-center gap-1.5 rounded-control bg-background px-2.5 text-meta font-medium text-foreground';

// Quality is how the call itself is going. Three bars and a number of
// milliseconds, because that is the whole of what a person can act on.
//
// It is deliberately not the video readout. That one answers "por que a imagem
// está feia" and lives on the picture; this one answers "é a minha internet?"
// and is about the line, which is why loss of the *voice* is what it counts.
//
// Two shapes of it: `chip` is the pill in the room's bar ("Boa · 38 ms"), and
// the plain one is the three bars beside "Voz conectada" in the column. Both
// open onto the same three numbers.
//
// Every two seconds, in a component of its own so that the thing redrawing on a
// timer is nowhere near the thing holding a picture (CLAUDE.md). Only the column
// writes the line to the log — two copies of it on the screen would otherwise
// write it twice.
export function Quality({ chip = false }: { chip?: boolean }) {
	const [link, setLink] = useState<LinkStats | null>(null);
	useEffect(() => {
		let alive = true;
		let ticks = 0;
		const tick = () => {
			void voice.link().then((l) => {
				if (!alive) return;
				setLink(l);
				// The line itself, written down every twenty readings (twenty
				// seconds, since the meter reads every second) — the same
				// cadence as the per-picture lines, so the two can be read
				// side by side in one log.
				//
				// It exists because of 18/09/2026: a watcher reported a
				// stuttering screen, and the log could say the picture had
				// fallen to 31 kbit/s but not whether the sender had been told
				// to stop sending. `banda` is that missing sentence. A -1 is
				// printed as "?" rather than as a number, because a browser
				// that has not estimated yet and a browser estimating zero are
				// not the same thing.
				if (!chip && l && ++ticks % 20 === 0) {
					const n = (v: number) => (v < 0 ? '?' : String(v));
					log(
						`rede: ida-e-volta=${l.ms}ms perda-voz=${l.lost}% jitter=${l.jitter}ms ` +
							`banda-subida=${n(l.outKbps)} banda-descida=${n(l.inKbps)} kbit/s`,
					);
				}
			});
		};
		tick();
		// Every second (the owner, 23/09/2026: "poderia atualizar mais
		// rapidamente"), which is the ceiling for stats (CLAUDE.md).
		const timer = setInterval(tick, 1000);
		return () => {
			alive = false;
			clearInterval(timer);
		};
	}, [chip]);
	const [open, setOpen] = useState(false);
	if (!link || link.ms < 0) return null;
	// The thresholds are the ones a conversation actually notices. Under 150 ms
	// nobody talks over anybody; past 300 ms people start doing it. Loss is
	// harsher because two per cent of the voice missing is already audible.
	const bad = link.ms >= 300 || link.lost >= 5;
	const rough = link.ms >= 150 || link.lost >= 2;
	const bars = bad ? 1 : rough ? 2 : 3;
	// Green, amber, red: the signal every service like this one draws, which
	// is the one the owner reads without thinking. The D4u made "fine" grey,
	// on the rule that green means somebody talking — and the owner's answer
	// the same night was that the little chart "não está mudando de cor". A
	// reading that never changes colour is not read at all, so the traffic
	// light is back and this is the one green that is not a voice.
	const tone = bad ? 'text-danger-soft-fg' : rough ? 'text-warning-soft-fg' : 'text-success';
	const words = bad ? 'ruim' : rough ? 'média' : 'boa';
	const signal = (
		<span class={'flex items-end gap-px ' + tone} aria-hidden="true">
			{[1, 2, 3].map((n) => (
				<span
					key={n}
					data-bar={n}
					class={
						// Round-ended: square bars read as a bar chart, not as a signal.
						'rounded-full ' +
						// One size in both places since the owner asked for bigger
						// marks in the column (23/09/2026).
						'w-[3px] ' +
						(n === 1 ? 'h-1.5 ' : n === 2 ? 'h-2.5 ' : 'h-3.5 ') +
						(n <= bars ? 'bg-current' : 'bg-surface-3')
					}
				/>
			))}
		</span>
	);
	return (
		// And it opens. A glance answers "is it me?"; the three numbers under it
		// answer "what exactly", which is what somebody needs at the moment they
		// are telling another person what is wrong. Shut by default, because
		// that answer is noise for the other fifty-nine minutes of the hour.
		<span class="relative flex shrink-0 items-center">
			<button
				class={
					chip
						? // The colour on the bars only; the words stay grey.
							'flex h-11 items-center gap-2 rounded-control bg-default px-4 text-meta font-medium text-foreground-muted hover:bg-default-hover'
						: 'flex items-end py-1'
				}
				aria-label={`conexão ${words}, ${link.ms} ms`}
				aria-expanded={open}
				data-quality={bad ? 'ruim' : rough ? 'media' : 'boa'}
				title={`Conexão ${words} — ${link.ms} ms. Clique para os detalhes.`}
				onClick={(ev) => {
					// The column's strip is a button that goes back to the call.
					ev.stopPropagation();
					setOpen((o) => !o);
				}}
			>
				{signal}
				{chip && (
					<span class="tabular-nums">
						{words[0].toUpperCase() + words.slice(1)} · {link.ms} ms
					</span>
				)}
			</button>
			{open && (
				// No shadow: in the room this opens over the picture, and nothing
				// shaded may sit on a video (CLAUDE.md) — and the kit's inset ring is
				// a box-shadow too, so the separation is a plain border.
				<span
					class="absolute bottom-full left-0 z-20 mb-2 w-56 rounded-control border border-border bg-surface p-3 text-meta"
					data-quality-detail
				>
					<span class={'block pb-1 font-medium ' + tone}>Conexão {words}</span>
					{(
						[
							['Ida e volta', `${link.ms} ms`, 'Quanto a sua voz demora para chegar e voltar'],
							['Perdido', `${link.lost}%`, 'Quanto da voz não chegou nos últimos segundos'],
							['Irregular', `${link.jitter} ms`, 'Quanto o ritmo da chegada varia'],
						] as const
					).map(([name, value, hint]) => (
						<span key={name} class="flex justify-between gap-2 py-px text-foreground-muted" title={hint}>
							<span>{name}</span>
							<span class="tabular-nums text-foreground">{value}</span>
						</span>
					))}
				</span>
			)}
		</span>
	);
}
