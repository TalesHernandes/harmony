// The noise suppressor, shared by the voice room (media.ts) and the direct
// call (direct.ts): there is no reason for somebody's fan to be removed in one
// kind of call and not the other.
//
// It is DeepFilterNet 3 since 25/09/2026, chosen by the owner by ear in a lab
// that played the same recording through every candidate, knowing its price:
// about 15% of one core on the owner's PC and a third of one on their phone (RNNoise was
// 2% and 7%), and 24 MB fetched once per device (~12 MB on the wire: the
// wasm compresses to a quarter, and Caddy sends it zstd). What it buys is the
// one thing RNNoise could not do, and the reason the owner said the old filter
// "basically did not work": it removes noise *while the person is talking*,
// and keeps the whole voice (48 kHz) instead of the telephone sound of the
// 16 kHz models.
//
// RNNoise stays, behind it, as the fallback: a browser that cannot fetch 24 MB,
// or compile them, or that runs its audio at a rate the network was not
// trained for, gets RNNoise instead of nothing, and the log says why.
//
// Everything is fetched and compiled once per page and each worklet registered
// once per audio context; making a node after that is cheap except for
// DeepFilterNet's own start, which unpacks the model (see READY_WAIT).
import { RnnoiseWorkletNode, loadRnnoise } from '@sapphi-red/web-noise-suppressor';
// ?url makes Vite copy each of these into the built assets and hand back the
// address it gave them, under /static/ like everything else.
import rnnoiseWorkletURL from '@sapphi-red/web-noise-suppressor/rnnoiseWorklet.js?url';
import rnnoiseWasmURL from '@sapphi-red/web-noise-suppressor/rnnoise.wasm?url';
import rnnoiseSimdWasmURL from '@sapphi-red/web-noise-suppressor/rnnoise_simd.wasm?url';
import dfnWorkletURL from './dfn/worklet.js?url';
import dfnWasmURL from '../vendor/dfn/df_bg.wasm?url';
import dfnModelURL from '../vendor/dfn/DeepFilterNet3_onnx.tar.gz?url';
import { log } from './diag';

// The rate both networks were trained at. Every audio context that a
// microphone goes through is made at this rate (audioContext below); one that
// is not gets RNNoise's honest limitation instead of DeepFilterNet's garbage.
export const RATE = 48000;

// How much DeepFilterNet may take away, in dB: its attenuation limit. 100 is
// no limit. 50 is what the owner listened to in the lab when choosing it, and
// the setting is theirs to move (Volume.tsx).
export const DEFAULT_LEVEL = 50;

// Unpacking the model is done on the audio thread when the node is made (the
// time it took goes to the log). Longer than this and something is wrong; the
// caller is not kept waiting past it.
const READY_WAIT = 15_000;

export interface Suppressor {
	kind: 'deepfilter' | 'rnnoise';
	node: AudioNode;
	setLevel(db: number): void;
	destroy(): void;
}

// audioContext is the one way this client makes a context that a microphone
// goes through: at the rate the suppressor needs, whatever the sound card says.
//
// Except in Firefox before 148, which cannot connect a microphone that runs at
// one rate to a context at another (bugzilla 1674892): there the context keeps
// the card's rate, as it did before DeepFilterNet, and a card that is not at
// 48 kHz gets RNNoise. Forcing it would have lost the microphone altogether.
export function audioContext(): AudioContext {
	const firefox = /Firefox\/(\d+)/.exec(navigator.userAgent);
	if (firefox && Number(firefox[1]) < 148) return new AudioContext();
	try {
		return new AudioContext({ sampleRate: RATE, latencyHint: 'interactive' });
	} catch {
		// A browser that will not resample: the card's own rate, and RNNoise.
		return new AudioContext();
	}
}

let dfnAssets: Promise<{ module: WebAssembly.Module; model: ArrayBuffer }> | null = null;
const dfnReady = new WeakSet<BaseAudioContext>();

async function deepFilter(ctx: BaseAudioContext, level: number): Promise<Suppressor> {
	if (ctx.sampleRate !== RATE) throw new Error(`o audio roda a ${ctx.sampleRate} Hz, e ele precisa de ${RATE}`);
	dfnAssets ??= (async () => {
		const t0 = performance.now();
		const [module, model] = await Promise.all([
			WebAssembly.compileStreaming(fetch(dfnWasmURL)),
			fetch(dfnModelURL).then((r) => {
				if (!r.ok) throw new Error(`modelo: HTTP ${r.status}`);
				return r.arrayBuffer();
			}),
		]);
		log(`supressor: DeepFilterNet baixado e compilado em ${Math.round(performance.now() - t0)} ms`);
		return { module, model };
	})();
	let assets;
	try {
		assets = await dfnAssets;
	} catch (err) {
		dfnAssets = null; // the next call tries the network again
		throw err;
	}
	if (!dfnReady.has(ctx)) {
		await ctx.audioWorklet.addModule(dfnWorkletURL);
		dfnReady.add(ctx);
	}
	const node = new AudioWorkletNode(ctx, 'harmony-deepfilter', {
		numberOfInputs: 1,
		numberOfOutputs: 1,
		outputChannelCount: [1],
		processorOptions: { module: assets.module, model: assets.model, level },
	});
	const started = await new Promise<{ ready?: boolean; error?: string; ms?: number }>((resolve) => {
		const timer = setTimeout(() => resolve({ error: `nao ficou pronto em ${READY_WAIT / 1000} s` }), READY_WAIT);
		node.port.onmessage = (e) => {
			clearTimeout(timer);
			resolve(e.data);
		};
	});
	// From here on the only thing it says is that it failed while running,
	// and then it lets the voice through raw (dfn/worklet.js).
	node.port.onmessage = (e) => {
		if (e.data?.failed) log(`supressor: DeepFilterNet falhou em uso (${e.data.failed}), voz seguindo sem filtro`);
		if (e.data?.restarted) log(`supressor: DeepFilterNet reiniciado (saiu NaN), vez ${e.data.restarted}`);
	};
	node.onprocessorerror = () => log('supressor: DeepFilterNet parou com erro');
	if (!started.ready) {
		node.port.postMessage('destroy');
		node.disconnect();
		throw new Error(started.error ?? 'falhou ao iniciar');
	}
	log(`supressor: DeepFilterNet pronto em ${started.ms} ms, limite ${level} dB`);
	return {
		kind: 'deepfilter',
		node,
		setLevel: (db) => node.port.postMessage({ level: db }),
		destroy: () => {
			node.port.postMessage('destroy');
			node.disconnect();
		},
	};
}

let rnnoiseWasm: Promise<ArrayBuffer> | null = null;
const rnnoiseReady = new WeakSet<BaseAudioContext>();

async function rnnoise(ctx: BaseAudioContext): Promise<Suppressor> {
	rnnoiseWasm ??= loadRnnoise({ url: rnnoiseWasmURL, simdUrl: rnnoiseSimdWasmURL });
	let wasmBinary;
	try {
		wasmBinary = await rnnoiseWasm;
	} catch (err) {
		rnnoiseWasm = null;
		throw err;
	}
	if (!rnnoiseReady.has(ctx)) {
		await ctx.audioWorklet.addModule(rnnoiseWorkletURL);
		rnnoiseReady.add(ctx);
	}
	// One channel: a microphone is one voice.
	const node = new RnnoiseWorkletNode(ctx as AudioContext, { maxChannels: 1, wasmBinary });
	return {
		kind: 'rnnoise',
		node,
		setLevel: () => undefined,
		destroy: () => {
			node.disconnect();
			node.destroy();
		},
	};
}

// makeSuppressor returns DeepFilterNet, or RNNoise if DeepFilterNet could not
// be had, or null if neither could — an old browser, a blocked fetch, a machine
// with no room. A voice that is noisy is a bad evening; a voice that is missing
// is a broken call, so every failure falls back to the plain microphone and
// says so in the log.
export async function makeSuppressor(ctx: BaseAudioContext, level: number): Promise<Suppressor | null> {
	try {
		return await deepFilter(ctx, level);
	} catch (err) {
		log(`supressor: sem DeepFilterNet (${String(err)}), usando o RNNoise`);
	}
	try {
		return await rnnoise(ctx);
	} catch (err) {
		log(`sem supressor de ruido, seguindo sem ele: ${String(err)}`);
		return null;
	}
}
