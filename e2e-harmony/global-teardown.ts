import { stop } from './server.mjs';
import { rmSync } from 'node:fs';
import { join } from 'node:path';

export default async function globalTeardown() {
	stop();
	try {
		rmSync(join(__dirname, 'invites.json'));
	} catch {
		// already gone
	}
}
