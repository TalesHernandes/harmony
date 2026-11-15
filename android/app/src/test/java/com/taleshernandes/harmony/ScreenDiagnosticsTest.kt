package com.taleshernandes.harmony

import com.taleshernandes.harmony.media.DecodeCap
import com.taleshernandes.harmony.media.ScreenDiagnostics
import com.taleshernandes.harmony.media.screenSyncGroup
import org.junit.Assert.*
import org.junit.Test

class ScreenDiagnosticsTest {
    @Test fun reconnectDropsCannotLowerTheCapOrCarryABadStreak() {
        val cap = DecodeCap()
        cap.judge("a", 0, 0, 0, 100_000)
        cap.judge("a", 20, 80, 100, 101_000)
        cap.judge("a", 40, 160, 200, 102_000)
        assertNull(cap.judge("a", 60, 240, 300, 103_000, eligible = false))
        assertNull(cap.judge("a", 80, 320, 400, 104_000))
        assertNull(cap.judge("a", 100, 400, 500, 108_000))
        assertNull(cap.judge("a", 120, 480, 600, 109_000))
        assertEquals("media", cap.judge("a", 140, 560, 700, 110_000))
        assertTrue(cap.evidence("a").contains("deltaDropped=20 deltaDecoded=80"))
    }

    @Test fun layerTransitionBeforeFirstStatsIsAlsoProtected() {
        val cap = DecodeCap()
        cap.suspendJudgment("a", 100_000)
        cap.judge("a", 0, 0, 0, 100_000)
        for (i in 1..4) assertNull(cap.judge("a", i * 20L, i * 80L, i * 100L, 100_000 + i * 1000L))
        for (i in 5..6) assertNull(cap.judge("a", i * 20L, i * 80L, i * 100L, 100_000 + i * 1000L))
        assertEquals("media", cap.judge("a", 140, 560, 700, 107_000))
    }

    @Test fun counterResetDoesNotContinueTheOldBadStreak() {
        val cap = DecodeCap()
        cap.judge("a", 0, 0, 0, 100_000)
        cap.judge("a", 20, 80, 100, 101_000)
        cap.judge("a", 40, 160, 200, 102_000)
        cap.judge("a", 0, 0, 0, 103_000)
        assertNull(cap.judge("a", 20, 80, 100, 104_000))
    }

    @Test fun jitterMeanUsesTheIntervalAndNeverInventsMissingValues() {
        val d = ScreenDiagnostics()
        assertTrue(d.sample("a", mapOf("jitterBufferDelay" to 10.0, "jitterBufferEmittedCount" to 100)).contains("jbIntervalMs=não encontrado"))
        val next = d.sample("a", mapOf("jitterBufferDelay" to 12.0, "jitterBufferEmittedCount" to 200, "token" to "secret"))
        assertTrue(next.contains("jbIntervalMs=20.0"))
        assertFalse(next.contains("secret"))
        assertTrue(next.contains("estimatedPlayoutTimestamp=não encontrado"))
        assertTrue(d.sample("a", mapOf("jitterBufferDelay" to 0.0, "jitterBufferEmittedCount" to 0)).contains("jbIntervalMs=não encontrado"))
        assertTrue(d.sample("b", emptyMap()).contains("jbIntervalMs=não encontrado"))
    }

    @Test fun cadenceAndClearUseElapsedTime() {
        val d = ScreenDiagnostics()
        assertTrue(d.due(100))
        assertFalse(d.due(10_099))
        assertTrue(d.due(10_100))
        d.clear()
        assertTrue(d.due(10_101))
    }

    @Test fun comparesStreamNotTrackAndHandlesMissingAndLegacyMsid() {
        val sdp = "m=audio 9 RTP/AVP 111\r\na=mid:1\r\na=msid:shared audio\r\nm=video 9 RTP/AVP 96\r\na=mid:2\r\na=msid:shared video\r\n"
        assertEquals("same", screenSyncGroup(sdp, "2", "1"))
        assertEquals("different", screenSyncGroup(sdp.replace("shared video", "other video"), "2", "1"))
        assertEquals("não encontrado", screenSyncGroup(sdp, "2", null))
        assertEquals("não encontrado", screenSyncGroup(sdp.replace("shared audio", "- audio"), "2", "1"))
        assertEquals("same", screenSyncGroup(sdp.replace("a=msid:", "a=ssrc:123 msid:"), "2", "1"))
    }
}
