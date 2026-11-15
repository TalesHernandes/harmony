// Roles (D12c), as pure functions over the list the server sends whole: what
// somebody may do, the colour their name is drawn in, and which group of the
// member list they fall into. The server decides everything that matters;
// these only draw it, and hide buttons it would refuse.

import type { Permission, Role, User } from './api';

// The palette, in the order the settings screen lays it out: ten columns, five
// rows from light to deep, Discord's twenty swatches in the middle two. The server accepts nothing
// else (store.RoleColors), because this page may not set a colour at run time
// (security_test.go, TestTheClientSourceHasNoInlineStyle) — each one is a class in style.css,
// `role-<hex>` for a name and `swatch-<hex>` for a square.
export const ROLE_COLORS = [
	'#48e5c2', '#7bed9f', '#74b9ff', '#c39bd3', '#f78fb3', '#ffeaa7', '#f8c291', '#ff7675', '#dfe6e9', '#ffffff',
	'#1abc9c', '#2ecc71', '#3498db', '#9b59b6', '#e91e63', '#f1c40f', '#e67e22', '#e74c3c', '#95a5a6', '#607d8b',
	'#11806a', '#1f8b4c', '#206694', '#71368a', '#ad1457', '#c27c0e', '#a84300', '#992d22', '#979c9f', '#546e7a',
	'#00bcd4', '#a3e635', '#5865f2', '#7c4dff', '#e040fb', '#ffd700', '#ff7f50', '#dc143c', '#8d6e63', '#b0bec5',
	'#004d40', '#1b5e20', '#1a3a8a', '#4a148c', '#880e4f', '#8a6d00', '#6d3410', '#7f1d1d', '#4e342e', '#37474f',
] as const;

export const PERMISSIONS: Array<{ id: Permission; name: string; detail: string }> = [
	{ id: 'manage_channels', name: 'Gerenciar canais', detail: 'Criar, renomear, organizar e apagar canais e categorias.' },
	{ id: 'manage_messages', name: 'Gerenciar mensagens', detail: 'Apagar a mensagem dos outros e fixar mensagens nos canais.' },
	{ id: 'manage_roles', name: 'Gerenciar cargos', detail: 'Criar, editar e dar cargos abaixo do seu cargo mais alto.' },
];

const known = new Set<string>(ROLE_COLORS);

// nameClass is the class that paints a colour, or '' for none (and for a
// colour this build does not know, which is the text colour: safe to be wrong).
export function nameClass(color: string): string {
	return known.has(color) ? 'role-' + color.slice(1) : '';
}

export function swatchClass(color: string): string {
	return known.has(color) ? 'swatch-' + color.slice(1) : 'swatch-none';
}

const byPosition = (a: Role, b: Role) => a.position - b.position;

// rolesOf is somebody's roles, highest first.
export function rolesOf(username: string, roles: Role[]): Role[] {
	return roles.filter((r) => r.members.includes(username)).sort(byPosition);
}

// colorOf is the colour of somebody's highest role **that has one**; a role
// without colour lets the next one down show, as on Discord.
export function colorOf(username: string, roles: Role[]): string {
	return rolesOf(username, roles).find((r) => r.color !== '')?.color ?? '';
}

// permissionsOf is what somebody may do: the owner everything, anybody else
// the union of their roles.
export function permissionsOf(me: User | null, roles: Role[]): Set<Permission> {
	if (!me) return new Set();
	if (me.is_admin) return new Set(PERMISSIONS.map((p) => p.id));
	const out = new Set<Permission>();
	for (const r of rolesOf(me.username, roles)) for (const p of r.permissions) out.add(p);
	return out;
}

// topOf is the position of somebody's highest role: -1 for the owner (above
// all of them), Infinity for nobody.
export function topOf(me: User | null, roles: Role[]): number {
	if (!me) return Infinity;
	if (me.is_admin) return -1;
	return rolesOf(me.username, roles)[0]?.position ?? Infinity;
}

export function canChangeRoleMember(me: User, roles: Role[], role: Role, target: User, giving: boolean): boolean {
	const mine = permissionsOf(me, roles);
	return mine.has('manage_roles') && role.position > topOf(me, roles) &&
		(!target.is_admin || !!me.is_admin) && (!giving || role.permissions.every((p) => mine.has(p)));
}

export function giveableRoles(me: User, roles: Role[], target: User): Role[] {
	return roles.filter((r) => !r.members.includes(target.username) && canChangeRoleMember(me, roles, r, target, true)).sort(byPosition);
}

// hoistOf is the role whose group of the member list somebody sits in: their
// highest role marked "show apart", or null for the ordinary groups.
export function hoistOf(username: string, roles: Role[]): Role | null {
	return rolesOf(username, roles).find((r) => r.hoist) ?? null;
}

// moveRole is the whole order after dragging one role onto another's place,
// or null when nothing moves.
export function moveRole(roles: Role[], id: string, onto: string, where: 'before' | 'after'): string[] | null {
	if (id === onto) return null;
	const ids = [...roles].sort(byPosition).map((r) => r.id);
	const before = ids.join();
	const rest = ids.filter((x) => x !== id);
	const at = rest.indexOf(onto);
	if (at < 0 || !ids.includes(id)) return null;
	rest.splice(where === 'before' ? at : at + 1, 0, id);
	return rest.join() === before ? null : rest;
}
