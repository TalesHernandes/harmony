package com.taleshernandes.harmony.media

import com.taleshernandes.harmony.model.Peer
import com.taleshernandes.harmony.model.TrackInfo

/**
 * Where this phone stands in its room, read from the server's list of who is
 * inside (GET /api/voice/rooms, or a voice_room event).
 *
 * The server drops a session whose connection has been silent for five
 * seconds (sfu.go SetICETimeouts), and it drops it just the same when the
 * account joins from another device (sfu.Join, LeaveAllFor). The first is a
 * fall to recover from; the second must not be answered by joining again, or
 * the two devices would take the room from each other forever. The list tells
 * them apart: after the second, this person is inside somewhere under another
 * session.
 */
enum class Standing {
    /** Inside, and every sound in the room has a slot here. */
    IN_STEP,

    /** Inside, but somebody who has been talking a while has no slot: an offer went missing. */
    BEHIND,

    /** Not inside, and not anywhere else: the server let go of this connection. */
    DROPPED,

    /** Inside under another session: the same account joined from another device. */
    ELSEWHERE,
}

object Stand {
    /** How long a newcomer may go without a slot here before that means an offer was lost. */
    const val SETTLE_MS = 15_000L

    /**
     * Everybody else in the room and what each is sending, as one string: two
     * that differ mean the room changed. Mute is left out — it changes no
     * offer. The person's own entries are left out by name, since this
     * phone's own session is on the list before it knows its id (the server
     * announces the room before answering the join).
     */
    fun signature(peers: List<Peer>, me: String): String =
        peers.filter { it.username != me }
            .map { "${it.sessionId}:${it.publishing.sorted().joinToString(",")}" }
            .sorted()
            .joinToString(" ")

    fun of(
        mySession: String,
        me: String,
        channelId: String,
        rooms: Map<String, List<Peer>>,
        tracks: Collection<TrackInfo>,
        now: Long,
    ): Standing {
        val room = rooms[channelId].orEmpty()
        if (room.none { it.sessionId == mySession }) {
            val other = rooms.values.flatten().any { it.username == me && it.sessionId != mySession }
            return if (other) Standing.ELSEWHERE else Standing.DROPPED
        }
        val heard = tracks.filter { it.kind == "audio" && it.source == "mic" }.map { it.username }.toSet()
        val missing = room.any { p ->
            p.username != me && p.username !in heard && now - p.joinedAt > SETTLE_MS
        }
        return if (missing) Standing.BEHIND else Standing.IN_STEP
    }
}
