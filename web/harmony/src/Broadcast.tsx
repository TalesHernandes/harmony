// Starting, changing and stopping what this device is sending.
//
// One panel per thing — the screen, the camera — opened by the button that
// starts it. Before it starts, the panel is where the choices are made; while
// it is running, the same panel is where they are changed and where it is
// turned off. That is the owner's design, and it is what took the row of tiny
// controls out of the channel column.
//
// The choices themselves are also exported one by one, because the same
// questions are asked in Ajustes, where somebody sets them up once and then
// never thinks about them again. Both places call the same functions in
// media.ts, so there is one answer and not two.

import { voice, type ScreenFps, type ScreenQuality, type VoiceView } from './media';
import { Action, Choice, Head, Modal } from './Modal';

const QUALITY: { id: ScreenQuality; label: string }[] = [
	{ id: 'auto', label: 'Automática' },
	{ id: '1080', label: '1080p' },
	{ id: '720', label: '720p' },
	{ id: '480', label: '480p' },
];

const FPS: { id: ScreenFps; label: string }[] = [
	{ id: 'auto', label: 'Automático' },
	{ id: '60', label: '60 fps' },
	{ id: '30', label: '30 fps' },
	{ id: '15', label: '15 fps' },
];

export function ScreenChoices({ view }: { view: VoiceView }) {
	return (
		<>
			<Choice
				label="Qualidade"
				name="quality"
				value={view.screenQuality}
				options={QUALITY}
				onPick={(v) => voice.setScreenQuality(v as ScreenQuality)}
			/>
			<Choice
				label="Quadros por segundo"
				name="fps"
				value={view.screenFps}
				options={FPS}
				onPick={(v) => voice.setScreenFps(v as ScreenFps)}
			/>
		</>
	);
}

export function CameraChoice({ view }: { view: VoiceView }) {
	return (
		<Choice
			label="Câmera"
			name="camera"
			value={view.cameraID}
			options={[{ id: '', label: 'Padrão do navegador' }, ...view.cameras]}
			onPick={(v) => void voice.setCamera(v)}
		/>
	);
}

export function AudioChoices({ view }: { view: VoiceView }) {
	return (
		<>
			<Choice
				label="Microfone"
				name="microphone"
				value={view.micID}
				options={[{ id: '', label: 'Padrão do navegador' }, ...view.microphones]}
				onPick={(v) => void voice.setMicrophone(v)}
			/>
			{view.speakers.length > 0 ? (
				<Choice
					label="Saída de som"
					name="speaker"
					value={view.speakerID}
					options={[{ id: '', label: 'Padrão do sistema' }, ...view.speakers]}
					onPick={(v) => void voice.setSpeaker(v)}
				/>
			) : (
				<p class="pt-4 text-meta text-foreground-muted">
					Saída de som: este navegador não deixa a página escolher. Vale a do sistema.
				</p>
			)}
		</>
	);
}

// ScreenPanel is the button "compartilhar a tela" turned into a place.
// canShare is false on a telephone browser that has no way to share a screen at
// all — Firefox on Android is one, and it says so by not having the function.
// Better to say it than to open a picker that never appears.
function canShare(): boolean {
	return typeof navigator.mediaDevices?.getDisplayMedia === 'function';
}

export function ScreenPanel({ view, onClose }: { view: VoiceView; onClose: () => void }) {
	const live = view.sharingScreen;
	const possible = canShare();
	return (
		<Modal label="compartilhar a tela" onClose={onClose}>
			<Head
				title={live ? 'Você está compartilhando' : 'Compartilhar a tela'}
				onClose={onClose}
			/>
			{!possible && !live && (
				<p class="pt-2 text-meta text-danger-soft-fg">
					Este navegador não compartilha tela. No celular, quase nenhum deixa — para mostrar a sua,
					use o computador. Assistir à tela dos outros funciona normalmente aqui.
				</p>
			)}
			<ScreenChoices view={view} />

			<div class="flex flex-col gap-2 pt-6">
				{live ? (
					<Action
						tone="warn"
						onClick={() => {
							void voice.stopScreen();
							onClose();
						}}
					>
						Parar a tela
					</Action>
				) : (
					possible && (
						<Action
							tone="accent"
							onClick={() => {
								// The browser only opens its picker from a real
								// press, and this is one.
								void voice.startScreen();
								onClose();
							}}
						>
							Compartilhar a tela
						</Action>
					)
				)}
			</div>
		</Modal>
	);
}

// CameraPanel is the same for the camera, which has one question instead of
// three.
export function CameraPanel({ view, onClose }: { view: VoiceView; onClose: () => void }) {
	const live = view.sharingCamera;
	return (
		<Modal label="câmera" onClose={onClose}>
			<Head title={live ? 'Câmera ligada' : 'Ligar a câmera'} onClose={onClose} />
			<CameraChoice view={view} />
			{view.cameras.length === 0 && (
				<p class="pt-2 text-meta text-foreground-muted">
					Nenhuma câmera encontrada ainda. Ligue-a uma vez e o navegador passa a dizer o nome
					delas.
				</p>
			)}

			<div class="flex flex-col gap-2 pt-6">
				{live ? (
					<Action
						tone="warn"
						onClick={() => {
							void voice.stopCamera();
							onClose();
						}}
					>
						Desligar a câmera
					</Action>
				) : (
					<Action
						tone="accent"
						onClick={() => {
							void voice.startCamera();
							onClose();
						}}
					>
						Ligar a câmera
					</Action>
				)}
			</div>
		</Modal>
	);
}
