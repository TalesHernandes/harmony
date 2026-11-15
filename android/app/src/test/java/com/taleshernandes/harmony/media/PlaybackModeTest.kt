package com.taleshernandes.harmony.media

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PlaybackModeTest {
    @Test fun callModeOnlyWithAnOpenMicrophoneOutOfThePhoneItself() {
        assertEquals(PlaybackMode.CALL, playbackMode(Output.SPEAKER, true))
        assertEquals(PlaybackMode.CALL, playbackMode(Output.EARPIECE, true))
        for (output in listOf(Output.BLUETOOTH, Output.WIRED, null)) {
            assertEquals(PlaybackMode.MEDIA, playbackMode(output, true))
        }
    }

    @Test fun aClosedMicrophoneIsAlwaysMedia() {
        for (output in Output.entries.toList() + null) {
            assertEquals(PlaybackMode.MEDIA, playbackMode(output, microphoneIsOpen(true, true)))
            assertEquals(PlaybackMode.MEDIA, playbackMode(output, microphoneIsOpen(true, false, true)))
            assertEquals(PlaybackMode.MEDIA, playbackMode(output, microphoneIsOpen(false, false)))
        }
    }

    @Test fun headsetsNeverUseTheTelephoneLink() {
        for (open in listOf(false, true)) {
            assertEquals(PlaybackMode.MEDIA, playbackMode(Output.BLUETOOTH, open))
            assertEquals(PlaybackMode.MEDIA, playbackMode(Output.WIRED, open))
        }
    }

    @Test fun microphoneMuteRules() {
        assertFalse(microphoneIsOpen(false, false))
        assertFalse(microphoneIsOpen(true, true))
        assertFalse(microphoneIsOpen(true, false, true))
        assertTrue(microphoneIsOpen(true, false))
    }
}
