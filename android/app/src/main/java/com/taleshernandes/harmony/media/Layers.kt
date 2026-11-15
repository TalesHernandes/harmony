package com.taleshernandes.harmony.media

object Layers {
    const val ALTA = "alta"
    const val MEDIA = "media"
    const val BAIXA = "baixa"
    val SCREEN_LAYERS: List<String> = listOf(ALTA, MEDIA, BAIXA)

    // Android callers already measure physical pixels: applying density again asks for oversized images.
    fun forWidth(px: Int): String = when {
        px >= 1200 -> ALTA
        px >= 600 -> MEDIA
        else -> BAIXA
    }

    // Unknown layers mean no restriction, as in the web client's layerIndexOf.
    internal fun index(layer: String?): Int = SCREEN_LAYERS.indexOf(layer).coerceAtLeast(0)
}

/** Feed only remote screen stats, once per second, on the caller's serialized call loop. */
class DecodeCap {
    private companion object {
        const val DECODE_PAIN = 0.1
        const val DECODE_FINE = 0.02
        const val DECODE_MIN_FRAMES = 10
        const val DECODE_BAD_BEFORE_DOWN = 3
        const val DECODE_CLEAN_BEFORE_UP = 20
        const val DECODE_BACKOFF_MAX = 6
        const val DECODE_FORGET = 5 * 60_000L
        const val DECODE_HOLD = 20_000L
    }

    private class State {
        var dropped = 0L
        var decoded = 0L
        var received = 0L
        var clean = 0
        var bad = 0
        var movedAt = 0L
        var cap: String? = null
        var fails = 0
        var liftedAt = 0L
        var ignoreUntil = 0L
        var sample = "não encontrado"
    }

    private val states = mutableMapOf<String, State>()
    private val transitions = mutableMapOf<String, Long>()

    fun cap(user: String): String? = states[user]?.cap

    fun evidence(user: String): String = states[user]?.sample ?: "não encontrado"

    /** A transition is not a decoder benchmark. Keep the cap, discard the streak. */
    fun suspendJudgment(user: String, now: Long) {
        transitions[user] = now + 5_000L
        states[user]?.let {
            it.ignoreUntil = now + 5_000L
            it.bad = 0
            it.clean = 0
        }
    }

    /** now is milliseconds on one consistent clock; no clock is read inside this logic.
     * As on the web, movedAt starts at zero: times below 20,000 are in the initial hold.
     * Counters are cumulative; the first reading only establishes a baseline.
     */
    fun judge(user: String, dropped: Long?, decoded: Long, received: Long, now: Long,
              eligible: Boolean = true): String? {
        val previous = states[user]
        val state = previous ?: State().also {
            it.ignoreUntil = transitions[user] ?: 0L
            states[user] = it
        }
        val nd = (decoded - state.decoded).coerceAtLeast(0)
        val nr = (received - state.received).coerceAtLeast(0)
        val reset = decoded < state.decoded || received < state.received ||
            (dropped != null && dropped < state.dropped)
        val lost = if (dropped == null) {
            (received - state.received - nd).coerceAtLeast(0)
        } else {
            (dropped - state.dropped).coerceAtLeast(0)
        }
        // Even ignored readings advance the baseline, so old frames cannot dilute the next sample.
        state.dropped = dropped ?: 0L
        state.decoded = decoded
        state.received = received
        state.sample = "deltaDropped=$lost deltaDecoded=$nd deltaReceived=$nr " +
            "dropSource=${if (dropped == null) "received-minus-decoded" else "framesDropped"} " +
            "dropped=${dropped ?: "não encontrado"} decoded=$decoded received=$received atElapsedMs=$now"
        if (!eligible || reset) suspendJudgment(user, now)
        if (previous == null) return state.cap
        if (now < state.ignoreUntil || reset) return state.cap
        val total = lost + nd
        if (total < DECODE_MIN_FRAMES || now - state.movedAt < DECODE_HOLD) return state.cap

        val share = lost.toDouble() / total
        val index = Layers.index(state.cap)
        var next = state.cap
        when {
            share >= DECODE_PAIN -> {
                state.clean = 0
                state.bad += 1
                if (state.bad >= DECODE_BAD_BEFORE_DOWN && index + 1 < Layers.SCREEN_LAYERS.size) {
                    next = Layers.SCREEN_LAYERS[index + 1]
                }
            }
            share <= DECODE_FINE -> {
                state.bad = 0
                state.clean += 1
                val times = (if (state.fails == 0) 1 else state.fails).coerceAtMost(DECODE_BACKOFF_MAX)
                if (state.clean >= DECODE_CLEAN_BEFORE_UP * times && index > 0) {
                    next = if (index == 1) null else Layers.SCREEN_LAYERS[index - 1]
                }
            }
            else -> {
                state.bad = 0
                state.clean = 0
            }
        }
        if (next == state.cap) return state.cap
        state.movedAt = now
        state.clean = 0
        state.bad = 0
        state.cap = next
        if (next != null) {
            // Match the web: every move to a non-null cap increments, including baixa -> media.
            state.fails = if (state.liftedAt > 0 && now - state.liftedAt >= DECODE_FORGET) {
                1
            } else {
                state.fails + 1
            }
        } else {
            state.liftedAt = now
        }
        return state.cap
    }

    // Call when the person's screen lifetime ends, before feeding a new set of counters.
    fun forget(user: String) { states.remove(user); transitions.remove(user) }
}

/** Request memory, used on the same serialized loop as DecodeCap. */
class LayerAsk {
    private val asked = mutableMapOf<String, String>()

    fun want(user: String, widthPx: Int, looking: Boolean, cap: String?): String? {
        val drawn = if (!looking || widthPx < 1) Layers.BAIXA else Layers.forWidth(widthPx)
        val layer = Layers.SCREEN_LAYERS[maxOf(Layers.index(drawn), Layers.index(cap))]
        if (asked[user] == layer) return null
        asked[user] = layer
        return layer
    }

    // A failed request must be retried on the next measurement, even if nothing resized.
    fun failed(user: String) { asked.remove(user) }
    fun forget(user: String) { asked.remove(user) }
}
