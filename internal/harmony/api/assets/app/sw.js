// harmony's service worker: the only part of this app that runs with the app
// closed.
//
// It is served from the root (`/sw.js`, a route in internal/harmony/api/
// pages.go) because a service worker's scope is its own directory, and this one
// has to cover the whole site. It is a plain file in `public/`, copied as-is by
// the build: nothing here is compiled, so what runs on the phone is what is
// written here.
//
// The push that wakes it carries **no payload**. The reasons are in
// internal/harmony/api/notify.go; the consequence here is that the first thing
// this file does when woken is ask the server what to say, over the session
// cookie the browser attaches to a same-origin request — even though there is
// no page anywhere.

const ICON = '/icon-192.png';

// A new worker takes over at once. The classic PWA failure is a new deploy
// sitting in `waiting` until every tab is closed, which for an app somebody
// keeps pinned means "still running last week's code".
self.addEventListener('install', (event) => {
	event.waitUntil(self.skipWaiting());
});

self.addEventListener('activate', (event) => {
	event.waitUntil(self.clients.claim());
});

// A fetch handler that intercepts nothing.
//
// Returning without calling respondWith() hands the request back to the browser
// untouched. It is here for one reason: a service worker with no fetch handler
// at all is not, to some browsers, an installable app. Caching the app shell
// would be the other way to earn it, and it is deliberately not done — harmony
// is useless without the server anyway, and a stale cached shell is a whole
// class of "por que não atualizou?" that this project does not need.
self.addEventListener('fetch', () => {});

// ---------------------------------------------------------------- the push

self.addEventListener('push', (event) => {
	event.waitUntil(announce());
});

async function announce() {
	let items = [];
	// Whether the server could be reached at all. It is a different fact from
	// "there was nothing to say", and since the D4i it is a message in its own
	// right: the watchman on the other machine (cmd/harmony/watch.go) wakes this
	// phone precisely when harmony has stopped answering, and the proof of that
	// is this fetch failing here, on the owner's own telephone, rather than a
	// machine in another country claiming it.
	let reached = true;
	try {
		const res = await fetch('/api/notifications/pending', {
			credentials: 'same-origin',
			cache: 'no-store',
		});
		if (res.ok) items = (await res.json()).items || [];
	} catch {
		reached = false;
	}

	if (!reached) {
		await self.registration.showNotification('harmony', {
			body: 'Não consegui falar com o servidor.',
			tag: 'harmony-offline',
			icon: ICON,
			badge: ICON,
			data: { url: '/' },
		});
		return;
	}

	// Chrome requires userVisibleOnly, so a push that shows nothing is a broken
	// promise to the browser, not a quiet one — it eventually takes the
	// subscription away. Something is therefore always shown, even when the
	// server could not be reached or had already forgotten what this was about.
	if (items.length === 0) {
		await self.registration.showNotification('harmony', {
			body: 'Tem algo novo para você.',
			tag: 'harmony',
			icon: ICON,
			badge: ICON,
			data: { url: '/' },
		});
		return;
	}

	// Newest three. More than that on a lock screen is a wall nobody reads, and
	// the app itself is one tap away with the rest.
	for (const item of items.slice(-3)) {
		const ring = item.kind === 'ring';
		await self.registration.showNotification(item.title, {
			body: item.body,
			// Same tag replaces instead of stacking: one line per conversation,
			// kept up to date, rather than a pile.
			tag: item.tag,
			renotify: true,
			icon: ICON,
			badge: ICON,
			// A call is worth insisting on and worth buzzing for; a message is
			// not woken up for twice.
			requireInteraction: ring,
			vibrate: ring ? [400, 200, 400, 200, 400] : [200],
			data: { url: item.url || '/' },
			actions: ring ? [{ action: 'join', title: 'Entrar' }] : [],
		});
	}
}

// A tap on the body and a tap on "Entrar" both land here and both mean the same
// thing: take me there.
self.addEventListener('notificationclick', (event) => {
	event.notification.close();
	const url = (event.notification.data && event.notification.data.url) || '/';
	event.waitUntil(
		(async () => {
			const windows = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
			for (const client of windows) {
				if (new URL(client.url).origin !== self.location.origin) continue;
				// A tab is already open: the phone was not as closed as the
				// notification suggested. Tell it where to go instead of opening
				// a second copy of the app, which for a voice room would take
				// the other slot and be thrown out of it.
				client.postMessage({ t: 'open', url });
				if ('focus' in client) return client.focus();
				return;
			}
			await self.clients.openWindow(url);
		})(),
	);
});

// The push service can retire an endpoint on its own. This fires with no page
// around — but a same-origin fetch from here still carries the session cookie,
// so it is an ordinary authenticated request like any other. If the session has
// ended as well, nothing can be done from here and the person turns
// notifications on again in Ajustes, which is what that screen is for.
self.addEventListener('pushsubscriptionchange', (event) => {
	event.waitUntil(
		(async () => {
			const old = event.oldSubscription;
			let fresh = event.newSubscription;
			if (!fresh) {
				const res = await fetch('/api/push/key', { credentials: 'same-origin' });
				if (!res.ok) return;
				const { enabled, key } = await res.json();
				if (!enabled || !key) return;
				fresh = await self.registration.pushManager.subscribe({
					userVisibleOnly: true,
					applicationServerKey: decodeKey(key),
				});
			}
			if (!fresh) return;
			await fetch('/api/push/subscribe', {
				method: 'POST',
				credentials: 'same-origin',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({
					endpoint: fresh.endpoint,
					keys: fresh.toJSON().keys || {},
					replaces: old ? old.endpoint : '',
				}),
			});
		})(),
	);
});

// base64url to the Uint8Array pushManager.subscribe() wants.
function decodeKey(key) {
	const padded = key.replace(/-/g, '+').replace(/_/g, '/');
	const raw = atob(padded + '='.repeat((4 - (padded.length % 4)) % 4));
	const bytes = new Uint8Array(raw.length);
	for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
	return bytes;
}
