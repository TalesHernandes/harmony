package com.taleshernandes.harmony.media

import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * How far the screen's picture still is from its sound, in the synchroniser's own
 * terms: estimatedPlayoutTimestamp (the capture time, on the sender's clock, of what
 * is playing now) of the video minus that of the audio, from one stats report. Both
 * go through the same RTP-to-NTP mapping the synchroniser uses, so the difference is
 * what it still has to remove, whatever delay the picture already carries.
 */
internal class SyncError {
    private var key: String? = null
    private var lastVideo: Double? = null
    private var lastAudio: Double? = null
    var errorMs: Int? = null
        private set
    var at = Long.MIN_VALUE
        private set

    /** A stalled timestamp (paused stream, frozen picture) yields no reading. */
    fun sample(stream: String?, video: Map<String, Any>?, audio: Map<String, Any>?, now: Long) {
        fun playout(values: Map<String, Any>?) =
            (values?.get("estimatedPlayoutTimestamp") as? Number)?.toDouble()?.takeIf { it.isFinite() && it > 0 }
        val v = playout(video)
        val a = playout(audio)
        val same = stream != null && stream == key
        val advancing = same && v != null && a != null &&
            lastVideo?.let { v > it } == true && lastAudio?.let { a > it } == true
        val diff = if (advancing) v!! - a!! else null
        errorMs = diff?.takeIf { abs(it) < 5_000 }?.roundToInt()
        at = if (errorMs != null) now else Long.MIN_VALUE
        key = stream
        lastVideo = v
        lastAudio = a
    }

    /** Readings older than a stats lag or two are ignored, and the target is sent as is. */
    fun fresh(now: Long): Int? = errorMs?.takeIf { at != Long.MIN_VALUE && now - at in 0..2_500 }
}
