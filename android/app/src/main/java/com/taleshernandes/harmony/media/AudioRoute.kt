package com.taleshernandes.harmony.media

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.MediaRecorder
import android.os.Build
import android.os.Handler
import android.os.Looper
import com.taleshernandes.harmony.diag.Diag
import org.webrtc.audio.PlaybackRoute

/**
 * Where the sound of a call goes, while one is running.
 *
 * What is plugged in is watched for the whole call, so a headset connected in
 * the middle of a sentence takes the sound, and taking it off gives it back to
 * the loudspeaker. The rule itself lives in [OutputPolicy]; this is only the
 * phone's side of it. [onChange] hears what is present and what is in use,
 * from whichever thread the phone reports on.
 *
 * It also switches between media and call mode ([playbackMode]) as the
 * output and the microphone change. The recorder's source and the player's
 * usage are fixed while they run, so a switch ends with [restartAudio]: the
 * call stops and starts its own recording and playout (a fraction of a
 * second), on its own thread.
 */
class AudioRoute(
    context: Context,
    phoneLike: Boolean = false,
    private val restartAudio: () -> Unit = {},
    private val onChange: (present: Set<Output>, current: Output?) -> Unit,
) {
    private val appContext = context.applicationContext
    private val am = context.getSystemService(AudioManager::class.java)
    private val main = Handler(Looper.getMainLooper())
    private val policy = OutputPolicy(phoneLike)
    private var focus: AudioFocusRequest? = null
    private var active = false
    private var present: Set<Output> = emptySet()
    private var current: Output? = null
    private var micUsed: Mic? = null
    // Which device of that kind: two USB ones, or a headset that comes back
    // under a new id, must be pointed at again (Codex, 25/10/2026).
    private var micId: Int? = null
    // Each choice starts its own wait for the recorder; older ones stop.
    private var checks = 0
    // The headset's voice link (SCO or LE audio), open only while its
    // microphone is the one chosen; the mode stays normal while it is open.
    private var link = false
    private var micOpen = false
    private var mode = PlaybackMode.MEDIA
    // Set by call mode only, so leaving it does not undo the headset's link.
    private var callDevice = false
    private val micChanged: () -> Unit = { main.post { refresh("escolha do microfone") } }

    private val devices = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(added: Array<out AudioDeviceInfo>) = refresh("chegou")
        override fun onAudioDevicesRemoved(removed: Array<out AudioDeviceInfo>) = refresh("saiu")
    }

    @Synchronized
    fun start() {
        if (active) return
        active = true
        mode = PlaybackMode.MEDIA
        enterMode(PlaybackMode.MEDIA)
        present = emptySet()
        micUsed = null
        micId = null
        Microphones.load(appContext)
        Microphones.listen(micChanged)
        // Registering reports every device already there, which is the first refresh.
        am.registerAudioDeviceCallback(devices, main)
        refresh("início")
    }

    @Synchronized
    fun stop() {
        if (!active) return
        active = false
        am.unregisterAudioDeviceCallback(devices)
        Microphones.unlisten(micChanged)
        closeLink()
        inputs()[Mic.PHONE]?.let(Engine::preferInput)
        micUsed = null
        micId = null
        checks++
        PlaybackRoute.setPreferredDevice(null)
        clearCallDevice()
        focus?.let { am.abandonAudioFocusRequest(it) }
        focus = null
        Engine.setCallMode(false)
        mode = PlaybackMode.MEDIA
        micOpen = false
        am.mode = AudioManager.MODE_NORMAL
        present = emptySet()
        current = null
        onChange(present, current)
    }

    /** The person picked an output by hand. */
    @Synchronized
    fun choose(o: Output) {
        if (!active) return
        policy.choose(o)
        refresh("escolha")
    }

    /**
     * The microphone opened or closed (mute, deafen, or none yet). Set before
     * [start] too, so a call does not begin in the wrong mode.
     */
    @Synchronized
    fun setMicOpen(open: Boolean) {
        if (micOpen == open) return
        micOpen = open
        refresh("microfone ${if (open) "aberto" else "fechado"}")
    }

    /** A picture arrived on the call, or went: see [OutputPolicy.video]. */
    @Synchronized
    fun setVideo(on: Boolean) {
        if (policy.video == on) return
        policy.video = on
        refresh("vídeo")
    }

    @Synchronized
    private fun refresh(why: String) {
        if (!active) return
        // The microphone first: opening or closing the headset's voice link
        // changes which of its outputs is there, and the sound must follow.
        val linked = link
        applyMic(why)
        val before = present
        val now = listed().keys
        present = now
        val pick = policy.pick(now, before)
        // The headset's voice link already is the telephone path, and call
        // mode would take its microphone away: never both.
        val want = if (link) PlaybackMode.MEDIA else playbackMode(pick, micOpen)
        val switched = want != mode
        if (switched) {
            mode = want
            enterMode(want)
        }
        if (switched || pick != current || why == "início" || before != now || linked != link) {
            use(pick)
            Diag.log("voz: saída ${pick?.label ?: "nenhuma"}, ${if (mode == PlaybackMode.CALL) "modo ligação" else "mídia"} " +
                "($why; há ${now.joinToString { it.label }})")
            current = pick
        }
        if (switched && why != "início") restartAudio()
        onChange(present, current)
    }

    /** The phone's mode and focus, and what the next recorder and player use. */
    private fun enterMode(m: PlaybackMode) {
        val call = m == PlaybackMode.CALL
        Engine.setCallMode(call)
        am.mode = if (call) AudioManager.MODE_IN_COMMUNICATION else AudioManager.MODE_NORMAL
        if (!call) clearCallDevice()
        focus?.let { am.abandonAudioFocusRequest(it) }
        focus = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(playbackAttributes(m)).build().also { am.requestAudioFocus(it) }
    }

    @Suppress("DEPRECATION") // isSpeakerphoneOn: the way before Android 12
    private fun clearCallDevice() {
        if (!callDevice) return
        callDevice = false
        try {
            if (Build.VERSION.SDK_INT >= 31) am.clearCommunicationDevice() else am.isSpeakerphoneOn = false
        } catch (e: RuntimeException) {
            Diag.log("voz: soltar a saída da ligação falhou (${e.javaClass.simpleName})")
        }
    }

    /** The inputs present, each with the device that is it. */
    private fun inputs(): Map<Mic, AudioDeviceInfo> {
        val out = LinkedHashMap<Mic, AudioDeviceInfo>()
        for (d in am.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
            val m = Microphones.kind(d.type) ?: continue
            if (m !in out) out[m] = d
        }
        return out
    }

    @Synchronized
    private fun applyMic(why: String) {
        if (!active) return
        val there = inputs()
        val chosen = Microphones.choice.value
        val want = Microphones.pick(chosen, there.keys)
        val device = there[want]
        // A refused link is tried again at the next change: the headset's
        // voice channel often turns up a moment after its microphone does.
        if (want == micUsed && device?.id == micId && (want != Mic.BLUETOOTH || link)) return
        if (want == Mic.BLUETOOTH) openLink() else closeLink()
        val again = want == micUsed && device?.id == micId
        device?.let(Engine::preferInput)
        micUsed = want
        micId = device?.id
        if (again) return
        Diag.log("voz: microfone ${want.label} (escolhido ${chosen.label}; $why; há ${there.keys.joinToString { it.label }})")
        val check = ++checks
        main.postDelayed({ checkMic(want, check, 1) }, CHECK_EVERY_MS)
    }

    @Suppress("DEPRECATION") // startBluetoothSco: the way before Android 12
    private fun openLink() {
        if (link) return
        link = try {
            if (Build.VERSION.SDK_INT >= 31) {
                val headset = am.availableCommunicationDevices.firstOrNull {
                    it.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO || it.type == AudioDeviceInfo.TYPE_BLE_HEADSET
                }
                headset != null && am.setCommunicationDevice(headset)
            } else {
                am.startBluetoothSco()
                am.isBluetoothScoOn = true
                true
            }
        } catch (e: RuntimeException) {
            Diag.log("voz: link de voz do fone falhou (${e.javaClass.simpleName})")
            false
        }
        Diag.log("voz: link de voz do fone ${if (link) "aberto" else "recusado"} (modo ${am.mode})")
    }

    @Suppress("DEPRECATION")
    private fun closeLink() {
        if (!link) return
        link = false
        try {
            if (Build.VERSION.SDK_INT >= 31) am.clearCommunicationDevice()
            else {
                am.isBluetoothScoOn = false
                am.stopBluetoothSco()
            }
        } catch (e: RuntimeException) {
            Diag.log("voz: fechar o link de voz do fone falhou (${e.javaClass.simpleName})")
        }
        Diag.log("voz: link de voz do fone fechado")
    }

    /**
     * What the phone really records from, a few seconds after the choice:
     * whether this phone gives the headset's microphone without the call
     * mode is the open question of 25/10/2026, and only the device answers.
     */
    @Synchronized
    private fun checkMic(asked: Mic, check: Int, attempt: Int) {
        if (!active || micUsed != asked || check != checks) return
        if (Build.VERSION.SDK_INT < 29) { Diag.log("voz: microfone em uso: não encontrado (Android antigo)"); return }
        val ours = am.activeRecordingConfigurations.firstOrNull {
            it.clientAudioSource == MediaRecorder.AudioSource.VOICE_RECOGNITION ||
                it.clientAudioSource == MediaRecorder.AudioSource.VOICE_COMMUNICATION
        }
        // A direct call routes before it is answered, and records only after:
        // wait for the recorder instead of reporting on one not yet open.
        if (ours == null && attempt < CHECK_TRIES) {
            main.postDelayed({ checkMic(asked, check, attempt + 1) }, CHECK_EVERY_MS)
            return
        }
        val device = ours?.audioDevice
        val real = device?.let { Microphones.kind(it.type) }
        Diag.log("voz: microfone em uso ${real?.label ?: "não encontrado"} (tipo ${device?.type ?: "-"}; pedido ${asked.label}; modo ${am.mode}; " +
            "saída tipo ${PlaybackRoute.output().deviceType})")
        if (asked == Mic.BLUETOOTH && ours != null && real != Mic.BLUETOOTH)
            Diag.log("voz: o aparelho não deu o microfone do fone Bluetooth sem o modo de chamada")
    }

    /** The outputs present, each with the device that is it. */
    private fun listed(): Map<Output, AudioDeviceInfo> {
        val all = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        val out = LinkedHashMap<Output, AudioDeviceInfo>()
        // With the voice link open the headset's music channel is suspended:
        // its sound goes out on the voice one.
        if (link) all.firstOrNull { it.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO || it.type == AudioDeviceInfo.TYPE_BLE_HEADSET }
            ?.let { out[Output.BLUETOOTH] = it }
        for (d in all) {
            val o = kind(d.type) ?: continue
            if (o !in out) out[o] = d
        }
        return out
    }

    @Suppress("DEPRECATION") // isSpeakerphoneOn: the way before Android 12
    private fun use(o: Output?) {
        // Media: only the player's preferred device. Call mode routes by the
        // communication device (the earpiece is its default, so the
        // loudspeaker must be asked for).
        PlaybackRoute.setPreferredDevice(listed()[o])
        if (mode != PlaybackMode.CALL || o == null) return
        try {
            callDevice = if (Build.VERSION.SDK_INT >= 31) {
                val type = if (o == Output.SPEAKER) AudioDeviceInfo.TYPE_BUILTIN_SPEAKER else AudioDeviceInfo.TYPE_BUILTIN_EARPIECE
                val device = am.availableCommunicationDevices.firstOrNull { it.type == type }
                device != null && am.setCommunicationDevice(device)
            } else {
                am.isSpeakerphoneOn = o == Output.SPEAKER
                true
            }
            if (!callDevice) Diag.log("voz: o aparelho recusou ${o.label} no modo ligação")
        } catch (e: RuntimeException) {
            Diag.log("voz: saída da ligação falhou (${e.javaClass.simpleName})")
        }
    }

    private fun kind(type: Int): Output? = when (type) {
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> Output.SPEAKER
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> Output.EARPIECE
        AudioDeviceInfo.TYPE_WIRED_HEADSET, AudioDeviceInfo.TYPE_WIRED_HEADPHONES, AudioDeviceInfo.TYPE_USB_HEADSET -> Output.WIRED
        // Bluetooth media; its voice channel only while its microphone is chosen (listed()).
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> Output.BLUETOOTH
        else -> null
    }

    private companion object {
        const val CHECK_EVERY_MS = 3_000L
        const val CHECK_TRIES = 40 // two minutes of ringing
    }
}

/**
 * The attributes of each mode. The module is created with media's; call mode
 * reaches the player through PlaybackRoute.setVoice (Engine.setCallMode).
 */
internal fun playbackAttributes(mode: PlaybackMode = PlaybackMode.MEDIA): AudioAttributes = when (mode) {
    PlaybackMode.MEDIA -> AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_MEDIA)
        .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
        .build()
    PlaybackMode.CALL -> AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
        .build()
}
