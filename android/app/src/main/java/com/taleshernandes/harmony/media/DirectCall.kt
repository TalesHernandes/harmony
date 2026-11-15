package com.taleshernandes.harmony.media

import android.content.Intent
import android.media.projection.MediaProjection
import org.webrtc.ScreenCapturerAndroid
import org.webrtc.SurfaceTextureHelper
import org.webrtc.VideoSource
import android.content.Context
import com.taleshernandes.harmony.BuildConfig
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.model.CallAccepted
import com.taleshernandes.harmony.model.CallEnded
import com.taleshernandes.harmony.model.CallIncoming
import com.taleshernandes.harmony.model.CallPerson
import com.taleshernandes.harmony.model.CallSignal
import com.taleshernandes.harmony.model.CallSignalEvent
import com.taleshernandes.harmony.model.CallStateEvent
import com.taleshernandes.harmony.model.CallTaken
import com.taleshernandes.harmony.model.Candidate
import com.taleshernandes.harmony.model.GatewayEvent
import com.taleshernandes.harmony.model.IceServer
import com.taleshernandes.harmony.model.Sdp
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.ApiError
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import org.webrtc.AudioSource
import org.webrtc.AudioTrack
import org.webrtc.DataChannel
import org.webrtc.IceCandidate
import org.webrtc.MediaConstraints
import org.webrtc.MediaStream
import org.webrtc.MediaStreamTrack
import org.webrtc.PeerConnection
import org.webrtc.RtpParameters
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
 * The direct call, from this phone's side: one person ringing another, the
 * voice and the pictures going **phone to computer** with no server in the
 * middle (docs/PROTOCOL-HARMONY §7, and the web's direct.ts, which this
 * follows step by step).
 *
 * What the server does is the introduction — who rings whom, and the handful
 * of messages the two ends need to find each other, relayed as they are. It
 * never sees a byte of the call.
 *
 * The two rules the web learned, kept here:
 *
 *  - **One connection per call, never rebuilt.** A camera switched on or off
 *    is a track put into a slot that already exists.
 *  - **Only the side that calls creates slots**, all four at once and in a
 *    fixed order — microphone, camera, screen, the screen's sound. Their
 *    position is the wire format between the two ends. The side that
 *    answers gets them from the offer, where they arrive receive-only and
 *    have to be told they may send (the D4b's silent person).
 *
 * Everything that touches the connection runs on one thread, in order; the
 * requests to the server go out on another, also in order, so an offer always
 * leaves before the candidates that belong to it.
 */
class DirectCall(
    private val context: Context,
    private val api: () -> Api?,
    private val hooks: Hooks,
    /** The room's store of each person's volume: one level per friend, call or room. */
    override val volumes: Volumes,
) : Stage {
    /** What the call needs from the rest of the app. */
    interface Hooks {
        /** Opens the live socket if it is not open, and waits for it. Blocking. */
        fun awaitLive(timeoutMs: Long): Boolean
        /** Shows the ringing notification (a telephone ringing). */
        fun ringing(callId: String, from: CallPerson, at: Long)
        /** Takes it away. */
        fun ringOver(callId: String)
        /** A voice room gives way to the call the person just chose. Blocking, until it has. */
        fun leaveRoom()
    }

    enum class Phase { IDLE, CALLING, RINGING, CONNECTING, LIVE, RECONNECTING }

    data class State(
        val phase: Phase = Phase.IDLE,
        val callId: String? = null,
        val peer: CallPerson? = null,
        /** This phone placed the call. */
        val outgoing: Boolean = false,
        val muted: Boolean = false,
        val camera: Boolean = false,
        val deafened: Boolean = false,
        val screen: Boolean = false,
        val screenChosen: Boolean = false,
        val speaking: Set<String> = emptySet(),
        // What the other end says it is sending (call_state). Said, not
        // measured: a muted microphone looks exactly like silence.
        val peerMuted: Boolean = false,
        val peerCam: Boolean = false,
        val peerScreen: Boolean = false,
        /** When the two were first connected, for the clock. */
        val since: Long = 0,
        val notice: String? = null,
        val outputs: List<Output> = emptyList(),
        val output: Output? = null,
    ) {
        /** On a call or placing one: what keeps the microphone service up. */
        val active get() = phase != Phase.IDLE && phase != Phase.RINGING
    }

    private val _state = MutableStateFlow(State())
    val state: StateFlow<State> = _state

    private val exec = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "direct") }
    init { com.taleshernandes.harmony.model.PersonalPreferences.listen { exec.execute { applyDeafen() } } }
    private val net = Executors.newSingleThreadExecutor { r -> Thread(r, "direct-net") }
    private val route = AudioRoute(context, phoneLike = true, restartAudio = { exec.execute { restartAudio() } }) { present, current ->
        _state.update { s -> s.copy(outputs = Output.entries.filter { it in present }, output = current) }
    }
    private val camera: LocalCamera = LocalCamera(context) { why ->
        exec.execute {
            if (why.isEmpty()) camera.reopen() else cameraOff(why)
        }
    }

    // Which call the work in flight belongs to. finish() moves it on, and
    // everything that waited (a request, the microphone, a description)
    // checks it when the wait is over.
    @Volatile private var gen = 0

    // Owned by the "direct" thread.
    private var callId: String? = null
    private var ice: List<IceServer> = emptyList()
    private var polite = false
    private var pc: PeerConnection? = null
    private var source: AudioSource? = null
    @Volatile private var mic: AudioTrack? = null
    private val micLock = Any()
    private var controls = DirectControls()
    private val speaking = Speaking()
    private val camWatch = CameraSendWatch()
    private var camTick = 0
    private var meter: ScheduledFuture<*>? = null
    private val screenChoice = ScreenChoice()
    private var screenCapturer: ScreenCapturerAndroid? = null
    private var screenHelper: SurfaceTextureHelper? = null
    private var screenRepeater: ScreenFrameRepeater? = null
    private var screenSource: VideoSource? = null
    private var screenTrack: VideoTrack? = null
    private var screenSize: ScreenShare.Size? = null
    private var screenTimeout: ScheduledFuture<*>? = null
    private var screenCodec: String? = null
    // The four slots, read once: reading PeerConnection.getTransceivers again
    // disposes every wrapper handed out before, tracks included (VoiceCall).
    private var slots: List<RtpTransceiver> = emptyList()
    // Whether the other end has picked up: until then the server refuses to
    // relay, so what this side has to say waits in [outbox], in order.
    private var live = false
    private val outbox = mutableListOf<Any>()
    // Signals that arrived before the connection existed.
    private val early = mutableListOf<CallSignal>()
    // Candidates that arrived before the description they belong to.
    private val pendingIce = mutableListOf<Candidate>()
    // Perfect negotiation, as the W3C names it.
    private var makingOffer = false
    private var isSettingRemoteAnswerPending = false
    private var ignoreOffer = false
    private var connected = false
    private var restartAt = 0L
    private var restartCredential = ""
    private var restartRetry: ScheduledFuture<*>? = null

    private fun credential(conn: PeerConnection): String =
        Regex("a=ice-ufrag:([^\\r\\n]+)").find(conn.localDescription?.description.orEmpty())?.groupValues?.get(1).orEmpty()

    private fun retrySignaling(g: Int, conn: PeerConnection) {
        restartRetry?.cancel(false)
        restartRetry = exec.schedule({
            if (g != gen || pc !== conn || restartAt == 0L || !live) return@schedule
            if (conn.signalingState() == PeerConnection.SignalingState.HAVE_LOCAL_OFFER) {
                Diag.log("direct restart: replay pending offer after gateway recovery")
                conn.localDescription?.let { post(Sdp("offer", it.description)) }
            } else if (conn.signalingState() == PeerConnection.SignalingState.STABLE) {
                val ice = conn.iceConnectionState()
                if (credential(conn) != restartCredential && (ice == PeerConnection.IceConnectionState.CONNECTED || ice == PeerConnection.IceConnectionState.COMPLETED)) {
                    Diag.log("direct ICE negotiation restored in ${android.os.SystemClock.elapsedRealtime() - restartAt} ms; same connection")
                    restartAt = 0L
                    connected = true
                    giveUp?.cancel(false)
                    giveUp = null
                    _state.update { it.copy(phase = Phase.LIVE) }
                    return@schedule
                }
                Diag.log("direct restart: negotiate again; gateway may have lost the offer")
                conn.restartIce()
            }
            retrySignaling(g, conn)
        }, 750, TimeUnit.MILLISECONDS)
    }
    private var ringWatch: ScheduledFuture<*>? = null
    private var ringEnd: ScheduledFuture<*>? = null
    private var repair: ScheduledFuture<*>? = null
    private var giveUp: ScheduledFuture<*>? = null
    // Calls that are over for this phone, so a late push (Firebase and the
    // socket both bring the same call) does not ring for one again.
    private val over = ArrayDeque<String>()

    // The pictures, under [videoLock] (touched by the screen's thread too).
    private val videoLock = Any()
    private val videoTracks = HashMap<String, VideoTrack>()
    private val sinks = HashMap<String, MutableSet<VideoSink>>()

    // ------------------------------------------------------------ pictures

    override fun attach(mid: String, sink: VideoSink) = synchronized(videoLock) {
        sinks.getOrPut(mid) { mutableSetOf() }.add(sink)
        runCatching { trackFor(mid)?.addSink(sink) }
        Unit
    }

    override fun detach(mid: String, sink: VideoSink) = synchronized(videoLock) {
        sinks[mid]?.remove(sink)
        runCatching { trackFor(mid)?.removeSink(sink) }
        Unit
    }

    private fun trackFor(mid: String): VideoTrack? = when (mid) { LOCAL -> camera.track; LOCAL_SCREEN -> screenTrack; else -> videoTracks[mid] }

    // ------------------------------------------------------------ ringing

    /** This phone places a call. The microphone permission is the screen's to ask for, before this. */
    fun ring(to: CallPerson) {
        if (_state.value.phase != Phase.IDLE) return
        _state.value = State(phase = Phase.CALLING, peer = to, outgoing = true, muted = !microphoneAllowed(context))
        val g = gen
        // On the network thread, not the call's: behind the hangup of a call
        // put down a moment ago (or the server answers "busy" with it), and
        // leaving the call's thread free to hear "put it down" meanwhile.
        net.execute {
            if (g != gen) return@execute
            hooks.leaveRoom()
            val a = api() ?: return@execute exec.execute { if (g == gen) finish("Sem conexão para ligar.") }
            val info = try {
                a.callStart(to.username)
            } catch (e: ApiError) {
                Diag.log("chamada: ligar recusado ${e.status} ${e.reason}")
                val why = when (e.reason) {
                    "busy" -> "${to.name} já está em outra chamada."
                    "ring_limit" -> "Espere alguns segundos para ligar de novo."
                    else -> "Não foi possível ligar."
                }
                return@execute exec.execute { if (g == gen) finish(why) }
            } catch (e: IOException) {
                Diag.log("chamada: ligar falhou (${e.javaClass.simpleName})")
                return@execute exec.execute { if (g == gen) finish("Sem conexão para ligar.") }
            }
            exec.execute { placed(g, a, info, to) }
        }
    }

    private fun placed(g: Int, a: Api, info: com.taleshernandes.harmony.model.CallInfo, to: CallPerson) {
        run {
            if (g != gen) {
                // Put down while it was being placed: that call is let go of too.
                net.execute { runCatching { a.callHangup(info.id, "hangup") } }
                return
            }
            callId = info.id
            ice = info.iceServers
            polite = info.polite
            _state.update { it.copy(callId = info.id, peer = info.to.takeIf { p -> p.username.isNotEmpty() } ?: to) }
            Diag.log("chamada para @${to.username} tocando, ${ice.size} servidor(es) de rota")
            // Built now, while it rings: finding a path is most of what makes a
            // call slow to start, and doing it after they pick up means they
            // spend those seconds looking at a silent screen (D4h).
            build()
            // Nobody picked up. The server forgets the call at the same moment
            // (call.go ringFor) and tells nobody, so this side says it.
            ringEnd = exec.schedule({
                if (g == gen && _state.value.phase == Phase.CALLING) {
                    Diag.log("chamada: ninguém atendeu")
                    endHere("missed", "${to.name} não atendeu.")
                }
            }, RING_MS, TimeUnit.MILLISECONDS)
        }
    }

    /**
     * Somebody is calling this phone, heard on the live socket or woken by
     * Firebase — often both, and the second is the same call. [at] is when
     * the server raised it (0 when unknown).
     */
    fun called(id: String, from: CallPerson, at: Long) = exec.execute {
        val s = _state.value
        if (s.callId == id || id in over) return@execute
        val now = System.currentTimeMillis()
        if (at > 0 && now - at > RING_MS) {
            Diag.log("chamada: aviso chegou depois de a chamada acabar")
            return@execute
        }
        if (s.phase != Phase.IDLE) {
            // Two calls crossing: the server refuses a second one to
            // anybody already on a call, so this is the rare race. The new
            // one is let go of, as the web does.
            Diag.log("chamada: outra chamada chegou durante esta; recusada")
            net.execute { runCatching { api()?.callHangup(id, "hangup") } }
            return@execute
        }
        callId = id
        val started = if (at > 0) at else now
        _state.value = State(phase = Phase.RINGING, callId = id, peer = from)
        Diag.log("chamada de @${from.username.ifEmpty { "?" }}")
        hooks.ringing(id, from, started)
        val g = gen
        // The socket may be closed (a phone woken by Firebase opens none), so
        // "they gave up" and "answered on the computer" are asked for.
        ringWatch = exec.scheduleWithFixedDelay({ checkRinging(g, id, from) }, WATCH_MS, WATCH_MS, TimeUnit.MILLISECONDS)
        ringEnd = exec.schedule({
            if (g == gen && _state.value.phase == Phase.RINGING) {
                Diag.log("chamada: tocou até o fim")
                finish("Chamada perdida de ${from.name}.")
            }
        }, (started + RING_MS - now).coerceAtLeast(1000), TimeUnit.MILLISECONDS)
    }

    private fun checkRinging(g: Int, id: String, from: CallPerson) {
        val a = api() ?: return
        net.execute {
            val answer = try {
                a.callGet(id)
            } catch (e: ApiError) {
                if (e.status == 404) null else return@execute
            } catch (_: IOException) {
                return@execute
            }
            exec.execute {
                if (g != gen || _state.value.phase != Phase.RINGING) return@execute
                when {
                    answer == null -> {
                        Diag.log("chamada: quem ligou desistiu")
                        finish("Chamada perdida de ${from.name}.")
                    }
                    answer.answered -> {
                        Diag.log("chamada: atendida em outro aparelho")
                        finish("Atendida em outro aparelho.")
                    }
                    // The name, if Firebase woke the phone and it was not known.
                    from.username.isEmpty() -> _state.update { it.copy(peer = answer.from) }
                }
            }
        }
    }

    /** Whether this call is the one ringing here now. */
    fun ringingNow(id: String) = _state.value.callId == id && _state.value.phase == Phase.RINGING

    /** Picks up. The microphone permission is the screen's to ask for, before this. */
    fun accept() {
        val s = _state.value
        val id = s.callId ?: return
        if (s.phase != Phase.RINGING) return
        // Only if it is still this call ringing: between reading it and here
        // the call's thread may have ended it and let another one ring
        // (audit, 26/09/2026). Everything after checks the id as well.
        if (!_state.compareAndSet(s, s.copy(phase = Phase.CONNECTING, muted = s.muted || !microphoneAllowed(context)))) return
        hooks.ringOver(id)
        val g = gen
        exec.execute { if (callId == id) stopRingTimers() }
        net.execute {
            hooks.leaveRoom()
            // The offer the other side sends on hearing "picked up" comes on
            // the live socket, and only to a socket that is open: it opens
            // first. Nothing is lost by waiting — the other end is still
            // ringing until the accept below.
            // Without it the other side's offer is relayed to nobody and
            // the call sits at "Conectando…" for good: better said now.
            if (!hooks.awaitLive(LIVE_WAIT_MS)) {
                Diag.log("chamada: o canal ao vivo não abriu; sem atender")
                exec.execute { if (g == gen && callId == id) endHere("hangup", "Sem conexão para atender.") }
                return@execute
            }
            val a = api()
            val info = try {
                a?.callAccept(id) ?: throw IOException("sem sessão")
            } catch (e: ApiError) {
                Diag.log("chamada: atender recusado ${e.status} ${e.reason}")
                exec.execute {
                    if (g == gen && callId == id) finish(if (e.reason == "taken") "Atendida em outro aparelho." else "A chamada já tinha acabado.")
                }
                return@execute
            } catch (e: IOException) {
                Diag.log("chamada: atender falhou (${e.javaClass.simpleName})")
                exec.execute { if (g == gen && callId == id) endHere("hangup", "Sem conexão para atender.") }
                return@execute
            }
            exec.execute {
                if (g != gen || callId != id) return@execute
                ice = info.iceServers
                polite = info.polite
                live = true
                if (info.from.username.isNotEmpty()) _state.update { it.copy(peer = info.from) }
                Diag.log("chamada atendida, ${ice.size} servidor(es) de rota")
                build()
                connectBy(g)
            }
        }
    }

    /** Refuses a call that is ringing, from the screen or the notification. [done] runs once the server heard it. */
    fun decline(id: String, done: (() -> Unit)? = null) = exec.execute {
        if (_state.value.callId != id || _state.value.phase != Phase.RINGING) {
            // Already over here; the server may still not know.
            net.execute {
                runCatching { api()?.callHangup(id, "rejected") }
                done?.invoke()
            }
            return@execute
        }
        endHere("rejected", null, done)
    }

    /** Puts the telephone down, at any point: giving up on a ring, or an hour in. */
    fun hangup(done: (() -> Unit)? = null) {
        silence()
        // The session of this moment: signing out clears it right after.
        val a = api()
        exec.execute {
            val s = _state.value
            if (s.phase == Phase.IDLE) {
                done?.invoke()
                return@execute
            }
            endHere(if (s.phase == Phase.RINGING) "rejected" else "hangup", null, done, a)
        }
    }

    // ------------------------------------------------------------ events

    /** Everything call-shaped that arrives on /ws. */
    fun onEvent(ev: GatewayEvent) {
        when (ev) {
            is CallIncoming -> called(ev.callId, ev.from, 0)
            is CallAccepted -> exec.execute { accepted(ev) }
            is CallTaken -> exec.execute {
                // The one this phone answered is taken too, by this phone:
                // by then it is no longer ringing here.
                if (ev.callId == callId && _state.value.phase == Phase.RINGING) finish("Atendida em outro aparelho.")
            }
            is CallEnded -> exec.execute {
                if (ev.callId != callId) return@execute
                Diag.log("chamada encerrada pelo outro lado: ${ev.reason}")
                val s = _state.value
                finish(
                    when {
                        s.phase == Phase.RINGING -> "Chamada perdida de ${s.peer?.name ?: "alguém"}."
                        ev.reason == "rejected" && s.outgoing && s.phase == Phase.CALLING -> "Chamada recusada."
                        else -> "Chamada encerrada."
                    },
                )
            }
            is CallSignalEvent -> exec.execute { signal(ev.signal) }
            is CallStateEvent -> exec.execute {
                if (ev.callId != callId) return@execute
                _state.update { it.copy(peerMuted = !ev.mic, peerCam = ev.cam, peerScreen = ev.screen) }
                videoChanged()
            }
            else -> Unit
        }
    }

    private fun accepted(ev: CallAccepted) {
        if (ev.callId != callId || _state.value.phase != Phase.CALLING) return
        ringEnd?.cancel(false)
        ringEnd = null
        Diag.log("chamada: atenderam")
        _state.update { it.copy(phase = Phase.CONNECTING, peer = ev.peer.takeIf { p -> p.username.isNotEmpty() } ?: it.peer) }
        live = true
        connectBy(gen)
        // Everything said during the ring, in the order it was said: the
        // offer before its candidates.
        val waiting = outbox.toList()
        outbox.clear()
        waiting.forEach(::send)
    }

    /** The phone's network changed under the call: the path is looked for again. */
    fun networkChanged(moved: Boolean) = exec.execute {
        val conn = pc ?: return@execute
        if (!moved || !live) return@execute
        Diag.log("chamada: a rede trocou, procurando caminho de novo")
        if (restartAt == 0L) {
            restartAt = android.os.SystemClock.elapsedRealtime()
            restartCredential = credential(conn)
        }
        _state.update { it.copy(phase = Phase.RECONNECTING) }
        dropLater(gen)
        conn.restartIce()
        retrySignaling(gen, conn)
        if (conn.signalingState() == PeerConnection.SignalingState.HAVE_LOCAL_OFFER) {
            conn.localDescription?.let { post(Sdp("offer", it.description)) }
        }
    }

    // ------------------------------------------------------------ the connection

    private fun build() {
        if (pc != null) return
        val factory = Engine.factory(context)
        val videoCodecs = factory.getRtpSenderCapabilities(MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO).codecs
        screenCodec = ScreenShare.codec(videoCodecs.map { it.name })
        route.start()
        applyMic() // the mode starts from the microphone as it is
        val config = PeerConnection.RTCConfiguration(
            ice.map { s ->
                PeerConnection.IceServer.builder(s.urls).apply {
                    s.username?.let(::setUsername)
                    s.credential?.let(::setPassword)
                }.createIceServer()
            },
        ).apply {
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
            bundlePolicy = PeerConnection.BundlePolicy.MAXBUNDLE
            rtcpMuxPolicy = PeerConnection.RtcpMuxPolicy.REQUIRE
            // Keep looking after connecting, so a new network can be offered
            // without starting over (A3).
            continualGatheringPolicy = PeerConnection.ContinualGatheringPolicy.GATHER_CONTINUALLY
        }
        val g = gen
        val conn = factory.createPeerConnection(config, Observer(g)) ?: run {
            Diag.log("chamada: a conexão não pôde ser criada")
            return endHere("hangup", "Não foi possível completar a chamada.")
        }
        if (microphoneAllowed(context)) {
            val src = factory.createAudioSource(MediaConstraints().apply {
                listOf("googEchoCancellation", "googNoiseSuppression", "googAutoGainControl", "googHighpassFilter")
                    .forEach { mandatory.add(MediaConstraints.KeyValuePair(it, if (it == "googNoiseSuppression") "false" else "true")) }
            })
            source = src
            synchronized(micLock) {
                mic = factory.createAudioTrack("direct-mic", src)
                applyMic()
            }
        }
        pc = conn
        controls = DirectControls(muted = _state.value.muted)
        meter?.cancel(false)
        meter = exec.scheduleWithFixedDelay({ readLevels(g) }, 0, 100, TimeUnit.MILLISECONDS)
        if (!polite) {
            // All four, now, in the order that is the wire format.
            slots = (0 until SLOTS).map { i ->
                val kind = if (i == SLOT_MIC || i == SLOT_SCREEN_AUDIO) {
                    MediaStreamTrack.MediaType.MEDIA_TYPE_AUDIO
                } else {
                    MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO
                }
                conn.addTransceiver(kind, RtpTransceiver.RtpTransceiverInit(directionOf(i)))
            }
            slots[SLOT_SCREEN].setCodecPreferences(videoCodecs.filterNot { it.name.equals("AV1", true) }
                .sortedBy { if (it.name.equals(screenCodec, true)) 0 else 1 })
            fill()
            collectVideos()
        }
        val waiting = early.toList()
        early.clear()
        waiting.forEach(::signal)
    }

    /** What this phone sends, into its slots: the voice, and the camera if it is on. */
    private fun fill() {
        if (slots.size < SLOTS) return
        synchronized(micLock) { slots[SLOT_MIC].sender.setTrack(mic, false) }
        camera.track?.let { fitCamera(it) }
        fitScreen()
        applyDeafen()
        // On the side that answered the slots came receive-only (see the
        // class comment). The camera's is opened now too, even with nothing in
        // it, so that switching the camera on later needs no negotiation.
        for (i in (0 until SLOTS)) {
            val tr = slots[i]
            if (tr.direction != directionOf(i)) tr.direction = directionOf(i)
        }
    }

    /**
     * The echo of 25–29/10/2026: this phone never sends the screen's sound in a
     * direct call, and a slot that may send it is a second audio send channel.
     * Its disabled track told the one shared audio processor that its output
     * was unused, so the processor handed the raw microphone on, echo and all;
     * the voice's channel said the opposite, and whichever spoke last won —
     * so the echo came and went (0.36 made the voice speak last after each
     * negotiation, and lost the race sometimes). Receive-only, the slot has no
     * send stream and never speaks; the other side's screen sound still
     * arrives in it. The room has a single audio sender and never had this.
     */
    private fun directionOf(slot: Int) =
        if (slot == SLOT_SCREEN_AUDIO) RtpTransceiver.RtpTransceiverDirection.RECV_ONLY
        else RtpTransceiver.RtpTransceiverDirection.SEND_RECV

    private fun fitCamera(track: VideoTrack) {
        if (slots.size < SLOTS) return
        val sender = slots[SLOT_CAM].sender
        sender.setTrack(track, false)
        runCatching {
            val params = sender.parameters
            params.encodings.firstOrNull()?.maxBitrateBps = CAM_MAX_BPS
            params.degradationPreference = RtpParameters.DegradationPreference.BALANCED
            sender.parameters = params
        }
    }

    /** The other end's camera and screen, by slot. */
    private fun collectVideos() = synchronized(videoLock) {
        for ((slot, mid) in listOf(SLOT_CAM to PEER_CAM, SLOT_SCREEN to PEER_SCREEN)) {
            val track = slots.getOrNull(slot)?.receiver?.track() as? VideoTrack ?: continue
            if (videoTracks[mid] === track) continue
            videoTracks[mid] = track
            sinks[mid]?.forEach { runCatching { track.addSink(it) } }
        }
    }

    private fun negotiate() {
        val conn = pc ?: return
        if (conn.signalingState() != PeerConnection.SignalingState.STABLE) return
        makingOffer = true
        try {
            val offer = awaitCreate { conn.createOffer(it, MediaConstraints()) }
            await("local") { conn.setLocalDescription(it, offer) }
            post(Sdp("offer", offer.description))
        } catch (e: IOException) {
            Diag.log("chamada: oferta falhou: ${e.message}")
        } finally {
            makingOffer = false
        }
    }

    /** One message from the other end: perfect negotiation, the W3C's steps. */
    private fun signal(sig: CallSignal) {
        if (sig.callId != callId) return
        val conn = pc ?: run {
            early.add(sig)
            return
        }
        when (sig) {
            is CallSignal.Ice -> {
				if (ignoreOffer) {
					Diag.log("direct candidate ignored during glare")
					return
				}
                val c = sig.candidate
                // The browser's "no more candidates" is an empty one.
                if (c.candidate.isBlank()) return
                if (conn.remoteDescription == null) {
                    pendingIce.add(c)
                    return
                }
                addCandidate(conn, c)
            }
            is CallSignal.Description -> {
                val offer = sig.sdp.type == "offer"
                // Glare: both offered at once. The side that called pretends
                // not to hear; the side that answered rolls its own offer back.
                val readyForOffer = !makingOffer && (conn.signalingState() == PeerConnection.SignalingState.STABLE || isSettingRemoteAnswerPending)
                val collision = offer && !readyForOffer
                ignoreOffer = !polite && collision
                if (ignoreOffer) {
                    Diag.log("chamada: oferta ignorada (as duas pontas ofertaram)")
                    return
                }
                try {
                    if (collision) {
                        await("desfazer") { conn.setLocalDescription(it, SessionDescription(SessionDescription.Type.ROLLBACK, "")) }
                    }
                    val type = if (offer) SessionDescription.Type.OFFER else SessionDescription.Type.ANSWER
                    isSettingRemoteAnswerPending = !offer
                    try {
                        var remote = loopbackAlias(sig.sdp.sdp)
                        if (offer) {
                            val screenSection = remote.split("m=").getOrNull(SLOT_SCREEN + 1).orEmpty()
                            val mid = Regex("(?m)^a=mid:([^\\r\\n]+)").find(screenSection)?.groupValues?.get(1)
                            if (mid != null && screenCodec != null) {
                                remote = runCatching { ScreenShare.preferCodec(remote, mid, screenCodec!!) }.getOrDefault(remote)
                            }
                        }
                        await("remota") { conn.setRemoteDescription(it, SessionDescription(type, remote)) }
                        applyDeafen()
                    } finally {
                        isSettingRemoteAnswerPending = false
                    }
                    val held = pendingIce.toList()
                    pendingIce.clear()
                    held.forEach { addCandidate(conn, it) }
                    if (offer) {
                        if (slots.isEmpty()) {
                            // The four slots, brought by the first offer.
                            slots = conn.transceivers
                            Diag.log("chamada: ${slots.size} vagas chegaram com a oferta")
                            collectVideos()
                        }
                        fill()
                        val answer = awaitCreate { conn.createAnswer(it, MediaConstraints()) }
                        await("local") { conn.setLocalDescription(it, answer) }
                        post(Sdp("answer", answer.description))
                    }
                } catch (e: IOException) {
                    Diag.log("chamada: não consegui aplicar ${sig.sdp.type}: ${e.message}")
                }
            }
        }
    }

    private fun addCandidate(conn: PeerConnection, c: Candidate) {
        val ok = conn.addIceCandidate(IceCandidate(c.sdpMid ?: "", c.sdpMLineIndex, loopbackAlias(c.candidate)))
        if (!ok && !ignoreOffer) Diag.log("chamada: candidato recusado")
    }

    // ------------------------------------------------------------ talking to the server

    private fun post(what: Any) {
        if (!live) {
            outbox.add(what)
            return
        }
        send(what)
    }

    private fun send(what: Any) {
        val id = callId ?: return
        val a = api() ?: return
        val g = gen
        net.execute {
            if (g != gen) return@execute
            try {
                when (what) {
                    is Sdp -> a.callDescription(id, what)
                    is Candidate -> a.callCandidate(id, what)
                }
            } catch (e: IOException) {
                Diag.log("chamada: não consegui mandar ${if (what is Sdp) what.type else "candidato"} (${e.javaClass.simpleName})")
                if (what is Sdp && live) {
                    exec.schedule({
                        if (g == gen && pc?.localDescription?.description == what.sdp) send(what)
                    }, 500, TimeUnit.MILLISECONDS)
                }
            }
        }
    }

    /** Tells the other end what this one is sending, so it can draw it. */
    private fun tellPeer() {
        val id = callId ?: return
        val a = api() ?: return
        val s = _state.value
        if (!live) return
        net.execute {
            try {
                a.callState(id, !s.muted && !s.deafened, s.camera, s.screen)
            } catch (_: IOException) {
                // The other end keeps the last thing it heard; the call does
                // not depend on it.
            }
        }
    }

    // ------------------------------------------------------------ what I send

    fun setMuted(on: Boolean) {
        if (_state.value.deafened || (!on && !microphoneAllowed(context))) return
        synchronized(micLock) { controls = controls.mute(on) }
        _state.update { it.copy(muted = on) }
        if (!on && mic == null) exec.execute {
            if (pc != null && mic == null && microphoneAllowed(context)) {
                val factory = Engine.factory(context)
                val src = factory.createAudioSource(MediaConstraints().apply {
                    listOf("googEchoCancellation", "googNoiseSuppression", "googAutoGainControl", "googHighpassFilter")
                        .forEach { mandatory.add(MediaConstraints.KeyValuePair(it, if (it == "googNoiseSuppression") "false" else "true")) }
                })
                source = src
                synchronized(micLock) {
                    mic = factory.createAudioTrack("direct-mic", src)
                    applyMic()
                }
                fill()
            }
        }
        applyMic()
        exec.execute { tellPeer() }
    }

    private fun applyMic() = synchronized(micLock) {
        val open = microphoneIsOpen(mic != null, _state.value.muted || _state.value.deafened)
        mic?.setEnabled(open)
        route.setMicOpen(open)
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
        if (mic != null) {
            c.setAudioRecording(false)
            c.setAudioRecording(true)
        }
        Diag.log("voz: áudio reiniciado para a troca de modo")
    }

    private fun silence() = synchronized(micLock) { mic?.setEnabled(false) }

    fun chooseOutput(o: Output) = route.choose(o)

    /** The camera permission is the screen's to ask for, before this. */
    fun setCamera(on: Boolean) = exec.execute {
        if (on == _state.value.camera || pc == null) return@execute
        if (!on) return@execute cameraOff(null)
        val track = camera.start() ?: return@execute
        synchronized(videoLock) { sinks[LOCAL]?.forEach { runCatching { track.addSink(it) } } }
        fitCamera(track)
        _state.update { it.copy(camera = true) }
        Diag.log("chamada: câmera ligada")
        videoChanged()
        tellPeer()
    }

    private fun cameraOff(why: String?) {
        val track = camera.track
        synchronized(videoLock) {
            if (track != null) sinks[LOCAL]?.forEach { runCatching { track.removeSink(it) } }
        }
        if (track != null && slots.size >= SLOTS && slots[SLOT_CAM].sender.track() === track) {
            slots[SLOT_CAM].sender.setTrack(null, false)
        }
        camera.stop()
        val wasOn = _state.value.camera
        _state.update { it.copy(camera = false, notice = why ?: it.notice) }
        if (wasOn) {
            Diag.log("chamada: câmera desligada")
            videoChanged()
            tellPeer()
        }
    }

    /** The app went out of sight or came back: Android lends a camera only to what is seen. */
    fun setLooking(on: Boolean) = exec.execute {
        if (!_state.value.camera) return@execute
        if (on) camera.resume() else camera.pause()
    }

    // A picture on the call means it is looked at: the sound leaves the ear.
    private fun videoChanged() {
        val s = _state.value
        route.setVideo(s.camera || s.peerCam || s.peerScreen || s.screenChosen)
    }

    fun dismissNotice() = _state.update { it.copy(notice = null) }


    fun setDeafened(on: Boolean) = exec.execute {
        if (!_state.value.active || on == _state.value.deafened) return@execute
        synchronized(micLock) {
            controls = DirectControls(_state.value.muted, _state.value.deafened, controls.beforeDeafen).deafen(on)
            speaking.clear()
            _state.update { it.copy(muted = controls.muted, deafened = controls.deafened, speaking = emptySet()) }
            applyMic()
        }
        applyDeafen()
        tellPeer()
        Diag.log("direct: hearing ${if (on) "off" else "on"}; mic muted=${controls.muted}")
    }

    // The received sound: off while deafened, and at the levels this phone
    // keeps for the person — the same ones the room uses for them.
    private fun applyDeafen() {
        val who = _state.value.peer?.username
        for ((slot, screen) in listOf(SLOT_MIC to false, SLOT_SCREEN_AUDIO to true)) {
            val track = slots.getOrNull(slot)?.receiver?.track() as? AudioTrack ?: continue
            track.setEnabled(!_state.value.deafened && (who == null || !com.taleshernandes.harmony.model.PersonalPreferences.silent(who, screen)))
            if (who != null) track.setVolume(volumes.get(who, screen).toDouble())
        }
    }

    private val _volumeRevision = MutableStateFlow(0)
    override val volumeRevision: StateFlow<Int> = _volumeRevision

    override fun setVolume(username: String, screen: Boolean, level: Float) {
        volumes.set(username, screen, level)
        _volumeRevision.update { it + 1 }
        exec.execute { applyDeafen() }
    }

    override val ownCamera get() = LOCAL
    override val frontCamera: StateFlow<Boolean> get() = camera.front
    override fun canSwitchCamera() = LocalCamera.canSwitch(context)

    override fun switchCamera() = exec.execute {
        camera.switch { exec.execute { _state.update { it.copy(notice = "Não deu para trocar de câmera.") } } }
    }

    private fun readLevels(g: Int) {
        pc?.getStats { report -> exec.execute {
            if (g != gen || pc == null) return@execute
            val s = _state.value
            val now = android.os.SystemClock.elapsedRealtime()
            // The camera line of the room's Diag, once a second like there.
            if (++camTick % 10 == 0) {
                if (s.camera && camera.track != null) camWatch.judge(report, slots.getOrNull(SLOT_CAM)?.mid, now)
                else camWatch.reset()
            }
            for (st in report.statsMap.values) {
                if (st.members["kind"] != "audio") continue
                val level = (st.members["audioLevel"] as? Number)?.toDouble() ?: continue
                if (st.type == "inbound-rtp" && st.members["mid"] == slots.getOrNull(SLOT_MIC)?.mid && !s.peerMuted && !s.deafened)
                    speaking.feed(PEER_CAM, level, now)
                if (st.type == "media-source" && st.members["trackIdentifier"] == "direct-mic") EchoProbe.sample(st.members)
                if (st.type == "media-source" && st.members["trackIdentifier"] == "direct-mic" && !s.muted && !s.deafened)
                    speaking.feed(LOCAL, level, now)
            }
            _state.update { it.copy(speaking = speaking.talking(now)) }
        } }
    }

    /** Capture starts only after the service acknowledges its projection type. */
    fun setScreen(on: Boolean, projection: Intent? = null) = exec.execute {
        if (!on) { endScreen(); return@execute }
        val id = callId ?: return@execute
        if (projection == null || screenChoice.wanted || !_state.value.active) return@execute
        val turn = screenChoice.choose(true)
        _state.update { it.copy(screenChosen = true) }
        videoChanged()
        screenTimeout = exec.schedule({
            if (screenChoice.accepts(turn)) captureFailed()
        }, 10, TimeUnit.SECONDS)
        CallService.startProjection(context, id, turn) { ready -> exec.execute {
            if (!screenChoice.accepts(turn) || callId != id) {
                CallService.endProjection(turn, direct = true)
                return@execute
            }
            screenTimeout?.cancel(false)
            screenTimeout = null
            if (!ready) { captureFailed(); return@execute }
            try {
                val factory = Engine.factory(context)
                val cap = ScreenCapturerAndroid(projection, object : MediaProjection.Callback() {
                    override fun onStop() { exec.execute { if (screenChoice.accepts(turn)) endScreen() } }
                    override fun onCapturedContentResize(width: Int, height: Int) {
                        exec.execute {
                            if (screenChoice.accepts(turn) && width > 0 && height > 0) resizeScreen(width, height)
                        }
                    }
                })
                screenCapturer = cap
                val helper = SurfaceTextureHelper.create("direct-screen", Engine.egl.eglBaseContext)
                screenHelper = helper
                val src = factory.createVideoSource(true)
                screenSource = src
                val repeater = ScreenFrameRepeater(helper, src.capturerObserver)
                screenRepeater = repeater
                cap.initialize(helper, context, repeater)
                val metrics = context.resources.displayMetrics
                val size = ScreenShare.size(metrics.widthPixels, metrics.heightPixels)
                screenSize = size
                src.adaptOutputFormat(size.width, size.height, ScreenShare.FPS)
                cap.startCapture(size.width, size.height, ScreenShare.FPS)
                val track = factory.createVideoTrack("direct-screen", src)
                synchronized(videoLock) {
                    screenTrack = track
                    sinks[LOCAL_SCREEN]?.forEach { track.addSink(it) }
                }
                fitScreen()
                _state.update { it.copy(screen = true, notice = "Tela compartilhada sem som do celular.") }
                tellPeer()
                Diag.log("direct screen: on, maintain-framerate; separate playback source unavailable")
            } catch (e: RuntimeException) {
                Diag.log("direct screen: capture failed (${e.javaClass.simpleName})")
                captureFailed()
            }
        } }
    }

    private fun resizeScreen(width: Int, height: Int) {
        val size = ScreenShare.size(width, height)
        // A projection reports resize again after its virtual display changes.
        // Match the room's guard to avoid repeatedly resizing the same display.
        if (size == screenSize) return
        runCatching {
            screenSize = size
            screenCapturer?.changeCaptureFormat(size.width, size.height, ScreenShare.FPS)
            screenSource?.adaptOutputFormat(size.width, size.height, ScreenShare.FPS)
        }.onFailure { Diag.log("direct screen: resize failed (${it.javaClass.simpleName})") }
    }

    private fun fitScreen() {
        if (slots.size < SLOTS) return
        val sender = slots[SLOT_SCREEN].sender
        sender.setTrack(screenTrack, false)
        if (screenTrack != null) {
            val params = sender.parameters
            ScreenShare.tune(params)
            check(sender.setParameters(params)) { "Screen parameters refused" }
        }
    }

    private fun captureFailed() {
        Diag.log("direct screen: capture unavailable")
        endScreen()
        _state.update { it.copy(notice = "Não deu para compartilhar a tela. Tente de novo com o app aberto.") }
    }

    private fun endScreen() {
        val had = screenChoice.wanted
        val turn = screenChoice.turn
        screenChoice.choose(false)
        screenTimeout?.cancel(false); screenTimeout = null
        slots.getOrNull(SLOT_SCREEN)?.sender?.setTrack(null, false)
        synchronized(videoLock) {
            screenTrack?.let { t -> sinks[LOCAL_SCREEN]?.forEach { runCatching { t.removeSink(it) } } }
            runCatching { screenTrack?.dispose() }
            screenTrack = null
        }
        runCatching { screenRepeater?.close() }; screenRepeater = null
        runCatching { screenCapturer?.stopCapture() }
        runCatching { screenCapturer?.dispose() }; screenCapturer = null
        runCatching { screenSource?.dispose() }; screenSource = null
        runCatching { screenHelper?.dispose() }; screenHelper = null
        screenSize = null
        _state.update { it.copy(screen = false, screenChosen = false) }
        if (had) {
            CallService.endProjection(turn, direct = true)
            tellPeer()
            videoChanged()
            Diag.log("direct screen: off")
        }
    }

    // ------------------------------------------------------------ ending

    /** Ends it here and tells the server, with [reason] (call.go: hangup, rejected, missed). */
    private fun endHere(reason: String, notice: String?, done: (() -> Unit)? = null, with: Api? = null) {
        restartAt = 0L
        restartRetry?.cancel(false)
        restartRetry = null
        val id = callId
        finish(notice)
        if (id == null) {
            done?.invoke()
            return
        }
        val a = with ?: api()
        net.execute {
            try {
                a?.callHangup(id, reason)
            } catch (_: IOException) {
                // Gone already, or the network went first: the server's own
                // sweep ends it either way.
            }
            done?.invoke()
        }
    }

    /** Tears everything down. The only place that does. */
    private fun finish(notice: String?) {
        gen++
        val id = callId
        if (id != null) {
            over.addLast(id)
            while (over.size > 16) over.removeFirst()
            hooks.ringOver(id)
        }
        stopRingTimers()
        ringEnd?.cancel(false)
        ringEnd = null
        repair?.cancel(false)
        repair = null
        giveUp?.cancel(false)
        giveUp = null
        synchronized(videoLock) {
            for ((mid, t) in videoTracks) sinks[mid]?.forEach { runCatching { t.removeSink(it) } }
            videoTracks.clear()
            camera.track?.let { t -> sinks[LOCAL]?.forEach { runCatching { t.removeSink(it) } } }
        }
        meter?.cancel(false)
        meter = null
        speaking.clear()
        endScreen()
        camera.stop()
        slots = emptyList()
        pc?.dispose()
        pc = null
        synchronized(micLock) {
            mic?.dispose()
            mic = null
        }
        source?.dispose()
        source = null
        route.stop()
        route.setVideo(false)
        callId = null
        live = false
        connected = false
        outbox.clear()
        early.clear()
        pendingIce.clear()
        makingOffer = false
        ignoreOffer = false
        if (id != null) Diag.log("chamada: fim${notice?.let { " ($it)" } ?: ""}")
        _state.value = State(notice = notice)
    }

    private fun stopRingTimers() {
        ringWatch?.cancel(false)
        ringWatch = null
    }

    // ------------------------------------------------------------ plumbing

    private inner class Observer(private val g: Int) : PeerConnection.Observer {
        private val found = HashMap<String, Int>()

        override fun onIceCandidate(c: IceCandidate) {
            // Types and counts in the log, never addresses.
            val type = Regex(" typ (\\w+)").find(c.sdp)?.groupValues?.get(1) ?: "?"
            synchronized(found) { found[type] = (found[type] ?: 0) + 1 }
            // Through the call's thread, so that it goes out after the offer
            // it belongs to, which that thread is still sending.
            exec.execute { if (g == gen) post(Candidate(c.sdp, c.sdpMid, c.sdpMLineIndex)) }
        }

        override fun onIceGatheringChange(s: PeerConnection.IceGatheringState) {
            if (s == PeerConnection.IceGatheringState.COMPLETE) {
                val kinds = synchronized(found) { found.entries.joinToString(" ") { "${it.key}=${it.value}" } }
                Diag.log("chamada caminhos: ${kinds.ifEmpty { "NENHUM" }}")
            }
        }

        override fun onRenegotiationNeeded() {
            exec.execute { if (g == gen) negotiate() }
        }

        override fun onConnectionChange(s: PeerConnection.PeerConnectionState) {
            exec.execute {
                if (g != gen) return@execute
                Diag.log("chamada ${s.name.lowercase()}")
                when (s) {
                    PeerConnection.PeerConnectionState.CONNECTED -> {
                        repair?.cancel(false)
                        repair = null
                        giveUp?.cancel(false)
                        giveUp = null
                        connected = true
                        val first = _state.value.since == 0L
                        _state.update { it.copy(phase = Phase.LIVE, since = if (first) System.currentTimeMillis() else it.since) }
                        if (first) tellPeer()
                    }
                    // Quiet — usually the network changed. A moment for the
                    // path to move by itself, then a new search.
                    PeerConnection.PeerConnectionState.DISCONNECTED -> {
                        connected = false
                        _state.update { it.copy(phase = Phase.RECONNECTING) }
                        Diag.log("direct ICE restart immediately: disconnected")
                        networkChanged(true)
                        dropLater(g)
                    }
                    PeerConnection.PeerConnectionState.FAILED -> {
                        connected = false
                        _state.update { it.copy(phase = Phase.RECONNECTING) }
                        Diag.log("direct ICE restart immediately: failed")
                        networkChanged(true)
                        dropLater(g)
                    }
                    else -> Unit
                }
            }
        }

        override fun onAddTrack(receiver: RtpReceiver, streams: Array<out MediaStream>?) {}
        override fun onSignalingChange(s: PeerConnection.SignalingState) {}
        override fun onIceConnectionChange(s: PeerConnection.IceConnectionState) {
            exec.execute {
                if (g != gen) return@execute
                Diag.log("direct ICE ${s.name.lowercase()}")
                if (s == PeerConnection.IceConnectionState.DISCONNECTED || s == PeerConnection.IceConnectionState.FAILED) {
                    networkChanged(true)
                } else if (restartAt != 0L && (s == PeerConnection.IceConnectionState.CONNECTED || s == PeerConnection.IceConnectionState.COMPLETED)) {
                    Diag.log("direct ICE restart connected in ${android.os.SystemClock.elapsedRealtime() - restartAt} ms; same connection")
                    restartAt = 0L
                    connected = true
                    giveUp?.cancel(false)
                    giveUp = null
                    _state.update { it.copy(phase = Phase.LIVE) }
                }
            }
        }
        override fun onIceConnectionReceivingChange(b: Boolean) {}
        override fun onIceCandidatesRemoved(c: Array<out IceCandidate>) {}
        override fun onAddStream(s: MediaStream) {}
        override fun onRemoveStream(s: MediaStream) {}
        override fun onDataChannel(d: DataChannel) {}
    }

    /**
     * The first connection has this long to happen. An offer or an answer
     * lost on the way (the socket dropping at the wrong moment) is otherwise
     * a call at "Conectando…" for ever, since nothing fails that never began
     * (audit, 26/09/2026).
     */
    private fun connectBy(g: Int) {
        exec.schedule({
            if (g == gen && _state.value.since == 0L) {
                Diag.log("chamada: não conectou em ${CONNECT_MS / 1000} s")
                endHere("hangup", "Não deu para completar a chamada.")
            }
        }, CONNECT_MS, TimeUnit.MILLISECONDS)
    }

    /** A call with no path for this long is over, and both ends are told. */
    private fun dropLater(g: Int) {
        if (giveUp != null) return
        giveUp = exec.schedule({
            giveUp = null
            if (g == gen && !connected) {
                Diag.log("chamada: sem caminho há ${DROP_MS / 1000} s")
                endHere("hangup", "A chamada caiu.")
            }
        }, DROP_MS, TimeUnit.MILLISECONDS)
    }

    /** Test builds only: see VoiceCall.loopbackAlias. */
    private fun loopbackAlias(sdp: String): String {
        val alias = BuildConfig.LOOPBACK_ALIAS
        if (alias.isEmpty() || " 127.0.0.1 " !in sdp) return sdp
        return sdp.replace(" 127.0.0.1 ", " $alias ")
    }

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
        if (!done.await(10, TimeUnit.SECONDS)) throw IOException("descrição: sem resposta em 10 s")
        return out ?: throw IOException("descrição: $error")
    }

    companion object {
        /** How long a call rings (call.go ringFor, and the web's timer). */
        const val RING_MS = 45_000L
        // The slots, in the order the side that calls creates them (direct.ts).
        const val SLOT_MIC = 0
        const val SLOT_CAM = 1
        const val SLOT_SCREEN = 2
        const val SLOT_SCREEN_AUDIO = 3
        private const val SLOTS = 4
        /** Ids of the pictures, for [attach]. */
        const val LOCAL = "direct-local"
        const val LOCAL_SCREEN = "direct-local-screen"
        const val PEER_CAM = "direct-cam"
        const val PEER_SCREEN = "direct-screen"
        private const val WATCH_MS = 2000L
        private const val LIVE_WAIT_MS = 8000L
        private const val REPAIR_MS = 3000L
        private const val DROP_MS = 30_000L
        private const val CONNECT_MS = 25_000L
        private const val CAM_MAX_BPS = 900_000
    }
}
