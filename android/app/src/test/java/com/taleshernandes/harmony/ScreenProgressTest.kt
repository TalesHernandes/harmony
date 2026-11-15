package com.taleshernandes.harmony

import com.taleshernandes.harmony.media.ScreenProgress
import com.taleshernandes.harmony.ui.isXiaomi
import org.junit.Assert.*
import org.junit.Test

class ScreenProgressTest {
    @Test fun stallStartsAfterTwoSecondsAndRecoversWithANewFrame() {
        val picture = ScreenProgress().sample(40, 1920, 1080, 1000)
        val unchanged = picture.sample(40, 1920, 1080, 2000)
        assertFalse(unchanged.stalled(2999, 500))
        assertTrue(unchanged.stalled(3000, 500))
        assertFalse(unchanged.sample(41, 1920, 1080, 3100).stalled(3100, 500))
    }

    @Test fun noStatsStillShowsTheStallAndUnknownSizeKeepsTheLastOrientation() {
        assertFalse(ScreenProgress().stalled(2499, 500))
        assertTrue(ScreenProgress().stalled(2500, 500))
        val portrait = ScreenProgress().sample(1, 1080, 1920, 100)
        assertTrue(portrait.portrait)
        assertTrue(portrait.sample(2, 0, 0, 200).portrait)
        assertFalse(portrait.sample(2, 1920, 1080, 200).portrait)
    }

    @Test fun counterResetIsANewPicture() {
        assertEquals(5000L, ScreenProgress(100, 1000).sample(1, 0, 0, 5000).changedAt)
    }

    @Test fun guideOnlyTargetsTheThreeManufacturers() {
        listOf("Xiaomi", "REDMI", "POCO").forEach { assertTrue(isXiaomi(it)) }
        listOf("Samsung", "Google", "", "NotXiaomi").forEach { assertFalse(isXiaomi(it)) }
    }
}
