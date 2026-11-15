package com.taleshernandes.harmony.media

import kotlin.math.abs
import kotlin.math.roundToInt

/** Pure policy. Only successful HTTP responses become the comparison baseline. */
internal class OutputDelay {
    companion object {
        // libwebrtc's Java audio module reports half of 150 ms as its output delay.
        const val ASSUMED_MS = 75
        const val MAX_TARGET_MS = 500
    }

    var sentMs = 0
        private set
    private var filtered: Double? = null
    private var sampleAt = Long.MIN_VALUE
    var exactMs = 0
        private set
    var desiredMs = 0
        private set
    var commandMs = 0
        private set
    private var pending: Int? = null
    private var uncertain = false // A lost response does not prove the server rejected the write.

    /**
     * The command is the target alone. 0.27 also pushed by the playout-time difference
     * of the screen and its audio, but that reading follows the command itself: in the
     * owner's log of 16/10/2026 (speaker, target 123 ms) it rose 61 -> 836 ms as the
     * command rose 123 -> 959 while the picture's buffer was still under 35 ms, so the
     * push fed on itself and the picture swung about a second either way, ~70 s a turn.
     */
    fun sample(latencyMs: Int, watching: Boolean, measuredAt: Long): Int? {
        exactMs = if (watching && latencyMs >= 0) (latencyMs.toLong() - ASSUMED_MS).coerceIn(0, MAX_TARGET_MS.toLong()).toInt() else 0
        if (!watching || latencyMs < 0) {
            filtered = null
            sampleAt = Long.MIN_VALUE
            desiredMs = 0
        } else if (sampleAt != measuredAt) {
            val extra = exactMs.toDouble()
            filtered = filtered?.let { it + 0.5 * (extra - it) } ?: extra
            desiredMs = filtered!!.roundToInt().coerceIn(0, MAX_TARGET_MS)
            sampleAt = measuredAt
        }
        commandMs = desiredMs
        if (pending != null) return null
        if (!uncertain && abs(commandMs - sentMs) <= 20 && !(commandMs == 0 && sentMs != 0)) return null
        pending = commandMs
        return commandMs
    }

    fun complete(ms: Int, success: Boolean) {
        if (pending != ms) return
        if (success) sentMs = ms
        uncertain = !success
        pending = null
    }
}
