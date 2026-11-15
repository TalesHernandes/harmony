package com.taleshernandes.harmony.ui

import androidx.compose.ui.geometry.Offset
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FullZoomTest {
    private val w = 1000f
    private val h = 500f

    /** Runs one gesture of equal pinch steps from [start]. */
    private fun gesture(start: FullZoom, vararg factors: Float, pan: Offset = Offset.Zero): FullZoom {
        var z = start
        var pinch = 1f
        for (f in factors) {
            pinch *= f
            z = z.step(start, pinch, f, pan, w, h)
        }
        return z
    }

    @Test fun pinchOutFromFitFills() {
        assertFalse(gesture(FullZoom(), 1.05f).fill)
        val z = gesture(FullZoom(), 1.1f, 1.1f)
        assertTrue(z.fill)
        assertEquals(1f, z.scale)
    }

    @Test fun zoomGoesPastFillUpToFive() {
        val z = gesture(FullZoom(fill = true), 2f, 2f, 2f)
        assertEquals(FullZoom.MAX, z.scale)
    }

    @Test fun pinchInFromAboveStopsAtFill() {
        val z = gesture(FullZoom(fill = true, scale = 2f), 0.5f, 0.5f, 0.5f)
        assertTrue(z.fill)
        assertEquals(1f, z.scale)
        assertEquals(0f, z.panX)
    }

    @Test fun aNewPinchInFromFillShowsItAll() {
        assertEquals(FullZoom(), gesture(FullZoom(fill = true), 0.9f, 0.9f))
        // A small one stays at fill.
        assertEquals(FullZoom(fill = true), gesture(FullZoom(fill = true), 0.95f))
    }

    @Test fun panStaysInsideThePicture() {
        val z = gesture(FullZoom(fill = true, scale = 2f), 1f, pan = Offset(10_000f, -10_000f))
        assertEquals(w / 2f, z.panX)
        assertEquals(-h / 2f, z.panY)
        // At fill there is nothing to move, so a one-finger tap is left alone.
        assertEquals(FullZoom(fill = true), gesture(FullZoom(fill = true), 1f, pan = Offset(30f, 0f)))
    }
}
