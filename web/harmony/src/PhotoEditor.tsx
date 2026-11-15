import { useEffect, useLayoutEffect, useRef, useState } from 'preact/hooks';
import { useDialogFocus } from './Confirm';
import { log } from './diag';
import { Lightbox } from './Messages';

const SIZE = 512;
type Crop = { zoom: number; x: number; y: number };

// Preview and export share the same crop in output pixels. CSS supplies the
// circular frame; the uploaded image remains a square, as the API expects.
export function PhotoEditor({ file, onSave, onCancel }: {
	file: File;
	onSave: (file: File) => Promise<boolean>;
	onCancel: () => void;
}) {
	const card = useRef<HTMLDivElement>(null);
	const canvas = useRef<HTMLCanvasElement>(null);
	const [image, setImage] = useState<HTMLImageElement | null>(null);
	const [crop, setCrop] = useState<Crop>({ zoom: 1, x: 0, y: 0 });
	const [busy, setBusy] = useState(false);
	const [error, setError] = useState('');
	const pointer = useRef<{ id: number; x: number; y: number } | null>(null);
	useDialogFocus(card, () => { if (!busy) onCancel(); });
	useEffect(() => {
		log('photo editor opened');
		let active = true;
		// img-src permits data: but deliberately excludes blob:.
		const reader = new FileReader();
		const img = new Image();
		img.onload = () => { if (active) setImage(img); };
		img.onerror = () => {
			if (!active) return;
			log('photo editor decode failed');
			setError('Não foi possível abrir esta foto. Escolha outra imagem.');
		};
		reader.onload = () => { if (active) img.src = String(reader.result); };
		reader.onerror = img.onerror;
		reader.readAsDataURL(file);
		return () => { active = false; if (reader.readyState === FileReader.LOADING) reader.abort(); log('photo editor closed'); };
	}, [file]);
	const base = image ? Math.max(SIZE / image.naturalWidth, SIZE / image.naturalHeight) : 1;
	function constrain(next: Crop): Crop {
		const zoom = Math.max(1, Math.min(4, next.zoom));
		const maxX = image ? (image.naturalWidth * base * zoom - SIZE) / 2 : 0;
		const maxY = image ? (image.naturalHeight * base * zoom - SIZE) / 2 : 0;
		return { zoom, x: Math.max(-maxX, Math.min(maxX, next.x)), y: Math.max(-maxY, Math.min(maxY, next.y)) };
	}
	useLayoutEffect(() => {
		const ctx = canvas.current?.getContext('2d');
		if (!ctx || !image) return;
		ctx.fillStyle = '#ffffff';
		ctx.fillRect(0, 0, SIZE, SIZE);
		const w = image.naturalWidth * base * crop.zoom;
		const h = image.naturalHeight * base * crop.zoom;
		ctx.drawImage(image, (SIZE - w) / 2 + crop.x, (SIZE - h) / 2 + crop.y, w, h);
	}, [image, crop]);
	useEffect(() => {
		const el = canvas.current;
		if (!el) return;
		const wheel = (ev: WheelEvent) => {
			ev.preventDefault();
			if (!busy) setCrop(cur => constrain({ ...cur, zoom: cur.zoom * Math.exp(-ev.deltaY * 0.002) }));
		};
		el.addEventListener('wheel', wheel, { passive: false });
		return () => el.removeEventListener('wheel', wheel);
	}, [image, busy]);
	async function save() {
		if (!canvas.current || !image || busy) return;
		setBusy(true);
		try {
			const blob = await new Promise<Blob | null>(resolve => canvas.current!.toBlob(resolve, 'image/jpeg', 0.9));
			if (!blob) throw new Error('encode');
			if (await onSave(new File([blob], 'avatar.jpg', { type: 'image/jpeg' }))) {
				log('photo crop uploaded 512x512');
				onCancel();
			} else {
				log('photo crop upload failed');
				setError('Não foi possível enviar a foto. Tente salvar novamente.');
			}
		} catch {
			log('photo crop export failed');
			setError('Não foi possível salvar esta foto. Tente novamente.');
		} finally { setBusy(false); }
	}
	return (
		<div class="fixed inset-0 z-50 grid place-items-center bg-veil p-4" data-photo-editor>
			<div ref={card} role="dialog" aria-modal="true" aria-label="Editar foto" tabIndex={-1} class="flex w-full max-w-[360px] flex-col gap-4 rounded-panel bg-surface p-5 text-foreground">
				<h2 class="text-title">Editar foto</h2>
				<p class="text-item text-foreground-muted">Arraste para reposicionar e ajuste o zoom.</p>
				<canvas ref={canvas} width={SIZE} height={SIZE} tabIndex={0} aria-label="Foto: arraste para reposicionar; use as setas ou a roda do mouse para ajustar" class="mx-auto aspect-square w-full max-w-[256px] touch-none rounded-full bg-surface-2 cursor-move"
					onPointerDown={e => { if (busy) return; pointer.current = { id: e.pointerId, x: e.clientX, y: e.clientY }; e.currentTarget.setPointerCapture(e.pointerId); }}
					onPointerMove={e => {
						const prev = pointer.current;
						if (!prev || prev.id !== e.pointerId || busy) return;
						const ratio = SIZE / e.currentTarget.getBoundingClientRect().width;
						const dx = (e.clientX - prev.x) * ratio, dy = (e.clientY - prev.y) * ratio;
						pointer.current = { id: e.pointerId, x: e.clientX, y: e.clientY };
						setCrop(cur => constrain({ ...cur, x: cur.x + dx, y: cur.y + dy }));
					}}
					onPointerUp={() => { pointer.current = null; }} onPointerCancel={() => { pointer.current = null; }} onLostPointerCapture={() => { pointer.current = null; }}
					onKeyDown={e => {
						if (busy || !['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown'].includes(e.key)) return;
						e.preventDefault();
						setCrop(cur => constrain({ ...cur, x: cur.x + (e.key === 'ArrowLeft' ? -10 : e.key === 'ArrowRight' ? 10 : 0), y: cur.y + (e.key === 'ArrowUp' ? -10 : e.key === 'ArrowDown' ? 10 : 0) }));
					}} />
				<label class="flex items-center gap-3 text-item">Zoom
					<span class="range"><progress class="range-fill" max="3" value={crop.zoom - 1} aria-hidden="true" /><input type="range" min="1" max="4" step="0.01" value={crop.zoom} disabled={busy || !image} aria-label="Zoom da foto" onInput={e => setCrop(cur => constrain({ ...cur, zoom: Number(e.currentTarget.value) }))} /></span>
				</label>
				{error && <p role="alert" class="text-meta text-danger-soft-fg">{error}</p>}
				<div class="flex justify-end gap-2">
					<button class="danger-action h-11 rounded-control bg-default px-4 text-item hover:bg-default-hover" disabled={busy} onClick={onCancel}>Cancelar</button>
					<button class="h-11 rounded-control bg-accent px-4 text-item hover:bg-accent-hover disabled:opacity-50" disabled={!image || busy} onClick={() => void save()}>{busy ? 'Salvando…' : 'Salvar'}</button>
				</div>
			</div>
		</div>
	);
}

export function PhotoLightbox({ src, name, onClose }: { src: string; name: string; onClose: () => void }) {
 return <Lightbox photo file={{id: '',url:src,name,mime:'image/jpeg',size:0,width:256,height:256}} onClose={onClose} />;
}
