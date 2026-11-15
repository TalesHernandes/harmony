import { PersonalActions } from './PersonalSafety';
// Who somebody is, and what you can do about it.
//
// One panel, opened by pressing a person anywhere they appear — the list of
// people, the room in the channel column, a tile in the call. It closes when
// you press outside it or press Escape, which is the thing the owner asked for
// and which the old volume control got wrong.
//
// Incoming voice and screen audio have separate, per-person levels here.

import { useLayoutEffect, useRef, useState } from 'preact/hooks';
import { api, message, type Role, type User, type VoicePeer } from './api';
import { canChangeRoleMember, giveableRoles, nameClass, swatchClass } from './ranks';
import { log } from './diag';
import { MIC, SCREEN, SCREEN_AUDIO, CAMERA } from './media';
import { avatarURL } from './Avatar';
import { PhotoEditor, PhotoLightbox } from './PhotoEditor';
import { initials } from './Call';
import { dotClass, stateLabel, type PersonState } from './presence';
import { nameOf, nicknameOf } from './names';
import { Icon } from './Icon';
import { MicVolume, Slider } from './Volume';

// Nickname is the name you give somebody, and the point of it is in the line
// under the box: nobody else is ever shown it. That is true by the shape of the
// table it lives in (store migration 7) rather than by anything this panel
// does, and it is worth saying here because a person deciding whether to write
// "chefe" into a box cannot see the table.
function Nickname({ user, onSave }: { user: User; onSave: (name: string) => Promise<void> }) {
	const [value, setValue] = useState(() => nicknameOf(user.username));
	const [busy, setBusy] = useState(false);
	const saved = nicknameOf(user.username);
	const changed = value.trim() !== saved;
	return (
		<label class="flex flex-col gap-2">
			<span class="text-label uppercase text-foreground-muted">Como você chama essa pessoa</span>
			<div class="flex items-center gap-2">
				<input
					class="field h-10 min-w-0 flex-1 rounded-control bg-surface-2 px-3 text-item text-foreground placeholder:text-foreground-muted"
					placeholder={user.display_name}
					maxLength={32}
					value={value}
					data-nickname
					onInput={(e) => setValue((e.target as HTMLInputElement).value)}
					onKeyDown={(e) => {
						if (e.key !== 'Enter' || !changed || busy) return;
						e.preventDefault();
						setBusy(true);
						void onSave(value.trim()).finally(() => setBusy(false));
					}}
				/>
				<button
					class="h-10 shrink-0 rounded-control bg-default px-3 text-meta font-medium text-foreground hover:bg-default-hover disabled:bg-surface-2 disabled:text-foreground-muted"
					disabled={!changed || busy}
					data-nickname-save
					onClick={() => {
						setBusy(true);
						void onSave(value.trim()).finally(() => setBusy(false));
					}}
				>
					{value.trim() === '' && saved !== '' ? 'Tirar' : 'Salvar'}
				</button>
			</div>
			<span class="text-meta text-foreground-muted">Só você vê esse nome.</span>
		</label>
	);
}

export function Profile({
	user,
	peer,
	state,
	isMe,
	onNickname,
	onAvatar,
	inMyRoom,
	roomName,
	canJoinRoom,
	onJoinRoom,
	onWatch,
	onRing,
	onMessage,
	onCall,
	onClose,
	roles,
	allRoles,
	me,
}: {
	allRoles: Role[];
	me: User;
	user: User;
	// Their roles, highest first (D12c): drawn under the name, and the first
	// with a colour paints it.
	roles: Role[];
	// peer is set when this person is in a voice room right now.
	peer: VoicePeer | null;
	// state is online / ausente / em chamada / offline, as the dot draws it.
	state: PersonState;
	// onRing is set only when there is a room to call them *into*: this device
	// is in one and they are not. Null draws no button, and the server refuses
	// the request anyway if the situation changed in between.
	onRing: (() => void) | null;
	// Null on your own profile: there is no conversation with yourself.
	onMessage: (() => void) | null;
	onCall: (() => void) | null;
	// Your own profile is the other place the microphone lever belongs: it is
	// where somebody looks after being told "você tá muito baixo".
	isMe: boolean;
	// Setting what you call this person. Null on your own profile: a name you
	// give yourself is a display name, and there is already one of those.
	onNickname: ((name: string) => Promise<void>) | null;
	onAvatar: (version: string) => void;
	// inMyRoom is true when it is the room I am in, which is what decides
	// whether the incoming volume controls are available.
	inMyRoom: boolean;
	roomName: string;
	canJoinRoom: boolean;
	onJoinRoom: () => void;
	onWatch: (() => void) | null;
	onClose: () => void;
}) {
	const card = useRef<HTMLDivElement | null>(null);
	const [addingRole, setAddingRole] = useState(false);
	const [roleBusy, setRoleBusy] = useState(false);
	const [roleError, setRoleError] = useState('');
	const candidates = giveableRoles(me, allRoles, user);
	async function changeRole(role: Role, giving: boolean) {
		if (roleBusy) return;
		setRoleBusy(true);
		setRoleError('');
		const action = giving ? 'give' : 'take';
		try {
			await api.roleMember(role.id, user.username, giving);
			log('roles: ' + action);
			setAddingRole(false);
		} catch (err) {
			log('roles: ' + action + ' refused (' + (err instanceof Error ? err.message : 'error') + ')');
			setRoleError(message(err));
		} finally { setRoleBusy(false); }
	}
	const picker = useRef<HTMLInputElement | null>(null);
	const [editingPhoto, setEditingPhoto] = useState<File | null>(null);
	const [largePhoto, setLargePhoto] = useState(false);
	const [photoMenu, setPhotoMenu] = useState(false);
	const [photoBusy, setPhotoBusy] = useState(false);
	const [photoError, setPhotoError] = useState('');
	const [failedPhoto, setFailedPhoto] = useState('');
	const photo = user.avatar ? avatarURL(user.username, user.avatar) : '';
	async function changePhoto(file?: File): Promise<boolean> {
		setPhotoBusy(true);
		setPhotoError('');
		try {
			if (file && file.size > 5 * 1024 * 1024) {
				setPhotoError('A foto é grande demais (o limite é 5 MiB).');
				return false;
			}
			const version = file ? (await api.putAvatar(file)).avatar : (await api.deleteAvatar(), '');
			onAvatar(version);
			return true;
		} catch (err) {
			setPhotoError(message(err));
			return false;
		} finally {
			setPhotoBusy(false);
		}
	}
	// onClose goes through a ref, and that is not tidiness — it is the
	// difference between Escape working and Escape working *most* of the time.
	//
	// The caller writes it inline, so it is a different function on every
	// render of the app, and the app re-renders often while a call is running
	// — a dot changing colour, somebody starting to talk. With it in the
	// dependency list, every one of those took the listener off the document
	// and put it back, and a key pressed inside that gap went nowhere. It is
	// the same trick, for the same reason, as the ring box's timer (D4).
	const close = useRef(onClose);
	close.current = onClose;
	// A foreground photo panel owns Escape even before its focus effect runs.
	const escape = useRef(onClose);
	escape.current = () => {
		if (editingPhoto) { if (!photoBusy) setEditingPhoto(null); }
		else if (largePhoto) setLargePhoto(false);
		else if (photoMenu) setPhotoMenu(false);
		else if (addingRole) setAddingRole(false);
		else onClose();
	};

	// Pressing outside closes it, and so does Escape. Both are what people
	// expect and neither was there before.
	//
	// Before the paint, not after it (24/09/2026): as an effect this ran once
	// the panel was already on the screen, and an Escape pressed in that gap
	// went nowhere — the suite hit it, pressing Escape the moment the panel
	// appeared. The same gap as the composer's in D4l.
	useLayoutEffect(() => {
		const away = (ev: MouseEvent) => {
			if (card.current && !card.current.contains(ev.target as Node)) close.current();
		};
		const key = (ev: KeyboardEvent) => {
			if (ev.key === 'Escape') escape.current();
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

	const sharing = peer?.publishing ?? [];
	// The design's actions are solid 44px rows: the one that shows you
	// something happening right now (a screen) in accent, the rest default.
	const button =
		'flex h-11 w-full items-center gap-3 rounded-control px-3 text-[14px] font-medium text-foreground ';
	const plain = button + 'bg-default hover:bg-default-hover';
	const since = user.created_at
		? new Date(user.created_at).toLocaleDateString('pt-BR', { month: 'long', year: 'numeric' })
		: '';
	const section = 'text-label uppercase text-foreground-muted';

	// A 16px piece on the veil like every other panel, and no dividers inside
	// it: the gaps between the groups do that job (the owner's "sem divisória").
	// 440 wide with 24 of padding and an 80px face since 03/10/2026: at 360
	// with 16 the owner found everything "espremido".
	return (
		<div class="safe-fixed fixed inset-0 z-20 grid place-items-center bg-veil">
			<div
				ref={card}
				class="scroller flex max-h-full w-full max-w-[440px] flex-col gap-5 rounded-panel bg-surface p-6"
				role="dialog"
				aria-label={'perfil de ' + user.display_name}
			>
				<div class="flex items-start gap-4">
					<button
						disabled={!isMe && (!photo || failedPhoto === photo)}
						aria-label={"Ampliar foto de " + user.display_name}
						onClick={() => isMe ? setPhotoMenu(!photoMenu) : setLargePhoto(true)}
						data-face
						class="relative grid h-20 w-20 shrink-0 place-items-center rounded-full bg-default text-title text-foreground"
					>
						{isMe && <span class="profile-photo-edit"><Icon name="pencil" /></span>}
						{photo && failedPhoto !== photo ? (
							<img
								src={photo}
								class="h-full w-full rounded-full object-cover"
								loading="lazy"
								alt=""
								onError={() => setFailedPhoto(photo)}
							/>
						) : (
							initials(user.display_name || user.username)
						)}
					</button>
					<div class="min-w-0 flex-1 self-center">
						<p class={'truncate text-[20px] font-semibold leading-7 ' + (nameClass(roles.find((r) => r.color !== '')?.color ?? '') || 'text-foreground')}>
							{nameOf(user.username, user.display_name)}
						</p>
						<p class="truncate text-meta text-foreground-muted">
							@{user.username}
							{/* Their own name, kept visible underneath yours for them.
							    A nickname that *replaced* the real name everywhere
							    would make "quem é @fulano?" unanswerable from inside
							    the app. */}
							{nicknameOf(user.username) !== '' && <span class="pl-1">· {user.display_name}</span>}
						</p>
						<p class="flex flex-wrap items-center gap-x-1.5 pt-1 text-meta text-foreground-muted">
							<span class={'h-2 w-2 rounded-full ' + dotClass(state)} />
							{stateLabel(state)}
							{since && <span>· entrou em {since}</span>}
						</p>
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

				{(roles.length > 0 || user.is_admin || candidates.length > 0) && (
					<div class="flex flex-col gap-2" data-profile-roles>
						<span class={section}>Cargos</span>
						<div class="flex flex-wrap gap-1.5">
							{user.is_admin && (
								<span class="flex h-7 items-center rounded-control bg-surface-2 px-2.5 text-meta text-foreground">Dono</span>
							)}
							{roles.map((r) => (
								<span key={r.id} class="flex h-7 items-center gap-1.5 rounded-control bg-surface-2 px-2.5 text-meta text-foreground" data-profile-role={r.name}>
									<span class={'h-2.5 w-2.5 rounded-full ' + swatchClass(r.color)} />
									{r.name}
									{canChangeRoleMember(me, allRoles, r, user, false) && <button
										class="grid h-6 w-6 place-items-center rounded-control text-foreground-muted hover:bg-default hover:text-foreground"
										title="Tirar o cargo" aria-label={'Tirar o cargo ' + r.name} disabled={roleBusy}
										onClick={() => void changeRole(r, false)}><Icon name="xmark" size={12} /></button>}
								</span>
							))}
							{candidates.length > 0 && <button class="flex h-7 items-center rounded-control bg-surface-2 px-2.5 text-meta text-foreground hover:bg-default"
								disabled={roleBusy} aria-expanded={addingRole} onClick={() => setAddingRole(!addingRole)}>+ Dar cargo</button>}
						</div>
						{addingRole && candidates.length > 0 && <div class="flex flex-col gap-1 rounded-control bg-surface-2 p-2" aria-label="Dar cargo">
							{candidates.map((r) => <button key={r.id} class="flex h-10 items-center gap-2 rounded-control px-2 text-left text-item text-foreground hover:bg-default"
								disabled={roleBusy} data-profile-role-give={r.name} onClick={() => void changeRole(r, true)}>
								<span class={'h-2.5 w-2.5 shrink-0 rounded-full ' + swatchClass(r.color)} />{r.name}
							</button>)}
						</div>}
						{roleError && <p class="text-meta text-danger-soft-fg" role="alert">{roleError}</p>}
					</div>
				)}

				<div class="text-meta text-foreground-muted">
					{peer ? (
						<p>
							Em <span class="text-foreground">{roomName}</span>
							{sharing.includes(SCREEN) && ' — compartilhando a tela'}
							{!sharing.includes(SCREEN) && sharing.includes(CAMERA) && ' — com a câmera ligada'}
						</p>
					) : (
						<p>Não está em nenhuma sala de voz.</p>
					)}
				</div>

				{(onWatch || (peer && canJoinRoom) || onRing || onMessage || onCall) && (
					<div class="flex flex-col gap-2">
						{onCall && <button class={plain} onClick={onCall}><Icon name="handset" size={20} /> Ligar</button>}
						{onMessage && (
							<button class={plain} onClick={onMessage}>
								<Icon name="comment" size={20} /> Conversar no privado
							</button>
						)}
						{onWatch && (
							<button class={button + 'bg-default hover:bg-accent'} onClick={onWatch} data-watch>
								<Icon name="display" size={20} /> Assistir à tela de {user.display_name}
							</button>
						)}
						{peer && canJoinRoom && (
							<button class={plain} onClick={onJoinRoom}>
								<Icon name="headphones" size={20} /> Entrar em {roomName}
							</button>
						)}
						{onRing && (
							<button class={plain} onClick={onRing}>
								<Icon name="bell" size={20} /> Chamar {user.display_name} para a sala
							</button>
						)}
					</div>
				)}

				{!isMe && <PersonalActions username={user.username} />}

				{isMe && (
					<div class="flex flex-col gap-2">
						<input
							ref={picker}
							type="file"
							accept="image/jpeg,image/png,image/gif"
							class="hidden"
							data-avatar-picker
							onChange={(ev) => {
								const file = ev.currentTarget.files?.[0];
								ev.currentTarget.value = '';
								if (!file) return;
								setPhotoError('');
								if (file.size > 5 * 1024 * 1024) setPhotoError('A foto é grande demais (o limite é 5 MiB).');
								else if (!['image/jpeg', 'image/png', 'image/gif'].includes(file.type)) setPhotoError('Escolha JPEG, PNG ou GIF. WebP não é aceito.');
								else setEditingPhoto(file);
							}}
						/>
						{photoMenu && <div role="menu" aria-label="Foto de perfil" class="flex flex-col gap-2 rounded-control bg-surface-2 p-2">
						<button class={plain} disabled={photoBusy} onClick={() => {setPhotoMenu(false); picker.current?.click();}}>
							Alterar foto
						</button>
						{user.avatar && <>
						<button class={plain} onClick={() => {setPhotoMenu(false); setLargePhoto(true);}}>Ver foto</button></>}
						{user.avatar && (
							<button class={plain + " danger-action"} disabled={photoBusy} onClick={() => {setPhotoMenu(false); void changePhoto();}}>
								Remover foto
							</button>
						)}
						</div>}
						{photoError && <p class="text-meta text-danger-soft-fg" role="alert">{photoError}</p>}
					</div>
				)}

				{inMyRoom && (
					<div>
						<p class={section}>Volume — só para você</p>
						<Slider username={user.username} source={MIC} title="Voz" />
						{sharing.includes(SCREEN_AUDIO) && (
							<Slider username={user.username} source={SCREEN_AUDIO} title="Tela" />
						)}
					</div>
				)}

				{isMe && (
					<div>
						<p class={section}>Como você é ouvido</p>
						<MicVolume />
					</div>
				)}

				{onNickname && <Nickname user={user} onSave={onNickname} />}
				{editingPhoto && <PhotoEditor file={editingPhoto} onSave={changePhoto} onCancel={() => setEditingPhoto(null)} />}
				{largePhoto && photo && <PhotoLightbox src={photo} name={user.display_name} onClose={() => setLargePhoto(false)} />}
			</div>
		</div>
	);
}
