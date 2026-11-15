package com.taleshernandes.harmony

import com.taleshernandes.harmony.media.Output
import com.taleshernandes.harmony.media.Output.BLUETOOTH
import com.taleshernandes.harmony.media.Output.EARPIECE
import com.taleshernandes.harmony.media.Output.SPEAKER
import com.taleshernandes.harmony.media.Output.WIRED
import com.taleshernandes.harmony.media.OutputPolicy
import com.taleshernandes.harmony.media.Stand
import com.taleshernandes.harmony.media.Standing
import com.taleshernandes.harmony.model.Peer
import com.taleshernandes.harmony.model.TrackInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** A3: where the sound goes, and where the phone stands in its room. */
class StayTest {
    private val phone = setOf(SPEAKER, EARPIECE)

    @Test
    fun aloneTheLoudspeakerWins() {
        assertEquals(SPEAKER, OutputPolicy().pick(phone))
    }

    @Test
    fun aHeadsetPluggedInMidCallTakesTheSoundAndGivesItBack() {
        val p = OutputPolicy()
        assertEquals(SPEAKER, p.pick(phone))
        assertEquals(WIRED, p.pick(phone + WIRED, phone))
        assertEquals(BLUETOOTH, p.pick(phone + WIRED + BLUETOOTH, phone + WIRED))
        assertEquals(WIRED, p.pick(phone + WIRED, phone + WIRED + BLUETOOTH))
        assertEquals(SPEAKER, p.pick(phone, phone + WIRED))
    }

    @Test
    fun aChoiceByHandSticksThroughUnrelatedChanges() {
        val p = OutputPolicy()
        p.pick(phone + WIRED)
        p.choose(SPEAKER)
        assertEquals(SPEAKER, p.pick(phone + WIRED))
        // Bluetooth dropping out is not a reason to take the choice away.
        assertEquals(SPEAKER, p.pick(phone + WIRED, phone + WIRED + BLUETOOTH))
        p.choose(EARPIECE)
        assertEquals(EARPIECE, p.pick(phone))
    }

    @Test
    fun aHeadsetArrivingOverridesTheChoice() {
        val p = OutputPolicy()
        p.choose(EARPIECE)
        assertEquals(EARPIECE, p.pick(phone))
        assertEquals(BLUETOOTH, p.pick(phone + BLUETOOTH, phone))
        assertNull(p.manual)
    }

    @Test
    fun aChosenOutputThatLeavesFallsBackToTheRule() {
        val p = OutputPolicy()
        p.pick(phone + BLUETOOTH)
        p.choose(BLUETOOTH)
        assertEquals(SPEAKER, p.pick(phone, phone + BLUETOOTH))
        assertNull(p.manual)
    }

    @Test
    fun nothingPresentIsNothing() {
        assertNull(OutputPolicy().pick(emptySet<Output>()))
    }

    private fun peer(sid: String, user: String, joinedAt: Long = 0) =
        Peer(sid, user, user, muted = false, deafened = false, joinedAt = joinedAt, publishing = emptyList())

    private fun mic(mid: String, user: String) = TrackInfo(mid, user, user, "audio", "mic")

    private val now = 1_000_000L

    @Test
    fun insideAndHearingEverybodyIsInStep() {
        val rooms = mapOf("r" to listOf(peer("S1", "tales"), peer("S2", "sol")))
        assertEquals(Standing.IN_STEP, Stand.of("S1", "tales", "r", rooms, listOf(mic("1", "sol")), now))
    }

    @Test
    fun somebodyWithoutASlotForAWhileMeansAnOfferWasLost() {
        val rooms = mapOf("r" to listOf(peer("S1", "tales"), peer("S2", "sol"), peer("S3", "beto", joinedAt = now - 60_000)))
        assertEquals(Standing.BEHIND, Stand.of("S1", "tales", "r", rooms, listOf(mic("1", "sol")), now))
    }

    @Test
    fun aNewcomerGetsTimeBeforeTheirSlotIsMissed() {
        // The server renegotiates once the newcomer's sound actually arrives.
        val rooms = mapOf("r" to listOf(peer("S1", "tales"), peer("S3", "beto", joinedAt = now - 2_000)))
        assertEquals(Standing.IN_STEP, Stand.of("S1", "tales", "r", rooms, emptyList(), now))
    }

    @Test
    fun aScreenSoundIsNotSomebodysVoice() {
        val rooms = mapOf("r" to listOf(peer("S1", "tales"), peer("S2", "sol")))
        val screen = TrackInfo("3", "sol", "sol", "audio", "screen_audio")
        assertEquals(Standing.BEHIND, Stand.of("S1", "tales", "r", rooms, listOf(screen), now))
    }

    @Test
    fun goneFromTheListAndNowhereElseIsAFall() {
        val rooms = mapOf("r" to listOf(peer("S2", "sol")))
        assertEquals(Standing.DROPPED, Stand.of("S1", "tales", "r", rooms, emptyList(), now))
        assertEquals(Standing.DROPPED, Stand.of("S1", "tales", "r", emptyMap(), emptyList(), now))
    }

    @Test
    fun theRoomSignatureSeesComingsGoingsAndShowingButNotMute() {
        val base = listOf(peer("S1", "tales"), peer("S2", "sol"))
        val sig = Stand.signature(base, "tales")
        assertEquals(sig, Stand.signature(listOf(peer("S2", "sol").copy(muted = true), peer("S1", "tales")), "tales"))
        // Before the phone knows its own session id it is already on the list.
        assertEquals(sig, Stand.signature(listOf(peer("S2", "sol")), "tales"))
        assertEquals(false, sig == Stand.signature(base + peer("S3", "beto"), "tales"))
        assertEquals(false, sig == Stand.signature(listOf(peer("S2", "sol").copy(publishing = listOf("screen"))), "tales"))
        // Sol leaving and coming back is a new session, and a new offer.
        assertEquals(false, sig == Stand.signature(listOf(peer("S7", "sol")), "tales"))
    }

    @Test
    fun theSameAccountUnderAnotherSessionIsElsewhere() {
        // In the same room, from the PC...
        val same = mapOf("r" to listOf(peer("S9", "tales"), peer("S2", "sol")))
        assertEquals(Standing.ELSEWHERE, Stand.of("S1", "tales", "r", same, emptyList(), now))
        // ...or in another room altogether.
        val other = mapOf("r" to listOf(peer("S2", "sol")), "q" to listOf(peer("S9", "tales")))
        assertEquals(Standing.ELSEWHERE, Stand.of("S1", "tales", "r", other, emptyList(), now))
    }
}
