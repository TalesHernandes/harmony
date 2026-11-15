package com.taleshernandes.harmony.media

import android.content.Context
import com.taleshernandes.harmony.diag.Diag
import org.webrtc.Camera2Enumerator
import org.webrtc.CameraVideoCapturer
import org.webrtc.SurfaceTextureHelper
import org.webrtc.VideoSource
import org.webrtc.VideoTrack
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * This phone's camera, for a direct call: made (the front one first), turned
 * to the back one and back as the room's is, paused while the app is out of
 * sight (Android gives no camera to an app nobody sees), and ended.
 *
 * The voice room has its own inside VoiceCall, grown over the A4 with the
 * room's needs (a slot the server names, publishing and unpublishing); a
 * direct call needs only the picture. Not thread-safe: the call's own thread
 * does everything here.
 */
class LocalCamera(private val context: Context, private val onBroken: (String) -> Unit) {
    private var capturer: CameraVideoCapturer? = null
    private var helper: SurfaceTextureHelper? = null
    private var source: VideoSource? = null
    private var running = false
    private var reopened = 0

    var track: VideoTrack? = null
        private set

    private val _front = MutableStateFlow(true)
    /** The picture is the front camera's, drawn as a mirror. */
    val front: StateFlow<Boolean> = _front

    /** Opens it; null (and said to [onBroken]) when it cannot be opened. */
    fun start(): VideoTrack? {
        track?.let { return it }
        val enumerator = Camera2Enumerator(context)
        val name = enumerator.deviceNames.firstOrNull { enumerator.isFrontFacing(it) }
            ?: enumerator.deviceNames.firstOrNull()
        if (name == null) {
            onBroken("Este celular não tem câmera que o app consiga usar.")
            return null
        }
        val cap = enumerator.createCapturer(name, events) ?: return null
        _front.value = enumerator.isFrontFacing(name)
        val factory = Engine.factory(context)
        var h: SurfaceTextureHelper? = null
        var src: VideoSource? = null
        var started = false
        val t = try {
            h = SurfaceTextureHelper.create("direct-camera", Engine.egl.eglBaseContext)
            src = factory.createVideoSource(false)
            cap.initialize(h, context, src.capturerObserver)
            cap.startCapture(WIDTH, HEIGHT, FPS)
            started = true
            factory.createVideoTrack("direct-cam", src)
        } catch (e: RuntimeException) {
            Diag.log("chamada: abrir a câmera falhou (${e.javaClass.simpleName})")
            if (started) runCatching { cap.stopCapture() }
            runCatching { cap.dispose() }
            src?.dispose()
            h?.dispose()
            onBroken("Não deu para abrir a câmera.")
            return null
        }
        capturer = cap
        helper = h
        source = src
        running = true
        reopened = 0
        track = t
        return t
    }

    /** Out of sight: the camera goes quiet, and the track stays in its slot. */
    fun pause() {
        if (!running) return
        runCatching { capturer?.stopCapture() }
        running = false
    }

    fun resume() {
        val cap = capturer ?: return
        if (running) return
        runCatching {
            cap.startCapture(WIDTH, HEIGHT, FPS)
            running = true
        }.onFailure { Diag.log("chamada: religar a câmera falhou (${it.javaClass.simpleName})") }
    }

    /**
     * Front or back, on the same capturer: the track and its slot stay, only
     * what feeds the source changes (no replaceTrack, no renegotiation).
     */
    fun switch(onFailed: () -> Unit) {
        val cap = capturer ?: return
        cap.switchCamera(object : CameraVideoCapturer.CameraSwitchHandler {
            override fun onCameraSwitchDone(isFrontCamera: Boolean) {
                Diag.log("chamada: câmera trocada, frontal=$isFrontCamera")
                _front.value = isFrontCamera
            }
            override fun onCameraSwitchError(errorDescription: String?) {
                Diag.log("chamada: trocar de câmera falhou: $errorDescription")
                onFailed()
            }
        })
    }

    /** After the camera failed by itself: closed and opened again. */
    fun reopen() {
        pause()
        resume()
    }

    /** Ends it. The caller takes the track out of its slot first. */
    fun stop() {
        if (running) runCatching { capturer?.stopCapture() }
        running = false
        capturer?.dispose()
        capturer = null
        track?.dispose()
        track = null
        source?.dispose()
        source = null
        helper?.dispose()
        helper = null
    }

    // The camera failing by itself (the system took it, it froze) is tried
    // again once; a second failure is said, and the call goes on without it.
    // These arrive on the camera's own thread, hence [onBroken] posting back.
    // [onBroken] with an empty text is "try once more" ([reopen]).
    private val events = object : CameraVideoCapturer.CameraEventsHandler {
        override fun onCameraOpening(cameraName: String?) = Diag.log("chamada: abrindo a câmera $cameraName")
        override fun onFirstFrameAvailable() = Diag.log("chamada: primeiro quadro da câmera")
        override fun onCameraClosed() = Diag.log("chamada: câmera fechada")
        override fun onCameraError(errorDescription: String?) = broke("erro: $errorDescription")
        override fun onCameraDisconnected() = broke("tomada pelo sistema")
        override fun onCameraFreezed(errorDescription: String?) = broke("congelou: $errorDescription")
    }

    private fun broke(why: String) {
        Diag.log("chamada: a câmera falhou ($why)")
        onBroken(if (reopened++ == 0) "" else "A câmera parou de funcionar. Toque em Câmera para tentar de novo.")
    }

    companion object {
        /** More than one camera to choose from: the button to switch shows only then. */
        fun canSwitch(context: Context): Boolean =
            runCatching { Camera2Enumerator(context).deviceNames.size > 1 }.getOrDefault(false)

        // The web's camera (media.ts): 1280x720 at 30.
        private const val WIDTH = 1280
        private const val HEIGHT = 720
        private const val FPS = 30
    }
}
