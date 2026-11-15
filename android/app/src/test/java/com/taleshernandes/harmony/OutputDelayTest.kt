package com.taleshernandes.harmony

import com.taleshernandes.harmony.media.OutputDelay
import org.junit.Assert.*
import org.junit.Test

class OutputDelayTest {
    @Test fun measuredSpeakerDelayIsCompensated() {
        val p = OutputDelay()
        // The caller no longer supplies a route exclusion: 199 ms is the owner's speaker reading.
        assertEquals(124, p.sample(199, true, 1))
        assertEquals(124, p.exactMs)
        assertEquals(124, p.desiredMs)
        p.complete(124, true)
        assertEquals(124, p.sentMs)
    }

    @Test fun subtractsAdmEstimateAndClamps() {
        for (latency in listOf(0, 74, 75, 95)) assertNull(OutputDelay().sample(latency, true, 1))
        assertEquals(100, OutputDelay().sample(175, true, 1))
        assertEquals(500, OutputDelay().sample(Int.MAX_VALUE, true, 1))
    }

    @Test fun smoothsOnlyFreshSamplesAndRequiresMoreThanTwenty() {
        val p = OutputDelay()
        assertEquals(100, p.sample(175, true, 1))
        p.complete(100, true)
        assertNull(p.sample(215, true, 2)) // Filtered 120: exactly 20.
        repeat(10) { assertNull(p.sample(215, true, 2)) }
        assertEquals(130, p.sample(215, true, 3))
        p.complete(130, true)
        assertEquals(65, p.sample(75, true, 4))
    }

    @Test fun failuresRetryAndPendingRequestsDoNotRaceResets() {
        val p = OutputDelay()
        assertEquals(100, p.sample(175, true, 1))
        assertNull(p.sample(-1, true, 2))
        p.complete(100, true)
        assertEquals(0, p.sample(-1, true, 2))
        p.complete(0, false)
        assertEquals(100, p.sentMs)
        assertEquals(0, p.sample(-1, true, 2))
        p.complete(0, true)
        assertNull(p.sample(-1, true, 3))
    }

    @Test fun missingMeasurementAndStoppedWatchResetImmediately() {
        for (reason in 0..1) {
            val p = OutputDelay()
            assertEquals(21, p.sample(96, true, 1))
            p.complete(21, true)
            assertEquals(0, p.sample(if (reason == 0) -1 else 500, reason != 1, 2))
            p.complete(0, true)
            assertEquals(100, p.sample(175, true, 3))
        }
    }

    @Test fun theCommandIsTheTargetAloneOnEveryRoute() {
        val p = OutputDelay()
        // Headphones at 325 ms, then off, then the speaker's own 199 ms.
        assertEquals(250, p.sample(325, true, 1))
        p.complete(250, true)
        assertEquals(0, p.sample(-1, true, 2))
        p.complete(0, true)
        assertEquals(124, p.sample(199, true, 3))
        p.complete(124, true)
        assertNull(p.sample(199, true, 3))
        assertEquals(OutputDelay.MAX_TARGET_MS, OutputDelay().sample(4_000, true, 1))
    }

    @Test fun lostHttpResponseStillRequiresResetEvenWithoutAcknowledgedDelay() {
        val p = OutputDelay()
        assertEquals(100, p.sample(175, true, 1))
        p.complete(100, false) // The server may have applied it before the response was lost.
        assertEquals(0, p.sentMs)
        assertEquals(0, p.sample(-1, true, 2))
        p.complete(0, true)
        assertNull(p.sample(-1, true, 3))
    }
}
