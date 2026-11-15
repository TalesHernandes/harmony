package com.taleshernandes.harmony.media

import android.content.Context
import android.media.AudioDeviceInfo
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/** Which microphone a call hears through, in the words the screen uses. */
enum class Mic(val label: String) {
    PHONE("Do celular"),
    WIRED("Fone com fio"),
    USB("USB"),
    BLUETOOTH("Fone Bluetooth"),
}

/**
 * The person's choice of microphone (owner, 25/10/2026): the phone's by
 * default; a wired or USB one; or the Bluetooth headset's, which only while
 * chosen moves the headset from its music profile to its call one — the
 * sound in it is call quality then, in every app. The playback stays media
 * and the mode stays normal (the owner refused the call mode).
 */
object Microphones {
    private val chosen = MutableStateFlow(Mic.PHONE)
    val choice: StateFlow<Mic> = chosen.asStateFlow()
    @Volatile private var loaded = false
    private val listeners = java.util.concurrent.CopyOnWriteArraySet<() -> Unit>()

    /** A call's route hears a new choice at once (Ajustes during a call). */
    fun listen(l: () -> Unit) { listeners.add(l) }
    fun unlisten(l: () -> Unit) { listeners.remove(l) }

    fun load(context: Context) {
        if (loaded) return
        loaded = true
        val saved = context.getSharedPreferences("audio", Context.MODE_PRIVATE).getString("mic", null)
        chosen.value = Mic.entries.firstOrNull { it.name == saved } ?: Mic.PHONE
    }

    fun choose(context: Context, mic: Mic) {
        loaded = true
        context.getSharedPreferences("audio", Context.MODE_PRIVATE).edit().putString("mic", mic.name).apply()
        chosen.value = mic
        listeners.forEach { it() }
    }

    /** The kind of an input the phone lists, or null for one nobody picks (telephony, FM, echo reference). */
    fun kind(type: Int): Mic? = when (type) {
        AudioDeviceInfo.TYPE_BUILTIN_MIC -> Mic.PHONE
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> Mic.WIRED
        AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET, AudioDeviceInfo.TYPE_USB_ACCESSORY -> Mic.USB
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO, AudioDeviceInfo.TYPE_BLE_HEADSET -> Mic.BLUETOOTH
        else -> null
    }

    /** The chosen one while it is there; otherwise the phone's, and never nothing. */
    fun pick(chosen: Mic, present: Set<Mic>): Mic = if (chosen in present) chosen else Mic.PHONE
}
