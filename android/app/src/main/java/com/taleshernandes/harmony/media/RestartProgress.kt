package com.taleshernandes.harmony.media

/** Recovery uses fresh inbound RTP counters, never an ICE state transition. */
object RestartProgress {
    data class Counters(val bytes: Long, val packets: Long)
    enum class Decision { COMPLETE, RETRY, REJOIN }

    fun growing(before: Map<String, Counters>?, after: Map<String, Counters>): Boolean =
        before != null && after.any { (id, now) ->
            before[id]?.let { now.bytes > it.bytes || now.packets > it.packets } == true
        }

    // Pending requests can be cancelled when ICE recovers on its own too.
    fun decide(connected: Boolean, mediaGrowing: Boolean, expired: Boolean): Decision = when {
        mediaGrowing && (connected || expired) -> Decision.COMPLETE
        expired -> Decision.REJOIN
        else -> Decision.RETRY
    }

    fun suppressNetworkTrigger(now: Long, completedAt: Long, iceBroken: Boolean): Boolean =
        !iceBroken && completedAt > 0 && now - completedAt < 2000
}
