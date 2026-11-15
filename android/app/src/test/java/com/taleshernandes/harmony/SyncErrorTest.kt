package com.taleshernandes.harmony

import com.taleshernandes.harmony.media.SyncError
import org.junit.Assert.*
import org.junit.Test

class SyncErrorTest {
    private fun ts(v: Double) = mapOf<String, Any>("estimatedPlayoutTimestamp" to v)

    @Test fun pictureMinusSoundOnceBothAdvance() {
        val e = SyncError()
        e.sample("1/tales", ts(1_000.0), ts(1_000.0), 10)
        assertNull("the first report only sets a baseline", e.fresh(10))
        e.sample("1/tales", ts(2_250.0), ts(2_000.0), 1_010)
        assertEquals(250, e.fresh(1_010))
        e.sample("1/tales", ts(3_000.0), ts(3_100.0), 2_010)
        assertEquals(-100, e.fresh(2_010))
    }

    @Test fun staleFrozenMissingOrAnotherStreamGiveNothing() {
        val e = SyncError()
        e.sample("1/tales", ts(1_000.0), ts(1_000.0), 0)
        e.sample("1/tales", ts(2_000.0), ts(1_900.0), 1_000)
        assertEquals(100, e.fresh(1_000))
        assertNull("old reading", e.fresh(3_501))
        e.sample("1/tales", ts(2_000.0), ts(2_900.0), 2_000) // picture frozen
        assertNull(e.fresh(2_000))
        e.sample("1/tales", ts(3_000.0), null, 3_000)
        assertNull(e.fresh(3_000))
        e.sample("2/tales", ts(4_000.0), ts(3_900.0), 4_000) // new call generation
        assertNull(e.fresh(4_000))
        e.sample("2/tales", ts(20_000.0), ts(5_000.0), 5_000) // 15 s apart: not a sync error
        assertNull(e.fresh(5_000))
    }
}
