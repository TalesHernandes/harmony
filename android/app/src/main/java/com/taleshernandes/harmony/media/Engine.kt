package com.taleshernandes.harmony.media

import android.content.Context
import android.media.MediaRecorder
import com.taleshernandes.harmony.diag.Diag
import org.webrtc.DefaultVideoDecoderFactory
import org.webrtc.DefaultVideoEncoderFactory
import org.webrtc.EglBase
import org.webrtc.PeerConnectionFactory
import org.webrtc.audio.JavaAudioDeviceModule

/**
 * The WebRTC engine, one per process: the native library loaded once, and the
 * one factory every voice connection is made from.
 */
object Engine {
    @Volatile
    private var initialized = false
    private var factory: PeerConnectionFactory? = null
    private var denoiseController: Denoise? = null
    @Volatile internal var playbackMix: PlaybackMix? = null
    // Kept for the microphone choice only: the factory owns the native side,
    // and the Java recorder this reaches lives as long as the process.
    @Volatile private var audio: JavaAudioDeviceModule? = null
    @Volatile private var preferred: android.media.AudioDeviceInfo? = null
    /** Whether the call plays as a voice call right now; the app's own sounds follow it. */
    @Volatile var callMode = false
        private set

    /**
     * The microphone the capture reads, now and when it next opens; kept if
     * the module is not made yet (never null: the module would throw).
     */
    @Synchronized // with factory(): the module and the last choice never cross
    /**
     * Call mode for the next recorder and player: the voice-call source, whose
     * path carries the phone's own echo canceller, and voice-call playback,
     * which that canceller hears along with every other app. Applied when the
     * call restarts its recording and playout (AudioRoute).
     */
    fun setCallMode(on: Boolean) {
        callMode = on
        org.webrtc.audio.PlaybackRoute.setVoice(on)
        org.webrtc.audio.WebRtcAudioRecord.sourceOverride =
            if (on) android.media.MediaRecorder.AudioSource.VOICE_COMMUNICATION else -1
    }

    fun preferInput(device: android.media.AudioDeviceInfo) {
        preferred = device
        audio?.setPreferredInputDevice(device)
    }

    @Synchronized
    fun denoise(context: Context): Denoise = denoiseController ?: Denoise(context.applicationContext).also {
        denoiseController = it
        val filter = it
        org.webrtc.audio.WebRtcAudioRecord.captureProcessor = object : org.webrtc.audio.WebRtcAudioRecord.CaptureProcessor {
            override fun captureStarted(pcm16: java.nio.ByteBuffer, sampleRate: Int, channels: Int) {
                filter.captureStarted(pcm16, sampleRate, channels)
                EchoProbe.captureStarted()
            }
            override fun process(pcm16: java.nio.ByteBuffer, bytes: Int, sampleRate: Int, channels: Int) =
                filter.process(pcm16, bytes, sampleRate, channels)
            override fun processAfterMute(pcm16: java.nio.ByteBuffer, bytes: Int, sampleRate: Int, channels: Int) {
                playbackMix?.process(pcm16, bytes, sampleRate, channels)
            }
            override fun captureStopped(pcm16: java.nio.ByteBuffer) {
                EchoProbe.captureStopped()
                filter.captureStopped(pcm16)
            }
        }
    }

    /**
     * The one drawing context every picture shares: the decoders hand their
     * frames over as textures in it, and every renderer on screen draws from
     * it. One for the process, made on first use and never released — a call
     * that ends and another that starts would otherwise pay for it again.
     */
    val egl: EglBase by lazy { EglBase.create() }

    @Synchronized
    fun init(context: Context) {
        if (initialized) return
        PeerConnectionFactory.initialize(
            PeerConnectionFactory.InitializationOptions.builder(context.applicationContext)
                .createInitializationOptions(),
        )
        initialized = true
    }

    /**
     * The factory for real calls. Echo cancellation belongs to WebRTC AEC3.
     * Noise suppression belongs to DFN; disabling it sends raw PCM
     * without needing to replace tracks or restart the capture.
     */
    /** The factory if a call already made it; the echo probe never makes one. */
    @Synchronized
    fun existingFactory(): PeerConnectionFactory? = factory

    @Synchronized
    fun factory(context: Context): PeerConnectionFactory {
        factory?.let { return it }
        init(context)
        EchoProbe.attach(context)
        denoise(context)
        Diag.log("voice: echo=WebRTC AEC3 (+ the phone's own in call mode), capture=VOICE_RECOGNITION or VOICE_COMMUNICATION, playback=media or call, noise=DFN")
        val adm = JavaAudioDeviceModule.builder(context.applicationContext)
            .setInputSampleRate(Denoise.RATE)
            .setUseStereoInput(false)
            .setAudioSource(MediaRecorder.AudioSource.VOICE_RECOGNITION)
            .setUseHardwareAcousticEchoCanceler(false)
            .setUseHardwareNoiseSuppressor(false)
            // Recognition avoids platform AGC/NS; DFN keeps its existing capture hook.
            // Playback attributes never change when the microphone opens.
            .setEnableVolumeLogger(false)
            .setAudioAttributes(playbackAttributes())
            .setAudioRecordErrorCallback(object : JavaAudioDeviceModule.AudioRecordErrorCallback {
                override fun onWebRtcAudioRecordInitError(msg: String?) = Diag.log("voz: microfone não iniciou: $msg")
                override fun onWebRtcAudioRecordStartError(
                    code: JavaAudioDeviceModule.AudioRecordStartErrorCode?,
                    msg: String?,
                ) = Diag.log("voz: microfone não começou: $code $msg")
                override fun onWebRtcAudioRecordError(msg: String?) = Diag.log("voz: erro no microfone: $msg")
            })
            .setAudioTrackErrorCallback(object : JavaAudioDeviceModule.AudioTrackErrorCallback {
                override fun onWebRtcAudioTrackInitError(msg: String?) = Diag.log("voz: saída de som não iniciou: $msg")
                override fun onWebRtcAudioTrackStartError(
                    code: JavaAudioDeviceModule.AudioTrackStartErrorCode?,
                    msg: String?,
                ) = Diag.log("voz: saída de som não começou: $code $msg")
                override fun onWebRtcAudioTrackError(msg: String?) = Diag.log("voz: erro na saída de som: $msg")
            })
            .createAudioDeviceModule()
        // Video: the phone's own decoders and encoders first (MediaCodec),
        // with WebRTC's software ones behind them where the phone has none.
        // The screen arrives as H.264 up to 1080p60 (CLAUDE.md), which only a
        // hardware decoder does without heating the phone.
        val ctx = egl.eglBaseContext
        val f = PeerConnectionFactory.builder()
            .setAudioDeviceModule(adm)
            .setVideoDecoderFactory(DefaultVideoDecoderFactory(ctx))
            .setVideoEncoderFactory(DefaultVideoEncoderFactory(ctx, true, true))
            .createPeerConnectionFactory()
        Diag.log(
            "vídeo: decodifica " +
                DefaultVideoDecoderFactory(ctx).supportedCodecs.joinToString(" ") { it.name } +
                "; codifica " +
                DefaultVideoEncoderFactory(ctx, true, true).supportedCodecs.joinToString(" ") { it.name },
        )
        audio = adm
        preferred?.let(adm::setPreferredInputDevice)
        // The factory holds its own reference to the module from here on.
        adm.release()
        factory = f
        return f
    }
}
