package com.taleshernandes.harmony

import com.taleshernandes.harmony.media.RestartProgress
import com.taleshernandes.harmony.media.RestartProgress.Counters
import com.taleshernandes.harmony.media.RestartProgress.Decision
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RestartProgressTest {
    @Test fun connectedWithoutAnyStateTransitionCompletesWithNewMedia() {
        assertEquals(Decision.COMPLETE, RestartProgress.decide(true, true, false))
        assertEquals(Decision.RETRY, RestartProgress.decide(true, false, false))
    }

    @Test fun deadlineNeverRejoinsWhileMediaArrives() {
        assertEquals(Decision.COMPLETE, RestartProgress.decide(true, true, true))
        assertEquals(Decision.COMPLETE, RestartProgress.decide(false, true, true))
        assertEquals(Decision.REJOIN, RestartProgress.decide(true, false, true))
        assertEquals(Decision.REJOIN, RestartProgress.decide(false, false, true))
    }

    @Test fun countersNeedTwoSamplesOfTheSameStream() {
        val before = mapOf("audio" to Counters(100, 2))
        assertFalse(RestartProgress.growing(null, before))
        assertFalse(RestartProgress.growing(before, before))
        assertFalse(RestartProgress.growing(before, mapOf("new" to Counters(500, 10))))
        assertFalse(RestartProgress.growing(before, mapOf("audio" to Counters(0, 0))))
        assertTrue(RestartProgress.growing(before, mapOf("audio" to Counters(101, 2))))
        assertTrue(RestartProgress.growing(before, mapOf("audio" to Counters(100, 3))))
    }

    @Test fun recentCompletionIgnoresNetworkHintsButNotIceFailure() {
        assertTrue(RestartProgress.suppressNetworkTrigger(2500, 1000, false))
        assertFalse(RestartProgress.suppressNetworkTrigger(2500, 1000, true))
        assertFalse(RestartProgress.suppressNetworkTrigger(3000, 1000, false))
        assertFalse(RestartProgress.suppressNetworkTrigger(1000, 0, false))
    }
}
