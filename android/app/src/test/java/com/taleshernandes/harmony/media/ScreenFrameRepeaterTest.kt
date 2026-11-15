package com.taleshernandes.harmony.media

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test
import org.webrtc.CapturerObserver
import org.webrtc.VideoFrame

class ScreenFrameRepeaterTest {
    private val interval = ScreenFrameCadence.INTERVAL_NS

    @Test fun noRefreshBeforeTheFirstTextureOrAfterFailedStart() {
        val c = ScreenFrameCadence()
        assertEquals(-1L, c.delayNs(0))
        c.start(false)
        assertFalse(c.capture(100, 0))
        c.start(true)
        assertEquals(interval, c.delayNs(10 * interval))
    }

    @Test fun idleTextureIsDueAtFiveHundredMillisecondsAndNotEarlier() {
        val c = running()
        assertTrue(c.capture(1_000_000, 20_000_000))
        assertEquals(1L, c.delayNs(20_000_000 + interval - 1))
        assertEquals(0L, c.delayNs(20_000_000 + interval))
        assertEquals(0L, c.delayNs(20_000_000 + 4 * interval))
    }

    @Test fun repeatedTextureGetsNewTimestampsEveryInterval() {
        val c = running()
        val source = 1_000_000_000L
        assertTrue(c.capture(source, 0))
        for (n in 1..5) {
            val now = n * interval
            assertEquals(0L, c.delayNs(now))
            assertTrue(c.capture(source, now))
            assertEquals(source + now, c.outputTimestampNs)
            assertEquals(interval, c.delayNs(now))
        }
    }

    @Test fun realMotionPostponesRefreshWithoutChangingNormalTimestamps() {
        val c = running()
        for (n in 0..100) {
            val now = n * 33_000_000L
            val source = 2_000_000_000L + now
            assertTrue(c.capture(source, now))
            assertEquals(source, c.outputTimestampNs)
            assertEquals(interval - 33_000_000, c.delayNs(now + 33_000_000))
        }
    }

    @Test fun newRealFrameCancelsTheOldIdleDeadline() {
        val c = running()
        c.capture(1_000_000, 0)
        c.capture(2_000_000, interval - 1)
        assertEquals(interval - 1, c.delayNs(interval))
        assertEquals(0L, c.delayNs(2 * interval - 1))
    }

    @Test fun aDelayedOldRefreshCannotSendAnOlderImageOrResetTheDeadline() {
        val c = running()
        c.capture(100_000_000, 0)
        c.capture(600_000_000, interval)
        assertFalse(c.capture(100_000_000, interval + 10))
        assertEquals(600_000_000L, c.outputTimestampNs)
        assertEquals(interval - 10, c.delayNs(interval + 10))
        assertTrue(c.capture(600_000_000, 2 * interval))
        assertEquals(1_100_000_000L, c.outputTimestampNs)
    }

    @Test fun aNewRealFrameCanResumeBehindASyntheticTimestampWithoutGoingBack() {
        val c = running()
        c.capture(1_000_000_000, 0)
        c.capture(1_000_000_000, interval)
        assertEquals(1_500_000_000L, c.outputTimestampNs)
        assertTrue(c.capture(1_400_000_000, interval + 1))
        assertEquals(1_500_001_000L, c.outputTimestampNs)
        assertFalse(c.capture(1_000_000_000, interval + 2))
        assertTrue(c.capture(1_400_000_000, 2 * interval + 1))
        assertEquals(1_900_000_000L, c.outputTimestampNs)
    }

    @Test fun aQueuedForceAfterARealFrameDoesNotRepeatBeforeTheNewDeadline() {
        val c = running()
        c.capture(1_000_000, 0)
        c.capture(2_000_000, interval - 1)
        assertFalse(c.capture(2_000_000, interval))
        assertEquals(2_000_000L, c.outputTimestampNs)
        assertEquals(interval - 1, c.delayNs(interval))
    }

    @Test fun backToBackRealFramesAfterARepeatAdvanceNativeMicrosecondTimestamps() {
        val c = running()
        c.capture(1_000_000, 0)
        c.capture(1_000_000, interval)
        val first = c.outputTimestampNs / 1_000
        c.capture(1_000_001, interval + 1)
        assertTrue(c.outputTimestampNs / 1_000 > first)
        val second = c.outputTimestampNs / 1_000
        c.capture(1_000_002, interval + 2)
        assertTrue(c.outputTimestampNs / 1_000 > second)
    }

    @Test fun aLateTimerDoesNotProduceACatchUpBurst() {
        val c = running()
        c.capture(1_000_000_000, 0)
        val now = 20 * interval
        assertEquals(0L, c.delayNs(now))
        c.capture(1_000_000_000, now)
        assertEquals(interval, c.delayNs(now))
    }

    @Test fun stopRejectsQueuedFramesAndCancelsRepeats() {
        val c = running()
        c.capture(1_000_000_000, 0)
        c.start(false)
        assertEquals(-1L, c.delayNs(interval))
        assertFalse(c.capture(1_000_000_000, interval))
        assertFalse(c.capture(2_000_000_000, 2 * interval))
    }

    @Test fun aNewCaptureStartsWithoutAnImageOrTimestampFromThePreviousCapture() {
        val c = running()
        c.capture(10_000_000_000, 0)
        c.start(false)
        c.start(true)
        assertEquals(interval, c.delayNs(10 * interval))
        assertTrue(c.capture(1_000, 10 * interval))
        assertEquals(1_000L, c.outputTimestampNs)
    }

    @Test fun aNormalFrameUsesTheOriginalEnvelopeAndBorrowsItsBuffer() {
        val buffer = CountedBuffer()
        val frame = VideoFrame(buffer, 0, 123)
        deliverScreenFrame(observer { assertSame(frame, it) }, frame, 123)
        assertEquals(1, buffer.refs)
        assertEquals(0, buffer.retains)
        frame.release()
        assertEquals(0, buffer.refs)
    }

    @Test fun aRepeatChangesOnlyTimestampAndBalancesBufferOwnership() {
        val buffer = CountedBuffer()
        val frame = VideoFrame(buffer, 90, 123)
        deliverScreenFrame(observer {
            assertSame(buffer, it.buffer)
            assertEquals(90, it.rotation)
            assertEquals(456L, it.timestampNs)
            assertEquals(2, buffer.refs)
        }, frame, 456)
        assertEquals(123L, frame.timestampNs)
        assertEquals(1, buffer.refs)
        frame.release()
        assertEquals(0, buffer.refs)
    }

    @Test fun aDownstreamFailureDoesNotLeakTheRepeatReference() {
        val buffer = CountedBuffer()
        val frame = VideoFrame(buffer, 0, 123)
        try {
            deliverScreenFrame(observer { error("Downstream refused") }, frame, 456)
            throw AssertionError("Expected downstream failure")
        } catch (_: IllegalStateException) {
            assertEquals(1, buffer.refs)
        } finally {
            frame.release()
        }
        assertEquals(0, buffer.refs)
    }

    @Test fun downstreamMayRetainARepeatWithoutItsTimestampBeingMutatedLater() {
        val buffer = CountedBuffer()
        val frame = VideoFrame(buffer, 0, 123)
        var held: VideoFrame? = null
        deliverScreenFrame(observer { it.retain(); held = it }, frame, 456)
        deliverScreenFrame(observer {}, frame, 789)
        assertEquals(456L, held!!.timestampNs)
        assertEquals(2, buffer.refs)
        held.release()
        frame.release()
        assertEquals(0, buffer.refs)
    }

    private fun observer(onFrame: (VideoFrame) -> Unit) = object : CapturerObserver {
        override fun onCapturerStarted(success: Boolean) {}
        override fun onCapturerStopped() {}
        override fun onFrameCaptured(frame: VideoFrame) = onFrame(frame)
    }

    private class CountedBuffer : VideoFrame.Buffer {
        var refs = 1
        var retains = 0
        override fun getWidth() = 2
        override fun getHeight() = 2
        override fun retain() { retains++; refs++ }
        override fun release() { check(refs > 0); refs-- }
        override fun toI420(): VideoFrame.I420Buffer = error("No pixel conversion expected")
        override fun cropAndScale(x: Int, y: Int, width: Int, height: Int, scaledWidth: Int, scaledHeight: Int): VideoFrame.Buffer =
            error("No pixel copy expected")
    }

    private fun running() = ScreenFrameCadence().apply { start(true) }
}
