// Asking somebody something: the panel it is asked in, and the one-line
// selector it is asked with.
//
// The two live together because they are the same job. A panel that does not
// close when you press outside it is the complaint the owner made about the old
// volume control, and a selector that each screen draws its own way is three
// chances to draw it differently.

import type { ComponentChildren } from 'preact';
import { useEffect, useRef, useState } from 'preact/hooks';
import { Icon } from './Icon';

// Modal is the panel. It closes when you press outside it and when you press
// Escape — both, always, everywhere it is used.
export function Modal({
	label,
	onClose,
	children,
}: {
	label: string;
	onClose: () => void;
	children: ComponentChildren;
}) {
	const card = useRef<HTMLDivElement | null>(null);
	// The way out, held in a box so that the effect below never has to run
	// twice.
	//
	// It used to depend on `onClose`, and every screen passes that as an arrow
	// function written in the JSX - a different function on every render. So
	// every redraw of the page under this panel tore the listener off the
	// document and put a new one back **on the next tick**, and a press landing
	// in that gap was simply not heard: the panel stayed open.
	//
	// Profile.tsx was fixed this way in the D4h, with the reason written beside
	// it; this one was left behind, and it carries Settings, Invites, Search and
	// every confirmation. Found on 20/09/2026 while looking for something else,
	// and it is worth being clear that it was **not** the something else: the
	// panel that misbehaved that day was the profile, which already read its
	// onClose through a box.
	//
	// Read through the box, the listener is attached once and stays.
	const latest = useRef(onClose);
	latest.current = onClose;
	useEffect(() => {
		const away = (ev: MouseEvent) => {
			if (card.current && !card.current.contains(ev.target as Node)) latest.current();
		};
		const key = (ev: KeyboardEvent) => {
			if (ev.key === 'Escape') latest.current();
		};
		// The listener goes on in the next tick, or the very click that opened
		// this panel closes it again.
		const timer = setTimeout(() => document.addEventListener('mousedown', away), 0);
		document.addEventListener('keydown', key);
		return () => {
			clearTimeout(timer);
			document.removeEventListener('mousedown', away);
			document.removeEventListener('keydown', key);
		};
	}, []);

	// The panels' drawing (D4v, Painéis and Etapa 5): a 400px surface on the
	// 30% veil, no border and no shadow — the step from the veil to the surface
	// is the edge. A 16px piece with its content 16 in, so anything in its
	// corners (the X) sits outside the corner's curve.
	return (
		<div class="safe-fixed fixed inset-0 z-30 grid place-items-center bg-veil p-4">
			<div
				ref={card}
				class="max-h-full w-full max-w-[400px] overflow-y-auto rounded-panel bg-surface p-6"
				role="dialog"
				aria-label={label}
			>
				{children}
			</div>
		</div>
	);
}

// Head is the title row of a panel, with the way out on the right, and the
// one line under the title that says what is about to happen (the design's
// "Tela inteira · 1080p · 30 fps").
export function Head({ title, sub, onClose }: { title: string; sub?: string; onClose: () => void }) {
	return (
		<div class="flex items-start gap-2">
			<div class="min-w-0 flex-1">
				<h2 class="truncate text-title">{title}</h2>
				{sub && <p class="pt-1 text-meta text-foreground-muted">{sub}</p>}
			</div>
			<button
				class="grid h-8 w-8 shrink-0 place-items-center rounded-control bg-default text-foreground hover:bg-default-hover"
				onClick={onClose}
				title="Fechar"
				aria-label="fechar"
			>
				<Icon name="xmark" />
			</button>
		</div>
	);
}

// Choice is one question with a list of answers. `name` is what the browser
// tests find it by, and what the diagnostic log calls it.
export function Choice({
	label,
	name,
	value,
	options,
	onPick,
	hint,
}: {
	label: string;
	name: string;
	value: string;
	options: { id: string; label: string }[];
	onPick: (value: string) => void;
	hint?: string;
}) {
	// Drawn here and not left to <select>: the list a native select opens is
	// the operating system's, light grey on Windows in the middle of a dark
	// panel, and nothing on the page can style it (the owner, 24/09/2026:
	// "drop down ta feio"). This is a button that says the value and a list
	// that is one of our own pieces.
	const [open, setOpen] = useState(false);
	const [up, setUp] = useState(false);
	const [hi, setHi] = useState(0);
	const box = useRef<HTMLDivElement | null>(null);
	const current = options.find((o) => o.id === value) ?? options[0];

	const show = () => {
		// Up or down by class, by the room the scrolling panel around it has
		// left — never a coordinate, which would be an inline style.
		const el = box.current;
		if (el) {
			const me = el.getBoundingClientRect();
			const pane = el.closest('.scroller, [role="dialog"]')?.getBoundingClientRect();
			const bottom = Math.min(window.innerHeight, pane?.bottom ?? window.innerHeight);
			const top = Math.max(0, pane?.top ?? 0);
			const need = Math.min(options.length * 40 + 8, 264);
			setUp(bottom - me.bottom < need && me.top - top > bottom - me.bottom);
		}
		setHi(Math.max(0, options.findIndex((o) => o.id === value)));
		setOpen(true);
	};
	const pick = (id: string) => {
		setOpen(false);
		if (id !== value) onPick(id);
	};

	useEffect(() => {
		if (!open) return;
		const away = (ev: PointerEvent) => {
			if (box.current && !box.current.contains(ev.target as Node)) setOpen(false);
		};
		document.addEventListener('pointerdown', away);
		return () => document.removeEventListener('pointerdown', away);
	}, [open]);

	return (
		<div class="pt-4 first:pt-0" ref={box}>
			<span class="block text-label uppercase text-foreground-muted">{label}</span>
			<div class="relative mt-2">
				<button
					type="button"
					class="field flex h-10 w-full items-center gap-2 rounded-control bg-surface-2 px-3 text-left text-item text-foreground hover:bg-surface-3"
					data-setting={name}
					data-value={value}
					aria-haspopup="listbox"
					aria-expanded={open}
					aria-label={label + ': ' + (current?.label ?? '')}
					onClick={() => (open ? setOpen(false) : show())}
					onKeyDown={(e) => {
						if (!open) {
							if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
								e.preventDefault();
								show();
							}
							return;
						}
						if (e.key === 'Escape') {
							// The list closes; the panel under it stays.
							e.preventDefault();
							e.stopPropagation();
							setOpen(false);
						} else if (e.key === 'ArrowDown') {
							e.preventDefault();
							setHi((i) => Math.min(options.length - 1, i + 1));
						} else if (e.key === 'ArrowUp') {
							e.preventDefault();
							setHi((i) => Math.max(0, i - 1));
						} else if (e.key === 'Enter' || e.key === ' ') {
							e.preventDefault();
							if (options[hi]) pick(options[hi].id);
						} else if (e.key === 'Tab') {
							setOpen(false);
						}
					}}
				>
					<span class="min-w-0 flex-1 truncate">{current?.label}</span>
					<span class={'shrink-0 text-foreground-muted ' + (open ? 'rotate-180' : '')}>
						<Icon name="chevron-down" size={16} />
					</span>
				</button>
				{open && (
					<ul
						role="listbox"
						aria-label={label}
						data-options={name}
						class={
							'scroller absolute inset-x-0 z-40 max-h-64 overflow-y-auto rounded-control bg-surface-3 p-1 ' +
							(up ? 'bottom-full mb-1' : 'top-full mt-1')
						}
					>
						{options.map((o, i) => (
							<li
								key={o.id}
								role="option"
								aria-selected={o.id === value}
								data-value={o.id}
								class={
									'flex h-9 cursor-pointer items-center rounded-control px-2 text-item ' +
									(o.id === value ? 'font-semibold text-accent-soft-fg ' : 'text-foreground ') +
									(i === hi ? 'bg-default' : 'hover:bg-default')
								}
								onPointerEnter={() => setHi(i)}
								onClick={() => pick(o.id)}
							>
								<span class="min-w-0 truncate">{o.label}</span>
							</li>
						))}
					</ul>
				)}
			</div>
			{hint && <span class="block pt-2 text-meta text-foreground-muted">{hint}</span>}
		</div>
	);
}

// Action is a full-width button in a panel: one of the two or three things the
// panel exists to do.
export function Action({
	children,
	onClick,
	tone = 'plain',
}: {
	children: ComponentChildren;
	onClick: () => void;
	tone?: 'plain' | 'accent' | 'warn';
}) {
	// Solid, the way the design draws every button now: accent for the thing
	// the panel is for, default for the rest, and danger-soft for stopping
	// something — the red that is not an error (Painéis §04: "o que desliga é
	// danger-soft dentro do painel").
	if (typeof children === 'string' && /^(Cancelar|Recusar|Remover|Desconectar|Sair|Fechar)/.test(children)) tone = 'warn';
	const ground =
		tone === 'accent'
			? 'bg-accent text-foreground hover:bg-accent-hover'
			: tone === 'warn'
				? 'bg-danger-soft text-danger-soft-fg hover:bg-danger hover:text-foreground'
				: 'bg-default text-foreground hover:bg-default-hover';
	return (
		<button
			class={'flex h-11 w-full items-center justify-center gap-2 rounded-control px-4 text-[14px] font-medium ' + ground}
			onClick={onClick}
		>
			{children}
		</button>
	);
}
