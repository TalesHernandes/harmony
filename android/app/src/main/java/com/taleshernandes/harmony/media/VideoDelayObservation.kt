package com.taleshernandes.harmony.media

import java.util.Locale

/** Buffer observations only. These are not an isolated measurement of A/V sync delay. */
internal class VideoDelayObservation {
    private data class Counters(val count: Double, val delay: Double?, val target: Double?, val minimum: Double?)
    private var key: String? = null
    private var previous: Counters? = null
    private var at = 0L
    private var interval: List<Double?>? = null

    fun sample(stream: String?, values: Map<String, Any>?, now: Long) {
        fun number(name: String) = (values?.get(name) as? Number)?.toDouble()?.takeIf { it.isFinite() && it >= 0 }
        val count = number("jitterBufferEmittedCount")
        val current = count?.let { Counters(it, number("jitterBufferDelay"),
            number("jitterBufferTargetDelay"), number("jitterBufferMinimumDelay")) }
        val old = previous
        interval = if (stream != null && stream == key && current != null && old != null &&
            now - at in 1..2500 && current.count > old.count) {
            fun mean(a: Double?, b: Double?): Double? = if (a != null && b != null && a >= b)
                (1000 * (a - b) / (current.count - old.count)).takeIf { it.isFinite() } else null
            listOf(mean(current.delay, old.delay), mean(current.target, old.target), mean(current.minimum, old.minimum))
        } else null
        key = stream
        previous = current
        at = now
    }

    fun diagnostic(stream: String?, now: Long): String {
        val valid = stream != null && stream == key && now - at in 0..2500
        val names = listOf("videoBufferMs", "videoBufferTargetMs", "videoBufferMinimumMs")
        return names.mapIndexed { i, name ->
            val value = if (valid) interval?.get(i) else null
            "$name=${value?.let { String.format(Locale.ROOT, "%.1f", it) } ?: "não encontrado"}"
        }.joinToString(" ") + " videoSyncAddedMs=não encontrado"
    }
}
