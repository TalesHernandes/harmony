package com.taleshernandes.harmony

import com.taleshernandes.harmony.media.DecodeCap
import com.taleshernandes.harmony.media.LayerAsk
import com.taleshernandes.harmony.media.Layers
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class LayersTest {
    private class Feed(val caps: DecodeCap = DecodeCap(), val user: String = "alice") {
        var now = 1_000_000L
        private var dropped = 0L
        private var decoded = 0L
        private var received = 0L

        init { caps.judge(user, 0, 0, 0, now) }

        fun sample(lost: Long = 0, done: Long = 100, elapsed: Long = 1000, missing: Boolean = false): String? {
            now += elapsed
            dropped += lost
            decoded += done
            received += lost + done
            return caps.judge(user, if (missing) null else dropped, decoded, received, now)
        }

        fun down(): String? {
            sample(10, 90)
            sample(10, 90)
            return sample(10, 90)
        }

        fun lift(clean: Int): String? {
            sample(elapsed = 20_000)
            repeat(clean - 1) { sample() }
            return caps.cap(user)
        }
    }

    @Test
    fun physicalPixelBoundariesAndLayerOrder() {
        // media.ts:456-460, 915-922: Android supplies the already converted physical width.
        assertEquals(listOf("alta", "media", "baixa"), Layers.SCREEN_LAYERS)
        assertEquals("baixa", Layers.forWidth(599))
        assertEquals("media", Layers.forWidth(600))
        assertEquals("media", Layers.forWidth(1199))
        assertEquals("alta", Layers.forWidth(1200))
    }

    @Test
    fun baselineThenThreeBadReadings() {
        // media.ts:939-957, 973-978: cumulative counters need a baseline, then three bad samples.
        val caps = DecodeCap()
        assertNull(caps.judge("alice", 100, 900, 1000, 1_000_000))
        assertNull(caps.judge("alice", 110, 990, 1100, 1_001_000))
        assertNull(caps.judge("alice", 120, 1080, 1200, 1_002_000))
        assertEquals("media", caps.judge("alice", 130, 1170, 1300, 1_003_000))
    }

    @Test
    fun holdDiscardsBadReadingsAndStartsCountingAtTwentySeconds() {
        // media.ts:524, 968, 996-998: samples inside the hold do not build a streak.
        val f = Feed()
        assertEquals("media", f.down())
        repeat(19) { assertEquals("media", f.sample(10, 90)) }
        assertEquals("media", f.sample(10, 90))
        assertEquals("media", f.sample(10, 90))
        assertEquals("baixa", f.sample(10, 90))
        repeat(30) { assertEquals("baixa", f.sample(10, 90)) }
    }

    @Test
    fun holdDiscardsCleanReadingsAndTopBecomesNoCap() {
        // media.ts:968, 979-989: twenty eligible clean samples, not twenty since the move.
        val f = Feed()
        f.down()
        repeat(19) { assertEquals("media", f.sample()) }
        repeat(19) { assertEquals("media", f.sample()) }
        assertNull(f.sample())
        assertNull(f.caps.cap(f.user))
    }

    @Test
    fun secondCapDoublesRecoveryAndLaterCapsGrowLinearlyToSix() {
        // media.ts:507, 515, 984-985, 1002-1006: multipliers 1,2,3,...,6,6 (not exponential).
        val f = Feed()
        for (failure in 1..7) {
            if (failure > 1) f.sample(10, 90, elapsed = 20_000)
            if (failure == 1) {
                assertEquals("media", f.down())
            } else {
                assertNull(f.sample(10, 90))
                assertEquals("media", f.sample(10, 90))
            }
            val required = 20 * minOf(failure, 6)
            assertEquals("media", f.sample(elapsed = 20_000))
            repeat(required - 2) { assertEquals("media", f.sample()) }
            assertNull(f.sample())
        }
    }

    @Test
    fun movingUpToMediaAlsoIncrementsBackoff() {
        // media.ts:985-1006: any new nonempty cap increments fails, even baixa -> media.
        val f = Feed()
        f.down()
        f.sample(10, 90, elapsed = 20_000)
        f.sample(10, 90)
        assertEquals("baixa", f.sample(10, 90))
        assertEquals("baixa", f.sample(elapsed = 20_000))
        repeat(38) { assertEquals("baixa", f.sample()) }
        assertEquals("media", f.sample())
        assertEquals("media", f.sample(elapsed = 20_000))
        repeat(58) { assertEquals("media", f.sample()) }
        assertNull(f.sample())
    }

    @Test
    fun backoffIsForgottenAtFiveMinutesWithoutCap() {
        // media.ts:519, 1001-1010: the boundary is measured from lifting, at the next move.
        for (away in listOf(299_999L, 300_000L)) {
            val f = Feed()
            f.down()
            assertNull(f.lift(20))
            f.sample(10, 90, elapsed = away - 2000)
            f.sample(10, 90)
            assertEquals("media", f.sample(10, 90))
            val afterTwenty = f.lift(20)
            if (away == 300_000L) assertNull(afterTwenty) else {
                assertEquals("media", afterTwenty)
                repeat(19) { assertEquals("media", f.sample()) }
                assertNull(f.sample())
            }
        }
    }

    @Test
    fun smallSamplesDoNotCountOrEraseStreaks() {
        // media.ts:962-964 precedes streak handling: fewer than ten frames preserves both streaks.
        val f = Feed()
        repeat(10) { assertNull(f.sample(9, 0)) }
        assertNull(f.sample(1, 9))
        assertNull(f.sample(0, 9))
        assertNull(f.sample(1, 9))
        assertEquals("media", f.sample(1, 9))
        f.sample(elapsed = 20_000)
        repeat(18) { assertEquals("media", f.sample()) }
        repeat(10) { assertEquals("media", f.sample(9, 0)) }
        assertNull(f.sample())
    }

    @Test
    fun middleShareResetsBothStreaksAndFineBoundaryIsInclusive() {
        // media.ts:496-498, 973-994: 10% is bad, 2% is clean, and the middle resets both.
        val f = Feed()
        repeat(2) { assertNull(f.sample(10, 90)) }
        assertNull(f.sample(3, 97))
        repeat(2) { assertNull(f.sample(10, 90)) }
        assertEquals("media", f.sample(10, 90))
        f.sample(2, 98, elapsed = 20_000)
        repeat(18) { f.sample(2, 98) }
        assertEquals("media", f.sample(3, 97))
        repeat(19) { assertEquals("media", f.sample(2, 98)) }
        assertNull(f.sample(2, 98))
    }

    @Test
    fun missingDroppedUsesReceivedMinusDecodedButPresentDroppedWins() {
        // media.ts:953-957: use the fallback only when dropped is absent, not when it is zero.
        val f = Feed()
        repeat(2) { assertNull(f.sample(10, 90, missing = true)) }
        assertEquals("media", f.sample(10, 90, missing = true))
        val caps = DecodeCap()
        caps.judge("bob", 0, 0, 0, 1_000_000)
        for (i in 1L..5L) assertNull(caps.judge("bob", 0, 90 * i, 100 * i, 1_000_000 + i * 1000))
    }

    @Test
    fun counterResetAdvancesBaselineAndWaitsForStableReadings() {
        // Android deliberately excludes the recovery window after a receiver resets.
        val caps = DecodeCap()
        caps.judge("alice", 100, 900, 1000, 1_000_000)
        assertNull(caps.judge("alice", 0, 0, 0, 1_001_000))
        for (i in 1L..6L) assertNull(caps.judge("alice", 10 * i, 90 * i, 100 * i, 1_001_000 + i * 1000))
        assertEquals("media", caps.judge("alice", 70, 630, 700, 1_008_000))
    }

    @Test
    fun usersHaveIndependentCapsAndCanBeForgotten() {
        // media.ts:970, 1000-1010: cap/backoff belong to the person, never the whole call.
        val caps = DecodeCap()
        val alice = Feed(caps, "alice")
        val bob = Feed(caps, "bob")
        alice.down()
        assertNull(bob.sample())
        assertEquals("media", caps.cap("alice"))
        caps.forget("alice")
        assertNull(caps.cap("alice"))
        assertNull(alice.sample(10, 90)) // A new lifetime needs a new baseline.
        assertNull(alice.sample(10, 90))
        assertNull(alice.sample(10, 90))
        assertEquals("media", alice.sample(10, 90))
        assertNull(alice.lift(20))
    }

    @Test
    fun initialHoldMatchesWebZeroTimestamp() {
        // media.ts:949, 968: movedAt starts at zero, even when the supplied clock starts at zero.
        val caps = DecodeCap()
        caps.judge("alice", 0, 0, 0, 0)
        for (i in 1L..21L) assertNull(caps.judge("alice", 10 * i, 90 * i, 100 * i, i * 1000))
        assertEquals("media", caps.judge("alice", 220, 1980, 2200, 22_000))
    }

    @Test
    fun smallerOfDrawnSizeAndCapWins() {
        // media.ts:528-530, 1048-1050: larger index means smaller image; unknown cap means top.
        for ((width, drawnIndex) in listOf(1200 to 0, 600 to 1, 599 to 2)) {
            for (cap in listOf(null, "", "unknown", "alta", "media", "baixa")) {
                val capIndex = when (cap) { "media" -> 1; "baixa" -> 2; else -> 0 }
                assertEquals(Layers.SCREEN_LAYERS[maxOf(drawnIndex, capIndex)], LayerAsk().want("alice", width, true, cap))
            }
        }
    }

    @Test
    fun notLookingOrNoWidthAlwaysAsksForLow() {
        // media.ts:1041-1042: background or width below one is the cheapest layer.
        assertEquals("baixa", LayerAsk().want("alice", 2000, false, null))
        assertEquals("baixa", LayerAsk().want("alice", 0, true, null))
        assertEquals("baixa", LayerAsk().want("alice", -1, true, "alta"))
    }

    @Test
    fun requestsAreDeduplicatedPerPersonAndFailureOrForgetAllowsRetry() {
        // media.ts:1051-1059: remember before sending, clear on failure for the next measure.
        val ask = LayerAsk()
        assertEquals("alta", ask.want("alice", 1200, true, null))
        assertNull(ask.want("alice", 1600, true, "alta"))
        assertEquals("alta", ask.want("bob", 1200, true, null))
        assertEquals("media", ask.want("alice", 1200, true, "media"))
        assertNull(ask.want("alice", 1200, true, "media"))
        ask.failed("alice")
        assertNull(ask.want("bob", 1200, true, null))
        assertEquals("media", ask.want("alice", 1200, true, "media"))
        ask.forget("alice")
        assertEquals("media", ask.want("alice", 1200, true, "media"))
        assertEquals("baixa", ask.want("alice", 1200, false, null))
        assertEquals("alta", ask.want("alice", 1200, true, null))
    }
}
