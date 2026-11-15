package com.taleshernandes.harmony

import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.net.Backoff
import com.taleshernandes.harmony.net.ServerUrl
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class NetTest {
    @Test
    fun serverAddressBecomesAnOrigin() {
        // The /ws Origin must equal the server's own, byte for byte (gateway.go).
        assertEquals("https://harmony.taleshernandes.com", ServerUrl.normalize("harmony.taleshernandes.com"))
        assertEquals("https://harmony.taleshernandes.com", ServerUrl.normalize(" https://harmony.taleshernandes.com/ "))
        assertEquals("https://harmony.taleshernandes.com", ServerUrl.normalize("https://harmony.taleshernandes.com:443/app/"))
        assertEquals("https://harmony.taleshernandes.com", ServerUrl.normalize("HTTPS://Harmony.TalesHernandes.com"))
        assertEquals("http://localhost:8099", ServerUrl.normalize("http://localhost:8099"))
        assertEquals("http://10.0.2.2:8099", ServerUrl.normalize("http://10.0.2.2:8099/"))
        assertNull(ServerUrl.normalize(""))
        assertNull(ServerUrl.normalize("ftp://x"))
    }

    @Test
    fun backoffDoublesToFiveSecondsAndResets() {
        val b = Backoff()
        assertEquals(listOf(500L, 1000, 2000, 4000, 5000, 5000, 5000), List(7) { b.next() })
        b.reset()
        assertEquals(500L, b.next())
    }

    @Test
    fun diagKeepsTheLastLinesOnly() {
        Diag.clearForTest()
        repeat(2000) { Diag.log("linha $it") }
        val lines = Diag.dump("cabeçalho").trimEnd().lines()
        assertEquals("cabeçalho", lines.first())
        assertEquals(1501, lines.size)
        assertTrue(lines[1].endsWith("linha 500"))
        assertTrue(lines.last().endsWith("linha 1999"))
    }
}
