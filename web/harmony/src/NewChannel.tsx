// The panel that names a new channel. It was the browser's own prompt() until
// 24/09/2026 — a grey system box at the top of the window, outside the app's
// drawing and outside its keyboard rules (the owner: "tem que ser do próprio
// app").
//
// The name is shaped as it is typed rather than refused after: the server
// takes lower-case letters, digits and hyphens (store.ValidChannelName), and
// "Meu Canal" becoming "meu-canal" under the fingers is kinder than an error
// that says so.

import { useEffect, useRef, useState } from 'preact/hooks';
import { message } from './api';
import { Action, Head, Modal } from './Modal';

const MAX = 32;

export function channelName(typed: string): string {
	return typed
		.normalize('NFD')
		.replace(/[̀-ͯ]/g, '')
		.toLowerCase()
		.replace(/[\s_]+/g, '-')
		.replace(/[^a-z0-9-]/g, '')
		.replace(/-{2,}/g, '-')
		.replace(/^-/, '')
		.slice(0, MAX);
}

// The same panel names a folder and renames a channel or a folder (D12a): a
// folder's name is free text ("Jogos"), so `folder` turns the shaping off.
export function NewChannel({
	kind,
	onCreate,
	onClose,
	title: asked,
	initial = '',
	action = 'Criar',
}: {
	kind: 'text' | 'voice' | 'folder';
	onCreate: (name: string) => Promise<void>;
	onClose: () => void;
	title?: string;
	initial?: string;
	action?: string;
}) {
	const free = kind === 'folder';
	const [name, setName] = useState(initial);
	const [error, setError] = useState('');
	const [busy, setBusy] = useState(false);
	const box = useRef<HTMLInputElement | null>(null);
	useEffect(() => box.current?.focus(), []);

	async function create() {
		const final = free ? name.trim() : name.replace(/-+$/, '');
		if (!final || busy) return;
		setBusy(true);
		try {
			await onCreate(final);
			onClose();
		} catch (err) {
			setError(message(err));
		} finally {
			setBusy(false);
		}
	}

	const title = asked ?? (kind === 'voice' ? 'Novo canal de voz' : kind === 'folder' ? 'Nova categoria' : 'Novo canal');
	return (
		<Modal label={title} onClose={onClose}>
			<Head title={title} onClose={onClose} />
			<label class="block pt-4">
				<span class="text-label uppercase text-foreground-muted">Nome</span>
				<input
					ref={box}
					class="field mt-2 h-10 w-full rounded-control bg-surface-2 px-3 text-item text-foreground placeholder:text-foreground-muted"
					placeholder={kind === 'voice' ? 'jogatina' : kind === 'folder' ? 'Jogos' : 'musica'}
					value={name}
					maxLength={MAX}
					data-new-channel
					onInput={(e) => {
						const typed = (e.target as HTMLInputElement).value;
						if (free) {
							setName(typed);
							setError('');
							return;
						}
						const shaped = channelName(typed);
						// Written back even when it did not change, so a character
						// the rule drops disappears from the box as well.
						(e.target as HTMLInputElement).value = shaped;
						setName(shaped);
						setError('');
					}}
					onKeyDown={(e) => {
						if (e.key === 'Enter') {
							e.preventDefault();
							void create();
						}
					}}
				/>
				<span class="block pt-2 text-meta text-foreground-muted">
					{free
						? 'Até 32 caracteres. A categoria aparece em maiúsculas na lista.'
						: 'Letras minúsculas, números e traço. Espaço vira traço.'}
				</span>
			</label>
			{error && <p class="pt-2 text-meta text-danger-soft-fg">{error}</p>}
			<div class="flex flex-col gap-2 pt-6">
				<Action tone="accent" onClick={() => void create()}>
					{action}
				</Action>
				<Action onClick={onClose}>Cancelar</Action>
			</div>
		</Modal>
	);
}
