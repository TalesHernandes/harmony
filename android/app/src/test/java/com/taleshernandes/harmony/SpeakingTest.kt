package com.taleshernandes.harmony

import com.taleshernandes.harmony.media.Speaking
import org.junit.Assert.*
import org.junit.Test

class SpeakingTest {
    @Test fun quietSpeechAttacksImmediatelyAndReleasesAfter300ms() {
        var last: Long? = null
        val samples = listOf(
            Triple(0L, 0.0, false), Triple(25L, 0.0009, false), Triple(50L, 0.001, false),
            Triple(75L, 0.0011, true), Triple(100L, 0.0, true), Triple(374L, 0.0, true),
            Triple(375L, 0.0, false), Triple(400L, 0.002, true), Triple(650L, 0.002, true),
            Triple(949L, 0.0, true), Triple(950L, 0.0, false),
        )
        for ((at, level, on) in samples) {
            last = Speaking.sample(last, level, at)
            assertEquals("sample at $at ms", on, last != null)
        }
    }

    @Test fun invalidReadingsDoNotLightOrRenewTheHold() {
        assertNull(Speaking.sample(null, Double.NaN, 0))
        assertNull(Speaking.sample(null, Double.POSITIVE_INFINITY, 0))
        assertEquals(0L, Speaking.sample(0L, Double.NaN, 299))
        assertNull(Speaking.sample(0L, Double.NaN, 300))
    }

    @Test fun defaultMeterUsesLowFloorAndExpiresIndependently() {
        val s = Speaking()
        s.feed("ana", 0.0011, 0)
        s.feed("bru", 0.002, 200)
        assertEquals(setOf("ana", "bru"), s.talking(299))
        assertEquals(setOf("bru"), s.talking(300))
        assertTrue(s.talking(500).isEmpty())
    }
}
