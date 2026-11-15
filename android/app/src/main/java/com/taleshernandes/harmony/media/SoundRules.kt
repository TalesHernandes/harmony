package com.taleshernandes.harmony.media

import com.taleshernandes.harmony.model.Peer

/** The harmony's own short sounds (sons/LISTA.md, chosen by the owner on 29/10/2026). */
enum class Sound { ENTRAR, SAIR, MUTAR, DESMUTAR, TELA_COMECOU, TELA_ACABOU }

/**
 * When each sound is heard: the same rules as the web (sounds.ts), kept apart
 * from the player so they can be tested without a phone.
 *
 * [later] runs a block after a delay and returns what cancels it; [now] is the
 * clock. Every call comes from one thread (Sounds' own).
 */
class SoundRules(
    private val play: (Sound) -> Unit,
    private val now: () -> Long,
    private val later: (Long, () -> Unit) -> (() -> Unit),
) {
    private val lastPlayed = HashMap<Sound, Long>()

    // The same sound twice inside 150 ms is one sound: three people arriving
    // with one room update is a chord, not a burst of identical pops.
    private fun sound(s: Sound) {
        val t = now()
        if (t - (lastPlayed[s] ?: Long.MIN_VALUE / 2) < 150) return
        lastPlayed[s] = t
        play(s)
    }

    // ------------------------------------------------------------ the room

    private class Seen(val channelId: String, val people: Set<String>, val screens: Set<String>)

    private var seen: Seen? = null
    private val screenGoneAt = HashMap<String, Long>()
    private val screenEnding = HashMap<String, () -> Unit>()

    /**
     * Every list of who is in this phone's room. While not live (quiet) it
     * only sets the baseline, and so does the first list of a room: everybody
     * already there is not arriving. Kept by username: a connection that
     * blinks comes back with a new session, and that is not a leave and a join.
     */
    fun room(channelId: String, peers: List<Peer>, me: String, quiet: Boolean) {
        val others = peers.filter { it.username != me }
        val people = others.map { it.username }.toSet()
        val screens = others.filter { it.sharingScreen }.map { it.username }.toSet()
        val before = seen
        seen = Seen(channelId, people, screens)
        if (quiet || before == null || before.channelId != channelId) return
        if (people.any { it !in before.people }) sound(Sound.ENTRAR)
        if (before.people.any { it !in people }) sound(Sound.SAIR)
        val t = now()
        var started = false
        for (u in screens) {
            if (u in before.screens) continue
            screenEnding.remove(u)?.invoke()
            // A screen flag blinks while sharing is set up or torn down.
            if (t - (screenGoneAt[u] ?: Long.MIN_VALUE / 2) >= SCREEN_GONE_MS) started = true
        }
        if (started) sound(Sound.TELA_COMECOU)
        for (u in before.screens) {
            if (u in screens) continue
            screenGoneAt[u] = t
            // Heard by the whole room, like its start (owner, 29/10/2026); a
            // person who left with the screen on is heard leaving instead.
            if (u !in people) continue
            screenEnding.remove(u)?.invoke()
            screenEnding[u] = later(SCREEN_END_MS) {
                screenEnding.remove(u)
                val s = seen
                if (s != null && s.channelId == channelId && u !in s.screens) sound(Sound.TELA_ACABOU)
            }
        }
    }

    private fun roomForget() {
        seen = null
        screenEnding.values.forEach { it() }
        screenEnding.clear()
        screenGoneAt.clear()
    }

    // ------------------------------------------------------------ this phone in a room

    private var own: VoiceCall.State? = null
    private var lost: (() -> Unit)? = null
    // Coming back goes RECONNECTING -> LIVE: recovering says it is a return,
    // and lostHeard whether the drop was long enough to have been heard.
    private var recovering = false
    private var lostHeard = false

    /**
     * The join is heard at the touch, not when the connection is ready (owner,
     * 29/10/2026); the others hear it when this phone shows up in their list.
     */
    fun voice(s: VoiceCall.State) {
        val before = own
        own = s
        if (before == null) return
        val off = VoiceCall.Phase.OFF
        val live = VoiceCall.Phase.LIVE
        if (before.phase == off && s.phase != off) {
            recovering = false
            lostHeard = false
            sound(Sound.ENTRAR)
        }
        if (s.phase == live && before.phase != live && before.phase != off) {
            lost?.invoke()
            lost = null
            // Back after a drop long enough to have been heard.
            if (recovering && lostHeard) sound(Sound.ENTRAR)
            recovering = false
            lostHeard = false
        }
        if (s.phase == VoiceCall.Phase.RECONNECTING && before.phase == live) {
            recovering = true
            lost?.invoke()
            lost = later(LOST_AFTER_MS) {
                lost = null
                if (own?.phase != live) {
                    lostHeard = true
                    sound(Sound.SAIR)
                }
            }
        }
        if (s.phase == off && before.phase != off) {
            lost?.invoke()
            lost = null
            // Leaving after the drop was already heard is not a second goodbye.
            if (!(recovering && lostHeard)) sound(Sound.SAIR)
            recovering = false
            lostHeard = false
            roomForget()
        }
        // Its own screen, heard by whoever shares it too (owner, 29/10/2026).
        if (s.phase != off && before.phase != off && s.screen != before.screen) {
            sound(if (s.screen) Sound.TELA_COMECOU else Sound.TELA_ACABOU)
        }
        if (s.phase == live && before.phase == live) toggles(before.muted, before.deafened, s.muted, s.deafened)
    }

    private fun toggles(wasMuted: Boolean, wasDeaf: Boolean, muted: Boolean, deaf: Boolean) {
        if (deaf != wasDeaf) sound(if (deaf) Sound.MUTAR else Sound.DESMUTAR)
        else if (muted != wasMuted) sound(if (muted) Sound.MUTAR else Sound.DESMUTAR)
    }

    // ------------------------------------------------------------ the direct call

    private var direct: DirectCall.State? = null

    /** What the ring should be now: null for silence, true for the caller's quieter one. */
    var ringback: Boolean? = null
        private set

    fun direct(s: DirectCall.State) {
        val before = direct
        direct = s
        // The phone being called rings through its notification (push/Ringing);
        // the one calling hears the same ring, lower, from here.
        ringback = if (s.phase == DirectCall.Phase.CALLING) true else null
        if (before == null) return
        if (s.since != 0L && before.since == 0L) sound(Sound.ENTRAR)
        if (s.phase == DirectCall.Phase.IDLE && before.phase != DirectCall.Phase.IDLE) {
            when {
                // They picked up and it ended: a goodbye.
                before.since != 0L -> sound(Sound.SAIR)
                // Refused or nobody answered, as the caller sees it. Putting
                // down one's own call is said by no notice and makes no sound.
                before.phase == DirectCall.Phase.CALLING && s.notice != null -> sound(Sound.TELA_ACABOU)
            }
        }
        if (s.since != 0L && before.since != 0L && s.phase != DirectCall.Phase.IDLE) {
            toggles(before.muted, before.deafened, s.muted, s.deafened)
        }
    }

    companion object {
        const val SCREEN_GONE_MS = 1000L
        const val SCREEN_END_MS = 1000L
        const val LOST_AFTER_MS = 1200L
    }
}
