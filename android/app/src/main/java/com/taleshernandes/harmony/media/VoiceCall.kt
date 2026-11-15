package com.taleshernandes.harmony.media

import android.content.Context
import android.content.Intent
import android.hardware.display.DisplayManager
import android.media.projection.MediaProjection
import android.net.ConnectivityManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.graphics.Point
import android.view.WindowManager
import com.taleshernandes.harmony.BuildConfig
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.model.GatewayEvent
import com.taleshernandes.harmony.model.Peer
import com.taleshernandes.harmony.model.TrackInfo
import com.taleshernandes.harmony.model.VoiceOffer
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.ApiError
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import org.webrtc.AudioSource
import org.webrtc.AudioTrack
import org.webrtc.Camera2Enumerator
import org.webrtc.CameraVideoCapturer
import org.webrtc.RtpParameters
import org.webrtc.MediaStreamTrack
import org.webrtc.ScreenCapturerAndroid
import org.webrtc.SurfaceTextureHelper
import org.webrtc.VideoSource
import org.webrtc.DataChannel
import org.webrtc.IceCandidate
import org.webrtc.MediaConstraints
import org.webrtc.MediaStream
import org.webrtc.PeerConnection
import org.webrtc.RtpReceiver
import org.webrtc.RtpTransceiver
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import org.webrtc.VideoSink
import org.webrtc.VideoTrack
import java.io.IOException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

/**
 * A voice room, from this phone's side (PROTOCOL-HARMONY §4).
 *
 * Every step that touches the connection runs on one thread, in order: the
 * join, each offer the server sends, leaving. That is the web's queue
 * (media.ts offer()) — two offers answered at once would be two answers to one
 * connection, and WebRTC refuses the second.
 */
class VoiceCall(private val context: Context, private val api: () -> Api?) : Stage {
    enum class Phase { OFF, JOINING, CONNECTING, LIVE, RECONNECTING }

    /**
     * A picture arriving: whose, and whether it is their screen or their
     * camera. Plain data — the track itself never goes into the screen's state
     * (CLAUDE.md); a renderer asks for it by [mid] through [attach].
     */
    data class Video(val mid: String, val username: String, val displayName: String, val source: String) {
        val isScreen get() = source == "screen"
    }

    data class State(
        val channelId: String? = null,
        val sessionId: String? = null,
        val phase: Phase = Phase.OFF,
        // Kept across rooms, the way Discord keeps them: muting and then
        // walking into another room should not open the microphone.
        val muted: Boolean = false,
        val deafened: Boolean = false,
        val speaking: Set<String> = emptySet(),
        val notice: String? = null,
        /** The outputs present right now, and the one the sound is going to. */
        val outputs: List<Output> = emptyList(),
        val output: Output? = null,
        /** The pictures arriving now: screens this phone asked for, and cameras. */
        val videos: List<Video> = emptyList(),
        /** Whose screen this phone asked for — one at a time on a phone. */
        val watching: String? = null,
        /** This phone's own camera is on (paused while the app is out of sight). */
        val camera: Boolean = false,
        /**
         * The person's choice, which outlives a reconnect: the button shows
         * this, not [camera] (whether a picture is being made right now), or
         * a reconnect shows "off" and then turns the camera back on by itself.
         */
        val cameraChosen: Boolean = false,
        /** Capture is running, including while the voice connection starts over. */
        val screen: Boolean = false,
        /** The button's choice, including a pending foreground-service handshake. */
        val screenChosen: Boolean = false,
    )

    private val _state = MutableStateFlow(State())
    val state: StateFlow<State> = _state

    private val exec = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "voice") }
    init { com.taleshernandes.harmony.model.PersonalPreferences.listen { exec.execute { applyVolumes() } } }
    private val net = Executors.newSingleThreadExecutor { r -> Thread(r, "voice-net") }
    private val route = AudioRoute(context, restartAudio = { exec.execute { restartAudio() } }) { present, current ->
        _state.update { s -> s.copy(outputs = Output.entries.filter { it in present }, output = current) }
    }
    private val connectivity = context.getSystemService(ConnectivityManager::class.java)
    private val speaking = Speaking()
    override val volumes = Volumes(context)

    // This phone's camera, while it is on. Made and ended on the voice thread;
    // the track is also read by [attach] for the preview, under [videoLock].
    private var capturer: CameraVideoCapturer? = null
    private var camHelper: SurfaceTextureHelper? = null
    private var camSource: VideoSource? = null
    private var camTrack: VideoTrack? = null
    private var camMid: String? = null
    private var camRunning = false
    // Counts the camera's turns on and off, so that a reply about an old turn
    // (a publish that failed after the camera was already off and on again)
    // does not act on the new one.
    private var camTurn = 0
    // The person's wish, which outlives a connection: a network switch starts
    // the call over, and the camera that was on comes back on by itself.
    private var cameraWanted = false
        set(v) { field = v; _state.update { it.copy(cameraChosen = v) } }
    // Reopenings after the camera itself failed, since its last good picture.
    private var camReopens = 0
    // Recoveries of a camera that was published and never got its slot (a lost
    // offer wedges the session) or whose publish failed: a few, then it gives up.
    private var camRecoveries = 0
    private var camRetries = 0
    // What the camera was sending at the last look (framesEncoded, when), and
    // whether it was moving — a stall is said once, not every second.
    private val camWatch = CameraSendWatch()

    // A projection grant is single-use. Keep this capture alive across a new
    // voice connection, and replace only the sender's track in its new slot.
    private val screenChoice = ScreenChoice()
    private var screenCapturer: ScreenCapturerAndroid? = null
    private var screenHelper: SurfaceTextureHelper? = null
    private var screenRepeater: ScreenFrameRepeater? = null
    private var screenSource: VideoSource? = null
    private var screenTrack: VideoTrack? = null
    private var screenCodec: String? = null
    private var screenSize: ScreenShare.Size? = null
    private var screenMid: String? = null
    private var screenSession: String? = null
    private var screenServiceTurn: Int? = null
    private var screenStartTimeout: ScheduledFuture<*>? = null
    private var screenMeter: ScheduledFuture<*>? = null
    private var screenDisplayListener: DisplayManager.DisplayListener? = null
    private var screenFrames = -1L
    private var screenFramesAt = 0L
    private var screenMoving = false
    private var screenSaidAt = 0L
    private var screenOptions = ScreenOptions()
    @Volatile private var screenAudio: ScreenAudio? = null

    // Owned by the "voice" thread.
    private var pc: PeerConnection? = null
    private var source: AudioSource? = null
    // Read from the screen's thread too: muting must not wait behind a slow
    // request on the voice thread while the microphone goes on sending.
    // Every touch of it, and its disposal, holds [micLock].
    @Volatile private var mic: AudioTrack? = null
    private val micLock = Any()
    private var info: Map<String, TrackInfo> = emptyMap()
    // Who else was in the room, as last heard on the live socket (Stand.signature).
    private var roomSig: String? = null
    // Read by getStats callbacks, which arrive on WebRTC's own thread.
    @Volatile private var generation = 0
    private var attempts = 0
    private var retry: ScheduledFuture<*>? = null
    private var meter: ScheduledFuture<*>? = null
    private var stall: ScheduledFuture<*>? = null
    private var giveUp: ScheduledFuture<*>? = null
    private var me: String = ""
    private var connected = false
    private var verifying = false
    // A reconnect that found no network at all, waiting for one to come back.
    private var waitingNetwork = false
    // Whether anybody is looking: the "who is talking" ring is read ten times
    // a second, and nobody needs that with the screen off in a pocket.
    private var watching = true

    // The pictures, by slot, and who is drawing each one. Touched from the
    // screen's thread (a renderer coming and going) and from the voice thread
    // (a renegotiation, the end of the call), always under [videoLock]: a
    // renderer must never be handed to, or taken from, a track that has
    // already been disposed with its connection.
    private val videoLock = Any()
    private val videoTracks = HashMap<String, VideoTrack>()
    // The connection's slots, read once per negotiation. Reading them again
    // (PeerConnection.getTransceivers) disposes every wrapper handed out the
    // time before — the tracks inside included — so a track kept from an
    // earlier read is a track that throws "has been disposed" the moment a
    // renderer touches it (the emulator, 25/09/2026). Everything reads this.
    private var slots: List<RtpTransceiver> = emptyList()
    private val sinks = HashMap<String, MutableSet<VideoSink>>()

    // Which picture of each screen to ask for (Layers.kt, the web's rules).
    private val layerAsk = LayerAsk()
    private val decodeCap = DecodeCap()
    private val screenDiagnostics = ScreenDiagnostics()
    private var videoDelayObservation = VideoDelayObservation()
    private var syncError = SyncError()
    private var outputDelay = OutputDelay()
    private var outputDelayJob: java.util.concurrent.ScheduledFuture<*>? = null
    private var outputDelayLogAt = Long.MIN_VALUE
    // How wide each screen is drawn right now, in real pixels, and whether
    // anybody can see it. Written by the screen, read on the voice thread.
    private val drawn = java.util.concurrent.ConcurrentHashMap<String, Int>()
    @Volatile private var looking = true
    private var tick = 0
    // The screen the person asked for. It outlives a reconnect, which starts
    // a new session with nothing watched, and is asked for again once the new
    // connection stands (media.ts does the same).
    private var watchWanted: String? = null
    // Counts the choices of screen, for the same reason as [camTurn].
    private var watchTurn = 0

    private val _screenProgress = MutableStateFlow<Map<String, ScreenProgress>>(emptyMap())
    override val screenProgress: StateFlow<Map<String, ScreenProgress>> = _screenProgress
    private val _front = MutableStateFlow(true)
    override val frontCamera: StateFlow<Boolean> = _front
    private val _volumeRevision = MutableStateFlow(0)
    override val volumeRevision: StateFlow<Int> = _volumeRevision

    // ------------------------------------------------------------ commands

    fun join(channelId: String, myUsername: String) {
        if (!microphoneAllowed(context)) _state.update { it.copy(muted = true) }
        // Said at once, not when the voice thread gets to it: the call's
        // service starts on this, while the tap still counts as the person
        // being here (Android refuses a microphone service started later).
        _state.update { it.copy(channelId = channelId, phase = Phase.JOINING, notice = null) }
        exec.execute {
            me = myUsername
            volumes.forAccount(myUsername)
            attempts = 0
            watchWanted = null
            cameraWanted = false
            endScreen()
            stopWaiting()
            doJoin(channelId)
        }
    }

    fun leave() {
        silence()
        exec.execute { doLeave() }
    }

    private fun doLeave() {
        stopWaiting()
        watchWanted = null
        cameraWanted = false
        endScreen()
        teardown(tellServer = true)
        _state.update { it.copy(channelId = null, sessionId = null, phase = Phase.OFF, notice = null) }
    }

    /** The microphone off now, on the caller's thread. */
    private fun silence() = synchronized(micLock) { mic?.setEnabled(false) }

    fun chooseOutput(o: Output) = route.choose(o)

    // ------------------------------------------------------------ pictures

    /** Draws the picture in slot [mid] into [sink] while it lasts. */
    override fun attach(mid: String, sink: VideoSink) = synchronized(videoLock) {
        sinks.getOrPut(mid) { mutableSetOf() }.add(sink)
        // A negotiation may have just replaced the track; the sink stays on
        // the list and goes onto the new one in collectVideos.
        runCatching { trackFor(mid)?.addSink(sink) }
        Unit
    }

    override fun detach(mid: String, sink: VideoSink) = synchronized(videoLock) {
        sinks[mid]?.remove(sink)
        runCatching { trackFor(mid)?.removeSink(sink) }
        Unit
    }

    private fun trackFor(mid: String): VideoTrack? = if (mid == LOCAL_CAMERA) camTrack else videoTracks[mid]

    /** The picture of this phone's own camera, for its preview. */
    fun localCamera(): Video = Video(LOCAL_CAMERA, me, "Você", "cam")

    override val ownCamera get() = LOCAL_CAMERA

    /** The person's volume changed: every sound of theirs follows at once. */
    override fun setVolume(username: String, screen: Boolean, level: Float) {
        volumes.set(username, screen, level)
        _volumeRevision.update { it + 1 }
        exec.execute { applyVolumes() }
    }

    // ------------------------------------------------------------ own camera

    /**
     * Turns this phone's camera on or off (PROTOCOL-HARMONY §4.6): the picture
     * is made first and the server asked for a slot after, because the offer
     * that answers names the slot and the track has to be ready to go in it.
     */
    fun setCamera(on: Boolean) = exec.execute {
        cameraWanted = on
        if (!on) { camRecoveries = 0; camRetries = 0 }
        // Off is off at once, whatever the connection is doing. It used to
        // wait for a session first, so a press during a reconnect left the
        // camera running and on screen, and the reconnect brought it back to
        // the others (the owner, 01/10/2026). The server is told when it can
        // be: a new session starts with no camera, and cameraBack() asks
        // cameraWanted, which is false now.
        if (!on) {
            val turn = ++camTurn
            stopCamera()
            val sid = _state.value.sessionId
            val a = api()
            if (sid != null && a != null) unpublishCamera(a, sid, turn)
            return@execute
        }
        val sid = _state.value.sessionId ?: return@execute
        val a = api() ?: return@execute
        if (on == _state.value.camera) return@execute
        val turn = ++camTurn
        if (on) {
            if (!startCamera()) return@execute
            _state.update { it.copy(camera = true) }
            net.execute {
                try {
                    a.voicePublish(sid, listOf("cam"))
                    Diag.log("vídeo: câmera ligada")
                    exec.schedule({ camOfferWatchdog(turn, sid) }, CAM_OFFER_MS, TimeUnit.MILLISECONDS)
                } catch (e: IOException) {
                    Diag.log("vídeo: ligar a câmera falhou (${e.javaClass.simpleName})")
                    exec.execute {
                        if (turn != camTurn || _state.value.sessionId != sid) return@execute
                        camTurn++
                        stopCamera()
                        // A failed publish is no reason to forget the choice: a
                        // couple of tries, a few seconds apart, then it gives up.
                        if (camRetries < 2) {
                            camRetries++
                            Diag.log("vídeo: tentando ligar a câmera de novo ($camRetries)")
                            exec.schedule({ cameraBack() }, 3000, TimeUnit.MILLISECONDS)
                        } else {
                            cameraWanted = false
                            notice("Não deu para ligar a câmera.")
                        }
                        // The request may have reached the server and only the
                        // reply been lost: a slot nobody fills is let go of.
                        unpublishCamera(a, sid, camTurn)
                    }
                }
            }
        } else {
            stopCamera()
            unpublishCamera(a, sid, turn)
        }
    }

    /**
     * Tells the server the camera is off, again if the first word is lost —
     * or the others go on seeing a slot that says camera and shows nothing.
     */
    private fun unpublishCamera(a: Api, sid: String, turn: Int, tries: Int = 0) {
        net.execute {
            try {
                a.voiceUnpublish(sid, listOf("cam"))
                Diag.log("vídeo: câmera desligada")
            } catch (e: IOException) {
                Diag.log("vídeo: desligar a câmera falhou (${e.javaClass.simpleName})")
                if (tries < 3) {
                    exec.schedule({
                        if (turn == camTurn && _state.value.sessionId == sid) unpublishCamera(a, sid, turn, tries + 1)
                    }, 2000, TimeUnit.MILLISECONDS)
                }
            }
        }
    }

    /** More than one camera to choose from: the button to switch shows only then. */
    override fun canSwitchCamera(): Boolean = LocalCamera.canSwitch(context)

    /**
     * Front or back, on the same capturer: the track and its publication stay,
     * only what feeds the source changes (no replaceTrack, no renegotiation).
     */
    override fun switchCamera() = exec.execute {
        val cap = capturer ?: return@execute
        cap.switchCamera(object : CameraVideoCapturer.CameraSwitchHandler {
            override fun onCameraSwitchDone(isFrontCamera: Boolean) {
                Diag.log("vídeo: câmera trocada, frontal=$isFrontCamera")
                _front.value = isFrontCamera
            }
            override fun onCameraSwitchError(errorDescription: String?) {
                Diag.log("vídeo: trocar de câmera falhou: $errorDescription")
                notice("Não deu para trocar de câmera.")
            }
        })
    }

    /**
     * The publish was accepted, but a slot arrives only in an offer over the
     * socket, once, and an offer sent while the socket was being reopened
     * (a network switch) is lost for good: the voice is up and the camera has
     * no slot. Nothing else notices, so after a while the room is rejoined —
     * the camera choice survives that — at most twice.
     */
    private fun camOfferWatchdog(turn: Int, sid: String) {
        if (turn != camTurn || _state.value.sessionId != sid || !_state.value.camera || camMid != null) return
        if (camRecoveries >= 2) {
            Diag.log("vídeo: a câmera continua sem oferta; desisto")
            return
        }
        camRecoveries++
        Diag.log("vídeo: a oferta da câmera não chegou em ${CAM_OFFER_MS / 1000} s; entrando de novo na sala ($camRecoveries)")
        rejoin("câmera sem oferta")
    }

    private fun startCamera(): Boolean {
        val enumerator = Camera2Enumerator(context)
        val name = enumerator.deviceNames.firstOrNull { enumerator.isFrontFacing(it) }
            ?: enumerator.deviceNames.firstOrNull()
        if (name == null) {
            notice("Este celular não tem câmera que o app consiga usar.")
            return false
        }
        val cap = enumerator.createCapturer(name, camEvents) ?: return false
        _front.value = enumerator.isFrontFacing(name)
        val factory = Engine.factory(context)
        var helper: SurfaceTextureHelper? = null
        var src: VideoSource? = null
        var started = false
        val track = try {
            helper = SurfaceTextureHelper.create("camera", Engine.egl.eglBaseContext)
            src = factory.createVideoSource(false)
            cap.initialize(helper, context, src.capturerObserver)
            // What the web asks for (media.ts, 1280x720 at 30): the server's
            // simulcast is for screens; a camera goes up as one picture.
            cap.startCapture(CAM_WIDTH, CAM_HEIGHT, CAM_FPS)
            started = true
            factory.createVideoTrack("cam", src)
        } catch (e: RuntimeException) {
            // Half made is let go of here: nothing points at it yet, so
            // leaving the room would not find it.
            Diag.log("vídeo: abrir a câmera falhou (${e.javaClass.simpleName})")
            if (started) runCatching { cap.stopCapture() }
            runCatching { cap.dispose() }
            src?.dispose()
            helper?.dispose()
            notice("Não deu para abrir a câmera.")
            return false
        }
        camRunning = true
        camWatch.reset()
        capturer = cap
        camHelper = helper
        camSource = src
        synchronized(videoLock) {
            camTrack = track
            sinks[LOCAL_CAMERA]?.forEach { track.addSink(it) }
        }
        return true
    }

    /**
     * What the camera itself says. Without this a camera that fails or stops
     * producing pictures is invisible: the slot stays open, the room says
     * "câmera", and the others see nothing (25/09/2026, on the owner's phone:
     * a camera turned on again after a network switch sent almost nothing for
     * 15 s, and nothing in the log said why).
     */
    private val camEvents = object : CameraVideoCapturer.CameraEventsHandler {
        override fun onCameraOpening(cameraName: String?) = Diag.log("vídeo: abrindo a câmera $cameraName")
        override fun onFirstFrameAvailable() {
            Diag.log("vídeo: primeiro quadro da câmera")
            exec.execute { camReopens = 0 }
        }
        override fun onCameraClosed() = Diag.log("vídeo: câmera fechada")
        override fun onCameraError(errorDescription: String?) {
            Diag.log("vídeo: erro na câmera: $errorDescription")
            exec.execute { reopenCamera("erro") }
        }
        override fun onCameraDisconnected() {
            Diag.log("vídeo: o sistema tomou a câmera")
            exec.execute { reopenCamera("tomada pelo sistema") }
        }
        override fun onCameraFreezed(errorDescription: String?) {
            Diag.log("vídeo: a câmera congelou: $errorDescription")
            exec.execute { reopenCamera("congelou") }
        }
    }

    /** Opens the camera again after it failed by itself, twice at most in a row. */
    private fun reopenCamera(why: String) {
        val cap = capturer ?: return
        if (!looking) return // paused on purpose; it reopens with the app
        if (camReopens >= 2) {
            Diag.log("vídeo: a câmera falhou de novo ($why); desligando")
            setCamera(false)
            notice("A câmera parou de funcionar. Toque em Câmera para tentar de novo.")
            return
        }
        camReopens++
        Diag.log("vídeo: reabrindo a câmera ($why, vez $camReopens)")
        runCatching { if (camRunning) cap.stopCapture() }
        camRunning = false
        runCatching {
            cap.startCapture(CAM_WIDTH, CAM_HEIGHT, CAM_FPS)
            camRunning = true
        }.onFailure { Diag.log("vídeo: reabrir a câmera falhou (${it.javaClass.simpleName})") }
    }

    /** Once a second, from the same report as the decoders: is the camera going out? */
    private fun judgeCamera(report: org.webrtc.RTCStatsReport, now: Long) {
        if (!camRunning || camTrack == null) return
        camWatch.judge(report, camMid, now)
    }

    private fun stopCamera() {
        val track = camTrack
        synchronized(videoLock) {
            if (track != null) sinks[LOCAL_CAMERA]?.forEach { runCatching { track.removeSink(it) } }
            camTrack = null
        }
        // Out of its slot first, so nothing sends a track that is going away.
        slots.forEach { tr -> if (track != null && tr.sender.track() === track) tr.sender.setTrack(null, false) }
        runCatching { if (camRunning) capturer?.stopCapture() }
        camRunning = false
        capturer?.dispose()
        capturer = null
        track?.dispose()
        camSource?.dispose()
        camSource = null
        camHelper?.dispose()
        camHelper = null
        camMid = null
        _state.update { it.copy(camera = false) }
    }

    /**
     * The camera was on and the call started over (a network switch, the
     * server letting go): it comes back on, as the voice does. Only with the
     * app on screen — Android does not open a camera for an app out of sight —
     * and otherwise when the app comes back.
     */
    private fun cameraBack() {
        val s = _state.value
        if (!cameraWanted || s.camera || s.phase != Phase.LIVE || !looking) return
        Diag.log("vídeo: religando a câmera depois de entrar de novo")
        setCamera(true)
    }

    /** Puts the camera into the slot the server named, at the web's ceiling. */
    private fun fitCamera(tr: RtpTransceiver) {
        val track = camTrack ?: return
        tr.sender.setTrack(track, false)
        camMid = tr.mid
        camRecoveries = 0
        camRetries = 0
        val params = tr.sender.parameters
        params.encodings.firstOrNull()?.maxBitrateBps = CAM_MAX_BPS
        params.degradationPreference = RtpParameters.DegradationPreference.BALANCED
        tr.sender.parameters = params
    }

    // ------------------------------------------------------------ own screen

    /** [projection] must be a fresh RESULT_OK result from Android's consent UI. */
    fun setScreen(on: Boolean, projection: Intent? = null, options: ScreenOptions = ScreenOptions()) = exec.execute {
        // Off precedes every session/API check, including while reconnecting.
        if (!on) {
            endScreen()
            return@execute
        }
        if (projection == null || screenChoice.wanted) return@execute
        val room = _state.value.channelId ?: return@execute
        val turn = screenChoice.choose(true)
        screenOptions = options
        _state.update { it.copy(screenChosen = true) }
        screenServiceTurn = turn
        screenStartTimeout = exec.schedule({
            if (screenChoice.accepts(turn) && screenTrack == null) {
                endScreen()
                notice("Não deu para iniciar a captura da tela. Tente de novo.")
            }
        }, 10, TimeUnit.SECONDS)
        // startForegroundService() is only a request. The service acknowledges
        // startForeground(mediaProjection) BEFORE getMediaProjection is called.
        CallService.startProjection(context, room, turn) { ready ->
            exec.execute {
                if (!screenChoice.accepts(turn) || _state.value.channelId != room) {
                    CallService.endProjection(turn)
                    return@execute
                }
                screenStartTimeout?.cancel(false)
                screenStartTimeout = null
                if (!ready || !startScreen(projection, turn)) {
                    endScreen()
                    notice("Não deu para compartilhar a tela. Tente de novo com o app aberto.")
                    return@execute
                }
                publishScreen()
            }
        }
    }

    private fun publishScreen() {
        val sid = _state.value.sessionId
        if (screenTrack == null || !screenChoice.shouldPublish(sid, screenSession)) return
        val session = sid ?: return
        val a = api() ?: return
        val turn = screenChoice.turn
        screenSession = session
        net.execute {
            try {
                a.voicePublish(session, listOf("screen"))
                Diag.log("screen: publication requested")
            } catch (e: IOException) {
                Diag.log("screen: publish failed (${e.javaClass.simpleName})")
                exec.execute {
                    if (!screenChoice.accepts(turn) || _state.value.sessionId != session) return@execute
                    endScreen()
                    notice("Não deu para enviar a tela para a sala.")
                }
            }
        }
    }

    private fun unpublishScreen(a: Api, sid: String, turn: Int, tries: Int = 0) {
        net.execute {
            try {
                a.voiceUnpublish(sid, listOf("screen"))
                Diag.log("screen: unpublished")
            } catch (e: IOException) {
                if (e is ApiError && e.status == 404) return@execute
                Diag.log("screen: unpublish failed (${e.javaClass.simpleName})")
                if (tries < 3) exec.schedule({
                    if (screenChoice.turn == turn && _state.value.sessionId == sid) {
                        unpublishScreen(a, sid, turn, tries + 1)
                    }
                }, 2000, TimeUnit.MILLISECONDS)
            }
        }
    }

    /** Revoke the choice and stop capture locally even without a live session. */
    private fun endScreen() {
        val hadScreen = screenChoice.wanted || screenServiceTurn != null || screenTrack != null
        val turn = screenChoice.choose(false)
        _state.update { it.copy(screenChosen = false) }
        stopScreen()
        val sid = _state.value.sessionId
        val a = api()
        if (hadScreen && sid != null && a != null) unpublishScreen(a, sid, turn)
    }

    private fun startScreen(projection: Intent, turn: Int): Boolean {
        var cap: ScreenCapturerAndroid? = null
        var helper: SurfaceTextureHelper? = null
        var src: VideoSource? = null
        var track: VideoTrack? = null
        var repeater: ScreenFrameRepeater? = null
        var initialized = false
        try {
            val factory = Engine.factory(context)
            val names = factory.getRtpSenderCapabilities(MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO).codecs.map { it.name }
            val codec = ScreenShare.codec(names) ?: error("No H264 or VP8 encoder")
            Diag.log(if (codec == "H264") "screen: H.264 encoder available" else "screen: H.264 encoder unavailable; using VP8")
            cap = ScreenCapturerAndroid(projection, object : MediaProjection.Callback() {
                override fun onStop() {
                    // This callback is on the texture thread. Cleanup waits for
                    // that thread, so it must be queued on the voice thread.
                    exec.execute {
                        if (!screenChoice.accepts(turn)) return@execute
                        Diag.log("screen: projection stopped by Android")
                        endScreen()
                    }
                }

                override fun onCapturedContentResize(width: Int, height: Int) {
                    exec.execute {
                        if (screenChoice.accepts(turn) && width > 0 && height > 0) resizeScreen(ScreenShare.size(width, height, screenOptions.height))
                    }
                }
            })
            helper = SurfaceTextureHelper.create("screen", Engine.egl.eglBaseContext)
            src = factory.createVideoSource(true)
            repeater = ScreenFrameRepeater(helper, src.capturerObserver)
            cap.initialize(helper, context, repeater)
            initialized = true
            val size = displayScreenSize()
            // ScreenCapturerAndroid ignores its FPS argument. Cap both the
            // source's adaptation and the RTP encoder to hold the chosen FPS.
            src.adaptOutputFormat(size.width, size.height, screenOptions.framesPerSecond)
            cap.startCapture(size.width, size.height, screenOptions.framesPerSecond)
            track = factory.createVideoTrack("screen", src)
            screenCapturer = cap
            screenHelper = helper
            screenRepeater = repeater
            screenSource = src
            screenTrack = track
            screenCodec = codec
            screenSize = size
            screenFrames = -1L
            screenMoving = false
            screenSaidAt = 0L
            _state.update { it.copy(screen = true) }
            if (screenOptions.sound) startScreenAudio(cap.mediaProjection)
            screenMeter = exec.scheduleWithFixedDelay({
                val gen = generation
                pc?.getStats { report ->
                    exec.execute { if (gen == generation) judgeScreen(report, System.currentTimeMillis()) }
                }
            }, 0, 1, TimeUnit.SECONDS)
            if (Build.VERSION.SDK_INT < 34) {
                // Older Android has no captured-content resize callback.
                val listener = object : DisplayManager.DisplayListener {
                    override fun onDisplayAdded(displayId: Int) {}
                    override fun onDisplayRemoved(displayId: Int) {}
                    override fun onDisplayChanged(displayId: Int) {
                        exec.execute { if (screenChoice.accepts(turn)) resizeScreen(displayScreenSize()) }
                    }
                }
                screenDisplayListener = listener
                context.getSystemService(DisplayManager::class.java).registerDisplayListener(listener, Handler(Looper.getMainLooper()))
            }
            Diag.log("screen: capturing ${size.width}x${size.height}, ${screenOptions.framesPerSecond} FPS, one layer, idle refresh 500 ms")
            return true
        } catch (e: RuntimeException) {
            Diag.log("screen: capture failed (${e.javaClass.simpleName})")
            stopScreenAudio()
            runCatching { repeater?.close() }
            if (initialized) runCatching { cap?.stopCapture() }
            runCatching { cap?.dispose() }
            runCatching { track?.dispose() }
            runCatching { src?.dispose() }
            runCatching { helper?.dispose() }
            // Clear ownership if failure occurred after capture was installed.
            screenCapturer = null
            screenHelper = null
            screenRepeater = null
            screenSource = null
            screenTrack = null
            screenCodec = null
            return false
        }
    }

    @Suppress("DEPRECATION")
    private fun displayScreenSize(): ScreenShare.Size {
        val wm = context.getSystemService(WindowManager::class.java)
        if (Build.VERSION.SDK_INT >= 30) {
            val bounds = wm.maximumWindowMetrics.bounds
            return ScreenShare.size(bounds.width(), bounds.height(), screenOptions.height)
        }
        val size = Point()
        wm.defaultDisplay.getRealSize(size)
        return ScreenShare.size(size.x, size.y, screenOptions.height)
    }

    private fun resizeScreen(size: ScreenShare.Size) {
        val cap = screenCapturer ?: return
        if (size == screenSize) return
        try {
            // libwebrtc resizes the existing virtual display on Android 12+;
            // it never consumes a second projection grant on Android 14+.
            cap.changeCaptureFormat(size.width, size.height, screenOptions.framesPerSecond)
            screenSource?.adaptOutputFormat(size.width, size.height, screenOptions.framesPerSecond)
            screenSize = size
            Diag.log("screen: resized to ${size.width}x${size.height}")
        } catch (e: RuntimeException) {
            Diag.log("screen: resize failed (${e.javaClass.simpleName})")
            endScreen()
            notice("A captura da tela parou. Toque para compartilhar de novo.")
        }
    }

    private fun stopScreen() {
        stopScreenAudio()
        screenMeter?.cancel(false)
        screenMeter = null
        screenStartTimeout?.cancel(false)
        screenStartTimeout = null
        screenDisplayListener?.let { runCatching { context.getSystemService(DisplayManager::class.java).unregisterDisplayListener(it) } }
        screenDisplayListener = null
        slots.firstOrNull { it.mid == screenMid }?.let { runCatching { it.sender.setTrack(null, false) } }
        screenMid = null
        screenSession = null
        screenFrames = -1L
        screenMoving = false
        runCatching { screenRepeater?.close() }
        screenRepeater = null
        runCatching { screenCapturer?.stopCapture() }
        runCatching { screenCapturer?.dispose() }
        screenCapturer = null
        runCatching { screenTrack?.dispose() }
        screenTrack = null
        screenCodec = null
        runCatching { screenSource?.dispose() }
        screenSource = null
        runCatching { screenHelper?.dispose() }
        screenHelper = null
        screenSize = null
        _state.update { it.copy(screen = false) }
        screenServiceTurn?.let { CallService.endProjection(it) }
        screenServiceTurn = null
    }

    private fun fitScreen(tr: RtpTransceiver) {
        val track = screenTrack ?: return
        val codecs = Engine.factory(context).getRtpSenderCapabilities(MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO).codecs
        val chosen = screenCodec ?: error("No screen encoder")
        val auxiliary = setOf("rtx", "red", "ulpfec", "flexfec-03")
        // Keep the remaining supported codecs in the answer. Pion remembers
        // the first negotiated video codec set for subsequent video slots.
        val prefs = codecs.filter { it.name.equals(chosen, true) } + codecs.filter {
            !it.name.equals(chosen, true) && (it.name.uppercase() in setOf("H264", "VP8", "VP9") || it.name.lowercase() in auxiliary)
        }
        tr.setCodecPreferences(prefs).throwError()
        check(tr.sender.setTrack(track, false)) { "Screen track rejected" }
        screenMid = tr.mid
        val params = tr.sender.parameters
        // Read before the answer exists, so it may name the previous choice
        // or none: said in the log, never a reason to refuse (that cost the
        // whole screen on the emulator, 01/10/2026). What is really sent is
        // checked by the stats once a second, in judgeScreen.
        val sending = params.codecs.firstOrNull()?.let { "${it.name}, PT=${it.payloadType}" } ?: "none yet"
        ScreenShare.tune(params, screenOptions)
        check(tr.sender.setParameters(params)) { "Screen parameters rejected" }
        Diag.log("screen: slot fitted, sender says $sending, requested=$chosen, ${screenOptions.framesPerSecond} FPS, ${screenOptions.maxBps / 1000} kbit/s ceiling, maintain-framerate")
    }

    /** Every sound arriving at the person's own level: voice and screen apart. */
    private fun applyVolumes() {
        val conn = pc ?: return
        val names = info
        for (tr in slots) {
            val track = tr.receiver.track() as? AudioTrack ?: continue
            val who = names[tr.mid] ?: continue
            track.setVolume(if (com.taleshernandes.harmony.model.PersonalPreferences.silent(who.username, who.source == "screen_audio")) 0.0 else volumes.get(who.username, who.source == "screen_audio").toDouble())
        }
    }

    /**
     * Asks for somebody's screen, or stops asking (PROTOCOL-HARMONY §4.6). A
     * phone watches one screen at a time: asking for a second lets go of the
     * first, which is a whole 1080p stream the phone would otherwise go on
     * receiving behind the one on screen.
     */
    fun watch(username: String?) = exec.execute {
        watchTurn++
        screenDiagnostics.clear()
        watchWanted = username
        _state.value.watching?.let(decodeCap::forget)
        doWatch(username)
    }

    private fun doWatch(username: String?) {
        val s = _state.value
        val sid = s.sessionId ?: return
        val a = api() ?: return
        val before = s.watching
        if (before == username) return
        _state.update { it.copy(watching = username) }
        if (outputDelayJob == null) {
            outputDelayJob = exec.scheduleWithFixedDelay({ updateOutputDelay() }, 0, 1000, TimeUnit.MILLISECONDS)
        }
        val turn = watchTurn
        net.execute {
            // What the server is known to hold: the old screen until letting
            // go of it is heard, and the new one only once asking for it is.
            var held = before
            try {
                if (before != null) {
                    a.voiceWatch(sid, before, false)
                    held = null
                }
                if (username != null) {
                    a.voiceWatch(sid, username, true)
                    held = username
                }
                Diag.log(if (username != null) "vídeo: assistindo a tela de $username" else "vídeo: parou de assistir")
            } catch (e: IOException) {
                Diag.log("vídeo: pedir a tela falhou (${e.javaClass.simpleName})")
                // Only if nothing was chosen since: a later choice has its own
                // requests on the way and says what is true after them.
                exec.execute {
                    if (turn != watchTurn || _state.value.sessionId != sid) return@execute
                    watchWanted = held
                    _state.update { it.copy(watching = held) }
                }
            }
        }
        before?.let {
            layerAsk.forget(it)
            drawn.remove(it)
        }
    }

    /**
     * The screen says how wide it is drawing somebody's screen, in real
     * pixels, and whether anybody can see it. The picture to ask for follows
     * from that (Layers.kt), and is asked for only when it changes.
     */
    override fun drawnAt(username: String, widthPx: Int) {
        drawn[username] = widthPx
        exec.execute { askLayer(username) }
    }

    /** Whether the app can be seen at all — on screen or in the floating window. */
    fun setLooking(on: Boolean) {
        looking = on
        exec.execute {
            drawn.keys.forEach(::askLayer)
            if (on) cameraBack()
            // Out of sight the camera keeps going only under the call service's
            // camera type (Android 14), which the service takes when the camera
            // comes on; without it the camera pauses and comes back with the app.
            val cap = capturer ?: return@execute
            if (!on && camRunning && CallService.cameraInBackground) {
                Diag.log("vídeo: câmera continua com o app fora da tela")
            } else if (!on && camRunning) {
                runCatching { cap.stopCapture() }
                camRunning = false
                Diag.log("vídeo: câmera pausada com o app fora da tela")
            } else if (on && !camRunning) {
                cap.startCapture(CAM_WIDTH, CAM_HEIGHT, CAM_FPS)
                camRunning = true
                Diag.log("vídeo: câmera de volta")
            }
        }
    }

    private fun askLayer(username: String) {
        val sid = _state.value.sessionId ?: return
        if (_state.value.watching != username) return
        val layer = layerAsk.want(username, drawn[username] ?: 0, looking, decodeCap.cap(username)) ?: return
        val a = api() ?: return
        decodeCap.suspendJudgment(username, android.os.SystemClock.elapsedRealtime())
        net.execute {
            try {
                a.voiceLayer(sid, username, layer)
                Diag.log("vídeo: pedindo a camada $layer da tela de $username (${drawn[username]} px)")
            } catch (e: IOException) {
                Diag.log("vídeo: pedir a camada falhou (${e.javaClass.simpleName})")
                exec.execute { layerAsk.failed(username) }
            }
        }
    }

    /** What the offer says is arriving, as pictures. Runs after every answer. */
    private fun collectVideos(conn: PeerConnection) {
        val found = ArrayList<Video>()
        synchronized(videoLock) {
            val live = HashMap<String, VideoTrack>()
            for (tr in slots) {
                val mid = tr.mid ?: continue
                val track = tr.receiver.track() as? VideoTrack ?: continue
                val who = info[mid] ?: continue
                if (who.kind != "video") continue
                live[mid] = track
                found += Video(mid, who.username, who.displayName, who.source)
            }
            // Slots that stopped carrying a picture let go of their renderers.
            for ((mid, old) in videoTracks) {
                if (live[mid] !== old) sinks[mid]?.forEach { runCatching { old.removeSink(it) } }
            }
            for ((mid, t) in live) {
                if (videoTracks[mid] !== t) sinks[mid]?.forEach { runCatching { t.addSink(it) } }
            }
            videoTracks.clear()
            videoTracks.putAll(live)
        }
        if (found != _state.value.videos) {
            _state.update { it.copy(videos = found) }
            Diag.log("vídeo: chegando ${found.joinToString { "${it.source} de ${it.username}" }.ifEmpty { "nada" }}")
        }
    }

    /** Runs on exec independently of stats and foreground metering. */
    private fun updateOutputDelay() {
        val sid = _state.value.sessionId ?: return
        val a = api() ?: return
        val sample = org.webrtc.audio.PlaybackRoute.output()
        val active = _state.value.watching != null
        val policy = outputDelay
        val now = android.os.SystemClock.elapsedRealtime()
        val ms = policy.sample(sample.latencyMs, active, sample.measuredAtNs)
        if (ms != null) net.execute {
            var success = false
            try {
                a.voiceOutputDelay(sid, ms)
                success = true
            } catch (e: IOException) {
                Diag.log("screen output delay: request failed (${e.javaClass.simpleName})")
            } finally {
                val accepted = success
                exec.execute {
                    if (_state.value.sessionId == sid && outputDelay === policy) policy.complete(ms, accepted)
                }
            }
        }
        // Every second while a command is unsent, every ten once it is steady.
        val every = if (policy.commandMs != policy.sentMs) 1_000 else 10_000
        if (active && (outputDelayLogAt == Long.MIN_VALUE || now - outputDelayLogAt >= every)) {
            outputDelayLogAt = now
            val type = if (sample.deviceType == 0) "não encontrado" else sample.deviceType.toString()
            val latency = if (sample.latencyMs < 0) "não encontrado" else sample.latencyMs.toString()
            val video = videoDelayObservation.diagnostic(_state.value.watching?.let { "$generation/$it" }, now)
            Diag.log("screen output delay: routedDeviceType=$type measuredMs=$latency " +
                "exactMs=${policy.exactMs} targetMs=${policy.desiredMs} syncErrorMs=${syncError.fresh(now) ?: "não encontrado"} " +
                "commandMs=${policy.commandMs} extraSentMs=${policy.sentMs} $video")
        }
    }

    /** Once a second: sample screen drops, excluding known transport transitions. */
    private fun judgeDecoders(report: org.webrtc.RTCStatsReport) {
        val names = info
        val measuredAt = android.os.SystemClock.elapsedRealtime()
        val viewed = _state.value.watching
        val video = if (viewed == null) null else report.statsMap.values.firstOrNull {
            val who = names[it.members["mid"] as? String]
            it.type == "inbound-rtp" && who?.username == viewed && who.source == "screen"
        }
        videoDelayObservation.sample(viewed?.let { "$generation/$it" }, video?.members, measuredAt)
        val audio = if (viewed == null) null else report.statsMap.values.firstOrNull {
            val who = names[it.members["mid"] as? String]
            it.type == "inbound-rtp" && who?.username == viewed && who.source == "screen_audio"
        }
        syncError.sample(viewed?.let { "$generation/$it" }, video?.members, audio?.members, measuredAt)
        if (viewed == null) screenDiagnostics.clear()
        if (viewed != null && screenDiagnostics.due(measuredAt)) {
            val samples = listOf("screen_audio", "screen").joinToString(" | ") { source ->
                val st = report.statsMap.values.firstOrNull {
                    val who = names[it.members["mid"] as? String]
                    it.type == "inbound-rtp" && who?.username == viewed && who.source == source
                }
                "$source: " + if (st == null) "não encontrado" else
                    screenDiagnostics.sample("$generation/$viewed/${st.id}", st.members)
            }
            Diag.log("screen stats: outputPreferred=${_state.value.output?.name ?: "não encontrado"} " +
                "phase=${_state.value.phase} iceRestart=${restartAt != 0L} | $samples")
        }
        for (st in report.statsMap.values) {
            if (st.type != "inbound-rtp" || st.members["kind"] != "video") continue
            val who = names[st.members["mid"] as? String] ?: continue
            if (who.source != "screen") continue
            val mid = st.members["mid"] as? String ?: continue
            val frames = (st.members["framesDecoded"] as? Number)?.toLong()
                ?: (st.members["framesReceived"] as? Number)?.toLong()
            if (frames != null) _screenProgress.update { old ->
                old + (mid to (old[mid] ?: ScreenProgress()).sample(frames,
                    (st.members["frameWidth"] as? Number)?.toInt() ?: 0,
                    (st.members["frameHeight"] as? Number)?.toInt() ?: 0,
                    android.os.SystemClock.elapsedRealtime()))
            }
            val before = decodeCap.cap(who.username)
            val cap = decodeCap.judge(
                who.username,
                (st.members["framesDropped"] as? Number)?.toLong(),
                (st.members["framesDecoded"] as? Number)?.toLong() ?: 0,
                (st.members["framesReceived"] as? Number)?.toLong() ?: 0,
                measuredAt,
                eligible = _state.value.phase == Phase.LIVE && restartAt == 0L,
            )
            if (cap != before) {
                Diag.log("screen cap: ${before ?: "none"}->${cap ?: "none"} ${decodeCap.evidence(who.username)}")
                exec.execute { askLayer(who.username) }
            }
        }
    }

    fun setWatching(on: Boolean) = exec.execute {
        watching = on
        if (on && connected) {
            startMeter(generation)
        } else if (!on) {
            meter?.cancel(false)
            meter = null
            speaking.clear()
            _state.update { it.copy(speaking = emptySet()) }
        }
    }

    /** Restart the route on the existing session when the default network changes. */
    fun networkChanged(moved: Boolean) = exec.execute {
        val target = _state.value.channelId ?: return@execute
        if (waitingNetwork) {
            Diag.log("voz: a rede voltou, entrando de novo")
            stopWaiting()
            doJoin(target)
        } else if (moved && pc != null) {
            restartICE("network changed")
        } else if (_state.value.phase == Phase.RECONNECTING && pc != null) {
            restartICE("network available")
        }
    }

    /** VALIDATED alone is useful only when fresh stats cannot prove a healthy route. */
    fun validationChanged() = exec.execute {
        if (_state.value.phase !in setOf(Phase.LIVE, Phase.RECONNECTING)) return@execute
        val conn = pc ?: return@execute
        if (restartAt != 0L) return@execute // The existing monitor owns this recovery.
        val gen = generation
        inbound(conn, gen, 0L) { before ->
            exec.schedule({
                if (gen != generation || pc !== conn || restartAt != 0L) return@schedule
                inbound(conn, gen, 0L) { after ->
                    if (!connectedIce(conn) || !RestartProgress.growing(before, after)) restartICE("network validated")
                }
            }, 350, TimeUnit.MILLISECONDS)
        }
    }

    /**
     * The live socket opened again during a call, and the rooms were fetched
     * again. Whatever the server said while it was closed did not arrive —
     * a renegotiation included — so the list is checked against this call.
     */
    fun reconcile(rooms: Map<String, List<Peer>>) = exec.execute {
        val s = _state.value
        if (s.phase != Phase.LIVE) return@execute
        val now = Stand.signature(rooms[s.channelId].orEmpty(), me)
        val before = roomSig
        roomSig = now
        // Somebody came, went or started showing something while the socket
        // was closed: the server sent this phone an offer that nobody read,
        // and it waits for the answer forever — every later change for this
        // phone queues behind it (sfu.go renegotiate). Joining again is the
        // one way out that needs nothing new from the server.
        if (before != null && before != now && Stand.of(s.sessionId ?: return@execute, me, s.channelId ?: return@execute, rooms, info.values, System.currentTimeMillis()) == Standing.IN_STEP) {
            Diag.log("voz: a sala mudou com o canal ao vivo fechado, entrando de novo")
            restartICE("oferta possivelmente perdida")
            return@execute
        }
        act(rooms, "o canal ao vivo voltou")
    }

    fun setMuted(on: Boolean) {
        if (!on && !microphoneAllowed(context)) return
        _state.update { it.copy(muted = on) }
        if (!on && mic == null) exec.execute { enableMicrophone() }
        applyMic()
        exec.execute { announce() }
    }

    fun setDeafened(on: Boolean) {
        _state.update { it.copy(deafened = on) }
        applyMic()
        exec.execute {
            applyDeafen()
            announce()
        }
    }

    fun dismissNotice() = _state.update { it.copy(notice = null) }

    fun notice(text: String) = _state.update { it.copy(notice = text) }

    /** Everything voice-shaped that arrives on /ws. */
    fun onEvent(ev: GatewayEvent) {
        when (ev) {
            is GatewayEvent.VoiceOfferEvent -> exec.execute {
                val s = _state.value
                if (pc == null || ev.offer.sessionId != s.sessionId) return@execute
                try {
                    apply(ev.offer)
                } catch (e: Exception) {
                    // Unanswered, the server waits on this offer for good.
                    Diag.log("voz renegociação falhou: ${e.javaClass.simpleName} ${e.message}")
                    restartICE("renegociação falhou")
                }
            }
            is GatewayEvent.VoiceRoom -> exec.execute {
                if (ev.channelId == _state.value.channelId) roomSig = Stand.signature(ev.peers, me)
                checkStillInside(ev)
            }
            else -> Unit
        }
    }

    // ------------------------------------------------------------ joining

    private fun doJoin(channelId: String) {
        val a = api() ?: return
        teardown(tellServer = true)
        _state.update {
            it.copy(channelId = channelId, sessionId = null, phase = if (attempts > 0) Phase.RECONNECTING else Phase.JOINING, notice = null)
        }
        route.start()
        applyMic() // the mode starts from the microphone as it is
        val t0 = System.currentTimeMillis()
        val offer = try {
            a.voiceJoin(channelId)
        } catch (e: ApiError) {
            Diag.log("voz: entrar recusado ${e.status} ${e.reason}")
            fail(if (e.reason == "room_full") "A sala está cheia." else "Não foi possível entrar na sala.")
            return
        } catch (e: IOException) {
            Diag.log("voz: entrar falhou (${e.javaClass.simpleName})")
            recover()
            return
        }
        Diag.log("voz: servidor respondeu em ${System.currentTimeMillis() - t0} ms, ${offer.iceServers.size} servidor(es) de rota")
        // From here on the server holds a session, so every way out has to hand it back.
        _state.update { it.copy(sessionId = offer.sessionId, phase = if (attempts > 0) Phase.RECONNECTING else Phase.CONNECTING) }

        val factory = Engine.factory(context)
        val gen = ++generation
        val config = PeerConnection.RTCConfiguration(
            offer.iceServers.map { s ->
                PeerConnection.IceServer.builder(s.urls).apply {
                    s.username?.let(::setUsername)
                    s.credential?.let(::setPassword)
                }.createIceServer()
            },
        ).apply {
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
            bundlePolicy = PeerConnection.BundlePolicy.MAXBUNDLE
            rtcpMuxPolicy = PeerConnection.RtcpMuxPolicy.REQUIRE
            // Keep looking for routes after connecting, so that a new network
            // (Wi-Fi to 4G) can be offered without starting over. A3 leans on it.
            continualGatheringPolicy = PeerConnection.ContinualGatheringPolicy.GATHER_CONTINUALLY
        }
        val conn = factory.createPeerConnection(config, Observer(gen, t0)) ?: run {
            Diag.log("voz: a conexão não pôde ser criada")
            fail("Não foi possível entrar na sala.")
            return
        }
        // A sendrecv slot without a track can still start native recording.
        conn.setAudioRecording(microphoneAllowed(context))
        pc = conn
        roomSig = null
        if (microphoneAllowed(context)) {
            val src = factory.createAudioSource(microphoneConstraints(screenAudio != null))
            source = src
            synchronized(micLock) { mic = factory.createAudioTrack("mic", src) }
        }
        applyMic()

        try {
            apply(offer)
        } catch (e: Exception) {
            Diag.log("voz: responder falhou: ${e.javaClass.simpleName} ${e.message}")
            recover()
            return
        }
        announce()
    }

    /** Answers one offer from the server (PROTOCOL-HARMONY §4.2). */
    private fun apply(offer: VoiceOffer) {
        val conn = pc ?: return
        if (appliedOffer == offer.sdp && conn.signalingState() == PeerConnection.SignalingState.STABLE && conn.localDescription?.type == SessionDescription.Type.ANSWER) {
            Diag.log("voice replay: resend the existing answer")
            postAnswer(offer.sessionId, appliedAnswer)
            return
        }
        // Whose slot is whose, recorded before the description goes in, so that
        // a sound arriving in the same breath already has a name.
        info = offer.tracks.associateBy { it.mid }
        var sdp = loopbackAlias(offer.sdp)
        offer.tracks.filter { it.source == "screen" }.forEach { video ->
            val audio = offer.tracks.firstOrNull { it.username == video.username && it.source == "screen_audio" }
            Diag.log("screen sync: firstMsid=${screenSyncGroup(sdp, video.mid, audio?.mid)} (remote offer)")
        }
        val outgoingScreen = offer.sendMids["screen"]
        if (screenTrack != null && outgoingScreen != null) {
            try {
                sdp = ScreenShare.preferCodec(sdp, outgoingScreen, screenCodec ?: error("No screen encoder"))
            } catch (e: RuntimeException) {
                // A missing/incompatible screen codec must not cost the room.
                // Answer the original offer with no local screen track instead.
                Diag.log("screen: offer codec refused (${e.javaClass.simpleName})")
                endScreen()
                notice("Não deu para enviar a tela para a sala.")
            }
        }
        await("remota") { conn.setRemoteDescription(it, SessionDescription(SessionDescription.Type.OFFER, sdp)) }
        synchronized(videoLock) {
            // Reading the slots disposes of the tracks read last time, so the
            // renderers come off them first, and go onto the new ones at once
            // (collectVideos) rather than after the answer.
            for ((mid, old) in videoTracks) sinks[mid]?.forEach { runCatching { old.removeSink(it) } }
            videoTracks.clear()
            slots = conn.transceivers
        }
        collectVideos(conn)

        for ((source, mid) in offer.sendMids) {
            val tr = slots.firstOrNull { it.mid == mid } ?: continue
            when (source) {
                "mic" -> synchronized(micLock) { micMid = mid; tr.sender.setTrack(mic, false) }
                "cam" -> fitCamera(tr)
                // A refused codec or parameter costs the screen, never the
                // room: thrown here, it would end the whole negotiation.
                "screen" -> try { fitScreen(tr) } catch (e: RuntimeException) {
                    Diag.log("screen: slot refused (${e.javaClass.simpleName}: ${e.message})")
                    runCatching { tr.sender.setTrack(null, false) }
                    exec.execute { endScreen(); notice("Não deu para enviar a tela para a sala.") }
                    continue
                }
                else -> continue
            }
            // A slot made while reading somebody else's offer starts recvonly,
            // and a recvonly slot with a track in it sends nothing, silently.
            // sendrecv, because the server puts other people's sound here too.
            if (tr.direction == RtpTransceiver.RtpTransceiverDirection.RECV_ONLY ||
                tr.direction == RtpTransceiver.RtpTransceiverDirection.INACTIVE
            ) {
                tr.direction = RtpTransceiver.RtpTransceiverDirection.SEND_RECV
            }
        }

        val answer = awaitCreate { conn.createAnswer(it, MediaConstraints()) }
        await("local") { conn.setLocalDescription(it, answer) }
        appliedOffer = offer.sdp
        appliedAnswer = answer.description
        postAnswer(offer.sessionId, answer.description)
        applyDeafen()
        applyVolumes()
        // A renegotiation may have made the screen's slot anew: the picture
        // is asked for again (media.ts clears what it asked on every offer).
        _state.value.watching?.let {
            layerAsk.forget(it)
            askLayer(it)
        }
        val described = slots.joinToString(" ") { tr ->
            "${tr.mid}:${tr.currentDirection?.name?.lowercase() ?: "-"}${if (tr.sender.track() != null) "+faixa" else ""}${info[tr.mid]?.let { "<${it.source}" } ?: ""}"
        }
        Diag.log("voz negociada: ${offer.tracks.size} fluxo(s) chegando; vagas $described")
    }

    // ------------------------------------------------------------ staying

    private var appliedOffer = ""
    private var appliedAnswer = ""

    private fun postAnswer(sid: String, sdp: String) {
        val a = api() ?: return
        val gen = generation
        net.execute {
            if (gen != generation) return@execute
            try {
                a.voiceAnswer(sid, sdp)
            } catch (e: IOException) {
                Diag.log("voice answer delivery failed: ${e.javaClass.simpleName}")
                exec.execute { if (gen == generation) restartICE("answer not delivered") }
            }
        }
    }

    @Volatile private var restartAt = 0L
    private var restartApplied = false
    private var restartCompletedAt = 0L
    private var restartTimeout: ScheduledFuture<*>? = null
    private var restartPoll: ScheduledFuture<*>? = null
    private var restartSample: Map<String, RestartProgress.Counters>? = null
    private var restartSampleVersion = 0

    private fun finishRestart() {
        if (restartAt == 0L) return
        val now = android.os.SystemClock.elapsedRealtime()
        Diag.log("voice ICE restart connected in ${now - restartAt} ms; same session; inbound RTP growing; applied=$restartApplied")
        restartCompletedAt = now
        _state.value.watching?.let { decodeCap.suspendJudgment(it, now) }
        restartAt = 0L
        restartApplied = false
        restartTimeout?.cancel(false)
        restartTimeout = null
        restartPoll?.cancel(false)
        restartPoll = null
        restartSample = null
        _state.update { it.copy(phase = Phase.LIVE, notice = null) }
    }

    private fun inbound(conn: PeerConnection, gen: Int, at: Long,
                        next: (Map<String, RestartProgress.Counters>) -> Unit) {
        try {
            conn.getStats { report ->
                val counters = report.statsMap.filterValues { it.type == "inbound-rtp" }.mapValues { (_, stat) ->
                    RestartProgress.Counters((stat.members["bytesReceived"] as? Number)?.toLong() ?: 0L,
                        (stat.members["packetsReceived"] as? Number)?.toLong() ?: 0L)
                }
                exec.execute { if (gen == generation && pc === conn && restartAt == at) next(counters) }
            }
        } catch (e: RuntimeException) {
            Diag.log("voice restart stats unavailable: ${e.javaClass.simpleName}")
            if (gen == generation && pc === conn && restartAt == at) next(emptyMap())
        }
    }

    private fun connectedIce(conn: PeerConnection): Boolean = conn.iceConnectionState().let {
        it == PeerConnection.IceConnectionState.CONNECTED || it == PeerConnection.IceConnectionState.COMPLETED
    }

    private fun pollRestart(conn: PeerConnection, gen: Int, at: Long) {
        if (gen != generation || pc !== conn || restartAt != at) return
        val version = restartSampleVersion
        inbound(conn, gen, at) { counters ->
            if (version == restartSampleVersion) {
                val growing = RestartProgress.growing(restartSample, counters)
                restartSample = counters
                if (RestartProgress.decide(connectedIce(conn), growing, false) == RestartProgress.Decision.COMPLETE) {
                    finishRestart()
                    return@inbound
                }
            }
            restartPoll = exec.schedule({ pollRestart(conn, gen, at) }, 350, TimeUnit.MILLISECONDS)
        }
    }

    private fun restartDeadline(conn: PeerConnection, gen: Int, at: Long) {
        // Two fresh samples at the deadline; earlier media may have stopped.
        inbound(conn, gen, at) { before ->
            exec.schedule({
                if (gen != generation || pc !== conn || restartAt != at) return@schedule
                inbound(conn, gen, at) { after ->
                    if (RestartProgress.decide(connectedIce(conn), RestartProgress.growing(before, after), true)
                        == RestartProgress.Decision.COMPLETE) {
                        finishRestart()
                    } else {
                        restartAt = 0L
                        restartPoll?.cancel(false)
                        restartPoll = null
                        Diag.log("voice ICE restart timeout; fallback rejoin; inbound RTP stopped")
                        if (connectivity?.activeNetwork != null) rejoin("ICE restart timeout")
                    }
                }
            }, 350, TimeUnit.MILLISECONDS)
        }
    }

    private fun restartICE(why: String) {
        val conn = pc ?: return
        val sid = _state.value.sessionId ?: return
        val ice = conn.iceConnectionState()
        val broken = ice == PeerConnection.IceConnectionState.DISCONNECTED || ice == PeerConnection.IceConnectionState.FAILED
        val now = android.os.SystemClock.elapsedRealtime()
        if (why.startsWith("network") && RestartProgress.suppressNetworkTrigger(now, restartCompletedAt, broken)) return
        if (broken || connectivity?.activeNetwork == null) _state.update { it.copy(phase = Phase.RECONNECTING) }
        if (restartAt != 0L) return
        Diag.log("voice ICE restart requested: $why")
        if (connectivity?.activeNetwork == null) {
            Diag.log("voice restart waiting for network")
            return
        }
        val gen = generation
        restartAt = now
        val at = restartAt
        restartApplied = false
        restartSample = null
        restartSampleVersion++
        pollRestart(conn, gen, at)
        restartTimeout = exec.schedule({
            if (gen == generation && restartAt == at) restartDeadline(conn, gen, at)
        }, 8, TimeUnit.SECONDS)
        requestRestart(gen, at, sid, conn)
    }

    private fun requestRestart(gen: Int, at: Long, sid: String, conn: PeerConnection) {
        if (gen != generation || restartAt != at) return
        if (connectivity?.activeNetwork == null) {
            exec.schedule({ requestRestart(gen, at, sid, conn) }, 300, TimeUnit.MILLISECONDS)
            return
        }
        val a = api() ?: return
        net.execute {
            if (gen != generation || restartAt != at) return@execute
            var sessionMissing = false
            val offer = try {
                a.voiceRestart(sid)
            } catch (e: ApiError) {
                sessionMissing = e.status == 404 && e.reason == "not_found"
                if (!sessionMissing) Diag.log("voice ICE restart retry: ${e.javaClass.simpleName}")
                null
            } catch (e: Exception) {
                Diag.log("voice ICE restart retry: ${e.javaClass.simpleName}")
                null
            }
            exec.execute {
                if (gen != generation || pc !== conn) return@execute
                if (restartAt != at) {
                    // Spontaneous recovery cancelled the request, but a late
                    // offer still needs an answer on the retained session.
                    if (offer != null) runCatching { apply(offer) }.onFailure {
                        Diag.log("voice late restart answer failed: ${it.javaClass.simpleName}")
                    }
                    return@execute
                }
                if (sessionMissing) {
                    restartTimeout?.cancel(false)
                    restartTimeout = null
                    restartPoll?.cancel(false)
                    restartPoll = null
                    restartAt = 0L
                    restartApplied = false
                    Diag.log("voice ICE restart session missing; fallback rejoin")
                    rejoin("ICE restart session missing")
                    return@execute
                }
                if (offer != null) {
                    try {
                        apply(offer)
                        restartApplied = offer.iceRestart
                        if (restartApplied) {
                            // The next two stats samples must both follow application.
                            restartSample = null
                            restartSampleVersion++
                            return@execute
                        }
                        Diag.log("voice replay answered; requesting fresh ICE credentials")
                    } catch (e: Exception) {
                        Diag.log("voice restart apply retry: ${e.javaClass.simpleName}")
                    }
                }
                exec.schedule({ requestRestart(gen, at, sid, conn) }, 300, TimeUnit.MILLISECONDS)
            }
        }
    }

    /**
     * The server says who is inside, and this session is not on the list. That
     * is either a fall (the server let go of a silent connection) or the same
     * account joining from another device; the full list, a moment later,
     * tells which ([Standing]).
     */
    private fun checkStillInside(ev: GatewayEvent.VoiceRoom) {
        val s = _state.value
        if (s.phase != Phase.LIVE || ev.channelId != s.channelId) return
        if (ev.peers.any { it.sessionId == s.sessionId }) return
        // The server says the leaving first and the arriving elsewhere after
        // (sfu.Join: LeaveAllFor, then the new session), so the look is taken
        // a second later, when the other device is on the list.
        verify("o servidor tirou esta sessão da sala", after = 1000)
    }

    /** Fetches who is where, and acts on it. A fetch that fails counts as a fall. */
    private fun verify(why: String, after: Long = 0) {
        if (verifying) return
        verifying = true
        val gen = generation
        exec.schedule({
            val a = api()
            if (a == null || gen != generation) {
                verifying = false
                return@schedule
            }
            net.execute {
                val rooms = try {
                    a.voiceRooms()
                } catch (e: Exception) {
                    Diag.log("voz: conferir a sala falhou (${e.javaClass.simpleName})")
                    null
                }
                exec.execute {
                    verifying = false
                    if (gen != generation) return@execute
                    if (rooms == null) rejoin("$why; sem resposta do servidor") else act(rooms, why)
                }
            }
        }, after, TimeUnit.MILLISECONDS)
    }

    private fun act(rooms: Map<String, List<Peer>>, why: String) {
        val s = _state.value
        val sid = s.sessionId ?: return
        val target = s.channelId ?: return
        when (Stand.of(sid, me, target, rooms, info.values, System.currentTimeMillis())) {
            Standing.IN_STEP -> Diag.log("voz em dia ($why)")
            Standing.BEHIND -> {
                // A renegotiation was lost with the socket; joining again is
                // the one way to get a complete offer.
                Diag.log("voz: falta o som de alguém ($why), entrando de novo")
                rejoin(why)
            }
            Standing.DROPPED -> {
                Diag.log("voz: o servidor soltou esta sessão ($why)")
                rejoin(why)
            }
            Standing.ELSEWHERE -> {
                // Not a fall: joining again would take the room back from the
                // other device, which would take it back in turn.
                Diag.log("voz: a mesma conta entrou em outro aparelho")
                stopWaiting()
                endScreen()
                teardown(tellServer = false)
                _state.update {
                    it.copy(
                        channelId = null, sessionId = null, phase = Phase.OFF,
                        notice = "Você não está mais na sala por este celular. Se entrou pelo PC, é lá que está agora.",
                    )
                }
            }
        }
    }

    /** Joins the same room again at once, counted against the three tries. */
    private fun rejoin(why: String) {
        val target = _state.value.channelId ?: return
        if (attempts >= 3) {
            Diag.log("voz caiu e não voltou depois de três tentativas ($why)")
            fail("A chamada caiu. Toque na sala para entrar de novo.")
            return
        }
        attempts++
        doJoin(target)
    }

    /**
     * Joining failed on the way to the server: try again, one, two, then four
     * seconds apart, and then stop and say so — a room that keeps trying
     * forever is a microphone somebody has forgotten about (media.ts recover).
     * With no network at all the tries are not spent: the call waits for one,
     * for a minute (a tunnel, a lift), and joins the moment it arrives.
     */
    private fun recover() {
        val target = _state.value.channelId ?: return
        teardown(tellServer = true)
        _state.update { it.copy(phase = Phase.RECONNECTING) }
        if (connectivity?.activeNetwork == null) {
            if (!waitingNetwork) {
                waitingNetwork = true
                Diag.log("voz: sem rede, esperando uma voltar")
                giveUp = exec.schedule({
                    if (!waitingNetwork) return@schedule
                    Diag.log("voz: um minuto sem rede")
                    stopWaiting()
                    fail("A chamada caiu sem internet. Toque na sala para entrar de novo.")
                }, NO_NETWORK_MS, TimeUnit.MILLISECONDS)
            }
            return
        }
        if (attempts >= 3) {
            Diag.log("voz caiu e não voltou depois de três tentativas")
            fail("A chamada caiu. Toque na sala para entrar de novo.")
            return
        }
        val wait = 1000L shl attempts
        attempts++
        Diag.log("voz caiu, voltando em $wait ms (tentativa $attempts)")
        retry = exec.schedule({
            if (_state.value.channelId == target) doJoin(target)
        }, wait, TimeUnit.MILLISECONDS)
    }

    private fun stopWaiting() {
        retry?.cancel(false)
        retry = null
        giveUp?.cancel(false)
        giveUp = null
        waitingNetwork = false
    }

    private fun fail(notice: String) {
        endScreen()
        teardown(tellServer = true)
        _state.update { it.copy(channelId = null, sessionId = null, phase = Phase.OFF, notice = notice) }
    }

    /** Ends the connection. [tellServer] is false when the server already ended it. */
    private fun teardown(tellServer: Boolean) {
        appliedOffer = ""
        appliedAnswer = ""
        restartTimeout?.cancel(false)
        restartTimeout = null
        restartPoll?.cancel(false)
        restartPoll = null
        restartSample = null
        restartCompletedAt = 0L
        restartAt = 0L
        restartApplied = false
        meter?.cancel(false)
        meter = null
        stall?.cancel(false)
        stall = null
        connected = false
        verifying = false
        generation++
        val sid = _state.value.sessionId
        // Handed back once: the next teardown (a retry starting over) must not
        // try to hand back the same session again.
        _state.update { it.copy(sessionId = null) }
        val a = api()
        if (tellServer && sid != null && a != null) {
            net.execute {
                try {
                    a.voiceLeave(sid)
                } catch (e: ApiError) {
                    // After a drop the server has usually ended it already.
                    if (e.status != 404) Diag.log("voz: avisar a saída falhou (${e.status} ${e.reason})")
                } catch (e: IOException) {
                    Diag.log("voz: avisar a saída falhou (${e.javaClass.simpleName})")
                }
            }
        }
        // A new connection needs a new publication, but the projection itself
        // stays alive: Android 14 forbids starting it again with the old grant.
        slots.firstOrNull { it.mid == screenMid }?.let { runCatching { it.sender.setTrack(null, false) } }
        screenMid = null
        screenSession = null
        screenFrames = -1L
        screenMoving = false
        synchronized(videoLock) {
            for ((mid, t) in videoTracks) sinks[mid]?.forEach { runCatching { t.removeSink(it) } }
            videoTracks.clear()
            slots = emptyList()
        }
        _state.update { it.copy(videos = emptyList(), watching = null) }
        outputDelayJob?.cancel(false)
        outputDelayJob = null
        outputDelay = OutputDelay()
        videoDelayObservation = VideoDelayObservation()
        syncError = SyncError()
        outputDelayLogAt = Long.MIN_VALUE
        drawn.clear()
        if (camTrack != null || capturer != null) stopCamera()
        pc?.dispose()
        pc = null
        screenDiagnostics.clear()
        synchronized(micLock) {
            mic?.dispose()
            mic = null
        }
        source?.dispose()
        source = null
        info = emptyMap()
        micMid = null
        _screenProgress.value = emptyMap()
        speaking.clear()
        _state.update { it.copy(speaking = emptySet()) }
        route.stop()
    }

    // ------------------------------------------------------------ mute

    private fun enableMicrophone() {
        if (pc == null) return
        if (!microphoneAllowed(context) || mic != null) return
        val factory = Engine.factory(context)
        val src = factory.createAudioSource(microphoneConstraints(screenAudio != null))
        source = src
        synchronized(micLock) { mic = factory.createAudioTrack("mic", src) }
        applyMic()
        // The send slot was already negotiated, including in listen-only mode.
        slots.firstOrNull { it.mid == micMid }?.sender?.setTrack(mic, false)
        pc?.setAudioRecording(true)
    }

    private var micMid: String? = null

    private fun applyMic() {
        synchronized(micLock) {
            val s = _state.value
            val audio = screenAudio
            audio?.mix?.microphoneMuted = s.muted || s.deafened
            // A disabled track would silence the game too. Zero voice in the
            // post-mute hook while keeping the shared RTP track enabled.
            mic?.setEnabled(ScreenAudioPolicy.sendTrack(s.muted, s.deafened, audio?.mix?.enabled == true))
            route.setMicOpen(microphoneIsOpen(mic != null, s.muted, s.deafened))
        }
    }

    /**
     * Media and call mode differ in the recorder's source and the player's
     * usage, both fixed while they run: stop and start them (AudioRoute).
     * WebRTC starts each again only if a stream needs it.
     */
    private fun restartAudio() {
        val c = pc ?: return
        c.setAudioPlayout(false)
        c.setAudioPlayout(true)
        if (microphoneAllowed(context)) {
            c.setAudioRecording(false)
            c.setAudioRecording(true)
        }
        Diag.log("voz: áudio reiniciado para a troca de modo")
    }

    private fun microphoneConstraints(playback: Boolean) = MediaConstraints().apply {
        listOf("googEchoCancellation", "googNoiseSuppression", "googAutoGainControl", "googHighpassFilter")
            .forEach { key -> mandatory.add(MediaConstraints.KeyValuePair(key,
                if (key == "googNoiseSuppression" || (key == "googAutoGainControl" && playback)) "false" else "true")) }
    }

    /** Replace only the mic track, so source options reach the existing sender. */
    private fun changeMicrophoneGain(playback: Boolean): Boolean {
        if (pc == null || mic == null) return true // The next join uses the current choice.
        var nextSource: AudioSource? = null
        var nextTrack: AudioTrack? = null
        try {
            val factory = Engine.factory(context)
            nextSource = factory.createAudioSource(microphoneConstraints(playback))
            nextTrack = factory.createAudioTrack("mic", nextSource)
            synchronized(micLock) {
                val s = _state.value
                nextTrack.setEnabled(ScreenAudioPolicy.sendTrack(s.muted, s.deafened, playback))
                val sender = slots.firstOrNull { it.mid == micMid }?.sender
                if (sender != null) check(sender.setTrack(nextTrack, false)) { "Microphone replacement rejected" }
                val old = mic
                val oldSource = source
                mic = nextTrack
                source = nextSource
                runCatching { old?.dispose() }
                runCatching { oldSource?.dispose() }
            }
            Diag.log("screen audio: googAutoGainControl=${!playback}; microphone track replaced on existing connection")
            return true
        } catch (e: RuntimeException) {
            runCatching { nextTrack?.dispose() }
            runCatching { nextSource?.dispose() }
            Diag.log("screen audio: gain change failed (${e.javaClass.simpleName}); existing microphone continues")
            return false
        }
    }

    private fun startScreenAudio(projection: MediaProjection?) {
        if (projection == null || !microphoneAllowed(context)) {
            Diag.log("screen audio: projection or RECORD_AUDIO unavailable; screen and microphone continue")
            return
        }
        val audio = ScreenAudio { failed -> exec.execute {
            if (screenAudio !== failed) return@execute
            stopScreenAudio()
            notice("O som do celular parou; a tela e sua voz continuam.")
        } }
        if (!audio.start(projection)) {
            notice("Não deu para compartilhar o som do celular; a tela continua.")
            return
        }
        synchronized(micLock) {
            val s = _state.value
            audio.mix.microphoneMuted = s.muted || s.deafened
            screenAudio = audio
            Engine.playbackMix = audio.mix
        }
        if (!changeMicrophoneGain(true)) {
            stopScreenAudio()
            notice("Não deu para compartilhar o som do celular; a tela e sua voz continuam.")
            return
        }
        applyMic()
    }

    private fun stopScreenAudio() {
        val audio = synchronized(micLock) {
            val old = screenAudio ?: return
            old.mix.enabled = false
            // Disable a muted sender BEFORE removing the hook that zeros voice.
            val s = _state.value
            mic?.setEnabled(!s.muted && !s.deafened)
            if (Engine.playbackMix === old.mix) Engine.playbackMix = null
            screenAudio = null
            old
        }
        audio.close()
        changeMicrophoneGain(false)
        applyMic()
    }

    private fun applyDeafen() {
        val deaf = _state.value.deafened
        slots.forEach { tr ->
            (tr.receiver.track() as? AudioTrack)?.setEnabled(!deaf)
        }
    }

    /**
     * Tells the room (PROTOCOL-HARMONY §4.5). The silencing itself is
     * [applyMic]. A notice that does not arrive is sent again, with whatever
     * the state is by then, or the room would show the old one all call long.
     */
    private fun announce(tries: Int = 0) {
        val s = _state.value
        val sid = s.sessionId ?: return
        val a = api() ?: return
        net.execute {
            try {
                a.voiceState(sid, s.muted, s.deafened)
            } catch (e: IOException) {
                Diag.log("voz: avisar mudo falhou (${e.javaClass.simpleName})")
                if (e is ApiError && e.status == 404) return@execute
                if (tries < 3) {
                    exec.schedule({ if (_state.value.sessionId == sid) announce(tries + 1) }, 2000, TimeUnit.MILLISECONDS)
                }
            }
        }
    }

    // ------------------------------------------------------------ who is talking

    private fun startMeter(gen: Int) {
        meter?.cancel(false)
        meter = exec.scheduleWithFixedDelay({ readLevels(gen) }, 0, METER_EVERY_MS, TimeUnit.MILLISECONDS)
    }

    private fun readLevels(gen: Int) {
        val conn = pc ?: return
        if (gen != generation) return
        val names = info
        val s = _state.value
        conn.getStats { report ->
            // Answered on WebRTC's thread, possibly after the call ended.
            if (gen != generation) return@getStats
            val now = System.currentTimeMillis()
            for (st in report.statsMap.values) {
                if (st.members["kind"] != "audio") continue
                val level = (st.members["audioLevel"] as? Number)?.toDouble() ?: continue
                when (st.type) {
                    "inbound-rtp" -> names[st.members["mid"] as? String]?.takeIf { it.source == "mic" }
                        ?.let { speaking.feed(it.username, level, now) }
                    // Muted is not quiet: the microphone still hears, it just
                    // does not send. Lighting up then would be a lie.
                    "media-source" -> {
                        EchoProbe.sample(st.members)
                        if (!s.muted && !s.deafened) speaking.feed(me, level, now)
                    }
                }
            }
            val talking = speaking.talking(now)
            if (talking != _state.value.speaking) _state.update { it.copy(speaking = talking) }
            if (++tick % (1000 / METER_EVERY_MS).toInt() == 0) {
                exec.execute {
                    if (gen == generation) {
                        judgeDecoders(report)
                        judgeCamera(report, now)
                    }
                }
            }
        }
    }

    private fun judgeScreen(report: org.webrtc.RTCStatsReport, now: Long) {
        if (!_state.value.screen) return
        val out = report.statsMap.values.firstOrNull {
            it.type == "outbound-rtp" && it.members["kind"] == "video" && it.members["mid"] == screenMid
        } ?: return
        val encodedCodec = report.statsMap[out.members["codecId"] as? String]?.members?.get("mimeType") as? String
        val requested = screenCodec ?: return
        if (encodedCodec != null && !encodedCodec.equals("video/$requested", true)) {
            Diag.log("screen: unexpected encoder codec $encodedCodec (requested $requested); stopping screen")
            endScreen()
            notice("A tela parou porque o aparelho não usou o codec solicitado.")
            return
        }
        val frames = (out.members["framesEncoded"] as? Number)?.toLong() ?: return
        if (screenFrames < 0) {
            screenFrames = frames
            screenFramesAt = now
            return
        }
        val elapsed = (now - screenFramesAt) / 1000.0
        if (elapsed < 0.9) return
        val fps = (frames - screenFrames) / elapsed
        screenFrames = frames
        screenFramesAt = now
        val moving = fps >= 1
        if (moving != screenMoving || now - screenSaidAt >= 30_000) {
            screenMoving = moving
            screenSaidAt = now
            val codec = encodedCodec ?: "unknown"
            Diag.log("screen: ${if (moving) "sending" else "not sending"}, $codec, " +
                "${out.members["frameWidth"] ?: "?"}x${out.members["frameHeight"] ?: "?"}, ${"%.0f".format(fps)} FPS, " +
                "encoder=${out.members["encoderImplementation"] ?: "unknown"}, limitation=${out.members["qualityLimitationReason"] ?: "unknown"}")
        }
    }

    // ------------------------------------------------------------ plumbing

    private inner class Observer(private val gen: Int, private val startedAt: Long) : PeerConnection.Observer {
        private val found = HashMap<String, Int>()

        override fun onIceCandidate(c: IceCandidate) {
            // Types and counts only in the log, never addresses (media.ts).
            val type = Regex(" typ (\\w+)").find(c.sdp)?.groupValues?.get(1) ?: "?"
            synchronized(found) { found[type] = (found[type] ?: 0) + 1 }
            val sid = _state.value.sessionId ?: return
            val a = api() ?: return
            net.execute {
                if (gen != generation) return@execute
                try {
                    a.voiceCandidate(sid, c.sdp, c.sdpMid, c.sdpMLineIndex)
                } catch (e: IOException) {
                    Diag.log("voz: candidato não enviado (${e.javaClass.simpleName})")
                }
            }
        }

        override fun onIceGatheringChange(s: PeerConnection.IceGatheringState) {
            if (s == PeerConnection.IceGatheringState.COMPLETE) {
                val kinds = synchronized(found) { found.entries.joinToString(" ") { "${it.key}=${it.value}" } }
                Diag.log("voz caminhos: ${kinds.ifEmpty { "NENHUM" }}")
            }
        }

        override fun onConnectionChange(s: PeerConnection.PeerConnectionState) {
            exec.execute {
                if (gen != generation) return@execute
                Diag.log("voz ${s.name.lowercase()}")
                when (s) {
                    PeerConnection.PeerConnectionState.CONNECTED -> {
                        stall?.cancel(false)
                        stall = null
                        if (_state.value.phase != Phase.LIVE) {
                            Diag.log("voz conectada em ${System.currentTimeMillis() - startedAt} ms")
                        } else if (!connected) {
                            Diag.log("voz reconectada sem sair da sala")
                        }
                        connected = true
                        attempts = 0
                        watchWanted?.let { w -> if (_state.value.watching == null) doWatch(w) }
                        _state.update { it.copy(phase = Phase.LIVE) }
                        cameraBack()
                        publishScreen()
                        if (watching) startMeter(gen)
                    }
                    // Repair immediately; the SFU retains the session for a bounded grace.
                    PeerConnection.PeerConnectionState.DISCONNECTED -> {
                        connected = false
                        restartICE("connection disconnected")
                    }
                    PeerConnection.PeerConnectionState.FAILED -> {
                        connected = false
                        restartICE("connection failed")
                    }
                    else -> Unit
                }
            }
        }

        override fun onAddTrack(receiver: RtpReceiver, streams: Array<out MediaStream>?) {
            // Played by the audio module on its own; all that is left is
            // "sem som" for a sound that arrives after it was switched on.
            (receiver.track() as? AudioTrack)?.setEnabled(!_state.value.deafened)
        }

        override fun onSignalingChange(s: PeerConnection.SignalingState) {}
        override fun onIceConnectionChange(s: PeerConnection.IceConnectionState) {
            exec.execute {
                if (gen != generation) return@execute
                Diag.log("voice ICE ${s.name.lowercase()}")
                when (s) {
                    PeerConnection.IceConnectionState.DISCONNECTED, PeerConnection.IceConnectionState.FAILED -> {
                        restartICE("ICE ${s.name.lowercase()}")
                    }
                    else -> Unit
                }
            }
        }
        override fun onIceConnectionReceivingChange(b: Boolean) {}
        override fun onIceCandidatesRemoved(c: Array<out IceCandidate>) {}
        override fun onAddStream(s: MediaStream) {}
        override fun onRemoveStream(s: MediaStream) {}
        override fun onDataChannel(d: DataChannel) {}
        override fun onRenegotiationNeeded() {}
    }

    /**
     * Test builds only. The local test server advertises 127.0.0.1 (it binds
     * the loopback so Windows never asks about its firewall — sfu.onThisMachine),
     * and inside the emulator 127.0.0.1 is the emulator itself; the PC's
     * loopback is 10.0.2.2. Release carries an empty alias and changes nothing,
     * and no real server advertises a loopback address anyway.
     */
    private fun loopbackAlias(sdp: String): String {
        val alias = BuildConfig.LOOPBACK_ALIAS
        if (alias.isEmpty() || " 127.0.0.1 " !in sdp) return sdp
        Diag.log("voz: teste local, 127.0.0.1 do servidor vira $alias")
        return sdp.replace(" 127.0.0.1 ", " $alias ")
    }

    /** Runs one WebRTC "set description" and waits for it, on the voice thread. */
    private fun await(what: String, call: (SdpObserver) -> Unit) {
        val done = CountDownLatch(1)
        var error: String? = null
        call(object : SdpObserver {
            override fun onSetSuccess() = done.countDown()
            override fun onSetFailure(e: String?) {
                error = e
                done.countDown()
            }
            override fun onCreateSuccess(d: SessionDescription?) {}
            override fun onCreateFailure(e: String?) {}
        })
        if (!done.await(10, TimeUnit.SECONDS)) throw IOException("descrição $what: sem resposta em 10 s")
        error?.let { throw IOException("descrição $what: $it") }
    }

    private fun awaitCreate(call: (SdpObserver) -> Unit): SessionDescription {
        val done = CountDownLatch(1)
        var out: SessionDescription? = null
        var error: String? = null
        call(object : SdpObserver {
            override fun onCreateSuccess(d: SessionDescription) {
                out = d
                done.countDown()
            }
            override fun onCreateFailure(e: String?) {
                error = e
                done.countDown()
            }
            override fun onSetSuccess() {}
            override fun onSetFailure(e: String?) {}
        })
        if (!done.await(10, TimeUnit.SECONDS)) throw IOException("resposta: sem resposta em 10 s")
        return out ?: throw IOException("resposta: $error")
    }

    private companion object {
        // getStats is a round trip into the engine; ten a second is plenty for
        // a ring, and the hold covers the gaps between readings.
        const val METER_EVERY_MS = 25L
        // Under the server's five, so that the look and the new join both fit.
        const val STALL_MS = 2500L
        const val NO_NETWORK_MS = 60_000L
        const val LOCAL_CAMERA = "local-camera"
        const val CAM_WIDTH = 1280
        const val CAM_HEIGHT = 720
        const val CAM_FPS = 30
        /** How long a published camera waits for the server's offer with its slot before the room is rejoined. */
        const val CAM_OFFER_MS = 8_000L
        // The web's ceiling for a camera (media.ts, 900 kbit/s).
        const val CAM_MAX_BPS = 900_000
    }
}
