package com.taleshernandes.harmony.media

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.junit.Assert.*
import org.junit.Test

class PlaybackPcmTest {
    private fun pcm(vararg values: Int): ByteBuffer = ByteBuffer.allocate(values.size * 2)
        .order(ByteOrder.LITTLE_ENDIAN).also { b -> values.forEachIndexed { i, v -> b.putShort(i * 2, v.toShort()) } }
    private fun values(pcm: ByteBuffer) = IntArray(pcm.limit() / 2) {
        ((pcm.get(it * 2).toInt() and 255) or (pcm.get(it * 2 + 1).toInt() shl 8)).toShort().toInt()
    }

    @Test fun saturatesBothSignsWithoutWrapping() {
        val ring = PlaybackRing()
        ring.offer(shortArrayOf(20000, -20000, 1000, -1000), 4)
        val b = pcm(20000, -20000, -500, 500)
        ring.mixInto(b, 4)
        assertArrayEquals(intArrayOf(32767, -32768, 500, -500), values(b))
    }

    @Test fun shortageAddsSilenceAndReportsExactMissingSamples() {
        val ring = PlaybackRing()
        ring.offer(shortArrayOf(100), 1)
        val b = pcm(10, 20, -30)
        ring.mixInto(b, 3)
        assertArrayEquals(intArrayOf(110, 20, -30), values(b))
        assertEquals(2L, ring.stats().missing)
        assertEquals(0L, ring.stats().missing)
    }

    @Test fun backlogKeepsLatestSamplesAcrossRingWrap() {
        val ring = PlaybackRing(4)
        ring.offer(shortArrayOf(1, 2, 3), 3)
        ring.mixInto(pcm(0, 0), 2)
        ring.offer(shortArrayOf(4, 5, 6, 7, 8), 5)
        val b = pcm(0, 0, 0, 0)
        ring.mixInto(b, 4)
        assertArrayEquals(intArrayOf(5, 6, 7, 8), values(b))
        val stats = ring.stats()
        assertEquals(2L, stats.dropped)
        assertEquals(0, stats.queued)
    }

    @Test fun oversizedFirstReadAlsoDiscardsOldest() {
        val ring = PlaybackRing(2)
        ring.offer(shortArrayOf(1, 2, 3, 4), 4)
        val b = pcm(0, 0)
        ring.mixInto(b, 2)
        assertArrayEquals(intArrayOf(3, 4), values(b))
        assertEquals(2L, ring.stats().dropped)
    }

    @Test fun meanLevelHandlesPcmMinimumWithoutOverflow() {
        val ring = PlaybackRing()
        ring.offer(shortArrayOf(-32768, 32767, 0, 1), 4)
        assertEquals(16384L, ring.stats().meanAbsolute)
        assertEquals(0L, ring.stats().meanAbsolute)
    }

    @Test fun callbackNeverWaitsForLockedProducer() {
        val ring = PlaybackRing()
        val held = CountDownLatch(1)
        val release = CountDownLatch(1)
        val producer = Thread {
            ring.lock.lock()
            try { held.countDown(); release.await() } finally { ring.lock.unlock() }
        }
        producer.start()
        assertTrue(held.await(1, TimeUnit.SECONDS))
        val completed = CountDownLatch(1)
        val b = pcm(1, 2)
        val consumer = Thread { ring.mixInto(b, 2); completed.countDown() }
        consumer.start()
        try {
            assertTrue("Consumer blocked on producer", completed.await(1, TimeUnit.SECONDS))
            assertArrayEquals(intArrayOf(1, 2), values(b))
        } finally { release.countDown(); producer.join(); consumer.join() }
        assertEquals(2L, ring.stats().missing)
    }

    @Test fun absoluteMixPreservesBufferPositionLimitAndBigEndianOrder() {
        val ring = PlaybackRing()
        ring.offer(shortArrayOf(500, -100), 2)
        val b = pcm(100, 100, 99).order(ByteOrder.BIG_ENDIAN)
        b.limit(4); b.position(3)
        ring.mixInto(b, 2)
        assertArrayEquals(intArrayOf(600, 0), values(b))
        assertEquals(3, b.position()); assertEquals(4, b.limit()); assertEquals(ByteOrder.BIG_ENDIAN, b.order())
    }

    @Test fun nativeMuteAndVoiceMuteBothLeaveGameAudible() {
        val mix = PlaybackMix()
        mix.microphoneMuted = true
        mix.ring.offer(ShortArray(480) { 1000 }, 480)
        val b = ByteBuffer.allocate(960).order(ByteOrder.LITTLE_ENDIAN)
        repeat(480) { b.putShort(it * 2, 5000) }
        mix.process(b, 960, 48000, 1)
        assertTrue(values(b).all { it == 1000 })
        // Java's ADM mute has already zeroed the voice in this second frame.
        repeat(960) { b.put(it, 0) }
        mix.ring.offer(ShortArray(480) { 2000 }, 480)
        mix.process(b, 960, 48000, 1)
        assertTrue(values(b).all { it == 2000 })
    }

    @Test fun gameIsAddedToAlreadyFilteredVoiceAndStopsOnDisable() {
        val mix = PlaybackMix()
        val b = ByteBuffer.allocate(960).order(ByteOrder.LITTLE_ENDIAN)
        repeat(480) { b.putShort(it * 2, 50) } // Result of the voice filter.
        mix.ring.offer(ShortArray(480) { 20000 }, 480)
        mix.process(b, 960, 48000, 1)
        assertTrue(values(b).all { it == 20050 })
        mix.ring.offer(ShortArray(480) { 1000 }, 480)
        mix.enabled = false
        mix.process(b, 960, 48000, 1)
        assertTrue(values(b).all { it == 20050 })
    }

    @Test fun incompatibleMicDisablesOnlyPlaybackAndPreservesVoice() {
        val mix = PlaybackMix()
        val b = pcm(1200, -1000)
        mix.process(b, 4, 44100, 1)
        assertArrayEquals(intArrayOf(1200, -1000), values(b))
        assertTrue(mix.formatFailed); assertFalse(mix.enabled)
    }

    @Test fun formatFailureStillZerosMutedVoiceUntilSenderIsDisabled() {
        val mix = PlaybackMix()
        mix.microphoneMuted = true
        val b = pcm(1200, -1000)
        mix.process(b, 4, 44100, 1)
        assertArrayEquals(intArrayOf(0, 0), values(b))
        b.putShort(0, 3000)
        mix.process(b, 4, 44100, 1)
        assertEquals(0, values(b)[0])
    }

    @Test fun senderPolicyAllowsMutedGameAndRestoresNormalMuteAfterStop() {
        for (muted in listOf(false, true)) for (deaf in listOf(false, true)) {
            assertTrue(ScreenAudioPolicy.sendTrack(muted, deaf, true))
            assertEquals(!muted && !deaf, ScreenAudioPolicy.sendTrack(muted, deaf, false))
        }
    }

    @Test fun mono48kPreservesEverySampleAcrossReadBoundaries() {
        val c = PlaybackResampler(48000, 1)
        val out = ShortArray(8)
        assertEquals(3, c.convert(shortArrayOf(1, -2, 32767), 3, out))
        assertArrayEquals(shortArrayOf(1, -2, 32767), out.copyOf(3))
        assertEquals(2, c.convert(shortArrayOf(-32768, 5), 2, out))
        assertArrayEquals(shortArrayOf(-32768, 5), out.copyOf(2))
    }

    @Test fun stereoDownmixDoesNotOverflowOrInvertPhase() {
        val c = PlaybackResampler(48000, 2)
        val out = ShortArray(5)
        val n = c.convert(shortArrayOf(32767, 32767, -32768, -32768, 20000, -20000, 2000, 4000), 8, out)
        assertEquals(4, n)
        assertArrayEquals(shortArrayOf(32767, -32768, 0, 3000), out.copyOf(n))
    }

    @Test fun interpolationRetainsFractionalPositionBetweenReads() {
        val c = PlaybackResampler(24000, 1)
        val out = ShortArray(8)
        assertEquals(1, c.convert(shortArrayOf(0), 1, out))
        assertEquals(2, c.convert(shortArrayOf(1000), 1, out))
        assertArrayEquals(shortArrayOf(500, 1000), out.copyOf(2))
        assertEquals(2, c.convert(shortArrayOf(2000), 1, out))
        assertArrayEquals(shortArrayOf(1500, 2000), out.copyOf(2))
    }

    @Test fun resampling44100HasNoDriftAndIsIndependentOfChunkSize() {
        val input = ShortArray(44100) { ((it % 200) * 100 - 10000).toShort() }
        val all = ShortArray(48004)
        val n = PlaybackResampler(44100, 1).convert(input, input.size, all)
        val c = PlaybackResampler(44100, 1)
        val chunk = ShortArray(500)
        val joined = ShortArray(48004)
        var written = 0
        repeat(100) { i ->
            val m = c.convert(input.copyOfRange(i * 441, (i + 1) * 441), 441, chunk)
            chunk.copyInto(joined, written, 0, m); written += m
        }
        assertEquals(47999, n)
        assertEquals(n, written)
        assertArrayEquals(all.copyOf(n), joined.copyOf(written))
    }

    @Test fun downsampling96kTracksTimeline() {
        val out = ShortArray(10)
        val n = PlaybackResampler(96000, 1).convert(shortArrayOf(0, 100, 200, 300, 400), 5, out)
        assertEquals(3, n)
        assertArrayEquals(shortArrayOf(0, 200, 400), out.copyOf(n))
    }

    @Test(expected = IllegalArgumentException::class) fun rejectsBrokenStereoFrame() {
        PlaybackResampler(48000, 2).convert(shortArrayOf(1), 1, ShortArray(10))
    }
}
