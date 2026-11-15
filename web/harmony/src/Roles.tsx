// The Roles tab of the settings (D12c): the list, highest first, dragged into
// order by its handle; and the role picked from it, with its name, colour,
// "show apart", permissions and who has it.
//
// Everything a person may not do is drawn but switched off rather than hidden,
// so a moderator sees why a role above theirs will not open: the rule is
// Discord's, and the server enforces it (store/roles.go). The list itself is
// not kept here — App holds it and the `roles` event replaces it whole, so a
// change made on another screen shows up in this one too.

import { useState } from 'preact/hooks';
import { api, message, type Permission, type Role, type User } from './api';
import { Avatar } from './Avatar';
import { Confirm } from './Confirm';
import { Icon } from './Icon';
import { log } from './diag';
import { nameOf } from './names';
import { PERMISSIONS, ROLE_COLORS, moveRole, nameClass, permissionsOf, swatchClass, topOf } from './ranks';

const card = 'rounded-control bg-surface-2 p-3';
const small =
	'h-8 shrink-0 rounded-control bg-default px-3 text-meta font-medium text-foreground hover:bg-default-hover disabled:text-foreground-muted disabled:hover:bg-default';
const danger =
	'h-8 shrink-0 rounded-control bg-danger-soft px-3 text-meta font-medium text-danger-soft-fg hover:bg-danger hover:text-foreground';
const label = 'text-label uppercase text-foreground-muted';
const errorLine = 'text-meta text-danger-soft-fg';

export function Roles({ me, roles, members }: { me: User; roles: Role[]; members: User[] }) {
	const mine = permissionsOf(me, roles);
	const top = topOf(me, roles);
	const canManage = mine.has('manage_roles');
	const sorted = [...roles].sort((a, b) => a.position - b.position);
	const [picked, setPicked] = useState('');
	const [error, setError] = useState('');
	const [busy, setBusy] = useState(false);
	const [drag, setDrag] = useState<{ id: string; onto: string; where: 'before' | 'after' } | null>(null);
	const role = sorted.find((r) => r.id === picked) ?? null;
	const below = (r: Role) => canManage && r.position > top;

	async function run(what: string, f: () => Promise<unknown>) {
		if (busy) return;
		setBusy(true);
		setError('');
		try {
			await f();
			log('roles: ' + what);
		} catch (err) {
			log('roles: ' + what + ' refused (' + (err instanceof Error ? err.message : 'error') + ')');
			setError(message(err));
		} finally {
			setBusy(false);
		}
	}

	function create() {
		void run('create', async () => {
			let name = 'Novo cargo';
			for (let i = 2; sorted.some((r) => r.name.toLowerCase() === name.toLowerCase()); i++) name = 'Novo cargo ' + i;
			const r = await api.createRole({ name, color: '', hoist: false, permissions: [] });
			setPicked(r.id);
		});
	}

	// The drag is by hand, like the channel column's (App.tsx, grab): the
	// pointer is followed on the window, the row under it found by its data
	// attribute, and the whole order goes to the server once, on release.
	function grab(r: Role, ev: PointerEvent) {
		if (ev.button !== 0 || !below(r)) return;
		ev.preventDefault();
		const y0 = ev.clientY;
		let started = false;
		let target: { onto: string; where: 'before' | 'after' } | null = null;
		const move = (e: PointerEvent) => {
			if (!started && Math.abs(e.clientY - y0) < 4) return;
			started = true;
			const row = document.elementFromPoint(e.clientX, e.clientY)?.closest<HTMLElement>('[data-role-row]');
			const onto = row?.dataset.roleRow ?? '';
			const other = sorted.find((x) => x.id === onto);
			// Only among the roles below one's own: the server refuses the rest.
			if (row && other && below(other)) {
				const box = row.getBoundingClientRect();
				target = { onto, where: e.clientY < box.top + box.height / 2 ? 'before' : 'after' };
			} else target = null;
			setDrag(target ? { id: r.id, ...target } : { id: r.id, onto: '', where: 'after' });
		};
		const stop = (commit: boolean) => {
			window.removeEventListener('pointermove', move);
			window.removeEventListener('pointerup', up);
			window.removeEventListener('pointercancel', cancel);
			setDrag(null);
			if (!commit || !started || !target) return;
			const order = moveRole(sorted, r.id, target.onto, target.where);
			if (order) void run('order', () => api.orderRoles(order));
		};
		const up = () => stop(true);
		const cancel = () => stop(false);
		window.addEventListener('pointermove', move);
		window.addEventListener('pointerup', up);
		window.addEventListener('pointercancel', cancel);
	}

	return (
		<div class="flex flex-col gap-4" data-roles>
			<div>
				<h3 class="text-item text-foreground">Cargos</h3>
				<p class="text-meta text-foreground-muted">
					Um cargo dá cor ao nome e permissões a quem o tem. O de cima vale mais: quem gerencia cargos só mexe nos que
					estão abaixo do seu. O dono está acima de todos e não tem cargo.
				</p>
			</div>
			{error && <p class={errorLine}>{error}</p>}
			{canManage && (
				<button class={small + ' flex items-center gap-2 self-start'} disabled={busy} onClick={create} data-role-create>
					<Icon name="plus" size={16} /> Criar cargo
				</button>
			)}
			{sorted.length === 0 ? (
				<p class="text-meta text-foreground-muted">Nenhum cargo ainda.</p>
			) : (
				<div class="flex flex-col gap-0.5">
					{sorted.map((r) => {
						const mark =
							drag && drag.id === r.id
								? ' opacity-50'
								: drag && drag.onto === r.id
									? drag.where === 'before' ? ' drop-before' : ' drop-after'
									: '';
						return (
							<div
								key={r.id}
								data-role-row={r.id}
								class={
									'relative flex h-11 items-center gap-2 rounded-control px-2 ' +
									(picked === r.id ? 'bg-default' : 'bg-surface-2 hover:bg-default') +
									mark
								}
							>
								<span
									class={
										'grid h-8 w-6 shrink-0 place-items-center text-foreground-muted ' +
										(below(r) ? 'cursor-grab touch-none' : 'opacity-0')
									}
									title={below(r) ? 'Arraste para mudar a ordem' : undefined}
									onPointerDown={(ev) => grab(r, ev)}
									data-role-grip={r.id}
								>
									<Icon name="bars" size={14} />
								</span>
								<button class="flex min-w-0 flex-1 items-center gap-2 text-left" onClick={() => setPicked(picked === r.id ? '' : r.id)} data-role={r.name}>
									<span class={'h-3 w-3 shrink-0 rounded-full ' + swatchClass(r.color)} />
									<span class={'min-w-0 flex-1 truncate text-item ' + (nameClass(r.color) || 'text-foreground')}>{r.name}</span>
									<span class="shrink-0 text-meta text-foreground-muted">
										{r.members.length} {r.members.length === 1 ? 'pessoa' : 'pessoas'}
									</span>
								</button>
							</div>
						);
					})}
				</div>
			)}
			{role && (
				<RoleEditor
					key={role.id}
					role={role}
					editable={below(role)}
					mine={mine}
					me={me}
					members={members}
					busy={busy}
					run={run}
					onGone={() => setPicked('')}
				/>
			)}
		</div>
	);
}

function RoleEditor({ role, editable, mine, me, members, busy, run, onGone }: {
	role: Role;
	editable: boolean;
	mine: Set<Permission>;
	me: User;
	members: User[];
	busy: boolean;
	run: (what: string, f: () => Promise<unknown>) => Promise<void>;
	onGone: () => void;
}) {
	const [name, setName] = useState(role.name);
	const [adding, setAdding] = useState('');
	const [asking, setAsking] = useState(false);
	const has = new Set(role.members);
	// Only the owner puts a role on the owner (store.SetRoleMember).
	const candidates = members.filter((m) => !has.has(m.username) && (!m.is_admin || me.is_admin));
	const patch = (what: string, p: Parameters<typeof api.updateRole>[1]) => void run(what, () => api.updateRole(role.id, p));

	return (
		<section class={card + ' flex flex-col gap-4'} data-role-editor={role.name}>
			{!editable && (
				<p class="text-meta text-foreground-muted">
					Só olhando: este cargo não está abaixo do seu cargo mais alto, ou você não gerencia cargos.
				</p>
			)}
			<label class="flex flex-col gap-2">
				<span class={label}>Nome</span>
				<div class="flex items-center gap-2">
					<input
						class="field h-10 min-w-0 flex-1 rounded-control bg-surface px-3 text-item text-foreground"
						maxLength={32}
						value={name}
						disabled={!editable}
						data-role-name
						onInput={(e) => setName((e.target as HTMLInputElement).value)}
						onKeyDown={(e) => {
							if (e.key === 'Enter' && name.trim() && name.trim() !== role.name) patch('rename', { name: name.trim() });
						}}
					/>
					{editable && (
						<button class={small + ' h-10'} disabled={busy || !name.trim() || name.trim() === role.name} onClick={() => patch('rename', { name: name.trim() })} data-role-name-save>
							Salvar
						</button>
					)}
				</div>
			</label>

			<div class="flex flex-col gap-2">
				<span class={label}>Cor</span>
				{/* "No colour" apart, then ten columns, so the five rows read light to
				    deep (ranks.ts). The ring sits outside the swatch: inside, it
				    vanishes on the white one. */}
				<div class="flex items-start gap-3">
					{['', ROLE_COLORS].map((group) => (
						<div key={group ? 'colors' : 'none'} class={group ? 'grid grid-cols-10 gap-2' : 'grid'}>
							{(group ? [...group] : ['']).map((c) => (
								<button
									key={c || 'none'}
									class={
										'grid h-6 w-6 place-items-center rounded-full ' +
										swatchClass(c) +
										(role.color === c ? ' ring-2 ring-foreground ring-offset-2 ring-offset-surface' : '')
									}
									disabled={!editable || busy}
									title={c === '' ? 'Sem cor' : c}
									aria-label={c === '' ? 'Sem cor' : 'Cor ' + c}
									aria-pressed={role.color === c}
									data-role-color={c || 'none'}
									onClick={() => patch('color', { color: c })}
								>
									{c === '' && <Icon name="ban" size={14} />}
								</button>
							))}
						</div>
					))}
				</div>
				<p class="text-meta text-foreground-muted">O nome aparece na cor do cargo mais alto que tiver cor.</p>
			</div>

			<label class="flex items-start gap-3">
				<input
					type="checkbox"
					class="mt-1 h-4 w-4 shrink-0"
					checked={role.hoist}
					disabled={!editable || busy}
					data-role-hoist
					onChange={(e) => patch('hoist', { hoist: (e.target as HTMLInputElement).checked })}
				/>
				<span>
					<span class="block text-item text-foreground">Mostrar separado na lista de pessoas</span>
					<span class="block text-meta text-foreground-muted">Quem está online com este cargo ganha um grupo próprio.</span>
				</span>
			</label>

			<div class="flex flex-col gap-2">
				<span class={label}>Permissões</span>
				{PERMISSIONS.map((p) => {
					const on = role.permissions.includes(p.id);
					// Only what one has oneself can be switched, either way.
					const can = editable && mine.has(p.id);
					return (
						<label key={p.id} class="flex items-start gap-3">
							<input
								type="checkbox"
								class="mt-1 h-4 w-4 shrink-0"
								checked={on}
								disabled={!can || busy}
								data-role-permission={p.id}
								onChange={(e) => {
									const want = (e.target as HTMLInputElement).checked;
									const next = PERMISSIONS.map((x) => x.id).filter((id) => (id === p.id ? want : role.permissions.includes(id)));
									patch('permissions', { permissions: next });
								}}
							/>
							<span>
								<span class="block text-item text-foreground">{p.name}</span>
								<span class="block text-meta text-foreground-muted">
									{p.detail}
									{editable && !mine.has(p.id) && ' Você não tem essa permissão, então não pode mudá-la.'}
								</span>
							</span>
						</label>
					);
				})}
				<p class="text-meta text-foreground-muted">Convites, contas e senhas continuam só do dono, com qualquer cargo.</p>
			</div>

			<div class="flex flex-col gap-2">
				<span class={label}>Quem tem · {role.members.length}</span>
				{role.members.map((username) => {
					const u = members.find((m) => m.username === username);
					return (
						<div key={username} class="flex h-10 items-center gap-2" data-role-member={username}>
							<Avatar username={username} avatar={u?.avatar} name={nameOf(username, u?.display_name ?? username)} />
							<span class="min-w-0 flex-1 truncate text-item text-foreground">{nameOf(username, u?.display_name ?? username)}</span>
							{editable && (
								<button
									class="grid h-8 w-8 shrink-0 place-items-center rounded-control text-foreground-muted hover:bg-default hover:text-foreground"
									title={'Tirar o cargo de ' + (u?.display_name ?? username)}
									aria-label={'Tirar o cargo de ' + (u?.display_name ?? username)}
									disabled={busy}
									data-role-member-remove={username}
									onClick={() => void run('take', () => api.roleMember(role.id, username, false))}
								>
									<Icon name="xmark" size={14} />
								</button>
							)}
						</div>
					);
				})}
				{editable && candidates.length > 0 && (
					<div class="flex items-center gap-2">
						<select
							class="field h-10 min-w-0 flex-1 rounded-control bg-surface px-3 text-item text-foreground"
							value={adding}
							data-role-member-pick
							onChange={(e) => setAdding((e.target as HTMLSelectElement).value)}
						>
							<option value="">Escolha uma pessoa…</option>
							{candidates.map((m) => (
								<option key={m.username} value={m.username}>
									{nameOf(m.username, m.display_name)} (@{m.username})
								</option>
							))}
						</select>
						<button
							class={small + ' h-10'}
							disabled={busy || !adding}
							data-role-member-add
							onClick={() => {
								const who = adding;
								setAdding('');
								void run('give', () => api.roleMember(role.id, who, true));
							}}
						>
							Dar o cargo
						</button>
					</div>
				)}
			</div>

			{editable && (
				<button class={danger + ' flex items-center gap-2 self-start'} disabled={busy} onClick={() => setAsking(true)} data-role-delete>
					<Icon name="trash-bin" size={14} /> Apagar cargo
				</button>
			)}
			{asking && (
				<Confirm
					question="Apagar este cargo?"
					description="Quem tem o cargo perde a cor e as permissões dele na hora. Isso não tem volta."
					actionLabel="Apagar"
					actionIcon="trash-bin"
					onCancel={() => setAsking(false)}
					onConfirm={() => {
						setAsking(false);
						void run('delete', () => api.removeRole(role.id)).then(onGone);
					}}
				>
					<p class={'text-item ' + (nameClass(role.color) || 'text-foreground')}>{role.name}</p>
				</Confirm>
			)}
		</section>
	);
}
