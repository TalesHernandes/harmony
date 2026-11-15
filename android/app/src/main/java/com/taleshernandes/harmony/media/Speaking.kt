package com.taleshernandes.harmony.media

/**
 * Who is talking, from volume readings. The web's rule (media.ts readMeters),
 * which the owner tuned by ear: fast to light up, slow to go dark. Speech is
 * mostly gaps between syllables, so a person stays lit until they have been
 * quiet for [holdMs] — longer than a gap inside a sentence, shorter than the
 * pause between two.
 *
 * [floor] is linear RMS: 0.001 (-60 dBFS), matching the web's float samples.
 * The denoiser's residual floor has not been measured on the owner's device.
 *
 * Each reading has one owner: this class is fed from one poll and read by the
 * screen, never fed from two places (the "dois donos" lesson, 20/09/2026).
 */
class Speaking(private val floor: Double = 0.001, private val holdMs: Long = 300) {
    private val lastLoud = HashMap<String, Long>()

    /** One reading for one person. Quiet readings change nothing: silence is the hold running out. */
    @Synchronized
    fun feed(username: String, level: Double, now: Long) {
        if (username.isNotEmpty()) {
            sample(lastLoud[username], level, now, floor, holdMs)?.let { lastLoud[username] = it }
        }
    }

    /** Everybody heard within the hold, as of [now]. */
    @Synchronized
    fun talking(now: Long): Set<String> {
        lastLoud.entries.removeAll { sample(it.value, 0.0, now, floor, holdMs) == null }
        return lastLoud.keys.toSet()
    }

    @Synchronized
    fun clear() = lastLoud.clear()

    companion object {
        /** Pure sample rule, identical to speakingSample in the web client. */
        fun sample(lastAbove: Long?, level: Double, now: Long, floor: Double = 0.001, holdMs: Long = 300): Long? =
            if (level.isFinite() && level > floor) now
            else lastAbove?.takeIf { now - it < holdMs }
    }
}
