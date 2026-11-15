package com.taleshernandes.harmony.media

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.ResultReceiver
import com.taleshernandes.harmony.HarmonyApp
import com.taleshernandes.harmony.R
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.ui.CallActivity
import com.taleshernandes.harmony.ui.MainActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.launch

/**
 * What keeps a call alive with the screen off: while it runs, Android counts
 * the app as in use, and keeps giving it the microphone and the network.
 *
 * It lives exactly as long as a call does — a voice room, or a direct call.
 * It starts on the person's tap (joining a room, placing a call, Atender: a
 * microphone service may only start while the app is on screen) and stops
 * itself when the call is over, for whatever reason. Its notification says
 * which room or who, and mutes and leaves without opening the app.
 */
class CallService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var wake: PowerManager.WakeLock? = null
    private var started = false
    private var foregroundMicrophone = false
    private var projectionTurn: Int? = null
    private var projectionDirect = false
    // The camera type, held while the room's camera is on: without it Android
    // cuts the camera the moment the app leaves the screen.
    private var cameraTyped = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val core = (application as HarmonyApp).core
        running = this
        val projectionReply = if (intent?.action == ACTION_PROJECTION) projectionReply(intent) else null
        // The buttons act on whichever call this is: a direct call, when there
        // is one, and otherwise the room.
        val direct = core.direct.state.value.active
        if (!started) {
            started = true
            if (!goForeground(render(What.of(core.voice.state.value, "", core.direct.state.value)))) {
                started = false
                projectionReply?.send(0, null)
                stopSelf()
                return START_NOT_STICKY
            }
            follow()
        }
        if (started && !foregroundMicrophone && microphoneAllowed(this) && Build.VERSION.SDK_INT >= 29) {
            runCatching {
                startForeground(ID, render(What.of(core.voice.state.value, "", core.direct.state.value)),
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE or projectionType())
                foregroundMicrophone = true
            }
                .onFailure { Diag.log("microphone service upgrade: ${it.javaClass.simpleName}") }
        }
        if (intent?.action == ACTION_PROJECTION) {
            val turn = intent.getIntExtra("turn", -1)
            val voice = core.voice.state.value
            val d = core.direct.state.value
            val valid = turn >= 0 && if (direct) d.screenChosen && intent.getStringExtra("room") == d.callId
                else voice.screenChosen && voice.channelId != null && intent.getStringExtra("room") == voice.channelId
            var ready = false
            if (valid) {
                projectionTurn = turn
                projectionDirect = direct
                ready = runCatching { refreshTypes() }
                    .onFailure {
                        projectionTurn = null
                        runCatching { refreshTypes() }
                        Diag.log("screen: foreground projection service refused (${it.javaClass.simpleName})")
                    }.isSuccess
            }
            // The voice thread can create MediaProjection only after this ack.
            projectionReply?.send(if (ready) 1 else 0, null)
        }
        when (intent?.action) {
            ACTION_MUTE -> if (direct) {
                core.direct.setMuted(!core.direct.state.value.muted)
            } else {
                val voice = core.voice.state.value
                if (!voice.deafened && (!voice.muted || Build.VERSION.SDK_INT < 29 || foregroundMicrophone)) {
                    core.voice.setMuted(!voice.muted)
                }
            }
            ACTION_STOP_SCREEN -> if (direct) core.direct.setScreen(false) else core.voice.setScreen(false)
            ACTION_LEAVE -> if (direct) core.direct.hangup() else core.voice.leave()
        }
        if (intent?.action == ACTION_MICROPHONE &&
            intent.getStringExtra("room") == core.voice.state.value.channelId &&
            (Build.VERSION.SDK_INT < 29 || foregroundMicrophone)
        ) {
            // Recording starts only after the foreground service has its microphone type.
            core.voice.setMuted(false)
        }
        // Not restarted by the system after being killed: the call it was
        // keeping died with the process, and the server has let go of it.
        return START_NOT_STICKY
    }

    @Suppress("DEPRECATION")
    private fun projectionReply(intent: Intent): ResultReceiver? =
        if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra("reply", ResultReceiver::class.java)
        else intent.getParcelableExtra("reply")

    private fun projectionType(): Int =
        (if (projectionTurn != null) ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION else 0) or
            (if (cameraTyped && Build.VERSION.SDK_INT >= 30) ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA else 0)

    /**
     * The camera came on or went off. Taking the type can only happen with the
     * app on screen — which is when the camera is turned on — and if Android
     * refuses, the camera simply pauses in the background as before.
     */
    private fun cameraChanged(on: Boolean) {
        if (on == cameraTyped || Build.VERSION.SDK_INT < 30) return
        cameraTyped = on
        runCatching { refreshTypes() }
            .onSuccess { cameraInBackground = on; Diag.log(if (on) "vídeo: serviço com câmera; ela segue com a tela desligada" else "vídeo: serviço sem câmera") }
            .onFailure {
                cameraTyped = false
                cameraInBackground = false
                runCatching { refreshTypes() }
                Diag.log("vídeo: serviço recusou a câmera (${it.javaClass.simpleName}); ela pausa fora da tela")
            }
    }

    /** Preserve the call's microphone/playback type while adding/removing capture. */
    private fun refreshTypes() {
        val core = (application as HarmonyApp).core
        val n = render(What.of(core.voice.state.value, "", core.direct.state.value))
        if (Build.VERSION.SDK_INT >= 29) {
            val base = if (foregroundMicrophone) ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
                else ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            startForeground(ID, n, base or projectionType())
        } else startForeground(ID, n)
    }

    private fun finishProjection(turn: Int, direct: Boolean) {
        // Cleanup from an old permission reply must not stop a newer capture.
        if (projectionTurn != turn || projectionDirect != direct) return
        projectionTurn = null
        runCatching { refreshTypes() }
            .onFailure { Diag.log("screen: foreground type reset failed (${it.javaClass.simpleName})") }
    }

    private fun goForeground(n: Notification): Boolean = try {
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(ID, n, if (microphoneAllowed(this)) ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
                else ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
            foregroundMicrophone = microphoneAllowed(this)
        } else {
            startForeground(ID, n)
        }
        // The processor, and only it, only during a call: the screen stays
        // off. Audio is paced by the sound card, but the network and the
        // encoder in between are ordinary threads.
        wake = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "harmony:call")
            .apply {
                setReferenceCounted(false)
                acquire(MAX_WAKE_MS)
            }
        Diag.log("serviço da chamada ligado")
        true
    } catch (e: Exception) {
        // Android 12 and later refuse a foreground service started from the
        // background. The call still goes on while the app is open.
        Diag.log("serviço da chamada recusado: ${e.javaClass.simpleName}")
        false
    }

    private fun follow() {
        val core = (application as HarmonyApp).core
        scope.launch {
            combine(core.voice.state, core.rooms, core.direct.state) { v, r, d ->
                val name = r.voiceRooms.firstOrNull { it.channel.id == v.channelId }?.channel?.name.orEmpty()
                What.of(v, name, d)
            }
                .distinctUntilChanged()
                .collect { what ->
                    if (!what.inCall) {
                        stopSelf()
                        return@collect
                    }
                    cameraChanged(what.camera)
                    getSystemService(NotificationManager::class.java).notify(ID, render(what))
                }
        }
    }

    /** What the notification says: a room, or a direct call, and the microphone. */
    private data class What(val inCall: Boolean, val direct: Boolean, val title: String, val who: String, val muted: Boolean, val screen: Boolean = false,
                              val camera: Boolean = false) {
        companion object {
            fun of(v: VoiceCall.State, room: String, d: DirectCall.State): What {
                if (d.active) {
                    val name = d.peer?.name?.ifEmpty { null } ?: "alguém"
                    val title = when (d.phase) {
                        DirectCall.Phase.CALLING -> "Chamando $name…"
                        DirectCall.Phase.CONNECTING -> "Conectando com $name…"
                        DirectCall.Phase.RECONNECTING -> "Reconectando…"
                        else -> "Em chamada com $name"
                    }
                    return What(true, true, title, name, d.muted, d.screen, d.camera)
                }
                val title = when (v.phase) {
                    VoiceCall.Phase.LIVE -> if (room.isEmpty()) "Na sala" else "Na sala $room"
                    VoiceCall.Phase.RECONNECTING -> "Reconectando…"
                    else -> "Entrando na sala…"
                }
                return What(v.channelId != null, false, title, room.ifEmpty { "harmony" }, v.muted, v.screen, v.camera)
            }
        }
    }

    private fun render(w: What): Notification {
        val nm = getSystemService(NotificationManager::class.java)
        if (nm.getNotificationChannel(CHANNEL) == null) {
            // Low: it appears in the shade without a sound or a pop-up, since
            // the person has just walked into the call themselves.
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL, "Chamada em andamento", NotificationManager.IMPORTANCE_LOW),
            )
        }
        // A direct call opens on its own screen, a room on the app.
        val target = if (w.direct) CallActivity::class.java else MainActivity::class.java
        val open = PendingIntent.getActivity(
            this, if (w.direct) 1 else 0, Intent(this, target).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val b = Notification.Builder(this, CHANNEL)
        // Shown at once, not after the ten seconds Android 12 may hold a
        // service's notification back: a microphone that is on must say so.
        if (Build.VERSION.SDK_INT >= 31) b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE)
        b.setSmallIcon(if (w.direct) R.drawable.notif_chamada_direta else R.drawable.notif_chamado_sala)
            .setContentTitle(w.title)
            .setContentText((if (w.screen) "Compartilhando tela · " else "") + (if (w.camera) "Câmera ligada · " else "") + if (w.muted) "Microfone desligado" else "Microfone ligado")
            .setContentIntent(open)
            .setOngoing(true)
            .setCategory(Notification.CATEGORY_CALL)
        val mute = Notification.Action.Builder(null, if (w.muted) "Ligar microfone" else "Mudo", self(ACTION_MUTE)).build()
        if (Build.VERSION.SDK_INT >= 31) {
            // The call style: Android keeps it at the top of the shade with
            // its buttons showing, where a plain low-importance notification
            // sat folded under "Silenciosas" and hid "Mudo" and "Sair" behind
            // a tap (seen on the emulator, 25/09/2026). Allowed without the
            // phone's call system because a foreground service carries it.
            val who = android.app.Person.Builder().setName(w.who).setImportant(true).build()
            b.setStyle(Notification.CallStyle.forOngoingCall(who, self(ACTION_LEAVE)))
            b.addAction(mute)
        } else {
            b.addAction(mute)
            b.addAction(Notification.Action.Builder(null, if (w.direct) "Desligar" else "Sair", self(ACTION_LEAVE)).build())
        }
        if (w.screen) b.addAction(Notification.Action.Builder(null, "Parar tela", self(ACTION_STOP_SCREEN)).build())
        return b.build()
    }

    private fun self(action: String) = PendingIntent.getService(
        this, action.hashCode(), Intent(this, CallService::class.java).setAction(action), PendingIntent.FLAG_IMMUTABLE,
    )

    override fun onDestroy() {
        if (running === this) running = null
        cameraInBackground = false
        if (projectionTurn != null) {
            val core = (application as HarmonyApp).core
            if (core.direct.state.value.active) core.direct.setScreen(false) else core.voice.setScreen(false)
        }
        projectionTurn = null
        scope.cancel()
        wake?.let { if (it.isHeld) it.release() }
        wake = null
        Diag.log("serviço da chamada desligado")
        super.onDestroy()
    }

    companion object {
        // Accessed only on Android's main thread; cleared when the service dies.
        private var running: CallService? = null
        /** The service holds the camera type: the room's camera may keep running out of sight. */
        @Volatile var cameraInBackground = false
            private set
        private const val ID = 1
        private const val CHANNEL = "call"
        private const val ACTION_MUTE = "mute"
        private const val ACTION_STOP_SCREEN = "stop-screen"
        private const val ACTION_LEAVE = "leave"
        private const val ACTION_PROJECTION = "com.taleshernandes.harmony.START_PROJECTION"
        // A limit on the lock even if the service is somehow never stopped.
        private const val MAX_WAKE_MS = 12 * 60 * 60 * 1000L

        fun start(context: Context) {
            try {
                context.startForegroundService(Intent(context, CallService::class.java))
            } catch (e: Exception) {
                Diag.log("serviço da chamada não pôde começar: ${e.javaClass.simpleName}")
            }
        }

        internal fun startProjection(context: Context, room: String, turn: Int, ready: (Boolean) -> Unit) {
            val reply = object : ResultReceiver(Handler(Looper.getMainLooper())) {
                override fun onReceiveResult(resultCode: Int, resultData: android.os.Bundle?) = ready(resultCode == 1)
            }
            try {
                context.startForegroundService(Intent(context, CallService::class.java)
                    .setAction(ACTION_PROJECTION).putExtra("room", room).putExtra("turn", turn).putExtra("reply", reply))
            } catch (e: Exception) {
                Diag.log("screen: foreground service unavailable (${e.javaClass.simpleName})")
                ready(false)
            }
        }

        internal fun endProjection(turn: Int, direct: Boolean = false) {
            // Never start a service merely to remove its projection type.
            Handler(Looper.getMainLooper()).post { running?.finishProjection(turn, direct) }
        }

        private const val ACTION_MICROPHONE = "com.taleshernandes.harmony.START_MICROPHONE"

        fun startMicrophone(context: Context, room: String) {
            try {
                context.startForegroundService(Intent(context, CallService::class.java)
                    .setAction(ACTION_MICROPHONE).putExtra("room", room))
            } catch (e: Exception) {
                Diag.log("microphone service unavailable: ${e.javaClass.simpleName}")
            }
        }
    }
}
