package com.taleshernandes.harmony.media

/** Small snapshots from the existing stats poll; no frames or tracks enter UI state. */
data class ScreenProgress(val frames: Long = -1, val changedAt: Long = 0, val portrait: Boolean = false) {
    fun sample(count: Long, width: Int, height: Int, now: Long): ScreenProgress = copy(
        frames = count,
        changedAt = if (count != frames) now else changedAt,
        portrait = if (width > 0 && height > 0) height > width else portrait,
    )

    fun stalled(now: Long, startedAt: Long): Boolean = now - (changedAt.takeIf { it > 0 } ?: startedAt) >= 2_000
}
