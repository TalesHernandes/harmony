package com.taleshernandes.harmony

import com.taleshernandes.harmony.media.DirectControls
import com.taleshernandes.harmony.media.ScreenChoice
import com.taleshernandes.harmony.media.Speaking
import org.junit.Assert.*
import org.junit.Test

class DirectControlsTest {
    @Test fun hearingRestoresBothMicrophoneChoices() {
        for (muted in listOf(false, true)) {
            val before = DirectControls(muted = muted)
            val deaf = before.deafen(true)
            assertTrue(deaf.muted)
            assertTrue(deaf.deafened)
            assertEquals(deaf, deaf.mute(false))
            assertEquals(deaf, deaf.deafen(true))
            assertEquals(muted, deaf.deafen(false).muted)
            assertFalse(deaf.deafen(false).deafened)
        }
    }

    @Test fun stoppedCaptureRejectsLateServiceAndProjectionCallbacks() {
        val choice = ScreenChoice()
        val old = choice.choose(true)
        assertTrue(choice.accepts(old))
        choice.choose(false)
        assertFalse(choice.accepts(old))
        val fresh = choice.choose(true)
        assertFalse(choice.accepts(old))
        assertTrue(choice.accepts(fresh))
    }

    @Test fun microphoneAndReceivedVoiceHaveIndependentRings() {
        val meter = Speaking()
        meter.feed("local", .002, 1000)
        meter.feed("peer", .001, 1000)
        assertEquals(setOf("local"), meter.talking(1000))
        meter.feed("peer", .02, 1100)
        assertEquals(setOf("local", "peer"), meter.talking(1299))
        assertEquals(setOf("peer"), meter.talking(1300))
        assertTrue(meter.talking(1400).isEmpty())
        meter.feed("local", .1, 1500)
        meter.clear()
        assertTrue(meter.talking(1500).isEmpty())
    }
}
