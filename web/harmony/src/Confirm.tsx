import type { ComponentChildren, RefObject } from 'preact';
import { useEffect, useRef } from 'preact/hooks';
import { Icon, type IconName } from './Icon';

// Keep keyboard input in the foreground panel and return to its opener.
// Callbacks live in refs so a gateway update cannot leave a listener gap.
export function useDialogFocus(card: RefObject<HTMLDivElement>, onClose: () => void, blockEnter = false) {
	const close = useRef(onClose);
	close.current = onClose;
	useEffect(() => {
		const opener = document.activeElement as HTMLElement | null;
		const el = card.current;
		if (!el) return;
		const controls = () => Array.from(el.querySelectorAll<HTMLElement>('button:not(:disabled), a[href], input, textarea, [tabindex="0"]'));
		(el.querySelector<HTMLElement>('[autofocus]') ?? controls()[0] ?? el).focus();
		const key = (ev: KeyboardEvent) => {
			if (ev.key === 'Escape') {
				ev.preventDefault();
				ev.stopPropagation();
				close.current();
			} else if (blockEnter && ev.key === 'Enter') {
				// Prevent native button activation too, even after tabbing to Delete.
				ev.preventDefault();
				ev.stopPropagation();
			} else if (ev.key === 'Tab') {
				const all = controls();
				const first = all[0] ?? el;
				const last = all[all.length - 1] ?? el;
				if (ev.shiftKey && document.activeElement === first) { ev.preventDefault(); last.focus(); }
				else if (!ev.shiftKey && document.activeElement === last) { ev.preventDefault(); first.focus(); }
			}
		};
		document.addEventListener('keydown', key, true);
		return () => {
			document.removeEventListener('keydown', key, true);
			if (opener?.isConnected) opener.focus();
		};
	}, []);
}

export function Confirm({ question, description, children, actionLabel, actionIcon, onConfirm, onCancel }: {
	question: string;
	description: string;
	children: ComponentChildren;
	actionLabel: string;
	actionIcon: IconName;
	onConfirm: () => void;
	onCancel: () => void;
}) {
	const card = useRef<HTMLDivElement>(null);
	useDialogFocus(card, onCancel, true);
	return (
		<div class="fixed inset-0 z-50 grid place-items-center bg-veil p-4" onClick={(e) => { if (e.target === e.currentTarget) onCancel(); }}>
			<div ref={card} role="dialog" aria-modal="true" aria-label={question} tabIndex={-1}
				data-confirm data-confirm-remove={actionIcon === 'trash-bin' || undefined}
				class="conversation-confirm flex w-full max-w-[400px] flex-col gap-4 overflow-y-auto rounded-panel bg-surface p-5 text-foreground">
				<div><h2 class="text-title">{question}</h2><p class="mt-1 text-meta text-foreground-muted">{description}</p></div>
				<div class="rounded-control bg-surface-2 p-3">{children}</div>
				<div class="flex justify-end gap-2">
					<button type="button" class="danger-action h-11 rounded-control px-4 text-item hover:bg-default" onClick={onCancel}>Cancelar</button>
					<button type="button" data-confirm-yes class="flex h-11 items-center gap-2 rounded-control bg-danger px-4 text-item font-semibold hover:bg-danger-hover" onClick={onConfirm}>
						<Icon name={actionIcon} size={16} />{actionLabel}
					</button>
				</div>
			</div>
		</div>
	);
}
