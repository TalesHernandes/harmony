import { start, BASE } from './server.mjs';
import { PASSWORD, readMail } from './fixtures';
import { readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

// signUp is the same invite-only signup the browser does, from here, so that
// one account can be made before any test runs.
async function signUp(code: string, username: string, displayName: string): Promise<void> {
	const res = await fetch(BASE + '/api/signup', {
		method: 'POST',
		headers: { 'content-type': 'application/json', origin: BASE },
		body: JSON.stringify({
			code,
			username,
			email: username + '@example.org',
			display_name: displayName,
			password: PASSWORD,
		}),
	});
	if (!res.ok) throw new Error(`signup ${username}: ${res.status} ${await res.text()}`);
	const raw = await readMail(username + '@example.org', 'confirmar seu');
	const codeReceived = raw.match(/\b\d{6}\b/)?.[0];
	const cookie = res.headers.get('set-cookie')?.split(';')[0];
	if (!codeReceived || !cookie) throw new Error('signup verification missing');
	const verified = await fetch(BASE + '/api/me/email/verify', {
		method: 'POST', headers: { 'content-type': 'application/json', origin: BASE, cookie },
		body: JSON.stringify({ code: codeReceived }),
	});
	if (!verified.ok) throw new Error('owner email verification failed');
}

// The invites are handed to the tests through a file rather than an
// environment variable, because they are minted here and read in another
// process.
// howManyAccounts counts what the suite is about to ask for instead of guessing
// at it.
//
// One invite per account, and the number was a constant that somebody had to
// remember to raise. On 17/09/2026 nobody did: the suite wanted 91 and had 89,
// so the **last** test of the run died of it — which looks like a broken test
// and is arithmetic. Counting the call sites is crude and always right in the
// direction that matters, because a spare invite costs nothing and a missing
// one costs a red run.
function howManyAccounts(): number {
	let n = 0;
	for (const f of readdirSync(__dirname).filter((f) => f.endsWith('.spec.ts'))) {
		n += (readFileSync(join(__dirname, f), 'utf8').match(/Person\.create\(/g) ?? []).length;
	}
	return n;
}

export default async function globalSetup() {
	// The owner, the accounts the suite creates, and room for a handful more —
	// enough that adding a test does not need this file touched at all, and a
	// `Person.create` inside a loop (which the count above cannot see) does not
	// bring the run down.
	const wanted = 1 + howManyAccounts() + 25;
	const { invites } = await start(wanted);
	// The first account on a fresh server is the owner (store/accounts.go), and
	// some things — creating a channel, for one — are only drawn for them.
	// Making it here is what turns "who is the owner" into something a test can
	// rely on instead of an accident of which file ran first.
	await signUp(invites[0], 'dono', 'Dono');
	const left = invites.slice(1);
	// Printed because running out of invites shows up as a signup page
	// answering 404 — correct behaviour, and an error that looks like anything
	// but "there were not enough invites".
	console.log(
		`harmony e2e: ${left.length} convites para ${howManyAccounts()} contas que os testes criam`,
	);
	writeFileSync(join(__dirname, 'invites.json'), JSON.stringify(left));
}
