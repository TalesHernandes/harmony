// Which build is on the screen, and whether the server is serving a newer one.
//
// The owner asked for this on 20/09/2026, and the complaint is worth quoting
// because it names the real failure: *"isso eh um problema no aplicativo do
// edge inclusive pois nunca sei qual a versao atual e se ta desatualizado e
// tenho que abrir e fechar"*.
//
// **It is not the service worker.** sw.js caches nothing (its fetch handler
// intercepts nothing, on purpose) and it already calls skipWaiting, so there
// is never a worker sitting in `waiting` for the usual PWA update banner to
// find. The staleness comes from somewhere simpler: an installed app loads
// index.html once, at launch, and then lives for days. The JavaScript is
// hashed and immutable, so it never expires — and nothing ever goes back to
// ask for a new index.html, which is the only file that knows which hash is
// current. Closing and reopening the app works because that is what fetches
// index.html again.
//
// So the version *is* the hashed filename, and the server is already telling
// anybody who asks. No /api/version endpoint and no build stamp compiled into
// the bundle: both would be a second thing to keep in step with the first, and
// the first cannot drift because the build writes it.

import { log } from './diag';

// The hashed names of everything the build wrote, which is the version.
//
// Both the script and the stylesheet, not just the script. A round that only
// touches style.css leaves the JavaScript byte-identical and its hash
// unchanged, and a check that only looks at the entry chunk would sit there
// saying everything is current through a deploy that changed the whole look
// of the app. That is not hypothetical: this sprint is nothing but style.
//
// Sorted and de-duplicated so the comparison is about the names and not about
// the order they happen to appear in, and read out of the live document's own
// markup so the running page and the fetched page are measured by the exact
// same rule.
const ASSETS = /\/static\/assets\/index-[A-Za-z0-9_-]+\.(?:js|css)/g;

function stamp(html: string): string {
	const found = html.match(ASSETS);
	if (!found) return '';
	return [...new Set(found)].sort().join(' ');
}

const RUNNING = stamp(document.documentElement.outerHTML);

// True only when the server answered *and* named a different build.
//
// Every other outcome is false, deliberately. A failed fetch, a redirect to
// something that is not the app, a page with nothing hashed in it: all of
// those mean "I could not tell", and "I could not tell" must never be shown
// as "there is a new version" — a badge that lies once is a badge nobody
// looks at again.
export async function newerBuild(): Promise<boolean> {
	if (!RUNNING) return false;
	try {
		const res = await fetch('/', { credentials: 'same-origin', cache: 'no-store' });
		if (!res.ok) return false;
		// /login and / are served the same index.html by the same handler, so
		// this works signed in or out.
		const served = stamp(await res.text());
		if (!served) return false;
		if (served === RUNNING) return false;
		log(`versão nova no servidor: ${served} (estou em ${RUNNING})`);
		return true;
	} catch {
		return false;
	}
}

// Ask the browser to re-check sw.js while we are at it.
//
// It is a separate question from the one above: the worker only changes when
// that file changes, which is rarely, and a deploy that only moves the bundle
// leaves it alone. Cheap, and it keeps the one part of the app that runs with
// the app closed from being the one part that is a month old.
async function refreshWorker(): Promise<void> {
	if (!('serviceWorker' in navigator)) return;
	try {
		const reg = await navigator.serviceWorker.getRegistration();
		await reg?.update();
	} catch {
		// An update check that fails changes nothing about the page.
	}
}

// Watch for a new build, and call back the first time there is one.
//
// The first version of this hung on `visibilitychange`, on the theory that an
// installed app becoming visible is the moment somebody looks at it. Two
// things were wrong with that. `visibilitychange` does **not** fire when a
// desktop window merely goes behind another one - it stays `visible`, which
// is how an installed app spends most of its day - and when I went to prove
// the feature worked, switching tabs in my own browser fired *nothing*:
// neither visibilitychange, nor focus, nor blur. A feature that only works
// when an event happens to fire is a feature you cannot promise.
//
// So the order is reversed. The trigger that cannot fail comes first:
//
//   - **whenever the socket comes back** (`check`, called from App.tsx). A
//     deploy restarts the server, so every open page loses its socket and
//     reconnects a second later. That is the earliest possible news that
//     there is a new version, and it is the same event that caused it;
//   - every five minutes, which needs nothing to fire and nobody to be
//     looking;
//   - once, twenty seconds after start, for a tab opened right after a
//     deploy;
//   - and focus / visibilitychange / pageshow, which cost nothing to listen
//     for and are pure profit on the browsers where they do fire.
//
// A floor of fifteen seconds between fetches keeps all five from turning a
// busy minute into a request a second. It starts armed, so the socket opening
// for the first time - which happens on every single load, against the very
// build that is running - does not spend a fetch saying nothing.
//
// It stops asking once it has found something: the answer cannot become
// "no" again, and the button is already on the screen.
export function watchForNewBuild(onFound: () => void): {
	stop: () => void;
	check: () => void;
} {
	let found = false;
	let asked = 0;
	// Declared before stop() closes over them: a const assigned further down
	// would be in its dead zone if stop() ever ran first.
	let timer = 0;
	let every = 0;

	const FLOOR = 10_000;

	const ask = async () => {
		if (found || document.visibilityState !== 'visible') return;
		const now = Date.now();
		if (now - asked < FLOOR) return;
		asked = now;
		void refreshWorker();
		if (!(await newerBuild())) return;
		found = true;
		stop();
		onFound();
	};

	const onWake = () => void ask();

	const stop = () => {
		clearTimeout(timer);
		clearInterval(every);
		document.removeEventListener('visibilitychange', onWake);
		window.removeEventListener('focus', onWake);
		window.removeEventListener('pageshow', onWake);
	};

	timer = window.setTimeout(onWake, 20_000);
	every = window.setInterval(onWake, 5 * 60_000);
	document.addEventListener('visibilitychange', onWake);
	window.addEventListener('focus', onWake);
	// Coming back from the back/forward cache, where the page was frozen whole
	// and neither of the other two fires.
	window.addEventListener('pageshow', onWake);
	return { stop, check: onWake };
}
