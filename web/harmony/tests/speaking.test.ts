import { test } from 'node:test';
import assert from 'node:assert/strict';
import { speakingSample } from '../src/speaking.ts';

test('quiet speech attacks immediately, syllable gaps hold, silence releases at 300 ms', () => {
	let last: number | undefined;
	for (const [at, level, expected] of [
		[0, 0, false], [25, 0.0009, false], [50, 0.001, false],
		[75, 0.0011, true], [100, 0, true], [374, 0, true],
		[375, 0, false], [400, 0.002, true], [650, 0.002, true],
		[949, 0, true], [950, 0, false],
	] as const) {
		last = speakingSample(last, level, at);
		assert.equal(last !== undefined, expected, `sample at ${at} ms`);
	}
});

test('invalid readings do not light silence or renew an existing hold', () => {
	assert.equal(speakingSample(undefined, NaN, 0), undefined);
	assert.equal(speakingSample(undefined, Infinity, 0), undefined);
	assert.equal(speakingSample(0, NaN, 299), 0);
	assert.equal(speakingSample(0, NaN, 300), undefined);
});
