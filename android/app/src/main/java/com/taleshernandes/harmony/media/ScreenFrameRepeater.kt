package com.taleshernandes.harmony.media

import org.webrtc.CapturerObserver
import org.webrtc.SurfaceTextureHelper
import org.webrtc.ThreadUtils
import org.webrtc.VideoFrame

/** All state and callbacks belong to the helper thread; no texture is cached. */
internal class ScreenFrameRepeater(
    private val helper: SurfaceTextureHelper,
    private val downstream: CapturerObserver,
) : CapturerObserver {
    private val handler = helper.handler
    private val cadence = ScreenFrameCadence()
    private var closed = false
    private val tick = object : Runnable {
        override fun run() {
            val delay = cadence.delayNs(System.nanoTime())
            if (delay < 0) return
            if (delay == 0L) {
                // The helper calls updateTexImage at delivery time, so a real
                // frame arriving after this request wins over the old image.
                // Its existing in-flight guard also applies to forced frames.
                helper.forceFrame()
            }
            val next = if (delay == 0L) ScreenFrameCadence.INTERVAL_NS else delay
            handler.postDelayed(this, (next + 999_999) / 1_000_000)
        }
    }

    override fun onCapturerStarted(success: Boolean) {
        ThreadUtils.invokeAtFrontUninterruptibly(handler, Runnable {
            if (closed) return@Runnable
            handler.removeCallbacks(tick)
            cadence.start(success)
            downstream.onCapturerStarted(success)
            if (success) handler.postDelayed(tick, 500)
        })
    }

    override fun onCapturerStopped() {
        ThreadUtils.invokeAtFrontUninterruptibly(handler, Runnable {
            stop()
            downstream.onCapturerStopped()
        })
    }

    override fun onFrameCaptured(frame: VideoFrame) {
        check(Thread.currentThread() == handler.looper.thread) { "Screen frame on wrong thread" }
        if (!cadence.capture(frame.timestampNs, System.nanoTime())) return
        deliverScreenFrame(downstream, frame, cadence.outputTimestampNs)
    }

    /** Call before stopping capture or disposing the source/helper, also on failure. */
    fun close() {
        ThreadUtils.invokeAtFrontUninterruptibly(handler, Runnable {
            closed = true
            stop()
        })
    }

    private fun stop() {
        handler.removeCallbacks(tick)
        cadence.start(false)
    }
}

/** Borrow the input frame; any extra ownership lasts only for this callback. */
internal fun deliverScreenFrame(downstream: CapturerObserver, frame: VideoFrame, timestamp: Long) {
    if (timestamp == frame.timestampNs) {
        downstream.onFrameCaptured(frame)
    } else {
        // forceFrame reuses the SurfaceTexture's timestamp. Only the
        // immutable frame envelope changes; pixels stay on the GPU.
        val updated = VideoFrame(frame.buffer, frame.rotation, timestamp)
        updated.retain()
        try {
            downstream.onFrameCaptured(updated)
        } finally {
            updated.release()
        }
    }
}

/** Pure timing policy, using an injected monotonic time in nanoseconds. */
internal class ScreenFrameCadence {
    companion object {
        const val INTERVAL_NS = 500_000_000L
    }

    private var running = false
    private var hasFrame = false
    private var latestSourceNs = 0L
    private var sourceAtNs = 0L
    private var deliveredAtNs = 0L
    var outputTimestampNs = 0L
        private set

    fun start(success: Boolean) {
        running = success
        hasFrame = false
        latestSourceNs = 0
        sourceAtNs = 0
        deliveredAtNs = 0
        outputTimestampNs = 0
    }

    /** Reject stale content; an equal source timestamp is the latest texture again. */
    fun capture(sourceNs: Long, nowNs: Long): Boolean {
        if (!running || (hasFrame && sourceNs < latestSourceNs)) return false
        // A force request may have been queued just before a real frame. Do
        // not immediately repeat that new image when the queued request runs.
        if (hasFrame && sourceNs == latestSourceNs && nowNs - deliveredAtNs < INTERVAL_NS) return false
        val newer = !hasFrame || sourceNs > latestSourceNs
        if (newer) {
            latestSourceNs = sourceNs
            sourceAtNs = nowNs
        }
        val timestamp = if (newer) sourceNs else sourceNs + (nowNs - sourceAtNs).coerceAtLeast(0)
        // Native video timestamps are microseconds. A 1 ns increment would
        // become a duplicate after conversion, including when real input resumes.
        outputTimestampNs = if (hasFrame) maxOf(timestamp, outputTimestampNs + 1_000) else timestamp
        deliveredAtNs = nowNs
        hasFrame = true
        return true
    }

    /** Negative means stopped; before the first frame, poll without forcing an empty texture. */
    fun delayNs(nowNs: Long): Long = when {
        !running -> -1
        !hasFrame -> INTERVAL_NS
        else -> (INTERVAL_NS - (nowNs - deliveredAtNs)).coerceAtLeast(0)
    }
}
