package com.taleshernandes.harmony.media

import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.*
import org.junit.Test

class DenoiseTest {
    @Test fun oneSlowFrameDoesNotDisableTheFilter() {
        val budget = DenoiseBudget()
        assertFalse(budget.record(DenoiseBudget.FRAME_NS + 1))
        repeat(200) { assertFalse(budget.record(DenoiseBudget.FRAME_NS)) }
    }

    @Test fun oneSpikeOnlyCatchesUp() {
        val budget = DenoiseBudget()
        assertFalse(budget.record(44_000_000)) // 34 ms behind
        assertTrue(budget.catchingUp)
        repeat(3) { budget.skip(); assertTrue(budget.catchingUp) }
        budget.skip()
        assertFalse(budget.catchingUp)
        repeat(1000) { assertFalse(budget.record(6_000_000)) }
    }

    @Test fun spikesFarApartNeverStopTheFilter() {
        val budget = DenoiseBudget()
        repeat(10) {
            assertFalse(budget.record(44_000_000))
            while (budget.catchingUp) budget.skip()
            repeat(600) { assertFalse(budget.record(6_000_000)) }
        }
    }

    // The 0.21.2 log: three trips in the first 1.8 s of a call while the model
    // warmed up, then nothing. Those only catch up.
    @Test fun tripsWhileWarmingUpDoNotStop() {
        val budget = DenoiseBudget()
        repeat(3) {
            assertFalse(budget.record(44_000_000))
            while (budget.catchingUp) budget.skip()
            repeat(20) { budget.record(6_000_000) }
        }
        assertFalse(budget.disabled)
    }

    private fun warm(budget: DenoiseBudget) = repeat(DenoiseBudget.GRACE.toInt()) { budget.record(5_000_000) }

    @Test fun threeTripsInFiveSecondsStop() {
        val budget = DenoiseBudget()
        warm(budget)
        repeat(2) {
            assertFalse(budget.record(44_000_000))
            while (budget.catchingUp) budget.skip()
            repeat(100) { budget.record(6_000_000) }
        }
        assertTrue(budget.record(44_000_000))
    }

    // The Redmi with the screen off (owner log, 03/10/2026): mean 6.5 ms, spikes
    // to 21 ms, one frame in nine over 10 ms. The quick frames absorb the spikes.
    @Test fun spikesTheFastFramesAbsorbDoNotTrip() {
        val budget = DenoiseBudget()
        // A third of the frames late, in bursts: the old "20 slow in 100" rule
        // paused on this, yet the microphone is never more than 9 ms behind.
        repeat(1000) {
            repeat(3) { assertFalse(budget.record(13_000_000)) }
            repeat(6) { assertFalse(budget.record(5_000_000)) }
        }
    }

    @Test fun slowOnAverageStops() {
        val budget = DenoiseBudget()
        repeat(1000) { if (budget.catchingUp) budget.skip() else budget.record(10_500_000) }
        assertTrue(budget.disabled)
    }

    private fun microphone() = ByteBuffer.allocateDirect(Denoise.FRAME_BYTES).apply {
        order(ByteOrder.BIG_ENDIAN)
        for (i in 0 until capacity()) put(i, (i % 127 + 1).toByte())
        position(capacity()) // WebRTC's mute path may leave this at the end.
    }

    @Test fun partiallyWrittenThrowingFilterPreservesEveryRawByteAndStops() {
        val pcm = microphone()
        val raw = ByteArray(pcm.capacity()) { pcm.get(it) }
        var calls = 0
        var failures = 0
        val frame = DenoiseFrame(filter = { scratch, _ ->
            calls++
            scratch.put(0, 0)
            throw IllegalStateException("simulated failure")
        }, failed = { failures++ })
        repeat(2) { frame.process(pcm, 960, 48000, 1, 50) }
        assertEquals(1, calls)
        assertEquals(1, failures)
        assertArrayEquals(raw, ByteArray(pcm.capacity()) { pcm.get(it) })
        assertEquals(960, pcm.position())
    }

    @Test fun successfulFilterUsesReusableLittleEndianPcm16Scratch() {
        val pcm = microphone()
        var previous: ByteBuffer? = null
        val frame = DenoiseFrame(filter = { scratch, _ ->
            assertEquals(ByteOrder.LITTLE_ENDIAN, scratch.order())
            assertEquals(960, scratch.capacity())
            assertEquals(0, scratch.position())
            if (previous != null) assertSame(previous, scratch)
            previous = scratch
            scratch.putShort(0, Short.MIN_VALUE)
            scratch.putShort(2, Short.MAX_VALUE)
        }, clock = { 0 })
        repeat(2) { frame.process(pcm, 960, 48000, 1, 50) }
        assertEquals(0, pcm.get(0).toInt())
        assertEquals(-128, pcm.get(1).toInt())
        assertEquals(-1, pcm.get(2).toInt())
        assertEquals(127, pcm.get(3).toInt())
        assertEquals(ByteOrder.BIG_ENDIAN, pcm.order())
        assertEquals(960, pcm.position())
        assertEquals(960, pcm.limit())
    }

    @Test fun slownessPausesTheFilterAndItComesBack() {
        val pcm = microphone()
        var now = 0L
        var slow = false
        var calls = 0
        val pauses = mutableListOf<Boolean>()
        val frame = DenoiseFrame(filter = { _, _ ->
            calls++
            if (slow) now += 20_000_000
        }, clock = { now }, paused = { pauses += it })
        repeat(DenoiseBudget.GRACE.toInt()) { frame.process(pcm, 960, 48000, 1, 50) }
        slow = true
        // Four filtered frames put it 40 ms behind, four raw ones catch up; the third time, it pauses.
        repeat(8 + 8 + 4) { frame.process(pcm, 960, 48000, 1, 50) }
        assertEquals(listOf(true), pauses)
        frame.process(pcm, 960, 48000, 1, 50)
        assertEquals(312, calls) // raw while paused
        // The screen comes back on: the processor is fast again.
        slow = false
        now += DenoiseFrame.PAUSE_NS
        repeat(10) { frame.process(pcm, 960, 48000, 1, 50) }
        assertEquals(listOf(true, false), pauses)
        assertEquals(322, calls)
    }

    @Test fun aThrowingFilterStaysOffEvenAfterThePause() {
        val pcm = microphone()
        var now = 0L
        var calls = 0
        val frame = DenoiseFrame(filter = { _, _ -> calls++; throw IllegalStateException("x") }, clock = { now })
        frame.process(pcm, 960, 48000, 1, 50)
        now += DenoiseFrame.PAUSE_NS * 2
        frame.process(pcm, 960, 48000, 1, 50)
        assertEquals(1, calls)
    }

    @Test fun aSpikeSendsOnlyTheCatchUpFramesRaw() {
        val pcm = microphone()
        var now = 0L
        var calls = 0
        val pauses = mutableListOf<Boolean>()
        var raws = -1L
        val frame = DenoiseFrame(filter = { scratch, _ ->
            scratch.put(0, 0)
            now += if (calls++ == 0) 44_000_000 else 5_000_000
        }, clock = { now }, paused = { pauses += it }, report = { _, _, _, _, _, raw -> raws = raw })
        frame.process(pcm, 960, 48000, 1, 50) // 34 ms behind: filtered, then catch up
        assertEquals(0, pcm.get(0).toInt())
        pcm.put(0, 99)
        repeat(4) { frame.process(pcm, 960, 48000, 1, 50) }
        assertEquals(1, calls)
        assertEquals(99, pcm.get(0).toInt())
        frame.process(pcm, 960, 48000, 1, 50)
        assertEquals(2, calls)
        assertEquals(0, pcm.get(0).toInt())
        assertEquals(emptyList<Boolean>(), pauses)
        frame.close()
        assertEquals(4, raws)
    }

    @Test fun incompatibleFormatNeverCallsNativeAndCloseIsIdempotent() {
        var calls = 0
        var releases = 0
        val pcm = microphone()
        val frame = DenoiseFrame(filter = { _, _ -> calls++ }, release = { releases++ })
        frame.process(pcm, 960, 44100, 1, 50)
        frame.process(pcm, 960, 48000, 1, 50)
        frame.close(); frame.close()
        assertEquals(0, calls)
        assertEquals(1, releases)
        assertEquals(1, pcm.get(0).toInt())
    }

    @Test fun failingLevelChangeIsRawAndNewLevelIsAppliedOnlyOnce() {
        var changes = 0
        var calls = 0
        val pcm = microphone()
        val frame = DenoiseFrame(filter = { _, _ -> calls++ }, setLevel = {
            changes++
            if (it == 100) error("simulated level failure")
        }, clock = { 0 })
        repeat(2) { frame.process(pcm, 960, 48000, 1, 75) }
        assertEquals(1, changes)
        frame.process(pcm, 960, 48000, 1, 100)
        assertEquals(2, calls)
        assertEquals(1, pcm.get(0).toInt())
    }

    @Test fun diagnosticsReportCountsAndTimesAfterThirtySeconds() {
        var now = 0L
        var frames = 0L
        var mean = 0L
        var maximum = 0L
        val frame = DenoiseFrame(filter = { _, _ -> now += 2_000_000 }, clock = { now },
            report = { count, avg, max, slow, behind, raw -> frames = count; mean = avg; maximum = max; assertEquals(0, slow); assertEquals(0, behind); assertEquals(0, raw) })
        val pcm = microphone()
        frame.process(pcm, 960, 48000, 1, 50)
        now += 30_000_000_000
        frame.process(pcm, 960, 48000, 1, 50)
        assertEquals(2, frames)
        assertEquals(2_000_000, mean)
        assertEquals(2_000_000, maximum)
    }

    @Test fun levelsFollowTheWebRangeAndFiveDbSteps() {
        assertEquals(50, Denoise.DEFAULT_LEVEL)
        assertEquals(10, Denoise.normalizeLevel(-1))
        assertEquals(100, Denoise.normalizeLevel(1000))
        assertEquals(55, Denoise.normalizeLevel(53))
    }
}
