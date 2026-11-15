// A throwaway harmony, for the browser tests to talk to.
//
// It is the real binary, built from this tree, on a database in a temporary
// directory that is thrown away afterwards. Nothing here ever touches the
// machine in Contabo or the database that has real people in it.
//
// The invites are minted before the server starts, by the same binary, because
// this service has no other way in — that is the point of it (PLANO-DISCORD.md
// §5), and a test that could sign up without one would be testing something
// this service does not do.

import { execFileSync, spawn } from 'node:child_process';
import { mkdtempSync, rmSync, mkdirSync, writeFileSync, appendFileSync, readFileSync, openSync, closeSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

// HARMONY_E2E_PORT moves the whole run elsewhere. The Android harness keeps a
// harmony on 8097 with the phone attached to it through adb reverse, and a
// browser test must not have to take that down to run (25/09/2026).
export const PORT = Number(process.env.HARMONY_E2E_PORT || 8097);
export const BASE = `http://127.0.0.1:${PORT}`;

let proc = null;
let dir = null;

// What the server itself said. A browser test that cannot read the server's log
// is a test that can only guess.
export const serverLog = [];
export function logPath() {
	return join(tmpdir(), 'harmony-e2e-server.log');
}

// The test binary is built to the same path every time, on purpose.
//
// Windows asks whether to allow a program through the firewall the first time
// it sees one, and it remembers the answer **by path**. Building into the throw
// away directory meant a brand new path on every run, which meant that dialog
// on every run — the owner's words were "não aguento mais". The database and
// the files still live in a fresh directory that is deleted afterwards; only
// the executable stays put.
function binary() {
	const home = join(tmpdir(), 'harmony-e2e-bin');
	mkdirSync(home, { recursive: true });
	const out = join(home, process.platform === 'win32' ? 'harmony.exe' : 'harmony');
	execFileSync('go', ['build', '-o', out, './cmd/harmony'], { stdio: 'inherit' });
	return out;
}

// invite mints one single-use code and returns it, by reading the same line the
// owner reads in the terminal.
// One process for the lot, not one per invite.
//
// It used to be a process each, and at ninety invites that was eighteen seconds
// added to the front of every run — long enough that nobody wanted to raise the
// number, which is how the suite ended up running out of them (D4l).
function mintInvites(bin, db, howMany) {
	const out = execFileSync(bin, ['-invite', '-n', String(howMany), '-note', 'e2e', '-db', db], {
		encoding: 'utf8',
		env: { ...process.env, HARMONY_BASE_URL: BASE },
	});
	const codes = out
		.split('\n')
		.filter((l) => l.startsWith('invite'))
		.map((l) => l.split(/\s+/)[1]);
	if (codes.length !== howMany) {
		throw new Error(`asked for ${howMany} invites and got ${codes.length}: ` + out);
	}
	return codes;
}

// vapid mints the Web Push identity the same way the owner does on the machine,
// by asking the binary. The browser tests never actually receive a push — that
// needs Google's push service and a real phone — but a server *with* keys is
// the one the owner runs, and it is the only way the settings screen can be
// checked in the state he will see it in.
function vapid(bin) {
	const out = execFileSync(bin, ['-vapid-keygen'], { encoding: 'utf8' });
	const env = {};
	for (const line of out.split(/\r?\n/)) {
		const [key, value] = line.trim().split('=');
		if (key && key.startsWith('HARMONY_VAPID') && value) env[key] = value;
	}
	env.HARMONY_VAPID_SUBJECT = 'mailto:e2e@example.org';
	return env;
}

// A harmony left over from a run that was interrupted is still listening on
// this port, still holding the database that run made — so the invites this
// run is about to mint go into a *different* database and the first signup is
// refused with `invite_unusable`, five seconds and one baffling message later.
//
// It happened three times on 11/09/2026 and cost more than everything it was
// interrupting. Saying so in one line, before anything else, is the whole fix:
// the command to run is in the message.
async function refuseIfSomethingIsAlreadyThere() {
	try {
		const res = await fetch(BASE + '/healthz', { signal: AbortSignal.timeout(1500) });
		if (!res.ok) return;
	} catch {
		return; // nothing there, which is what we want
	}
	throw new Error(
		`something is already serving ${BASE} — almost certainly a harmony left ` +
			'behind by a test run that was interrupted. It is holding the old ' +
			'database, so every invite this run mints would be refused. ' +
			'Kill it and try again: ' +
			'powershell -c "Get-Process harmony | Stop-Process -Force"',
	);
}

export async function start(howManyInvites = 3) {
	await refuseIfSomethingIsAlreadyThere();
	dir = mkdtempSync(join(tmpdir(), 'harmony-e2e-'));
	const db = join(dir, 'h.db');
	const files = join(dir, 'files');
	const mailDir = join(dir, 'mail');
	mkdirSync(mailDir, { recursive: true });
	process.env.HARMONY_E2E_MAIL_DIR = mailDir;
	mkdirSync(files, { recursive: true });

	const bin = binary();
	const invites = mintInvites(bin, db, howManyInvites);

	proc = spawn(
		bin,
		[
			'-addr',
			`127.0.0.1:${PORT}`,
			'-db',
			db,
			'-files',
			files,
			// Port zero would be easier, but the browser has to be able to
			// reach the media server, and a fixed port is one less thing to
			// discover. 40001 so a harmony running on this machine for real
			// keeps 40000.
			'-voice-port',
			String(40001 + PORT - 8097),
			'-voice-ip',
			'127.0.0.1',
		],
		{
			env: {
				...process.env,
				...vapid(bin),
				HARMONY_INSECURE_COOKIE: '1',
				HARMONY_BASE_URL: BASE,
				HARMONY_MAIL_DIR: mailDir,
				HARMONY_EMAIL_REQUIRED: '0',
				// Pion's own complaints. A binding that fails is a warning and
				// nothing else, and without this the packets simply vanish.
				PION_LOG_WARN: 'all',
				PION_LOG_ERROR: 'all',
			},
			stdio: ['ignore', 'pipe', 'pipe'],
		},
	);
	// Only test-process metadata, never cookies or keys. Workers may restart
	// this exact child on its existing SQLite file; teardown reads the new PID.
	process.env.HARMONY_E2E_SERVER_RUNTIME = join(dir, 'runtime.json');
	writeFileSync(process.env.HARMONY_E2E_SERVER_RUNTIME, JSON.stringify({
		pid: proc.pid, bin, args: proc.spawnargs.slice(1), mailDir,
	}));
	serverLog.length = 0;
	const sink = (b) => {
		serverLog.push(String(b));
		try {
			appendFileSync(logPath(), String(b));
		} catch {
			// best effort
		}
	};
	writeFileSync(logPath(), '');
	proc.stdout.on('data', sink);
	proc.stderr.on('data', sink);
	const log = serverLog;

	const deadline = Date.now() + 30_000;
	for (;;) {
		try {
			const res = await fetch(`${BASE}/healthz`);
			if (res.ok) break;
		} catch {
			// not up yet
		}
		if (Date.now() > deadline) throw new Error('harmony did not start:\n' + log.join(''));
		await new Promise((r) => setTimeout(r, 200));
	}
	return { invites, log };
}

export function dumpLog() {
	try {
		writeFileSync(logPath(), serverLog.join(''));
	} catch {
		// nothing to do about it
	}
}

/** Restart the owned local test server, preserving its database and sessions. */
export async function restart(outageMs = 0) {
	if (!Number.isInteger(outageMs) || outageMs < 0 || outageMs > 15_000) throw new Error('invalid local test outage');
	const path = process.env.HARMONY_E2E_SERVER_RUNTIME;
	if (!path) throw new Error('local test server metadata missing');
	const runtime = JSON.parse(readFileSync(path, 'utf8'));
	process.kill(runtime.pid);
	const deadline = Date.now() + 10_000;
	for (;;) {
		try { process.kill(runtime.pid, 0); }
		catch { break; }
		if (Date.now() > deadline) throw new Error('local test server did not exit');
		await new Promise((r) => setTimeout(r, 100));
	}
	if (outageMs) await new Promise((r) => setTimeout(r, outageMs));
	const restartDeadline = Date.now() + 10_000;
	const log = openSync(logPath(), 'a');
	proc = spawn(runtime.bin, runtime.args, {
		env: { ...process.env, ...vapid(runtime.bin), HARMONY_INSECURE_COOKIE: '1',
			HARMONY_BASE_URL: BASE, HARMONY_MAIL_DIR: runtime.mailDir, HARMONY_EMAIL_REQUIRED: '0' },
		// Detached, writing to the log file itself: a child with its output
		// piped into this worker died when Playwright retired the worker, and
		// the next project's tests found nothing on the port.
		stdio: ['ignore', log, log],
		detached: true,
		windowsHide: true,
	});
	proc.unref();
	closeSync(log);
	writeFileSync(path, JSON.stringify({ ...runtime, pid: proc.pid }));
	for (;;) {
		try {
			if ((await fetch(BASE + '/healthz', { signal: AbortSignal.timeout(1000) })).ok) return;
		} catch { /* waiting for the new process */ }
		if (Date.now() > restartDeadline) throw new Error('local test server did not restart');
		await new Promise((r) => setTimeout(r, 100));
	}
}

export function stop() {
	dumpLog();
	try {
		const runtime = JSON.parse(readFileSync(process.env.HARMONY_E2E_SERVER_RUNTIME, 'utf8'));
		process.kill(runtime.pid);
	} catch { /* child already exited */ }
	proc?.kill();
	proc = null;
	if (dir) {
		try {
			rmSync(dir, { recursive: true, force: true });
		} catch {
			// Windows sometimes still holds the binary; the directory is in
			// the system temp and will go on its own.
		}
		dir = null;
	}
}
