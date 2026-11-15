package com.taleshernandes.harmony

import android.media.AudioDeviceInfo
import com.taleshernandes.harmony.media.Mic
import com.taleshernandes.harmony.media.Microphones
import org.junit.Assert.*
import org.junit.Test

class MicrophonesTest {
    @Test fun theChosenOneWhileItIsThereOtherwiseThePhones() {
        assertEquals(Mic.BLUETOOTH, Microphones.pick(Mic.BLUETOOTH, setOf(Mic.PHONE, Mic.BLUETOOTH)))
        // The headset switched off mid-call: back to the phone, never to nothing.
        assertEquals(Mic.PHONE, Microphones.pick(Mic.BLUETOOTH, setOf(Mic.PHONE)))
        assertEquals(Mic.PHONE, Microphones.pick(Mic.WIRED, emptySet()))
        assertEquals(Mic.PHONE, Microphones.pick(Mic.PHONE, setOf(Mic.PHONE, Mic.WIRED, Mic.BLUETOOTH)))
    }

    @Test fun onlyMicrophonesAPersonPicksAreListed() {
        assertEquals(Mic.PHONE, Microphones.kind(AudioDeviceInfo.TYPE_BUILTIN_MIC))
        assertEquals(Mic.WIRED, Microphones.kind(AudioDeviceInfo.TYPE_WIRED_HEADSET))
        assertEquals(Mic.USB, Microphones.kind(AudioDeviceInfo.TYPE_USB_HEADSET))
        assertEquals(Mic.BLUETOOTH, Microphones.kind(AudioDeviceInfo.TYPE_BLUETOOTH_SCO))
        assertEquals(Mic.BLUETOOTH, Microphones.kind(AudioDeviceInfo.TYPE_BLE_HEADSET))
        assertNull(Microphones.kind(AudioDeviceInfo.TYPE_TELEPHONY))
        assertNull(Microphones.kind(AudioDeviceInfo.TYPE_FM_TUNER))
        // Music-only Bluetooth has no microphone.
        assertNull(Microphones.kind(AudioDeviceInfo.TYPE_BLUETOOTH_A2DP))
    }
}
