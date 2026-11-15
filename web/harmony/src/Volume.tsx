// The loudness controls, in one place because two screens draw them: the
// profile of a person, and the settings panel.
//
// They are two different things wearing the same shape. Volume is this device's
// opinion about a sound arriving — it changes nothing for anybody else and
// never leaves the browser. MicVolume is the one that changes what other people
// hear, and it is the only lever in the app that does.

import { useVoice } from './Voice';
import { voice, type VoiceView } from './media';
import { DEFAULT_LEVEL } from './suppressor';

// The design's "Volume dela" row: a raised 44px strip with the name, the
// slider and the number, the number being the way back to 100%.
const row = 'mt-2 flex h-11 items-center gap-3 rounded-control bg-surface-2 px-3';
const name = 'w-16 shrink-0 text-meta text-foreground';
const reset = 'w-10 shrink-0 text-right text-meta text-foreground-muted hover:text-foreground';

// Slider is one loudness control for somebody else's sound. There may be two
// for the same person — their voice and the film they are showing — because
// "fala mais alto" and "abaixa esse filme" are two different complaints.
export function Slider({
	username,
	source,
	title,
	compact = false,
}: {
	username: string;
	source: string;
	title: string;
	compact?: boolean;
}) {
	// Read from the store on every render, never copied into state — and that
	// is a repair, not a preference.
	//
	// It used to be `useState(voice.volume(username, source))`, which reads the
	// number once, when the slider first appears. The profile panel is *reused*
	// between people: opening Ana and then Bruno keeps the same slider and only
	// changes its props, so it went on showing Ana's number — and the moment it
	// was touched, it wrote Ana's number onto Bruno. That is how the owner
	// ended up with the whole room at nought per cent without ever having asked
	// for it. A control that keeps its own copy of a shared number is a control
	// that will eventually disagree with it.
	useVoice();
	const value = voice.volume(username, source);
	return (
		<div class={compact ? 'flex h-8 items-center gap-2 rounded-control bg-background px-2' : row}>
			{!compact && <span class={name}>{title}</span>}
			<span class="range">
				<progress class="range-fill" max="2" value={value} aria-hidden="true" />
				<input
					type="range"
					min="0"
					max="2"
					step="0.05"
					value={String(value)}
					aria-label={"Volume de " + title.toLowerCase() + " de @" + username}
					data-volume={username + ':' + source}
					onInput={(e) => {
						voice.setVolume(username, source, Number((e.target as HTMLInputElement).value));
					}}
				/>
			</span>
			<button
				class={reset}
				// Over a live no tooltip may sit on the picture (chamada-layouts.spec).
				title={compact ? undefined : 'Voltar ao normal (100%)'}
				aria-label="Voltar ao normal (100%)"
				onClick={() => voice.setVolume(username, source, 1)}
			>
				{Math.round(value * 100)}%
			</button>
		</div>
	);
}

// MicVolume is how loud this device's own microphone is sent. It is the answer
// to the complaint nobody can fix from their own side — "você tá muito baixo" —
// and the owner asked for it in both the places somebody would look: their own
// profile and the settings.
//
// At 100% it is not doing anything at all: the microphone goes into the call
// untouched, which is what it has always done (media.ts). Anywhere else the
// sound passes through a gain on the way out, swapped in without the call
// noticing.
export function MicVolume() {
	// Same rule as the slider above: the number lives in one place and is read
	// from it.
	const view = useVoice();
	const value = view.micGain;
	return (
		<div class={row}>
			<span class={name}>Microfone</span>
			<span class="range">
				<progress class="range-fill" max="2" value={value} aria-hidden="true" />
				<input
					type="range"
					min="0"
					max="2"
					step="0.05"
					value={String(value)}
					data-mic-volume
					aria-label="volume do meu microfone"
					title="Quanto do seu microfone sai para os outros"
					onInput={(e) => {
						void voice.setMicGain(Number((e.target as HTMLInputElement).value));
					}}
				/>
			</span>
			<button
				class={reset}
				title="Voltar ao normal (100%)"
				onClick={() => void voice.setMicGain(1)}
			>
				{Math.round(value * 100)}%
			</button>
		</div>
	);
}

// MicFilters is what the browser does to your voice before it leaves: the noise
// suppressor, the echo canceller and the automatic level.
//
// All three have been on since D2 and could not be turned off, which was the
// right default and the wrong rule. The suppressor is the reason a fan or a
// keyboard does not go out with your voice — and it is also why a good
// microphone in a quiet room can come out sounding thin, because it is deciding
// what is voice and sometimes deciding wrong. Echo cancellation does nothing at
// all for somebody on headphones. The automatic level is what evens out
// somebody who leans in and back, and it is the one that argues with the slider
// above: with it on, turning yourself up is turning up something the browser
// has already levelled.
//
// So they are switches now, changed on a microphone that is already running.
export function MicFilters({ view }: { view: VoiceView }) {
	const which: { key: 'noise' | 'echo' | 'agc' | 'rnnoise'; label: string; hint: string }[] = [
		{
			key: 'rnnoise',
			label: 'Tirar ruído de fundo (DeepFilterNet)',
			hint: 'Tira ventilador, teclado e ar-condicionado, inclusive enquanto você fala. Roda neste aparelho, antes da sua voz sair. Na primeira vez baixa ~12 MB; gasta ~15% de um núcleo no PC e ~35% no celular.',
		},
		{
			key: 'noise',
			label: 'Tirar ruído de fundo (do navegador)',
			hint: 'O supressor simples do navegador. Fica desligado enquanto o de cima estiver ligado: os dois juntos deixam a voz fina.',
		},
		{
			key: 'echo',
			label: 'Cancelar eco',
			hint: 'Só faz diferença em quem usa caixas de som em vez de fone.',
		},
		{
			key: 'agc',
			label: 'Nivelar automaticamente',
			hint: 'Iguala quem se aproxima e se afasta do microfone. Mexe no mesmo som que o controle de volume acima.',
		},
	];
	return (
		<div class="pt-2">
			{which.map((w) => {
				// The browser's own is off, and greyed, while the good one is on:
				// a switch that is being overruled has to look overruled, or it
				// is the same lie the whole of D4d was about.
				const overruled = w.key === 'noise' && view.micFilters.rnnoise;
				return (
					<label
						key={w.key}
						class={
							'flex items-center gap-3 py-1.5 text-item ' +
							(overruled ? 'text-foreground-muted opacity-50' : 'text-foreground')
						}
						title={w.hint}
					>
						<input
							type="checkbox"
							data-filter={w.key}
							disabled={overruled}
							checked={view.micFilters[w.key] && !overruled}
							onChange={(e) => {
								void voice.setMicFilter(w.key, (e.target as HTMLInputElement).checked);
							}}
						/>
						{w.label}
					</label>
				);
			})}
			{view.micFilters.rnnoise && view.noiseActive !== 'rnnoise' && view.noiseActive !== 'none' && (
				<NoiseLevel view={view} />
			)}
			{view.micFilters.rnnoise && <NoiseFallback view={view} />}
		</div>
	);
}

// NoiseFallback says so when the room is not running what the switch above
// names. The switch is what this person asked for; this is what came of it on
// this device (media.ts, noiseActive), and it is shown only when they differ.
function NoiseFallback({ view }: { view: VoiceView }) {
	let text = '';
	if (view.noiseActive === 'rnnoise')
		text = 'Neste aparelho o DeepFilterNet não carregou; está rodando o filtro antigo (RNNoise), que só tira ruído nas pausas.';
	else if (view.noiseActive === 'none')
		text = 'Nenhum filtro conseguiu carregar neste aparelho: sua voz está saindo sem filtro.';
	if (!text) return null;
	return (
		<p class="py-1.5 text-meta text-foreground-muted" data-noise-fallback={view.noiseActive}>
			{text}
		</p>
	);
}

// NoiseLevel is how much the suppressor may take away: DeepFilterNet's
// attenuation limit, in dB. Low leaves some of the room in (a voice that sounds
// less processed); 100 takes everything it can. It changes the running filter
// in place, the connection never notices.
function NoiseLevel({ view }: { view: VoiceView }) {
	const value = view.noiseLevel;
	return (
		<div class={row}>
			<span class={name}>Força</span>
			<span class="range">
				<progress class="range-fill" max="90" value={value - 10} aria-hidden="true" />
				<input
					type="range"
					min="10"
					max="100"
					step="5"
					value={String(value)}
					data-noise-level
					aria-label="força do filtro de ruído"
					title="Quanto ruído o filtro pode tirar"
					onInput={(e) => voice.setNoiseLevel(Number((e.target as HTMLInputElement).value))}
				/>
			</span>
			<button
				class={reset}
				title={`Voltar ao padrão (${DEFAULT_LEVEL} dB)`}
				onClick={() => voice.setNoiseLevel(DEFAULT_LEVEL)}
			>
				{value} dB
			</button>
		</div>
	);
}
