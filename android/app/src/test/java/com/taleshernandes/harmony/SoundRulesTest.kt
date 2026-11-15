package com.taleshernandes.harmony

import com.taleshernandes.harmony.media.DirectCall
import com.taleshernandes.harmony.media.Sound
import com.taleshernandes.harmony.media.SoundRules
import com.taleshernandes.harmony.media.VoiceCall
import com.taleshernandes.harmony.model.Peer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** When each of the harmony's sounds is heard, on a clock the test turns by hand. */
class SoundRulesTest {
    private var clock = 10_000L
    private val heard = mutableListOf<Sound>()
    private val timers = mutableListOf<Pair<Long, () -> Unit>>()
    private val rules = SoundRules(
        play = { heard += it },
        now = { clock },
        later = { ms, block ->
            val t = clock + ms to block
            timers += t
            ({ timers.remove(t) })
        },
    )

    private fun pass(ms: Long) {
        clock += ms
        timers.filter { it.first <= clock }.forEach { timers.remove(it); it.second() }
    }

    private fun peer(u: String, screen: Boolean = false) =
        Peer("s-$u", u, u, muted = false, deafened = false, joinedAt = 0, publishing = if (screen) listOf("screen") else emptyList())

    private fun voice(phase: VoiceCall.Phase, muted: Boolean = false, deafened: Boolean = false, screen: Boolean = false) =
        VoiceCall.State(channelId = if (phase == VoiceCall.Phase.OFF) null else "sala", phase = phase, muted = muted, deafened = deafened, screen = screen)

    private fun room(vararg peers: Peer, quiet: Boolean = false) {
        rules.room("sala", listOf(peer("eu")) + peers, "eu", quiet)
        pass(200) // past the 150 ms that makes two equal sounds one
    }

    @Test
    fun joiningAndLeavingAreHeardOnce() {
        rules.voice(voice(VoiceCall.Phase.OFF))
        rules.voice(voice(VoiceCall.Phase.JOINING))
        // Heard at the touch, before the connection is up.
        assertEquals(listOf(Sound.ENTRAR), heard)
        room(peer("ana"), quiet = true) // already there: not arriving
        rules.voice(voice(VoiceCall.Phase.LIVE))
        room(peer("ana"))
        room(peer("ana"), peer("beto"))
        room(peer("beto"))
        rules.voice(voice(VoiceCall.Phase.OFF))
        assertEquals(listOf(Sound.ENTRAR, Sound.ENTRAR, Sound.SAIR, Sound.SAIR), heard)
    }

    @Test
    fun muteAndDeafen() {
        rules.voice(voice(VoiceCall.Phase.OFF))
        rules.voice(voice(VoiceCall.Phase.LIVE))
        pass(200)
        rules.voice(voice(VoiceCall.Phase.LIVE, muted = true)); pass(200)
        rules.voice(voice(VoiceCall.Phase.LIVE, muted = false)); pass(200)
        // Deafening also mutes: one sound, not two.
        rules.voice(voice(VoiceCall.Phase.LIVE, muted = true, deafened = true)); pass(200)
        rules.voice(voice(VoiceCall.Phase.LIVE)); pass(200)
        assertEquals(listOf(Sound.ENTRAR, Sound.MUTAR, Sound.DESMUTAR, Sound.MUTAR, Sound.DESMUTAR), heard)
    }

    @Test
    fun aBlinkOfTheConnectionIsSilentAndALongDropIsHeard() {
        rules.voice(voice(VoiceCall.Phase.OFF))
        rules.voice(voice(VoiceCall.Phase.LIVE))
        heard.clear()
        rules.voice(voice(VoiceCall.Phase.RECONNECTING)); pass(500)
        rules.voice(voice(VoiceCall.Phase.LIVE)); pass(2000)
        assertEquals(emptyList<Sound>(), heard)
        rules.voice(voice(VoiceCall.Phase.RECONNECTING)); pass(1300)
        rules.voice(voice(VoiceCall.Phase.LIVE))
        assertEquals(listOf(Sound.SAIR, Sound.ENTRAR), heard)
    }

    @Test
    fun aScreenThatBlinksStartsOnceAndItsEndIsHeardByTheWholeRoom() {
        rules.voice(voice(VoiceCall.Phase.OFF))
        rules.voice(voice(VoiceCall.Phase.LIVE))
        room(peer("ana"))
        heard.clear()
        room(peer("ana", screen = true))
        room(peer("ana")) // a blink while sharing is set up
        room(peer("ana", screen = true))
        assertEquals(listOf(Sound.TELA_COMECOU), heard)
        room(peer("ana"))
        pass(1500)
        assertEquals(listOf(Sound.TELA_COMECOU, Sound.TELA_ACABOU), heard)
        // Leaving with the screen on is a leave, not a screen ending.
        room(peer("ana", screen = true))
        room()
        pass(1500)
        assertEquals(listOf(Sound.TELA_COMECOU, Sound.TELA_ACABOU, Sound.TELA_COMECOU, Sound.SAIR), heard)
    }

    @Test
    fun oneOwnScreenIsHeardByWhoSharesIt() {
        rules.voice(voice(VoiceCall.Phase.OFF))
        rules.voice(voice(VoiceCall.Phase.LIVE)); pass(200)
        rules.voice(voice(VoiceCall.Phase.LIVE, screen = true)); pass(200)
        rules.voice(voice(VoiceCall.Phase.LIVE)); pass(200)
        // Leaving while sharing is one goodbye, not a screen ending too.
        rules.voice(voice(VoiceCall.Phase.LIVE, screen = true)); pass(200)
        rules.voice(voice(VoiceCall.Phase.OFF))
        assertEquals(listOf(Sound.ENTRAR, Sound.TELA_COMECOU, Sound.TELA_ACABOU, Sound.TELA_COMECOU, Sound.SAIR), heard)
    }

    @Test
    fun theDirectCallRingsBackAnswersAndEnds() {
        rules.direct(DirectCall.State())
        rules.direct(DirectCall.State(phase = DirectCall.Phase.CALLING, outgoing = true))
        assertEquals(true, rules.ringback)
        rules.direct(DirectCall.State(phase = DirectCall.Phase.CONNECTING, outgoing = true))
        assertNull(rules.ringback)
        rules.direct(DirectCall.State(phase = DirectCall.Phase.LIVE, since = 1)); pass(200)
        rules.direct(DirectCall.State(phase = DirectCall.Phase.LIVE, since = 1, muted = true)); pass(200)
        rules.direct(DirectCall.State(notice = "Chamada encerrada."))
        assertEquals(listOf(Sound.ENTRAR, Sound.MUTAR, Sound.SAIR), heard)
    }

    @Test
    fun aRefusedCallIsHeardAndPuttingItDownIsNot() {
        rules.direct(DirectCall.State())
        rules.direct(DirectCall.State(phase = DirectCall.Phase.CALLING, outgoing = true))
        rules.direct(DirectCall.State(notice = "Chamada recusada."))
        assertEquals(listOf(Sound.TELA_ACABOU), heard)
        pass(200)
        rules.direct(DirectCall.State(phase = DirectCall.Phase.CALLING, outgoing = true))
        rules.direct(DirectCall.State())
        // Rung here and missed: the notification rang, nothing more.
        rules.direct(DirectCall.State(phase = DirectCall.Phase.RINGING))
        rules.direct(DirectCall.State(notice = "Chamada perdida de ana."))
        assertEquals(listOf(Sound.TELA_ACABOU), heard)
    }
}
