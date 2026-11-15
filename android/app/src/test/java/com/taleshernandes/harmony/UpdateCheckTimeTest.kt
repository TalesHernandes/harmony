package com.taleshernandes.harmony

import com.taleshernandes.harmony.model.updateCheckTime
import java.time.Instant
import java.time.ZoneId
import org.junit.Assert.assertEquals
import org.junit.Test

class UpdateCheckTimeTest {
    private val zone = ZoneId.of("America/Sao_Paulo")
    private fun time(s: String) = Instant.parse(s).toEpochMilli()

    @Test fun neverChecked() {
        assertEquals("Ainda não procurado", updateCheckTime(null))
    }

    @Test fun usesLocalDayAndTime() {
        val checked = time("2026-09-28T00:10:00Z")
        assertEquals("procurado hoje às 21:10", updateCheckTime(checked, time("2026-09-28T02:59:00Z"), zone))
        assertEquals("procurado ontem às 21:10", updateCheckTime(checked, time("2026-09-28T03:00:00Z"), zone))
        assertEquals("procurado 27/09/2026 às 21:10", updateCheckTime(checked, time("2026-09-29T03:00:00Z"), zone))
    }

    @Test fun yesterdayAcrossDaylightSavingAndYearBoundary() {
        val newYork = ZoneId.of("America/New_York")
        assertEquals("procurado ontem às 00:30", updateCheckTime(
            time("2026-11-01T04:30:00Z"), time("2026-11-02T05:15:00Z"), newYork))
        assertEquals("procurado ontem às 21:10", updateCheckTime(
            time("2026-01-01T00:10:00Z"), time("2026-01-01T15:00:00Z"), zone))
    }
}
