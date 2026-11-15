package com.taleshernandes.harmony

import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.media.EchoProbe
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test

class EchoProbeTest {
    @Before fun clean() { Diag.clearForTest(); EchoProbe.resetForTest() }

    private fun echoLines() = Diag.dump("t").lines().filter { "eco:" in it }

    @Test fun logsTheCancellersFiguresEveryFiveSeconds() {
        val stats = mapOf<String, Any>("echoReturnLoss" to 12.34, "echoReturnLossEnhancement" to 3.0, "audioLevel" to 0.05)
        EchoProbe.sample(stats, 1_000)
        EchoProbe.sample(stats, 5_999)
        EchoProbe.sample(stats, 6_000)
        val lines = echoLines()
        assertEquals(2, lines.size)
        assertTrue(lines[0], "ERL=12.3dB ERLE=3.0dB mic=0.050" in lines[0])
        // No call has measured the output yet: said, not guessed.
        assertTrue(lines[0], "saída=não encontrado" in lines[0])
    }

    @Test fun saysOnceWhenThePhoneHasNoEchoFigures() {
        EchoProbe.sample(mapOf("audioLevel" to 0.1), 0)
        EchoProbe.sample(mapOf("audioLevel" to 0.1), 10_000)
        assertEquals(listOf(true), echoLines().map { "não encontrado" in it })
    }
}
