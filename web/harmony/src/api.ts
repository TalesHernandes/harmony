// Everything this client asks the server, in one place, so that the shapes the
// server answers with are written down once.
//
// Nothing here holds a token: the session is an HttpOnly cookie the browser
// attaches and this code cannot read, which is the point of it.

import { log } from './diag';

export interface User {
	email?: string;
	email_verified?: boolean;
	twofa?: '' | 'email' | 'totp';
	username: string;
	avatar?: string;
	display_name: string;
	is_admin?: boolean;
	created_at?: string;
	// What *you* call them. Present only when you have given them a name, and
	// never anybody else's (api/chat.go, store migration 7).
	nickname?: string;
}

export interface AdminAccount extends User {
	has_twofa: boolean;
	is_admin: boolean;
	created_at: string;
	disabled: boolean;
	disabled_at: string;
	invited_by: string;
	last_seen_at?: string;
}

// One answer from the search: a message, and where it was said.
export interface Hit extends Message {
	channel?: Channel;
}

// The biggest thing the server will take, mirrored from api/files.go
// (MaxUploadBytes). The server is still the one that decides — this copy only
// saves somebody thirty-two megabytes of waiting to be told no.
export const MAX_UPLOAD_BYTES = 32 * 1024 * 1024;

export interface Attachment {
	id: string;
	name: string;
	mime: string;
	size: number;
	width: number;
	height: number;
	url: string;
	// The server found no bytes behind the row (api/chat.go): the file was
	// lost, and the message says so instead of offering a dead address.
	missing?: boolean;
}

// One emoji on one message, and everybody who pressed it. There is no "mine"
// here and there cannot be: the same message is sent to the whole room at once,
// so the page works it out by looking for its own name in the list.
export interface Reaction {
	emoji: string;
	users: string[];
}

// The card under a link (api/preview.go): the server reads the page, so a
// friend's address never reaches the site, and the picture comes from here.
export interface Embed {
	url: string;
	kind: 'link' | 'video';
	site?: string;
	title?: string;
	description?: string;
	image?: string;
	width?: number;
	height?: number;
}

export interface Message {
	id: number;
	channel_id: string;
	author: { username: string; display_name: string; avatar?: string };
	body: string;
	created_at: string;
	edited_at?: string;
	deleted: boolean;
	deleted_at?: string;
	deleted_by?: string;
	attachments: Attachment[];
	// Absent until the server has read the links (a message_changed brings it).
	embeds?: Embed[];
	// What wrote this. Absent for everything a person typed; 'call' for the line
	// the server leaves when a call went unanswered (store migration 9).
	kind?: string;
	mentions: string[];
	reactions: Reaction[];
	// The line this message answers, as it is now. The server sends it along
	// rather than leaving the page to look it up: the message being answered is
	// very often older than the page is holding, and asking for it would be one
	// request per reply on every scroll-back.
	reply_to?: {
		id: number;
		author: { username: string; display_name: string; avatar?: string };
		body: string;
		deleted: boolean;
	};
	// Fixed to its channel (D12a). Absent when it is not.
	pinned?: boolean;
}

// A call, as the server hands it over. ice_servers is the address book the two
// browsers use to find each other, minted fresh for this call and never
// reused; polite says which of the two gives way when both speak at once
// (docs/PROTOCOL.md §5), and the server decides it so the browsers never guess.
export interface CallTicket {
	id: string;
	from: { username: string; display_name: string; avatar?: string };
	to: { username: string; display_name: string; avatar?: string };
	ice_servers: RTCIceServer[];
	polite: boolean;
}

export interface Channel {
	id: string;
	kind: 'text' | 'dm' | 'voice';
	name: string;
	topic: string;
	// This person's own silence on this channel: it never notifies them, and
	// the server reports no unread for it at all.
	muted: boolean;
	unread: number;
	mentions: number;
	last_message_id: number;
	last_read_id: number;
	peer?: { username: string; display_name: string; avatar?: string };
	// Where a room sits (D12a): the folder it is in, '' for the top of the
	// column, and its place among the rooms of its own kind there. Never on a
	// conversation.
	category_id?: string;
	position?: number;
}

// Category is one folder of the channel column. Everybody sees the same ones;
// whether it is open or closed is this device's own business (App.tsx).
export interface Category {
	id: string;
	name: string;
	position: number;
}

// Layout is the whole shape of the column, as the server sends it on every
// change: the folders in order, and where each room sits.
export interface Layout {
	categories: Category[];
	channels: Array<{ id: string; category_id: string; position: number }>;
}

// VoicePeer is one person inside a voice channel, as everybody sees them. Note
// what is not here: nothing about the connection itself. Who is in the room is
// public to the room; how their connection is put together is not.
export interface VoicePeer {
	avatar?: string;
	session_id: string;
	username: string;
	display_name: string;
	muted: boolean;
	deafened: boolean;
	joined_at: number;
	// What this person is sending right now: "cam", "screen", "screen_audio".
	// It comes from what is arriving at the server, not from what anybody said
	// they would send, so it is safe to draw as fact.
	publishing: string[];
}

// VoiceTrack is one thing arriving on the connection. source is what makes a
// wall of squares readable: "screen" is drawn large, "cam" small, "mic" is a
// voice and "screen_audio" is the sound of somebody's film — which gets a
// volume of its own, separate from the voice of the person sharing it.
export interface VoiceTrack {
	avatar?: string;
	mid: string;
	username: string;
	display_name: string;
	kind: 'audio' | 'video';
	source: string;
}

// VoiceOffer is what the server asks this device to answer. send_mids says
// which slot to put each thing this device sends into, by source; tracks says
// what arrives on which slot, which is how a volume slider knows whose volume
// it is changing and how a picture knows whether it is a face or a screen.
export interface VoiceOffer {
	ice_restart?: boolean;
	session_id: string;
	sdp: string;
	type: string;
	send_mids: Record<string, string>;
	tracks: VoiceTrack[];
	// The ways out of this network, minted for this person and good for an
	// hour. Empty when the server has no relay configured, which is a working
	// setup and not an error: a host candidate reaches the media server on its
	// own. This list is what is left when the browser refuses to offer one.
	ice_servers: RTCIceServer[];
}

export interface Session {
	id: string;
	created_at: string;
	last_seen_at: string;
	expires_at: string;
	user_agent: string;
	current: boolean;
}

export interface Invite {
	id: string;
	note: string;
	created_at: string;
	expires_at: string;
	used: boolean;
	created_by: string;
	used_by: string;
	code?: string;
	url?: string;
}

// PushStatus is the honest answer to "as notificações estão ligadas?": what the
// server holds, not what this browser believes. The two disagreeing is the
// failure this whole feature is prone to.
export interface PushStatus {
	enabled: boolean;
	subscribed: boolean;
	devices: number;
}

// ApiError carries the server's short machine-readable reason, which is what
// the screens turn into a sentence in Portuguese.
// A role (D12c). position 0 is the highest; color is "" or one of the
// palette's hex values (ranks.ts); members are usernames. The list always
// comes whole, highest first — from GET /api/roles and the `roles` event.
export type Permission = 'manage_channels' | 'manage_messages' | 'manage_roles';

export interface Role {
	id: string;
	name: string;
	color: string;
	position: number;
	hoist: boolean;
	permissions: Permission[];
	members: string[];
}

export interface RoleList {
	roles: Role[];
	// The asker's own: what they may do, and the position of their highest
	// role (-1 for the owner, null for nobody). Only GET answers these.
	my_permissions?: Permission[];
	my_top?: number | null;
	colors?: string[];
}

export class ApiError extends Error {
	constructor(readonly status: number, readonly code: string) {
		super(code);
	}
}

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
	const init: RequestInit = { method, credentials: 'same-origin' };
	if (body !== undefined) {
		init.headers = { 'Content-Type': body instanceof Blob ? 'application/octet-stream' : 'application/json' };
		init.body = body instanceof Blob ? body : JSON.stringify(body);
	}
	// What goes into the diagnostic log is the route, never the query string:
	// a search puts the words being looked for after the "?", and those are a
	// piece of somebody's message — the log is copied and shared, and promises
	// never to carry what anybody wrote.
	const route = path.split('?')[0].replace(/^(\/api\/me\/(?:blocks|mutes))\/[^/]+$/, '$1/:person');
	let res: Response;
	try {
		res = await fetch(path, init);
	} catch (err) {
		log(`net ${method} ${route} falhou`);
		throw new ApiError(0, 'offline');
	}
	if (res.status === 204) return undefined as T;
	const text = await res.text();
	const data = text ? JSON.parse(text) : {};
	if (!res.ok) {
		log(`api ${method} ${route} ${res.status} ${data.error ?? ''}`);
		throw new ApiError(res.status, data.error ?? 'erro');
	}
	return data as T;
}

export interface AppRelease {
	version_code: number;
	version_name: string;
	size: number;
	sha256: string;
	url: string;
}

export type AndroidApp = AppRelease;

export interface PersonPreference { username: string; muted: boolean; blocked: boolean }
export const api = {
 personPreferences: () => request<{items: PersonPreference[]}>('GET','/api/me/blocks'),
 personPreference: (username: string, block: boolean, enabled: boolean) => request<{items: PersonPreference[]}>(enabled ? 'PUT' : 'DELETE', '/api/me/' + (block ? 'blocks/' : 'mutes/') + encodeURIComponent(username)),
	putAvatar: (file: File) => request<{ avatar: string }>('PUT', '/api/me/avatar', file),
	deleteAvatar: () => request<void>('DELETE', '/api/me/avatar'),
	me: () => request<{ user: User; session_id: string; push_enabled: boolean; email_required: boolean }>('GET', '/api/me'),
	login: (username: string, password: string) =>
		request<{ user?: User; twofa?: 'email' | 'totp'; challenge?: string }>('POST', '/api/login', { username, password }),
	signup: (code: string, username: string, display_name: string, password: string, email: string) =>
		request<{ user: User }>('POST', '/api/signup', { code, username, display_name, password, email }),
	login2FA: (challenge: string, code: string) => request<{ user: User }>('POST', '/api/login/2fa', { challenge, code }),
	forgotPassword: (identifier: string) => request<void>('POST', '/api/password/forgot', { identifier }),
	resetPassword: (token: string, password: string, code?: string) => request<void>('POST', '/api/password/reset', { token, password, code }),
	changeEmail: (password: string, email: string) => request<void>('POST', '/api/me/email', { password, email }),
	verifyEmail: (code: string) => request<{ user: User }>('POST', '/api/me/email/verify', { code }),
	resendEmail: () => request<void>('POST', '/api/me/email/resend', {}),
	changePassword: (current: string, next: string) => request<void>('POST', '/api/me/password', { current, new: next }),
	factor: (action: string, password: string, code?: string) => request<{ user?: User; secret?: string; uri?: string; recovery_codes?: string[]; code_required?: boolean }>('POST', '/api/me/2fa/' + action, { password, code }),
	logout: () => request<unknown>('POST', '/api/logout'),

	members: () => request<{ members: User[] }>('GET', '/api/members'),
	// "Where did I see that?" Every message this person can read that contains
	// the words, newest first, each one carrying the conversation it was said in
	// so the answer can be read without opening anything.
	search: (q: string) =>
		request<{ messages: Hit[] }>('GET', '/api/search?q=' + encodeURIComponent(q)),
	channels: () =>
		request<{ channels: Channel[]; categories?: Category[]; can_manage: boolean }>('GET', '/api/channels'),
	// The owner's, all five (D12a). The order goes whole: every folder and
	// every room, in the order the column should draw them.
	renameChannel: (channelID: string, name: string) =>
		request<Channel>('PATCH', `/api/channels/${encodeURIComponent(channelID)}`, { name }),
	createCategory: (name: string) => request<Category>('POST', '/api/categories', { name }),
	renameCategory: (id: string, name: string) =>
		request<unknown>('PATCH', `/api/categories/${encodeURIComponent(id)}`, { name }),
	removeCategory: (id: string) => request<unknown>('DELETE', `/api/categories/${encodeURIComponent(id)}`),
	saveLayout: (categories: string[], channels: Array<{ id: string; category_id: string }>) =>
		request<Layout>('PUT', '/api/layout', { categories, channels }),
	// Pinned messages. In a channel only the owner pins; in a conversation of
	// two, either person. What comes back is the whole message, like a reaction.
	pin: (id: number, on: boolean) => request<Message>(on ? 'POST' : 'DELETE', `/api/messages/${id}/pin`),
	pins: (channelID: string) =>
		request<{ messages: Message[]; limit: number }>('GET', `/api/channels/${encodeURIComponent(channelID)}/pins`),
	// Roles (D12c). The server checks every rule: Manage roles touches only
	// roles below the asker's highest one and never gives what it lacks.
	roles: () => request<RoleList>('GET', '/api/roles'),
	createRole: (role: { name: string; color: string; hoist: boolean; permissions: Permission[] }) =>
		request<Role>('POST', '/api/roles', role),
	updateRole: (id: string, patch: Partial<{ name: string; color: string; hoist: boolean; permissions: Permission[] }>) =>
		request<unknown>('PATCH', `/api/roles/${encodeURIComponent(id)}`, patch),
	removeRole: (id: string) => request<unknown>('DELETE', `/api/roles/${encodeURIComponent(id)}`),
	orderRoles: (ids: string[]) => request<RoleList>('PUT', '/api/roles/order', { roles: ids }),
	roleMember: (id: string, username: string, on: boolean) =>
		request<unknown>(on ? 'PUT' : 'DELETE', `/api/roles/${encodeURIComponent(id)}/members/${encodeURIComponent(username)}`),
	createChannel: (name: string, topic: string, kind: 'text' | 'voice' = 'text') =>
		request<Channel>('POST', '/api/channels', { name, topic, kind }),
	openDM: (username: string) => request<Channel>('POST', '/api/dms', { username }),

	history: (channelID: string, before?: number, window?: { around?: number; after?: number }) =>
		request<{ messages: Message[]; has_more: boolean; has_newer?: boolean }>(
			'GET',
			`/api/channels/${encodeURIComponent(channelID)}/messages` +
				(window?.around ? `?around=${window.around}` : window?.after ? `?after=${window.after}` : before ? `?before=${before}` : ''),
		),
	send: (channelID: string, body: string, attachments: string[], replyTo = 0) =>
		request<Message>('POST', `/api/channels/${encodeURIComponent(channelID)}/messages`, {
			body,
			attachments,
			reply_to: replyTo,
		}),
	edit: (id: number, body: string) => request<Message>('PATCH', `/api/messages/${id}`, { body }),
	// One emoji on or off. `on` says which way it should end up rather than
	// "toggle", so that two devices of the same person pressing at the same
	// moment agree instead of taking turns undoing each other. What comes back
	// is the whole message, the same as an edit.
	react: (id: number, emoji: string, on: boolean) =>
		request<Message>('POST', `/api/messages/${id}/reactions`, { emoji, on }),
	remove: (id: number) => request<Message>('DELETE', `/api/messages/${id}`),
	// "I am writing something here." Nothing comes back and nothing is stored;
	// see api/chat.go for why it is a POST and not something on the socket.
	// The name you call somebody. Only you are ever shown it — that is the shape
	// of the table it lives in, not a rule this page keeps. An empty name
	// clears it.
	nickname: (username: string, name: string) =>
		request<{ name: string }>(
			'PUT',
			`/api/people/${encodeURIComponent(username)}/nickname`,
			{ name },
		),
	typing: (channelID: string) =>
		request<unknown>('POST', `/api/channels/${encodeURIComponent(channelID)}/typing`, {}),
	mute: (channelID: string, muted: boolean) =>
		request<{ muted: boolean }>('POST', `/api/channels/${encodeURIComponent(channelID)}/mute`, {
			muted,
		}),
	// Closing a conversation. Nothing is deleted and the other person keeps
	// theirs; it comes back when there is something new in it, or when this
	// person asks for it again.
	hide: (channelID: string) =>
		request<{ hidden: boolean }>('POST', `/api/channels/${encodeURIComponent(channelID)}/hide`, {}),
	// Deleting a channel or a room, for everybody - the owner's alone. The
	// server keeps the messages (store.DeleteRoom); the channel is gone from
	// every column and nobody can read it.
	removeChannel: (channelID: string) =>
		request<unknown>('DELETE', `/api/channels/${encodeURIComponent(channelID)}`),
	markRead: (channelID: string, lastReadID: number) =>
		request<unknown>('POST', `/api/channels/${encodeURIComponent(channelID)}/read`, {
			last_read_id: lastReadID,
		}),

	// The direct 1:1 call. Nothing here carries audio or picture: those go
	// browser to browser (direct.ts). What these five do is the introduction,
	// and the signalling ones hand the server bytes it does not read.
	callStart: (username: string) => request<CallTicket>('POST', '/api/calls', { username }),
	// "Am I still being called?" — the app opened from a notification asks this,
	// because the live event that told every other screen was sent before this
	// one was running.
	callGet: (id: string) =>
		request<CallTicket & { answered: boolean }>('GET', `/api/calls/${encodeURIComponent(id)}`),
	callAccept: (id: string) =>
		request<CallTicket>('POST', `/api/calls/${encodeURIComponent(id)}/accept`),
	callHangup: (id: string, reason: string) =>
		request<unknown>('POST', `/api/calls/${encodeURIComponent(id)}/hangup`, { reason }),
	callSignal: (id: string, kind: string, payload: unknown) =>
		request<unknown>('POST', `/api/calls/${encodeURIComponent(id)}/signal`, { kind, payload }),
	callState: (id: string, mic: boolean, cam: boolean, screen: boolean) =>
		request<unknown>('POST', `/api/calls/${encodeURIComponent(id)}/state`, { mic, cam, screen }),

	// Voice. Everything a person does here is an ordinary request, exactly like
	// sending a message: the live socket only ever delivers the server's side
	// of the negotiation, and so has no authority of its own.
	voiceRooms: () => request<{ rooms: Record<string, VoicePeer[]> }>('GET', '/api/voice/rooms'),
	voiceJoin: (channelID: string, publish: string[]) =>
		request<VoiceOffer>('POST', `/api/voice/${encodeURIComponent(channelID)}/join`, { publish }),
	voiceAnswer: (session_id: string, sdp: string) =>
		request<unknown>('POST', '/api/voice/answer', { session_id, sdp }),
	voiceRestart: (session_id: string) =>
		request<VoiceOffer>('POST', '/api/voice/restart', { session_id }),
	voiceCandidate: (
		session_id: string,
		c: { candidate: string; sdp_mid: string | null; sdp_mline_index: number | null },
	) => request<unknown>('POST', '/api/voice/candidate', { session_id, ...c }),
	// Turning a camera or a screen on and off happens on the connection that is
	// already up: the server opens a slot and offers again, and the offer comes
	// down the live socket like every other renegotiation. Nothing here waits
	// for a picture — this only asks for the space to put one in.
	voicePublish: (session_id: string, sources: string[]) =>
		request<unknown>('POST', '/api/voice/publish', { session_id, sources }),
	voiceUnpublish: (session_id: string, sources: string[]) =>
		request<unknown>('POST', '/api/voice/unpublish', { session_id, sources }),
	// A tela e a unica excecao a "quem oferta e o servidor" (D4r). Ela sobe em
	// tres camadas, e a biblioteca do servidor sabe RECEBER tres camadas mas
	// nao sabe PEDIR tres — isso foi medido, nao lido. Entao aqui o navegador
	// oferta, e o servidor responde.
	voiceOffer: (session_id: string, sources: string[], sdp: string) =>
		request<{ type: string; sdp: string }>('POST', '/api/voice/offer', {
			session_id,
			sources,
			sdp,
		}),
	voiceLeave: (session_id: string) => request<unknown>('POST', '/api/voice/leave', { session_id }),
	voiceState: (session_id: string, muted: boolean, deafened: boolean) =>
		request<unknown>('POST', '/api/voice/state', { session_id, muted, deafened }),
	// Being in the room without receiving somebody's screen. It is a real
	// refusal on the server, not a hidden picture: the megabits stop.
	voiceWatch: (session_id: string, username: string, watching: boolean) =>
		request<unknown>('POST', '/api/voice/watch', { session_id, username, watching }),
	// De que tamanho a tela de alguém precisa ser NESTE aparelho. O servidor
	// não tem como saber: quem sabe é quem está desenhando. Vazio quer dizer
	// "do tamanho que tiver".
	voiceLayer: (session_id: string, username: string, layer: string) =>
		request<unknown>('POST', '/api/voice/layer', { session_id, username, layer }),

	// Presence and the doorbell (D4). "em chamada" is not asked for here: the
	// client already knows who is inside the voice rooms it belongs to, so it
	// draws that itself and the server never answers a question about a room
	// somebody may not see.
	presence: () => request<{ presence: Record<string, 'online' | 'away'> }>('GET', '/api/presence'),
	ring: (channelID: string, username: string) =>
		request<{ ringing: boolean }>('POST', `/api/voice/${encodeURIComponent(channelID)}/ring`, {
			username,
		}),

	pushKey: () => request<{ enabled: boolean; key?: string }>('GET', '/api/push/key'),
	pushStatus: (endpoint: string) =>
		request<PushStatus>('GET', '/api/push/status?endpoint=' + encodeURIComponent(endpoint)),
	pushSubscribe: (endpoint: string, keys: { p256dh: string; auth: string }) =>
		request<PushStatus>('POST', '/api/push/subscribe', { endpoint, keys }),
	pushUnsubscribe: (endpoint: string) =>
		request<PushStatus>('POST', '/api/push/unsubscribe', { endpoint }),

	sessions: () => request<{ sessions: Session[] }>('GET', '/api/sessions'),
	// The newest Android APK the server has (PROTOCOL-HARMONY §8); 404 no_app when none.
	androidApp: () => request<AppRelease>('GET', '/api/app/android'),
	windowsApp: () => request<AppRelease>('GET', '/api/app/windows'),
	revokeSession: (id: string) => request<unknown>('DELETE', `/api/sessions/${encodeURIComponent(id)}`),
	revokeOthers: () => request<{ revoked: number }>('POST', '/api/sessions/revoke-others'),

	accounts: () => request<{ accounts: AdminAccount[] }>('GET', '/api/admin/accounts'),
	disableAccount: (username: string) => request<{ ok: boolean }>('POST', `/api/admin/accounts/${encodeURIComponent(username)}/disable`),
	enableAccount: (username: string) => request<{ ok: boolean }>('POST', `/api/admin/accounts/${encodeURIComponent(username)}/enable`),
	resetAccountPassword: (username: string) => request<{ password: string }>('POST', `/api/admin/accounts/${encodeURIComponent(username)}/reset-password`),

	invites: () => request<{ invites: Invite[]; can_invite: boolean }>('GET', '/api/invites'),
	createInvite: (note: string) => request<Invite>('POST', '/api/invites', { note }),
	revokeInvite: (id: string) => request<unknown>('DELETE', `/api/invites/${encodeURIComponent(id)}`),

	// The upload is its own shape twice over. It is the one request that is not
	// JSON, and it is the one whose progress is worth watching — and fetch()
	// cannot report that: it hands over a body and goes quiet until the answer
	// comes back, which on a phone sending a video is a minute of a screen that
	// looks broken. XMLHttpRequest is the older interface and the only one that
	// says how much of a body has left the machine, so this one call is written
	// with it.
	//
	// The session rides along on its own: this is same-origin and the browser
	// attaches the cookie, which is why nothing here can read it.
	//
	// The server decides the type from the bytes, so what is sent here as a
	// name is only a label.
	upload(
		channelID: string,
		file: File,
		// Called as the bytes go. It is only ever the *sending*: sent === total
		// means this machine is done, not that the server is.
		onProgress?: (sent: number, total: number) => void,
		// Aborting rejects with 'canceled', which is the one failure that is not
		// a failure — somebody changed their mind, or changed channel — and so
		// the one the screens do not report.
		signal?: AbortSignal,
	): Promise<Attachment> {
		return new Promise<Attachment>((resolve, reject) => {
			if (signal?.aborted) {
				reject(new ApiError(0, 'canceled'));
				return;
			}
			const form = new FormData();
			form.append('file', file, file.name);

			const xhr = new XMLHttpRequest();
			xhr.open('POST', `/api/channels/${encodeURIComponent(channelID)}/uploads`);
			xhr.upload.onprogress = (e: ProgressEvent) => {
				if (e.lengthComputable) onProgress?.(e.loaded, e.total);
			};

			const stop = () => xhr.abort();
			signal?.addEventListener('abort', stop);
			const done = () => signal?.removeEventListener('abort', stop);

			xhr.onload = () => {
				done();
				let data: Record<string, unknown> = {};
				try {
					data = xhr.responseText ? JSON.parse(xhr.responseText) : {};
				} catch {
					data = {};
				}
				if (xhr.status < 200 || xhr.status >= 300) {
					log(`api upload ${xhr.status} ${data.error ?? ''}`);
					reject(new ApiError(xhr.status, String(data.error ?? 'erro')));
					return;
				}
				log(`upload ok ${data.size} bytes`);
				resolve(data as unknown as Attachment);
			};
			xhr.onerror = () => {
				done();
				log('net POST uploads falhou');
				reject(new ApiError(0, 'offline'));
			};
			xhr.onabort = () => {
				done();
				reject(new ApiError(0, 'canceled'));
			};

			xhr.send(form);
		});
	},
};

// message turns a server reason into something the owner's friends can read.
export function message(err: unknown): string {
	const code = err instanceof ApiError ? err.code : 'erro';
	switch (code) {
		case 'mail_unavailable': return 'O envio de e-mail está indisponível. Procure o dono do servidor.';
		case 'bad_email': return 'Digite um e-mail válido.';
		case 'email_unavailable': return 'Não foi possível usar esse e-mail. Confira o endereço ou procure o dono.';
		case 'signup_unavailable': return 'Não foi possível usar esses dados. Confira o nome e o e-mail.';
		case 'invalid_code': return 'Código ou link inválido, vencido ou já usado.';
		case 'email_required': return 'Confirme seu e-mail para continuar.';
		case 'totp_required': return 'Digite o código do app ou um código de recuperação.';
		case 'cannot_disable_owner':
			return 'A conta do dono não pode ser desativada.';
		case 'use_own_settings':
			return 'A sua própria senha não se troca por aqui.';
		case 'avatar_too_large':
			return 'A foto é grande demais (o limite é 5 MiB).';
		case 'avatar_dimensions':
			return 'A foto excede 8000 pixels de largura ou altura, ou 40 megapixels.';
		case 'avatar_type':
			return 'Escolha uma foto JPEG, PNG ou GIF. WebP não é aceito.';
		case 'bad_image':
			return 'Não foi possível ler essa foto. Escolha outra imagem.';
		case 'avatar_limit':
			return 'Você já mudou a foto dez vezes na última hora. Espere um pouco.';
		case 'offline':
			return 'Sem conexão com o servidor.';
		case 'bad_credentials':
			return 'Usuário ou senha não conferem.';
		case 'rate_limited':
			return 'Tentativas demais. Espere quinze minutos.';
		case 'message_limit':
			return 'Você mandou mensagens rápido demais. Espere um minuto.';
		case 'upload_limit':
			return 'Limite de arquivos por dia atingido.';
		case 'upload_bytes_limit':
			return 'Você já mandou muita coisa hoje. Amanhã dá de novo.';
		case 'too_large':
			return 'O arquivo é grande demais (o limite é 32 MB).';
		case 'bad_file_type':
			return 'Esse tipo de arquivo não passa: imagem, PDF, texto, som, vídeo ou zip.';
		case 'invite_unusable':
			return 'Esse convite não vale mais.';
		case 'username_taken':
			return 'Esse nome de usuário já existe.';
		case 'bad_username':
			return 'Nome de usuário inválido: use de 3 a 32 letras, números, ponto, traço ou _.';
		case 'weak_password':
			return 'A senha precisa ter pelo menos 10 caracteres.';
		case 'delivery_failed': return 'Não foi possível entregar.';
		case 'bad_message':
			return 'Mensagem vazia ou comprida demais.';
		case 'bad_reaction':
			return 'Só dá para reagir com emoji, e com no máximo seis por mensagem.';
		case 'bad_channel_name':
			return 'Nome de canal inválido: letras minúsculas, números e traço.';
		case 'name_taken':
			return 'Já existe um com esse nome.';
		case 'bad_category_name':
			return 'Dê um nome à categoria, com até 32 caracteres.';
		case 'stale_layout':
			return 'A lista mudou enquanto você arrastava. Ela foi recarregada; tente de novo.';
		case 'pin_limit':
			return 'Este canal já tem 250 mensagens fixadas. Desafixe uma para fixar outra.';
		case 'last_channel':
			return 'Esse é o último canal de texto: o app precisa de um lugar para abrir.';
		case 'room_busy':
			return 'Tem gente na sala. Espere ela esvaziar para apagar.';
		case 'not_allowed':
			return 'Você não tem permissão para isso.';
		case 'bad_role_name':
			return 'Dê um nome ao cargo, com até 32 caracteres.';
		case 'bad_color':
			return 'Escolha uma das cores da lista.';
		case 'role_limit':
			return 'O servidor já tem 50 cargos. Apague um para criar outro.';
		case 'stale_roles':
			return 'A lista de cargos mudou enquanto você arrastava. Ela foi recarregada; tente de novo.';
		case 'not_found':
			return 'Não encontrado.';
		case 'room_full':
			return 'A sala de voz está cheia.';
		case 'busy':
			return 'Essa pessoa já está em outra chamada.';
		case 'ring_limit':
			return 'Espere um pouco antes de ligar de novo.';
		case 'taken':
			return 'Atendida em outro aparelho.';
		case 'not_answered':
			return 'A chamada ainda não foi atendida.';
		case 'voice_unavailable':
			return 'A voz está fora do ar neste servidor.';
		case 'reconnect_needed':
			return 'Saia da sala e entre de novo para compartilhar outra vez.';
		case 'already_here':
			return 'Essa pessoa já está na sala.';
		case 'not_in_room':
			return 'Entre na sala antes de chamar alguém para ela.';
		case 'push_disabled':
			return 'Este servidor está sem notificação configurada.';
		case 'unauthenticated':
			return 'Sua sessão terminou. Entre de novo.';
		default:
			return 'Deu algo errado. Tente de novo.';
	}
}
