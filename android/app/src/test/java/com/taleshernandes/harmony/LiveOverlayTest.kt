package com.taleshernandes.harmony

import com.taleshernandes.harmony.ui.LiveOverlay
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class LiveOverlayTest {
    @Test fun portraitVolumeLeavesBothButtonsClear() {
        assertEquals(192f, LiveOverlay.volumeWidth(320f), 0f)
        assertEquals(248f, LiveOverlay.volumeWidth(390f), 0f)
        for (width in listOf(280f, 320f, 360f, 390f, 780f)) {
            val used = 2 * LiveOverlay.INSET + LiveOverlay.volumeWidth(width) +
                2 * LiveOverlay.TARGET + 2 * LiveOverlay.GAP
            assertTrue("controls fit in $width dp", used <= width)
        }
    }
}
