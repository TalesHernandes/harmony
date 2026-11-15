package com.taleshernandes.harmony.media

import android.content.Context
import android.os.Handler
import android.os.Looper
import com.rikorose.deepfilternet.NativeDeepFilterNet
import com.taleshernandes.harmony.diag.Diag
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.webrtc.audio.WebRtcAudioRecord

/** One controller for the ADM, with a new streaming model for each capture. */
class Denoise(private val context: Context) : WebRtcAudioRecord.CaptureProcessor {
    private val preferences = context.getSharedPreferences("audio", Context.MODE_PRIVATE)
    @Volatile var enabled = preferences.getBoolean("deepfilter", true)
        private set
    @Volatile var level = normalizeLevel(preferences.getInt("deepfilter_level", DEFAULT_LEVEL))
        private set
    private val status = MutableStateFlow("")
    val state = status.asStateFlow()
    private val worker = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "harmony-denoise") }
    private val main = Handler(Looper.getMainLooper())
    @Volatile private var capture: Capture? = null

    private class Capture(val buffer: ByteBuffer) {
        @Volatile var generation = 0
        @Volatile var pipeline: DenoiseFrame? = null
    }

    @Synchronized
    fun configure(on: Boolean = enabled, db: Int = level) {
        val changed = enabled != on
        enabled = on
        level = normalizeLevel(db)
        preferences.edit().putBoolean("deepfilter", enabled).putInt("deepfilter_level", level).apply()
        if (changed) capture?.let { restart(it) }
    }

    @Synchronized
    override fun captureStarted(pcm16: ByteBuffer, sampleRate: Int, channels: Int) {
        capture?.let { retire(it) }
        if (sampleRate != RATE || channels != 1 || pcm16.capacity() != FRAME_BYTES) {
            capture = null
            status.value = "Sua voz está saindo sem filtro: o formato do microfone não é compatível."
            Diag.log("DFN: incompatible capture rate=$sampleRate channels=$channels bytes=${pcm16.capacity()}")
            return
        }
        val next = Capture(pcm16)
        capture = next
        restart(next)
    }

    override fun process(pcm16: ByteBuffer, bytes: Int, sampleRate: Int, channels: Int) {
        if (!enabled) return
        val current = capture ?: return
        if (current.buffer !== pcm16) return
        current.pipeline?.process(pcm16, bytes, sampleRate, channels, level)
    }

    @Synchronized
    override fun captureStopped(pcm16: ByteBuffer) {
        val current = capture ?: return
        if (current.buffer !== pcm16) return
        capture = null
        retire(current)
        status.value = ""
    }

    // Called on control/lifecycle threads only. Heavy model work never holds this monitor.
    private fun retire(current: Capture) {
        current.generation++
        val old = current.pipeline
        current.pipeline = null
        if (old != null) worker.execute { old.close() }
    }

    private fun restart(current: Capture) {
        retire(current)
        if (!enabled) { status.value = ""; return }
        status.value = "Preparando o filtro; sua voz está saindo sem filtro por enquanto."
        val generation = current.generation
        // The wrapper owns its pointer on Main until onModelLoaded. Construct and
        // register there to avoid its check-then-register race across dispatchers.
        main.post {
            if (!wanted(current, generation)) return@post
            try {
                // Kaleyra initializes asynchronously. Its callback publishes the native pointer;
                // never poll frameLength across threads or wait on the main dispatcher.
                val model = NativeDeepFilterNet(context, level.toFloat())
                var completed = false // Accessed only by this single worker.
                val timeout = worker.schedule({
                    if (!completed && wanted(current, generation)) {
                        status.value = "O filtro não carregou neste aparelho; sua voz está saindo sem filtro."
                        Diag.log("DFN: model loading timed out after 15 s")
                    }
                    completed = true
                    // release() before onModelLoaded would leak the later native allocation.
                }, 15, TimeUnit.SECONDS)
                model.onModelLoaded {
                    worker.execute loaded@ {
                        if (completed || !wanted(current, generation)) {
                            model.release()
                            return@loaded
                        }
                        completed = true
                        timeout.cancel(false)
                        try {
                            // Unlike libDF's C API (480 samples), Kaleyra's JNI returns PCM16 BYTES.
                            check(model.frameLength == FRAME_BYTES.toLong()) { "Unexpected DFN frame bytes" }
                            val scratch = ByteBuffer.allocateDirect(FRAME_BYTES).order(ByteOrder.LITTLE_ENDIAN)
                            val warmStart = System.nanoTime()
                            repeat(32) {
                                for (i in 0 until FRAME_BYTES) scratch.put(i, 0)
                                checkResult(model.processFrame(scratch))
                            }
                            val readyLevel = level
                            check(model.setAttenuationLimit(readyLevel.toFloat()))
                            val pipeline = DenoiseFrame(
                                scratch = scratch,
                                filter = { buffer, _ ->
                                    checkResult(model.processFrame(buffer))
                                },
                                setLevel = { db -> check(model.setAttenuationLimit(db.toFloat())) },
                                release = model::release,
                                initialLevel = readyLevel,
                                failed = { reason ->
                                    if (capture === current && current.generation == generation && enabled) {
                                        status.value = "O filtro foi desligado neste aparelho; sua voz está saindo sem filtro."
                                    }
                                    Diag.log("DFN: disabled ($reason); raw microphone continues")
                                },
                                paused = { on ->
                                    if (capture === current && current.generation == generation && enabled) {
                                        status.value = if (on) "O filtro pausou: o celular está lento agora (tela apagada?). Volta sozinho em 10 s." else ""
                                    }
                                    Diag.log(if (on) "DFN: paused for 10 s (fell behind 3 times in 5 s)" else "DFN: resumed")
                                },
                                report = { count, mean, max, overruns, behind, raw ->
                                    Diag.log("DFN: frames=$count mean_us=${mean / 1000} max_us=${max / 1000} overruns=$overruns behind_max_ms=${behind / 1_000_000} raw=$raw")
                                },
                            )
                            synchronized(this@Denoise) {
                                if (wanted(current, generation)) {
                                    current.pipeline = pipeline
                                    status.value = ""
                                    Diag.log("DFN: ready rate=$RATE channels=1 bytes=$FRAME_BYTES warm_ms=${(System.nanoTime() - warmStart) / 1_000_000}")
                                } else pipeline.close()
                            }
                        } catch (e: Throwable) {
                            model.release()
                            loadFailed(current, generation, e)
                        }
                    }
                }
            } catch (e: Throwable) { loadFailed(current, generation, e) }
        }
    }

    @Synchronized
    private fun wanted(current: Capture, generation: Int) =
        capture === current && current.generation == generation && enabled

    @Synchronized
    private fun loadFailed(current: Capture, generation: Int, e: Throwable) {
        if (!wanted(current, generation)) return
        status.value = "O filtro não carregou neste aparelho; sua voz está saindo sem filtro."
        Diag.log("DFN: initialization failed (${e.javaClass.simpleName}); raw microphone continues")
    }

    companion object {
        const val RATE = 48000
        const val FRAME_BYTES = 960
        const val DEFAULT_LEVEL = 50
        fun normalizeLevel(db: Int) = ((db.coerceIn(10, 100) + 2) / 5) * 5
        private fun checkResult(result: Float) {
            // JNI error codes are -20..-70; -1 is also a valid model LSNR, not an error.
            check(result.isFinite()) { "DFN processing failed" }
            when (result) {
                -20f, -30f, -40f, -50f, -60f, -70f -> error("DFN processing failed")
            }
        }
    }
}

/** Pure fail-open processing, tested on the JVM without loading a native library. */
internal fun interface DenoiseClock { fun now(): Long }

internal class DenoiseFrame(
    private val scratch: ByteBuffer = ByteBuffer.allocateDirect(Denoise.FRAME_BYTES).order(ByteOrder.LITTLE_ENDIAN),
    private val filter: (ByteBuffer, Int) -> Unit,
    private val setLevel: (Int) -> Unit = {},
    private val release: () -> Unit = {},
    initialLevel: Int = Denoise.DEFAULT_LEVEL,
    private val clock: DenoiseClock = DenoiseClock(System::nanoTime),
    private val failed: (String) -> Unit = {},
    // frames, mean ns, max ns, frames over 10 ms, most the microphone fell behind (ns),
    // frames passed raw to catch up
    private val report: (Long, Long, Long, Long, Long, Long) -> Unit = { _, _, _, _, _, _ -> },
    // Slowness is not a fault: with the screen off the phone slows its
    // processor down and the frames come out late (owner, 03/10/2026: "quando
    // apaga a tela, o filtro para"). The filter then steps aside for
    // [PAUSE_NS] and tries again with a fresh budget, instead of going away
    // until the next call. [paused] says when it steps aside and comes back.
    private val paused: (Boolean) -> Unit = {},
) {
    private var budget = DenoiseBudget()
    private var resumeAt = -1L
    private var level = initialLevel
    private var closed = false
    private var count = 0L
    private var sum = 0L
    private var max = 0L
    private var overruns = 0L
    private var behind = 0L
    private var raw = 0L
    private var lastReport = clock.now()

    @Synchronized
    fun process(pcm: ByteBuffer, bytes: Int, sampleRate: Int, channels: Int, db: Int) {
        if (closed) return
        if (resumeAt >= 0) {
            if (clock.now() < resumeAt) return
            resumeAt = -1
            budget = DenoiseBudget()
            paused(false)
        }
        if (budget.disabled) return
        if (bytes != Denoise.FRAME_BYTES || sampleRate != Denoise.RATE || channels != 1 ||
            pcm.capacity() != Denoise.FRAME_BYTES || pcm.limit() < bytes) {
            disable("capture format changed")
            return
        }
        if (budget.catchingUp) { budget.skip(); raw++; return }
        val start = clock.now()
        try {
            if (level != db) { setLevel(db); level = db }
            // Absolute copies preserve the ADM's position, limit and byte order. Never let
            // a throwing JNI filter partially overwrite the original microphone frame.
            for (i in 0 until bytes) scratch.put(i, pcm.get(i))
            scratch.clear()
            filter(scratch, db)
        } catch (e: Throwable) {
            val elapsed = (clock.now() - start).coerceAtLeast(0)
            count++
            sum += elapsed
            max = maxOf(max, elapsed)
            if (elapsed > DenoiseBudget.FRAME_NS) overruns++
            logStats()
            disable("processing ${e.javaClass.simpleName}")
            return
        }
        val end = clock.now()
        val elapsed = (end - start).coerceAtLeast(0)
        count++
        sum += elapsed
        max = maxOf(max, elapsed)
        if (elapsed > DenoiseBudget.FRAME_NS) overruns++
        val tripped = budget.record(elapsed)
        behind = maxOf(behind, budget.behindNs)
        if (tripped) {
            logStats()
            resumeAt = end + PAUSE_NS
            paused(true)
            return
        }
        for (i in 0 until bytes) pcm.put(i, scratch.get(i))
        if (end - lastReport >= 30_000_000_000L) {
            logStats()
            lastReport = end
            count = 0; sum = 0; max = 0; overruns = 0; behind = 0; raw = 0
        }
    }

    private fun logStats() { if (count > 0) report(count, sum / count, max, overruns, behind, raw) }

    companion object { const val PAUSE_NS = 10_000_000_000L }
    private fun disable(reason: String) { budget.fail(); failed(reason) }

    @Synchronized
    fun close() {
        if (closed) return
        closed = true
        logStats()
        release()
    }
}

// How far the microphone has fallen behind live. A frame that takes longer
// than the 10 ms it covers puts the capture thread behind by the difference;
// a quicker one wins some of that back, and nothing is behind once the queue
// is empty. Counting slow frames instead (the rule until 0.21) paused the
// filter every half minute with the screen off, at a mean of 6.5 ms a frame —
// spikes the fast frames were already absorbing. The limit sits under the
// smallest AudioRecord buffer WebRTC asks for (2 × the minimum, ≥ 40 ms), so
// the filter steps aside before the system has to drop audio.
//
// Past the limit it first only *catches up*: the next few frames go out raw,
// costing nothing, until the queue is empty — 30–40 ms of unfiltered sound.
// One spike is the system taking the processor away for an instant (0.21.1,
// screen off: 90 s at 6–10 ms behind, then a single 44 ms frame), and a
// 10 s pause for it was all cost. Only falling behind [TRIPS] times within
// [TRIP_WINDOW] frames — slow on average, not once — stops the filter.
internal class DenoiseBudget {
    var disabled = false
        private set
    var behindNs = 0L
        private set
    var catchingUp = false
        private set
    private var frame = 0L
    private val trips = LongArray(TRIPS - 1) { -TRIP_WINDOW } // the previous trips
    private var tripIndex = 0

    fun record(elapsedNs: Long): Boolean {
        if (disabled) return true
        frame++
        behindNs = (behindNs + elapsedNs - FRAME_NS).coerceAtLeast(0)
        if (behindNs > LIMIT_NS) {
            catchingUp = true
            // The first seconds are the model warming up, not the phone being
            // slow (0.21.2: three trips in the first 1.8 s of a call, then
            // fine): catch up, but do not count them towards a pause.
            if (frame <= GRACE) return false
            val oldest = trips[tripIndex]
            trips[tripIndex] = frame
            tripIndex = (tripIndex + 1) % trips.size
            if (frame - oldest < TRIP_WINDOW) fail()
        }
        return disabled
    }

    /** A frame passed raw while catching up: it costs nothing, so it wins back its 10 ms. */
    fun skip() {
        frame++
        behindNs = (behindNs - FRAME_NS).coerceAtLeast(0)
        if (behindNs == 0L) catchingUp = false
    }

    fun fail() { disabled = true }
    companion object {
        const val FRAME_NS = 10_000_000L
        const val LIMIT_NS = 30_000_000L
        const val TRIPS = 3
        const val TRIP_WINDOW = 500L // frames: 5 s
        const val GRACE = 300L // frames: 3 s after the filter starts
    }
}
