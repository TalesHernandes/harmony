import { PersonalList } from './PersonalSafety';
// The drawer behind the gear: this device and the others, the invites (owner
// only, and the server is what decides that), and the diagnostic log.

import type { ComponentChildren } from 'preact';
import { useEffect, useState } from 'preact/hooks';
import { AccountSecurity } from './Security';
import { api, message, ApiError, type AdminAccount, type AppRelease, type Invite, type Role, type Session, type User } from './api';
import { Roles } from './Roles';
import { permissionsOf } from './ranks';
import { AudioChoices, CameraChoice, ScreenChoices } from './Broadcast';
import { voice, type VoiceView } from './media';
import { asText, entries, geometry, line, rememberedGeometry, subscribe } from './diag';
import { NotifySettings } from './Notifications';
import { MicFilters, MicVolume } from './Volume';
import { forget } from './drafts';
import { Icon } from './Icon';
import { Avatar } from './Avatar';
import { Confirm } from './Confirm';

// The settings are one 16px piece, so everything in them is an 8px piece
// sitting on the raised ground (the two-corner rule, style.css).
const card = 'rounded-control bg-surface-2 p-3';
const small =
	'h-8 shrink-0 rounded-control bg-default px-3 text-meta font-medium text-foreground hover:bg-default-hover';
const danger =
	'h-8 shrink-0 rounded-control bg-danger-soft px-3 text-meta font-medium text-danger-soft-fg hover:bg-danger hover:text-foreground';
const label = 'text-label uppercase text-foreground-muted';
const errorLine = 'text-meta text-danger-soft-fg';

function when(iso: string): string {
	return new Date(iso).toLocaleString('pt-BR', {
		day: '2-digit',
		month: '2-digit',
		hour: '2-digit',
		minute: '2-digit',
	});
}

function Section({ title, children }: { title: string; children: ComponentChildren }) {
	return (
		<section class="flex flex-col gap-2">
			<h3 class={label}>{title}</h3>
			{children}
		</section>
	);
}

function Devices() {
	const [list, setList] = useState<Session[]>([]);
	const [error, setError] = useState('');

	async function load() {
		try {
			setList((await api.sessions()).sessions);
		} catch (err) {
			setError(message(err));
		}
	}
	useEffect(() => {
		void load();
	}, []);

	return (
		<Section title="Aparelhos conectados">
			{error && <p class={errorLine}>{error}</p>}
			<div class="flex flex-col gap-0.5">
				{list.map((s) => (
					<div key={s.id} class={card + ' flex items-center gap-3'}>
						<div class="min-w-0 flex-1">
							<p class="truncate text-item text-foreground">
								{s.current ? 'Este aparelho' : s.user_agent || 'Aparelho'}
							</p>
							<p class="text-meta text-foreground-muted">
								Entrou {when(s.created_at)} · visto {when(s.last_seen_at)}
							</p>
						</div>
						{!s.current && (
							<button
								class={"danger-action " + (danger)}
								onClick={async () => {
									// Taking a device's access away is the one action
									// here whose failure matters most: that device can
									// still get in, and has to be told so.
									try {
										await api.revokeSession(s.id);
										setError('');
									} catch (err) {
										setError('Não desconectou: ' + message(err));
									}
									void load();
								}}
							>
								Desconectar
							</button>
						)}
					</div>
				))}
			</div>
			<button
				class={"danger-action " + (small + ' self-start')}
				onClick={async () => {
					try {
						await api.revokeOthers();
						setError('');
					} catch (err) {
						setError('Não desconectou os outros: ' + message(err));
					}
					void load();
				}}
			>
				Desconectar todos os outros
			</button>
		</Section>
	);
}

function AppDownload({ platform }: { platform: 'android' | 'windows' }) {
	const [app, setApp] = useState<AppRelease | null>(null);
	useEffect(() => {
		const load = platform === 'android' ? api.androidApp : api.windowsApp;
		load().then(setApp, () => setApp(null));
	}, [platform]);
	if (!app) return null;
	const mb = (app.size / 1024 / 1024).toFixed(1).replace('.', ',');
	return (
		<Section title={platform === 'android' ? 'App para Android' : 'Programa para Windows'}>
			<div class={card + ' flex items-center gap-3'} data-android-app={platform === 'android' || undefined} data-windows-app={platform === 'windows' || undefined}>
				<div class="min-w-0 flex-1">
					<p class="truncate text-item text-foreground">{platform === 'windows' ? 'harmony para Windows' : `harmony ${app.version_name}`}</p>
					<p class="text-meta text-foreground-muted">
						{mb} MB · depois de instalado, ele se atualiza sozinho
					</p>
				</div>
				<a class={small + ' flex items-center'} href={app.url} download>
					Baixar
				</a>
			</div>
		</Section>
	);
}

const DAY = 24 * 60 * 60 * 1000;

// The invite's state in one line, the way the design words it: who made it and
// when, and then either what is left of it or who came in through it. An
// expired one is not listed — the server throws those away (store/accounts.go).
function inviteState(inv: Invite, me: string): string {
	const by = !inv.created_by
		? 'Criado pela linha de comando'
		: inv.created_by === me
			? 'Criado por você'
			: 'Criado por @' + inv.created_by;
	if (inv.used) return `${by} · ${when(inv.created_at)} · usado por @${inv.used_by}`;
	const days = Math.max(1, Math.ceil((Date.parse(inv.expires_at) - Date.now()) / DAY));
	return `${by} · ${when(inv.created_at)} · vence em ${days} dia${days === 1 ? '' : 's'}`;
}

// Painéis §03: one door in, and only the owner sees it. The note comes first
// because it is what makes the list mean something three weeks later.
function Invites({ me }: { me: string }) {
	const [list, setList] = useState<Invite[]>([]);
	const [can, setCan] = useState(false);
	const [note, setNote] = useState('');
	const [fresh, setFresh] = useState<Invite | null>(null);
	const [copied, setCopied] = useState(false);
	const [error, setError] = useState('');

	async function load() {
		try {
			const res = await api.invites();
			setList(res.invites);
			setCan(res.can_invite);
		} catch (err) {
			setError(message(err));
		}
	}
	useEffect(() => {
		void load();
	}, []);

	// The server decides; this only stops drawing a section that would answer
	// 403 anyway.
	if (!can) return null;

	return (
		<div class="flex flex-col gap-4" data-invites>
			<div>
				<h3 class="text-item text-foreground">Convites</h3>
				<p class="text-meta text-foreground-muted">Cada convite serve uma vez e vale por sete dias.</p>
			</div>
			{error && <p class={errorLine}>{error}</p>}
			<label class="flex flex-col gap-2">
				<span class={label}>para quem é (só você vê)</span>
				<input
					class="field h-11 rounded-control bg-surface-2 px-3 text-item text-foreground placeholder:text-foreground-muted"
					placeholder="Uma anotação sua"
					value={note}
					onInput={(e) => setNote((e.target as HTMLInputElement).value)}
				/>
				<span class="text-meta text-foreground-muted">
					Fica só nesta lista, para você lembrar de quem é cada convite. Quem recebe não vê.
				</span>
			</label>
			<button
				class="flex h-11 items-center justify-center gap-2 rounded-control bg-accent text-[14px] font-semibold text-foreground hover:bg-accent-hover"
				onClick={async () => {
					try {
						setFresh(await api.createInvite(note));
						setCopied(false);
						setNote('');
						setError('');
						void load();
					} catch (err) {
						setError(message(err));
					}
				}}
			>
				<Icon name="person-plus" size={16} />
				Gerar convite
			</button>
			{fresh?.url && (
				<Section title="o que você acabou de gerar">
					<div class={card + ' flex items-center gap-2'}>
						<span class="min-w-0 flex-1 truncate text-item text-foreground" title={fresh.url}>
							{fresh.url}
						</span>
						<button
							class={small + ' flex h-9 items-center gap-2'}
							onClick={async () => {
								await navigator.clipboard?.writeText(fresh.url ?? '');
								setCopied(true);
							}}
						>
							<Icon name={copied ? 'circle-check' : 'copy'} size={16} />
							{copied ? 'Copiado' : 'Copiar'}
						</button>
					</div>
					<p class="text-meta text-foreground-muted">
						Link inteiro: dá para colar numa conversa sem explicar o que fazer com ele.
					</p>
				</Section>
			)}
			{list.length > 0 && (
				<Section title={`seus convites — ${list.length}`}>
					<div class="flex flex-col gap-0.5">
						{list.map((inv) => (
							<div key={inv.id} class={card + ' flex items-center gap-3'} data-invite-state={inv.used ? 'usado' : 'aberto'}>
								{/* Open or used, said by the mark before it is read
								    (Painéis §03). An expired one never reaches this
								    list, so the third mark the design has for it is
								    not drawn. */}
								<span class={'shrink-0 ' + (inv.used ? 'text-success' : 'text-foreground-muted')}>
									<Icon name={inv.used ? 'circle-check' : 'clock'} size={16} />
								</span>
								<div class="min-w-0 flex-1">
									<p
										class={
											'truncate text-meta ' +
											(inv.used ? 'text-foreground-muted' : 'text-foreground')
										}
									>
										{inv.note || 'Sem anotação'}
									</p>
									<p class="text-meta text-foreground-muted">{inviteState(inv, me)}</p>
								</div>
								{/* Cancelling is only for the ones not used yet: somebody
								    who came in through one is already inside. */}
								{!inv.used && (
									<button
										class={"danger-action " + (danger)}
										onClick={async () => {
											try {
												await api.revokeInvite(inv.id);
												setError('');
											} catch (err) {
												setError('Não cancelou: ' + message(err));
											}
											void load();
										}}
									>
										Cancelar
									</button>
								)}
							</div>
						))}
					</div>
				</Section>
			)}
		</div>
	);
}

function accountState(a: AdminAccount): string {
	const entry = a.invited_by ? `Entrou por convite de @${a.invited_by}` : 'Entrou pela linha de comando';
	if (!a.last_seen_at) return `${entry} · ainda não entrou`;
	const days = Math.max(0, Math.floor((Date.now() - Date.parse(a.last_seen_at)) / DAY));
	return `${entry} · ${days === 0 ? 'visto hoje' : `visto há ${days} dia${days === 1 ? '' : 's'}`}`;
}

function People({ me }: { me: User }) {
	const [list, setList] = useState<AdminAccount[]>([]);
	const [allowed, setAllowed] = useState(false);
	const [error, setError] = useState('');
	const [busy, setBusy] = useState(false);
	const [asking, setAsking] = useState<{ account: AdminAccount; action: 'disable' | 'reset' } | null>(null);
	const [fresh, setFresh] = useState<{ username: string; password: string } | null>(null);
	const [copied, setCopied] = useState(false);

	async function load() {
		try {
			setList((await api.accounts()).accounts);
			setAllowed(true);
		} catch (err) {
			if (err instanceof ApiError && err.status === 403) setAllowed(false);
			else setError(message(err));
		}
	}
	useEffect(() => { if (me.is_admin) void load(); }, [me.is_admin]);

	async function act(a: AdminAccount, action: 'disable' | 'enable' | 'reset') {
		if (busy) return;
		setAsking(null);
		setBusy(true);
		setError('');
		setFresh(null);
		try {
			if (action === 'reset') {
				setFresh({ username: a.username, ...(await api.resetAccountPassword(a.username)) });
				setCopied(false);
			} else if (action === 'disable') await api.disableAccount(a.username);
			else await api.enableAccount(a.username);
			await load();
		} catch (err) { setError(message(err)); }
		finally { setBusy(false); }
	}

	if (!me.is_admin || (!allowed && !error)) return null;
	return (
		<div class="flex flex-col gap-4" data-accounts>
			<div><h3 class="text-item text-foreground">Pessoas</h3>
				<p class="text-meta text-foreground-muted">Quem tem conta neste harmony.</p></div>
			{error && <p class={errorLine}>{error}</p>}
			{fresh && <Section title={`nova senha de @${fresh.username}`}>
				<div class={card + ' flex items-center gap-2'}>
					<span class="min-w-0 flex-1 break-all text-item text-foreground" data-fresh-password>{fresh.password}</span>
					<button class={small + ' flex h-9 items-center gap-2'} onClick={async () => {
						try {
							await navigator.clipboard.writeText(fresh.password);
							setCopied(true);
						} catch { setError('Não copiou. Selecione a senha e copie manualmente.'); }
					}}><Icon name={copied ? 'circle-check' : 'copy'} size={16} />{copied ? 'Copiado' : 'Copiar'}</button>
					<button class={"danger-action " + (small)} onClick={() => setFresh(null)}>Fechar senha</button>
				</div>
				<p class="text-meta text-foreground-muted">Passe para a pessoa por um caminho seu; ela entra e todas as sessões antigas dela já caíram.</p>
			</Section>}
			<div class="flex flex-col gap-0.5">
				{list.map((a) => <div key={a.username} class={card + ' flex flex-wrap items-center gap-3'} data-account={a.username}>
					<Avatar username={a.username} avatar={a.avatar} name={a.display_name || a.username} />
					<div class="min-w-0 flex-1">
						<p class="truncate text-item text-foreground">{a.display_name} <span class="text-meta text-foreground-muted">@{a.username}</span></p>
						<p class="text-meta text-foreground-muted">{accountState(a)}</p>
						<p class="text-meta text-foreground-muted">E-mail {a.email_verified ? 'confirmado' : 'não confirmado'} · 2FA: {a.has_twofa ? 'sim' : 'não'}</p>
						{a.disabled && <p class="text-meta text-danger-soft-fg">desativada</p>}
					</div>
					{a.username !== me.username && <div class="flex flex-wrap gap-2">
						{!a.is_admin && <button disabled={busy} class={a.disabled ? small : danger} onClick={() => {
							if (a.disabled) void act(a, 'enable');
							else setAsking({ account: a, action: 'disable' });
						}}>{a.disabled ? 'Reativar' : 'Desativar'}</button>}
						<button disabled={busy} class={danger} onClick={() => setAsking({ account: a, action: 'reset' })}>Nova senha</button>
					</div>}
				</div>)}
			</div>
			{asking && <Confirm
				question={asking.action === 'disable' ? 'Desativar esta conta?' : 'Gerar nova senha?'}
				description={asking.action === 'disable' ? 'Todos os aparelhos serão desconectados. A pessoa só volta a entrar quando você reativar a conta.' : 'A senha antiga deixa de funcionar e todos os aparelhos serão desconectados. A nova senha aparece só desta vez.'}
				actionLabel={asking.action === 'disable' ? 'Desativar' : 'Nova senha'} actionIcon="person"
				onCancel={() => setAsking(null)} onConfirm={() => void act(asking.account, asking.action)}>
				<p class="text-item text-foreground">{asking.account.display_name} · @{asking.account.username}</p>
			</Confirm>}
		</div>
	);
}

function Diagnostics() {
	const [, bump] = useState(0);
	const [copied, setCopied] = useState('');
	useEffect(() => subscribe(() => bump((n) => n + 1)), []);
	const lines = entries().slice(-120).reverse();

	return (
		<Section title="Diagnóstico">
			<div class="flex items-center gap-2">
				<button
					class={small}
					onClick={async () => {
						await navigator.clipboard?.writeText(asText());
						setCopied('Copiado');
					}}
				>
					Copiar log
				</button>
				{navigator.share !== undefined && (
					<button
						class={small}
						onClick={() => void navigator.share({ title: 'harmony diag', text: asText() })}
					>
						Compartilhar
					</button>
				)}
				{copied && <span class="text-meta text-foreground-muted">{copied}</span>}
			</div>
			<div class={card + ' font-mono text-label font-normal normal-case tracking-normal text-foreground-muted'}>
				<p class="break-all">Agora: {geometry()}</p>
				{rememberedGeometry() && <p class="break-all">Última: {rememberedGeometry()}</p>}
			</div>
			<div class={card + ' scroller max-h-80 font-mono text-label font-normal normal-case tracking-normal text-foreground-muted'}>
				{lines.map((e, i) => (
					<div key={i}>{line(e)}</div>
				))}
			</div>
		</Section>
	);
}

// VoiceAndVideo is where somebody sets this up once and stops thinking about
// it: which microphone, which speakers, which camera, and how much a shared
// screen is allowed to cost.
//
// The same questions are asked in the panel that starts a screen or a camera,
// and both call the same functions in media.ts. They are here as well because
// choosing a headset is not something anybody wants to do in the middle of a
// call, and because everything here is remembered for this person on this
// device — so it is done once and it is there next time.
function VoiceAndVideo({ view }: { view: VoiceView }) {
	useEffect(() => {
		// Names of devices go from blank to real the moment permission exists,
		// so the list is asked for again every time this is opened.
		void voice.refreshDevices();
	}, []);
	// Three groups rather than one column of everything: a microphone, a camera
	// and a shared screen are three separate decisions, and on a wide panel the
	// first two sit side by side instead of making somebody scroll past them.
	// No cards around them: the fields are the raised pieces, and a card under
	// a field would be a third ground (the design's "sem divisória"). The first
	// two need no heading of their own — their fields already say "Microfone"
	// and "Câmera", and a heading on top said it twice.
	return (
		<div class="grid gap-6 lg:grid-cols-2">
			<div>
				<AudioChoices view={view} />
				<MicVolume />
				<MicFilters view={view} />
			</div>
			<div>
				<CameraChoice view={view} />
			</div>
			<div class="lg:col-span-2">
				<h3 class="text-item text-foreground">Quando você compartilha a tela</h3>
				<ScreenChoices view={view} />
			</div>
		</div>
	);
}

function Account({ me }: { me: User }) {
	const [leaveError, setLeaveError] = useState('');
	// Leaving asks first, with the same box as removing a message (Etapa 5
	// §03): on a shared computer it is the difference between signing out and
	// losing what you typed, and the button sits right under the list of
	// devices somebody came here to tidy.
	const [asking, setAsking] = useState(false);
	async function leave() {
		setAsking(false);
		try {
			await api.logout();
		} catch (err) {
			// Still signed in on the server, so this page stays: going to
			// /login would look like it worked.
			setLeaveError('Não saiu: ' + message(err));
			return;
		}
		// Nothing typed on a shared computer should outlive the session it was
		// typed in. The preferences stay — they are about this browser, not
		// about anybody's sentences (drafts.ts).
		forget();
		location.href = '/login';
	}
	return (
		<div class="flex flex-col gap-6">
			<Section title="Você">
				<div class="flex flex-col gap-0.5">
					<div class={card + ' flex items-center justify-between gap-3'}>
						<span class="text-meta text-foreground-muted">Nome</span>
						<span class="min-w-0 truncate text-item text-foreground">{me.display_name}</span>
					</div>
					<div class={card + ' flex items-center justify-between gap-3'}>
						<span class="text-meta text-foreground-muted">Usuário</span>
						<span class="min-w-0 truncate text-item text-foreground">@{me.username}</span>
					</div>
				</div>
			</Section>
			<PersonalList />
			<AccountSecurity />
			<AppDownload platform="android" />
			<AppDownload platform="windows" />
			{/* Invites draws nothing at all for anybody but the owner, and that
			    is the server's decision, not this panel's. */}
			<Invites me={me.username} />
			<People me={me} />
			<Devices />
			<div class="flex flex-col gap-2">
				<button
					class="danger-action flex h-11 items-center justify-center rounded-control bg-danger-soft px-4 text-[14px] font-medium text-danger-soft-fg hover:bg-danger hover:text-foreground"
					onClick={() => setAsking(true)}
				>
					Sair desta conta
				</button>
				{leaveError && <p class={errorLine}>{leaveError}</p>}
			</div>
			{/* The licence the emoji pictures come under asks for the credit to
			    be somewhere a person can find it (Twemoji.tsx). */}
			<p class="text-meta text-foreground-muted" data-credits>
				Emojis: Twemoji, © Twitter e colaboradores, licença CC-BY 4.0.
			</p>
			{asking && (
				<Confirm
					question="Sair desta conta?"
					description="Neste aparelho, o que você deixou escrito e não mandou vai embora junto. Para voltar, entre de novo com a sua senha."
					actionLabel="Sair"
					actionIcon="arrow-right-from-square"
					onCancel={() => setAsking(false)}
					onConfirm={() => void leave()}
				>
					<div class="flex items-center gap-3">
						<Avatar username={me.username} avatar={me.avatar} name={me.display_name || me.username} />
						<div class="min-w-0">
							<p class="truncate text-meta font-semibold text-foreground">{me.display_name}</p>
							<p class="truncate text-meta text-foreground-muted">@{me.username}</p>
						</div>
					</div>
				</Confirm>
			)}
		</div>
	);
}

// The four things the settings are actually about. They used to be one column
// of five sections stacked on top of each other, which meant the diagnostic log
// — the thing somebody opens once a month — sat between them and nothing, and
// the microphone they came here for was four scrolls up.
//
// Tabs and not an accordion: an accordion still asks somebody to remember which
// of five drawers has the thing in it, and these four are genuinely separate
// errands. Which one is open is remembered per device, so coming back lands
// where you were.
const TABS = [
	['av', 'Voz e vídeo', 'microphone'],
	['avisos', 'Avisos', 'bell'],
	['conta', 'Conta', 'person'],
	['cargos', 'Cargos', 'persons'],
	['diag', 'Diagnóstico', 'file-text'],
] as const;

type Tab = (typeof TABS)[number][0];
const TAB_KEY = 'harmony.settings.tab';

function savedTab(): Tab {
	try {
		const raw = localStorage.getItem(TAB_KEY) ?? '';
		if (TABS.some(([id]) => id === raw)) return raw as Tab;
	} catch {
		// Storage switched off: start where everybody starts.
	}
	return 'av';
}

// Settings is a centred panel on the veil, with more room around its desktop
// content: the tabs down its left side on a monitor, and across its
// top on a telephone, where it takes the screen but for the veil's margin. Escape, the X and a
// click on the veil all close it, the way every panel closes.
//
// The grid's one row is a fixed 1fr and not auto: in an auto row the card's
// percentage height has nothing to resolve against, and the owner's list of
// invites grew it past the top of the screen, tabs and X with it.
export function Settings({ view, me, roles, members, onClose }: { view: VoiceView; me: User; roles: Role[]; members: User[]; onClose: () => void }) {
	const [chosen, setTab] = useState<Tab>(savedTab);
	// Roles is a tab only for whoever manages them (the owner included); the
	// others see roles where they are used, on names and profiles.
	const managesRoles = permissionsOf(me, roles).has('manage_roles');
	const tabs = TABS.filter(([id]) => id !== 'cargos' || managesRoles);
	const tab: Tab = tabs.some(([id]) => id === chosen) ? chosen : 'av';
	const choose = (id: Tab) => {
		setTab(id);
		try {
			localStorage.setItem(TAB_KEY, id);
		} catch {
			// This visit remembers it; the next one does not.
		}
	};
	useEffect(() => {
		const key = (e: KeyboardEvent) => {
			// Something on top (a confirmation) answers its own Escape first.
			if (e.key === 'Escape' && !e.defaultPrevented) onClose();
		};
		window.addEventListener('keydown', key);
		return () => window.removeEventListener('keydown', key);
	}, [onClose]);
	return (
		<div
			class="safe-fixed fixed inset-0 z-30 grid grid-rows-[minmax(0,1fr)] place-items-center bg-veil"
			onClick={(e) => {
				if (e.target === e.currentTarget) onClose();
			}}
		>
			{/* min-w-0: the veil's column is auto, and an auto column grows to
			    the card's narrowest width - the tab row, which cannot wrap, is
			    429 pixels of it. Without a floor of its own the card took that
			    on a 375 telephone and cut "@username" off on the right; with it
			    the column is the screen and the row slides inside the card. */}
			<div
				class="flex h-full w-full min-w-0 flex-col rounded-panel bg-surface p-2 md:h-[min(720px,100%)] md:max-w-[880px] md:flex-row md:gap-4 md:p-6"
				role="dialog"
				aria-label="Ajustes"
				data-settings
			>
				<div class="flex shrink-0 flex-col gap-2 md:w-52">
					<div class="flex h-10 items-center gap-2 pl-2">
						<h2 class="min-w-0 flex-1 truncate text-title">Ajustes</h2>
						<button
							class="grid h-8 w-8 shrink-0 place-items-center rounded-control bg-default text-foreground hover:bg-default-hover md:hidden"
							onClick={onClose}
							title="fechar"
							aria-label="fechar"
						>
							<Icon name="xmark" />
						</button>
					</div>
					{/* On a telephone the row slides sideways rather than wrapping
					    to two lines: four words do not fit across 390 pixels, and a
					    tab row that changes height as you press it moves everything
					    under it. */}
					<div class="scroller-x flex gap-1 md:flex-col">
						{tabs.map(([id, name, icon]) => (
							<button
								key={id}
								class={
									'flex h-10 shrink-0 items-center gap-2 rounded-control px-3 text-item ' +
									(tab === id
										? 'bg-default text-foreground'
										: 'text-foreground-muted hover:bg-surface-2 hover:text-foreground')
								}
								aria-pressed={tab === id}
								data-tab={id}
								onClick={() => choose(id)}
							>
								<Icon name={icon} size={16} />
								{name}
							</button>
						))}
					</div>
				</div>

				<div class="relative flex min-h-0 min-w-0 flex-1 flex-col md:gap-2">
					{/* Keep the desktop close button above the scrolling area:
					    the bar can reach neither the X nor the card's corners. */}
					<div class="hidden shrink-0 justify-end md:flex">
						<button
							class="grid h-8 w-8 place-items-center rounded-control bg-default text-foreground hover:bg-default-hover"
							onClick={onClose}
							title="fechar"
							aria-label="fechar"
						>
							<Icon name="xmark" />
						</button>
					</div>
					<div class="scroller relative min-h-0 flex-1 px-2 pb-2 pt-4 md:p-0 md:pr-4">
						{tab === 'av' && <VoiceAndVideo view={view} />}
						{tab === 'avisos' && <NotifySettings />}
						{tab === 'conta' && <Account me={me} />}
						{tab === 'cargos' && <Roles me={me} roles={roles} members={members} />}
						{tab === 'diag' && <Diagnostics />}
					</div>
				</div>
			</div>
		</div>
	);
}
