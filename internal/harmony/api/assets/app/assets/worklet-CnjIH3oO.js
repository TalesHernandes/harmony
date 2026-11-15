// DeepFilterNet 3 in an AudioWorklet: the noise suppressor of this client.
//
// Everything down to the "harmony" marker is the wasm-bindgen glue for
// df_bg.wasm, copied unchanged from deepfilternet3-noise-filter@1.3.0
// (mezonai/mezon-noise-suppression, Apache-2.0 OR MIT), where it ships as a
// string that the package loads through a blob: URL. This server's CSP allows
// scripts from this origin only, so it lives here as a file instead. The wasm
// it drives is libDF, DeepFilterNet's own runtime (Rikorose/DeepFilterNet,
// MIT OR Apache-2.0); see scripts/fetch-dfn.sh for both big files and their
// checksums. A new df_bg.wasm needs the glue from the same release.
(function () {
    'use strict';

    /* @ts-self-types="./df.d.ts" */

    // --- AudioWorklet polyfill ---------------------------------------------------
    // AudioWorkletGlobalScope has no TextDecoder/TextEncoder. wasm-bindgen 0.2.126
    // constructs `new TextDecoder()` at module top-level (unguarded), which throws
    // `ReferenceError: TextDecoder is not defined` inside a worklet and prevents
    // registerProcessor() from running. Provide minimal UTF-8 shims when absent.
    // (Only used for error/diagnostic strings; the audio path passes bytes/floats.)
    if (typeof TextDecoder === 'undefined') {
        globalThis.TextDecoder = class {
            decode(bytes) {
                if (!bytes) return '';
                const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes.buffer || bytes);
                let out = '';
                for (let i = 0; i < u8.length;) {
                    const c = u8[i++];
                    if (c < 0x80) out += String.fromCharCode(c);
                    else if (c < 0xE0) out += String.fromCharCode(((c & 0x1F) << 6) | (u8[i++] & 0x3F));
                    else if (c < 0xF0) out += String.fromCharCode(((c & 0x0F) << 12) | ((u8[i++] & 0x3F) << 6) | (u8[i++] & 0x3F));
                    else {
                        const cp = (((c & 0x07) << 18) | ((u8[i++] & 0x3F) << 12) | ((u8[i++] & 0x3F) << 6) | (u8[i++] & 0x3F)) - 0x10000;
                        out += String.fromCharCode(0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
                    }
                }
                return out;
            }
        };
    }
    if (typeof TextEncoder === 'undefined') {
        globalThis.TextEncoder = class {
            encode(str) {
                const out = [];
                for (let i = 0; i < str.length; i++) {
                    let c = str.charCodeAt(i);
                    if (c < 0x80) out.push(c);
                    else if (c < 0x800) out.push(0xC0 | (c >> 6), 0x80 | (c & 0x3F));
                    else if (c >= 0xD800 && c < 0xDC00) {
                        const c2 = str.charCodeAt(++i);
                        c = 0x10000 + ((c & 0x3FF) << 10) + (c2 & 0x3FF);
                        out.push(0xF0 | (c >> 18), 0x80 | ((c >> 12) & 0x3F), 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F));
                    } else out.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F));
                }
                return new Uint8Array(out);
            }
            encodeInto(str, dst) {
                const enc = this.encode(str);
                dst.set(enc);
                return { read: str.length, written: enc.length };
            }
        };
    }

    /**
     * Create a DeepFilterNet Model
     *
     * Args:
     *     - path: File path to a DeepFilterNet tar.gz onnx model
     *     - atten_lim: Attenuation limit in dB.
     *
     * Returns:
     *     - DF state doing the full processing: stft, DNN noise reduction, istft.
     * @param {Uint8Array} model_bytes
     * @param {number} atten_lim
     * @returns {number}
     */
    function df_create(model_bytes, atten_lim) {
        const ptr0 = passArray8ToWasm0(model_bytes, wasm.__wbindgen_malloc_command_export);
        const len0 = WASM_VECTOR_LEN;
        const ret = wasm.df_create(ptr0, len0, atten_lim);
        return ret >>> 0;
    }

    /**
     * Get DeepFilterNet frame size in samples.
     * @param {number} st
     * @returns {number}
     */
    function df_get_frame_length(st) {
        const ret = wasm.df_get_frame_length(st);
        return ret >>> 0;
    }

    /**
     * Processes a chunk of samples.
     *
     * Args:
     *     - df_state: Created via df_create()
     *     - input: Input buffer of length df_get_frame_length()
     *     - output: Output buffer of length df_get_frame_length()
     *
     * Returns:
     *     - Local SNR of the current frame.
     * @param {number} st
     * @param {Float32Array} input
     * @returns {Float32Array}
     */
    function df_process_frame(st, input) {
        const ptr0 = passArrayF32ToWasm0(input, wasm.__wbindgen_malloc_command_export);
        const len0 = WASM_VECTOR_LEN;
        const ret = wasm.df_process_frame(st, ptr0, len0);
        return ret;
    }

    /**
     * Set DeepFilterNet attenuation limit.
     *
     * Args:
     *     - lim_db: New attenuation limit in dB.
     * @param {number} st
     * @param {number} lim_db
     */
    function df_set_atten_lim(st, lim_db) {
        wasm.df_set_atten_lim(st, lim_db);
    }
    function __wbg_get_imports() {
        const import0 = {
            __proto__: null,
            __wbg___wbindgen_throw_344f42d3211c4765: function(arg0, arg1) {
                throw new Error(getStringFromWasm0(arg0, arg1));
            },
            __wbg_getRandomValues_cc7f052a444bb2ce: function() { return handleError(function (arg0, arg1) {
                globalThis.crypto.getRandomValues(getArrayU8FromWasm0(arg0, arg1));
            }, arguments); },
            __wbg_new_from_slice_ddf8b82c4d6af38e: function(arg0, arg1) {
                const ret = new Float32Array(getArrayF32FromWasm0(arg0, arg1));
                return ret;
            },
            __wbindgen_init_externref_table: function() {
                const table = wasm.__wbindgen_externrefs;
                const offset = table.grow(4);
                table.set(0, undefined);
                table.set(offset + 0, undefined);
                table.set(offset + 1, null);
                table.set(offset + 2, true);
                table.set(offset + 3, false);
            },
        };
        return {
            __proto__: null,
            "./df_bg.js": import0,
        };
    }

    (typeof FinalizationRegistry === 'undefined')
        ? { }
        : new FinalizationRegistry(ptr => wasm.__wbg_dfstate_free(ptr, 1));

    function addToExternrefTable0(obj) {
        const idx = wasm.__externref_table_alloc_command_export();
        wasm.__wbindgen_externrefs.set(idx, obj);
        return idx;
    }

    function getArrayF32FromWasm0(ptr, len) {
        ptr = ptr >>> 0;
        return getFloat32ArrayMemory0().subarray(ptr / 4, ptr / 4 + len);
    }

    function getArrayU8FromWasm0(ptr, len) {
        ptr = ptr >>> 0;
        return getUint8ArrayMemory0().subarray(ptr / 1, ptr / 1 + len);
    }

    let cachedFloat32ArrayMemory0 = null;
    function getFloat32ArrayMemory0() {
        if (cachedFloat32ArrayMemory0 === null || cachedFloat32ArrayMemory0.byteLength === 0) {
            cachedFloat32ArrayMemory0 = new Float32Array(wasm.memory.buffer);
        }
        return cachedFloat32ArrayMemory0;
    }

    function getStringFromWasm0(ptr, len) {
        return decodeText(ptr >>> 0, len);
    }

    let cachedUint8ArrayMemory0 = null;
    function getUint8ArrayMemory0() {
        if (cachedUint8ArrayMemory0 === null || cachedUint8ArrayMemory0.byteLength === 0) {
            cachedUint8ArrayMemory0 = new Uint8Array(wasm.memory.buffer);
        }
        return cachedUint8ArrayMemory0;
    }

    function handleError(f, args) {
        try {
            return f.apply(this, args);
        } catch (e) {
            const idx = addToExternrefTable0(e);
            wasm.__wbindgen_exn_store_command_export(idx);
        }
    }

    function passArray8ToWasm0(arg, malloc) {
        const ptr = malloc(arg.length * 1, 1) >>> 0;
        getUint8ArrayMemory0().set(arg, ptr / 1);
        WASM_VECTOR_LEN = arg.length;
        return ptr;
    }

    function passArrayF32ToWasm0(arg, malloc) {
        const ptr = malloc(arg.length * 4, 4) >>> 0;
        getFloat32ArrayMemory0().set(arg, ptr / 4);
        WASM_VECTOR_LEN = arg.length;
        return ptr;
    }

    let cachedTextDecoder = new TextDecoder('utf-8', { ignoreBOM: true, fatal: true });
    cachedTextDecoder.decode();
    const MAX_SAFARI_DECODE_BYTES = 2146435072;
    let numBytesDecoded = 0;
    function decodeText(ptr, len) {
        numBytesDecoded += len;
        if (numBytesDecoded >= MAX_SAFARI_DECODE_BYTES) {
            cachedTextDecoder = new TextDecoder('utf-8', { ignoreBOM: true, fatal: true });
            cachedTextDecoder.decode();
            numBytesDecoded = len;
        }
        return cachedTextDecoder.decode(getUint8ArrayMemory0().subarray(ptr, ptr + len));
    }

    let WASM_VECTOR_LEN = 0;

    let wasm;
    function __wbg_finalize_init(instance, module) {
        wasm = instance.exports;
        cachedFloat32ArrayMemory0 = null;
        cachedUint8ArrayMemory0 = null;
        wasm.__wbindgen_start();
        return wasm;
    }

    function initSync(module) {
        if (wasm !== undefined) return wasm;


        if (module !== undefined) {
            if (Object.getPrototypeOf(module) === Object.prototype) {
                ({module} = module);
            } else {
                console.warn('using deprecated parameters for `initSync()`; pass a single object instead');
            }
        }

        const imports = __wbg_get_imports();
        if (!(module instanceof WebAssembly.Module)) {
            module = new WebAssembly.Module(module);
        }
        const instance = new WebAssembly.Instance(module, imports);
        return __wbg_finalize_init(instance);
    }

    // ---------------------------------------------------------------- harmony
    //
    // The processor is ours, not the package's, for three reasons:
    //
    // - It says whether it started. The package's own falls back to passing
    //   the microphone through untouched when the model fails to load, and
    //   says nothing — a switch that reads "on" while every noise goes out.
    //   This one posts { ready } or { error } and the page decides.
    // - It frees the model's state when it is taken down. Each state holds a
    //   whole network, and the page rebuilds the microphone graph whenever the
    //   volume or the device changes.
    // - Its delay is fixed from the first sample: the output starts one frame
    //   behind, so a 480-sample frame never leaves a 128-sample block short.
    // - If the network fails once it is running, the voice goes on raw. An
    //   exception out of process() would leave the node silent for good (that
    //   is what the Web Audio spec does with a processor that throws), and a
    //   silent microphone is a broken call; a noisy one is a bad evening.
    // - Exact zeros never reach the network, and what comes out is
    //   checked. Fed nothing but zeros (a microphone muted on its own button)
    //   the network turned to NaN after about 80 s in Firefox and stayed NaN,
    //   which the meter reads as full volume: the owner's ring went green and
    //   stayed green while muted (16/10/2026). A silent frame now goes out as
    //   silence, and the network is kept running on a hiss far below hearing
    //   instead of on exact zeros, so it comes back from a mute as it did
    //   before (skipping those frames let 250 ms of room noise through on
    //   unmute). A frame that comes back NaN or infinite restarts the network
    //   instead of poisoning everything after it.
    const FRAME_FALLBACK = 480;
    // Below about -140 dBFS: nothing a microphone that is switched on produces.
    const SILENCE = 1e-7;
    // The hiss the network hears during digital silence: about -100 dBFS.
    const HISS = 1e-5;
    // A network that keeps breaking is given up on: the voice goes raw.
    const RESTARTS_MAX = 3;

    class HarmonyDeepFilter extends AudioWorkletProcessor {
        constructor(options) {
            super();
            this.alive = true;
            this.handle = 0;
            const o = options.processorOptions;
            this.failed = false;
            const t0 = Date.now();
            try {
                initSync({ module: o.module });
                this.model = new Uint8Array(o.model);
                this.level = o.level ?? 100;
                this.restarts = 0;
                this.handle = df_create(this.model, this.level);
                this.frame = df_get_frame_length(this.handle) || FRAME_FALLBACK;
                this.inBuf = new Float32Array(this.frame);
                this.hiss = new Float32Array(this.frame);
                this.seed = 1;
                this.inPos = 0;
                this.ring = new Float32Array(this.frame * 4);
                this.r = 0;
                this.w = this.frame; // one frame of silence ahead of the first output
            } catch (err) {
                // A state that was made and then failed half way is freed here:
                // it holds a whole network.
                this.free();
                this.port.postMessage({ error: String(err && err.message || err) });
            }
            // Ready is said only once everything above has worked.
            if (this.handle) this.port.postMessage({ ready: true, frame: this.frame, ms: Date.now() - t0 });
            this.port.onmessage = (e) => {
                const m = e.data;
                if (m === 'destroy') return this.destroy();
                if (m && typeof m.level === 'number' && this.handle) {
                    this.level = Math.max(0, Math.min(100, m.level));
                    df_set_atten_lim(this.handle, this.level);
                }
            };
        }

        process(inputs, outputs) {
            if (!this.alive) return false;
            const out = outputs[0] && outputs[0][0];
            if (!out) return true;
            const x = inputs[0] && inputs[0][0];
            // Failed while running: the microphone passes through untouched.
            if (this.failed) {
                if (x) out.set(x);
                else out.fill(0);
                for (let c = 1; c < outputs[0].length; c++) outputs[0][c].set(out);
                return true;
            }
            // Not started: nothing goes out at all. The page is told, and swaps
            // this node for another; until then silence is the honest answer.
            if (!this.handle) {
                out.fill(0);
                return true;
            }
            const n = this.ring.length;
            try {
                for (let i = 0; i < out.length; i++) {
                    this.inBuf[this.inPos++] = x ? x[i] : 0;
                    if (this.inPos === this.frame) {
                        const y = this.clean(this.inBuf);
                        for (let k = 0; k < this.frame; k++) {
                            this.ring[this.w] = y ? y[k] : 0;
                            this.w = (this.w + 1) % n;
                        }
                        this.inPos = 0;
                    }
                }
            } catch (err) {
                this.failed = true;
                this.free();
                this.port.postMessage({ failed: String(err && err.message || err) });
                if (x) out.set(x);
                return true;
            }
            for (let i = 0; i < out.length; i++) {
                out[i] = this.ring[this.r];
                this.r = (this.r + 1) % n;
            }
            for (let c = 1; c < outputs[0].length; c++) outputs[0][c].set(out);
            return true;
        }

        // clean runs one frame through the network, or answers null for "send
        // silence": the frame was silent, or the network broke on it.
        clean(frame) {
            let loud = false;
            for (let k = 0; k < frame.length; k++) {
                if (Math.abs(frame[k]) > SILENCE) { loud = true; break; }
            }
            if (!this.handle) return null;
            if (!loud) {
                for (let k = 0; k < frame.length; k++) {
                    this.seed = (Math.imul(this.seed, 1103515245) + 12345) >>> 0;
                    this.hiss[k] = HISS * (this.seed / 2147483648 - 1);
                }
            }
            const y = df_process_frame(this.handle, loud ? frame : this.hiss);
            for (let k = 0; k < y.length; k++) {
                if (Number.isFinite(y[k])) continue;
                this.restart();
                return null;
            }
            return loud ? y : null;
        }

        // restart replaces a state that produced NaN with a fresh one. It is
        // slow (the model is unpacked again), so it is rare by construction.
        restart() {
            this.free();
            this.restarts++;
            if (this.restarts > RESTARTS_MAX) {
                this.failed = true;
                this.port.postMessage({ failed: 'NaN repetido' });
                return;
            }
            try {
                this.handle = df_create(this.model, this.level);
            } catch (err) {
                this.failed = true;
                this.port.postMessage({ failed: String(err && err.message || err) });
                return;
            }
            this.port.postMessage({ restarted: this.restarts });
        }

        destroy() {
            if (!this.alive) return;
            this.alive = false;
            this.free();
        }

        free() {
            if (this.handle) {
                try { wasm.__wbg_dfstate_free(this.handle, 0); } catch (_) { /* already gone */ }
            }
            this.handle = 0;
        }
    }

    registerProcessor('harmony-deepfilter', HarmonyDeepFilter);

})();
