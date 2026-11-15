package com.taleshernandes.harmony.ui

import android.Manifest
import android.app.KeyguardManager
import android.app.PictureInPictureParams
import android.content.Intent
import android.content.res.Configuration
import android.util.Rational
import com.taleshernandes.harmony.R
import android.content.pm.PackageManager
import android.graphics.Color as AndroidColor
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.taleshernandes.harmony.HarmonyApp
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.media.CallService
import com.taleshernandes.harmony.media.DirectCall
import com.taleshernandes.harmony.media.Output
import com.taleshernandes.harmony.media.VoiceCall
import com.taleshernandes.harmony.model.Member
import com.taleshernandes.harmony.model.Peer
import com.taleshernandes.harmony.model.User
import kotlinx.coroutines.delay

/**
 * The direct call's own screen: ringing, and the call itself.
 *
 * It opens over the lock screen, the way a telephone call does — from the
 * ringing notification's full-screen intent with the phone locked, or by
 * hand. Answering and talking need no unlocking; "Voltar ao app" asks for it,
 * because the rest of the app has no business over a locked phone.
 */
class CallActivity : ComponentActivity() {
    private val core get() = (application as HarmonyApp).core
    // A picture full size: the other's screen, or the camera tapped.
    private var fullScreen by mutableStateOf(false)
    private var fullCamera by mutableStateOf<String?>(null)
    // Leaving the app with the other's camera or screen on keeps it in a
    // floating window, as a watched live in a room (owner, 26/10/2026).
    private var floating by mutableStateOf(false)
    private var canFloat = false
    private val pipAction = object : android.content.BroadcastReceiver() {
        override fun onReceive(context: android.content.Context, intent: Intent) {
            val st = core.direct.state.value
            if (!floating || st.callId == null || st.callId != intent.getStringExtra("call")) return
            Diag.log("pip action: ${intent.action?.substringAfterLast('.')}")
            when (intent.action) {
                PIP_SOUND -> st.peer?.username?.let { core.direct.toggleScreenSound(it) }
                PIP_MIC -> if (!st.deafened) toggleMic()
                PIP_HANGUP -> core.direct.hangup()
            }
        }
    }
    private var proximity: PowerManager.WakeLock? = null

    private lateinit var permissions: PermissionFlow
    private var pendingUnmute = false

    private fun permissionComplete(kind: PermissionKind, listening: Boolean) {
        when (kind) {
            PermissionKind.MICROPHONE -> if (pendingUnmute) {
                pendingUnmute = false
                if (!listening) { CallService.start(this); core.direct.setMuted(false) }
            } else acceptCall()
            PermissionKind.CAMERA -> if (core.direct.state.value.active) core.direct.setCamera(true)
            else -> Unit
        }
    }

    private fun toggleMic() {
        if (!core.direct.state.value.muted) core.direct.setMuted(true)
        else { pendingUnmute = true; permissions.request(PermissionKind.MICROPHONE) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        permissions = PermissionFlow(this, ::permissionComplete)
        permissions.restore(savedInstanceState)
        pendingUnmute = savedInstanceState?.getBoolean("pending_unmute") ?: false
        // Over the lock screen, and lighting it: the telephone ringing.
        if (Build.VERSION.SDK_INT >= 27) {
            setShowWhenLocked(true)
            setTurnScreenOn(true)
        } else {
            @Suppress("DEPRECATION")
            window.addFlags(WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED or WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON)
        }
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(AndroidColor.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(AndroidColor.TRANSPARENT),
        )
        Diag.log("tela da chamada aberta${if (savedInstanceState != null) " (refeita)" else ""}")
        androidx.core.content.ContextCompat.registerReceiver(this, pipAction,
            android.content.IntentFilter().apply { addAction(PIP_SOUND); addAction(PIP_MIC); addAction(PIP_HANGUP) },
            androidx.core.content.ContextCompat.RECEIVER_NOT_EXPORTED)
        floating = isInPictureInPictureMode
        if (savedInstanceState == null) handle(intent)
        setContent {
            HarmonyTheme {
                val s by core.direct.state.collectAsState()
                LaunchedEffect(s.phase) {
                    if (s.phase == DirectCall.Phase.IDLE && !resuming) {
                        Diag.log("tela da chamada fechada: a chamada acabou")
                        s.notice?.let { Toast.makeText(this@CallActivity, it, Toast.LENGTH_LONG).show() }
                        core.direct.dismissNotice()
                        finish()
                    }
                }
                val video = s.peerScreen || s.peerCam || s.camera || s.screenChosen
                val user by core.screen.collectAsState()
                val me = (user as? com.taleshernandes.harmony.Screen.Rooms)?.user
                val chat by core.chat.state.collectAsState()
                val roleState by core.rooms.collectAsState()
                val call = DirectSeats(s, me, chat.members)
                var screenRequest by rememberSaveable { mutableStateOf<String?>(null) }
                var person by rememberSaveable { mutableStateOf(false) }
                val share = rememberScreenShare { projection ->
                    val requestedCall = screenRequest
                    screenRequest = null
                    if (projection == null) Diag.log("direct screen: capture consent cancelled")
                    else if (requestedCall == core.direct.state.value.callId && core.direct.state.value.active)
                        core.direct.setScreen(true, projection)
                    else Diag.log("direct screen: obsolete capture consent ignored")
                }
                LaunchedEffect(s.phase, s.output, video) { hold(s, video) }
                val reconnecting = s.phase == DirectCall.Phase.RECONNECTING || s.phase == DirectCall.Phase.CONNECTING
                val full = when {
                    !fullScreen -> null
                    fullCamera == null -> call.screen
                    else -> call.cameras.firstOrNull { it.mid == fullCamera }
                }
                // The other's picture, the screen first: what floats on leaving.
                val floatPicture = call.screen ?: call.cameras.firstOrNull { it.mid == DirectCall.PEER_CAM }
                val revision by core.direct.volumeRevision.collectAsState()
                val mayFloat = floatPicture != null && !person && permissions.kind == null &&
                    (s.phase == DirectCall.Phase.LIVE || s.phase == DirectCall.Phase.RECONNECTING)
                LaunchedEffect(mayFloat, floatPicture?.mid, s.muted, s.deafened, revision, floating) { floatWhenLeaving(mayFloat) }
                if (floating) {
                    FullPicture(core.direct, floatPicture, reconnecting, floating = true, onExit = {},
                        muted = s.muted, deafened = s.deafened, onMute = ::toggleMic, onLeave = core.direct::hangup,
                        leaveLabel = "desligar", nameOf = { call.peer.displayName }, lastName = call.peer.displayName,
                        ended = { "A imagem de $it acabou" }, stillHere = "A chamada continua")
                } else if (fullScreen && (full != null || reconnecting)) {
                    // The room's full screen, the same pinch, controls and live volume.
                    FullPicture(core.direct, full, reconnecting, floating = false, onExit = { setFull(false) },
                        muted = s.muted, deafened = s.deafened, onMute = ::toggleMic, onLeave = core.direct::hangup,
                        leaveLabel = "desligar", nameOf = { if (it.mid == DirectCall.LOCAL) "Você" else call.peer.displayName },
                        lastName = call.peer.displayName)
                } else {
                    if (fullScreen) LaunchedEffect(Unit) { setFull(false) }
                    CallScreen(
                        s = s,
                        call = call,
                        pictures = core.direct,
                        onDeafen = { core.direct.setDeafened(!s.deafened) },
                        onScreen = {
                            if (s.screenChosen) core.direct.setScreen(false)
                            else { screenRequest = s.callId; share() }
                        },
                        onAnswer = ::answer,
                        onDecline = { s.callId?.let { core.direct.decline(it) } },
                        onHangup = core.direct::hangup,
                        onMute = ::toggleMic,
                        onCamera = ::camera,
                        onOutput = core.direct::chooseOutput,
                        onFull = { fullCamera = null; setFull(true) },
                        onFullCamera = { fullCamera = it; setFull(true) },
                        onPerson = { if (it == call.peer.username && s.phase != DirectCall.Phase.RINGING) person = true },
                        onApp = ::openApp,
                    )
                }
                if (person && !fullScreen && !floating) {
                    val username = call.peer.username
                    val revision by core.direct.volumeRevision.collectAsState()
                    val member = chat.members[username] ?: Member(username, s.peer?.displayName.orEmpty().ifEmpty { username }, false, null)
                    // The room's profile, with the volumes; what would leave the
                    // call (a message, a room) is the app's, behind the lock.
                    val voiceLevel = remember(revision, username) { core.direct.volumes.get(username, false) }
                    val screenLevel = remember(revision, username) { core.direct.volumes.get(username, true) }
                    PersonSheet(
                        person = member,
                        me = me,
                        roles = roleState.roles,
                        isMe = false,
                        state = "in_call",
                        api = core::currentApi,
                        onNickname = { core.chat.nicknameSaved(member, it) },
                        inMyRoom = true,
                        sharing = s.peerScreen,
                        roomName = "chamada com você",
                        voiceVolume = voiceLevel,
                        screenVolume = screenLevel,
                        onVolume = { screen, level -> core.direct.setVolume(username, screen, level) },
                        onClose = { person = false },
                    )
                }
                permissions.Content()
            }
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        permissions.save(outState)
        outState.putBoolean("pending_unmute", pendingUnmute)
    }

    override fun onResume() {
        super.onResume()
        permissions.resumed()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handle(intent)
    }

    // Looking for the call the notification was about; not closed meanwhile.
    private var resuming = false

    private fun handle(i: Intent?) {
        val id = i?.getStringExtra(EXTRA_CALL)
        val answering = i?.action == ACTION_ANSWER
        if (id == null || core.direct.state.value.phase != DirectCall.Phase.IDLE) {
            if (answering) answer()
            return
        }
        // The ringing notification outlived the process: this app knows
        // of no call, and the server is asked (audit, 26/09/2026).
        resuming = true
        core.resumeCall(id) { ringing ->
            runOnUiThread {
                resuming = false
                when {
                    !ringing -> {
                        Diag.log("tela da chamada fechada: a chamada já tinha acabado")
                        Toast.makeText(this, "A chamada já acabou.", Toast.LENGTH_LONG).show()
                        finish()
                    }
                    answering -> answer()
                }
            }
        }
    }

    override fun onStart() {
        super.onStart()
        core.direct.setLooking(true)
        // Opened for a call that ended meanwhile (an old notification).
        if (core.direct.state.value.phase == DirectCall.Phase.IDLE && !resuming) {
            Diag.log("tela da chamada fechada: não havia chamada")
            finish()
        }
    }

    override fun onStop() {
        core.direct.setLooking(false)
        super.onStop()
    }

    override fun onDestroy() {
        unregisterReceiver(pipAction)
        release()
        super.onDestroy()
    }

    private fun answer() {
        if (core.direct.state.value.phase != DirectCall.Phase.RINGING) return
        pendingUnmute = false
        permissions.request(PermissionKind.MICROPHONE)
    }

    private fun acceptCall() {
        if (core.direct.state.value.phase != DirectCall.Phase.RINGING) return
        core.direct.accept()
        // Now, while this screen is in front: Android starts a microphone
        // service only for an app the person can see (over the lock screen
        // counts).
        CallService.start(this)
    }

    private fun camera() {
        val on = !core.direct.state.value.camera
        if (!on || checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            core.direct.setCamera(on)
        } else {
            permissions.request(PermissionKind.CAMERA)
        }
    }

    /** The rest of the app, unlocking first if the phone is locked. */
    private fun openApp() {
        val go = { startActivity(Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)) }
        val kg = getSystemService(KeyguardManager::class.java)
        if (kg != null && kg.isKeyguardLocked && Build.VERSION.SDK_INT >= 26) {
            kg.requestDismissKeyguard(this, object : KeyguardManager.KeyguardDismissCallback() {
                override fun onDismissSucceeded() = go()
            })
        } else {
            go()
        }
    }

    /**
     * Held to the ear with no picture: the screen goes dark against the
     * cheek (so the cheek does not press Desligar), like a telephone. A
     * picture keeps the screen on instead.
     */
    private fun hold(s: DirectCall.State, video: Boolean) {
        val talking = s.phase == DirectCall.Phase.LIVE || s.phase == DirectCall.Phase.CONNECTING ||
            s.phase == DirectCall.Phase.RECONNECTING || s.phase == DirectCall.Phase.CALLING
        if (talking && video) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val ear = talking && !video && s.output == Output.EARPIECE
        if (ear && proximity == null) {
            val pm = getSystemService(PowerManager::class.java)
            if (pm?.isWakeLockLevelSupported(PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK) == true) {
                proximity = pm.newWakeLock(PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK, "harmony:ear").apply {
                    setReferenceCounted(false)
                    acquire(MAX_EAR_MS)
                }
            }
        } else if (!ear) {
            release()
        }
    }

    private fun release() {
        proximity?.let { if (it.isHeld) it.release(PowerManager.RELEASE_FLAG_WAIT_FOR_NO_PROXIMITY) }
        proximity = null
    }

    private fun setFull(on: Boolean) {
        Diag.log(if (on) "chamada: tela cheia" else "chamada: saiu da tela cheia")
        fullScreen = on
        if (!on) fullCamera = null
        fullScreenWindow(on)
    }

    // ------------------------------------------------------------ floating window

    /** The room's floating window: the screen's sound when there is one, the microphone, and hanging up. */
    private fun pipParams(): PictureInPictureParams {
        val st = core.direct.state.value
        val speaking = !st.muted && !st.deafened
        val silent = st.peer?.let { core.direct.volumes.get(it.username, true) == 0f } ?: false
        fun action(id: String, icon: Int, label: String) = pipButton(id, icon, label) { putExtra("call", st.callId) }
        val actions = listOfNotNull(
            if (st.peerScreen) action(PIP_SOUND, if (silent) R.drawable.pip_volume_mudo else R.drawable.pip_volume,
                if (silent) "Ligar som da tela" else "Silenciar som da tela") else null,
            action(PIP_MIC, if (!speaking) R.drawable.pip_microfone_mudo else R.drawable.pip_microfone,
                if (!speaking) "Ligar microfone" else "Desligar microfone").apply {
                    isEnabled = !st.deafened && (speaking || permissions.has(PermissionKind.MICROPHONE))
                },
            action(PIP_HANGUP, R.drawable.pip_sair_da_sala, "Desligar"),
        )
        return PictureInPictureParams.Builder()
            .setAspectRatio(Rational(16, 9))
            .setActions(actions)
            .apply { if (Build.VERSION.SDK_INT >= 31) { setAutoEnterEnabled(canFloat); setSeamlessResizeEnabled(true) } }
            .build()
    }

    private fun floatWhenLeaving(on: Boolean) {
        canFloat = on
        // Android 12 and later float by themselves on the way out, when told in advance.
        if (Build.VERSION.SDK_INT >= 26) runCatching { setPictureInPictureParams(pipParams()) }
    }

    override fun onUserLeaveHint() {
        super.onUserLeaveHint()
        // Before Android 12 the app has to ask at the moment of leaving.
        if (Build.VERSION.SDK_INT < 31 && canFloat) runCatching { enterPictureInPictureMode(pipParams()) }
    }

    override fun onPictureInPictureModeChanged(isInPictureInPictureMode: Boolean, newConfig: Configuration) {
        super.onPictureInPictureModeChanged(isInPictureInPictureMode, newConfig)
        floating = isInPictureInPictureMode
        Diag.log(if (isInPictureInPictureMode) "chamada: quadro flutuante" else "chamada: de volta do quadro flutuante")
    }

    companion object {
        const val ACTION_ANSWER = "com.taleshernandes.harmony.ANSWER"
        const val EXTRA_CALL = "harmony.call"
        private const val PIP_SOUND = "com.taleshernandes.harmony.CALL_PIP_SOUND"
        private const val PIP_MIC = "com.taleshernandes.harmony.CALL_PIP_MIC"
        private const val PIP_HANGUP = "com.taleshernandes.harmony.CALL_PIP_HANGUP"
        private const val MAX_EAR_MS = 4 * 60 * 60 * 1000L
    }
}

/**
 * The direct call said the way the room's pieces read a room: two seats, the
 * cameras on (the other's first) and the other's screen. A shared screen
 * of one's own is not staged, as in a room.
 */
private class DirectSeats(s: DirectCall.State, me: User?, members: Map<String, Member>) {
    private val username = s.peer?.username.orEmpty()
    private val name = s.peer?.let { nameOf(it.username, it.displayName, members) }?.ifEmpty { null } ?: "Alguém"
    val peer = Peer(DirectCall.PEER_CAM, username, name, s.peerMuted, false, 0,
        listOfNotNull("cam".takeIf { s.peerCam }, "screen".takeIf { s.peerScreen }))
    val own = Peer(DirectCall.LOCAL, me?.username.orEmpty(), me?.displayName?.ifEmpty { null } ?: "Você", s.muted, s.deafened, 0,
        listOfNotNull("cam".takeIf { s.camera }, "screen".takeIf { s.screen }))
    val peers = listOf(peer, own)
    private val peerCam = VoiceCall.Video(DirectCall.PEER_CAM, username, name, "cam").takeIf { s.peerCam }
    private val ownCam = VoiceCall.Video(DirectCall.LOCAL, own.username, "Você", "cam").takeIf { s.camera }
    val cameras = listOfNotNull(peerCam, ownCam)
    val screen = VoiceCall.Video(DirectCall.PEER_SCREEN, username, name, "screen")
        .takeIf { s.peerScreen && s.phase != DirectCall.Phase.RINGING }
    fun camOf(p: Peer) = if (p.sessionId == DirectCall.LOCAL) ownCam else peerCam
}

@Composable
private fun CallScreen(
    s: DirectCall.State,
    call: DirectSeats,
    pictures: DirectCall,
    onDeafen: () -> Unit,
    onScreen: () -> Unit,
    onAnswer: () -> Unit,
    onDecline: () -> Unit,
    onHangup: () -> Unit,
    onMute: () -> Unit,
    onCamera: () -> Unit,
    onOutput: (Output) -> Unit,
    onFull: () -> Unit,
    onFullCamera: (String) -> Unit,
    onPerson: (String) -> Unit,
    onApp: () -> Unit,
) {
    var outputs by remember { mutableStateOf(false) }
    val ringing = s.phase == DirectCall.Phase.RINGING
    BoxWithConstraints(Modifier.fillMaxSize().background(Background).safeDrawingPadding()) {
        val landscape = maxWidth > maxHeight
        Column(Modifier.fillMaxSize()) {
            // Where a room has its name: the person, and what the call is doing.
            Row(Modifier.fillMaxWidth().padding(start = 12.dp, end = 12.dp, top = 8.dp),
                verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Column(Modifier.weight(1f)) {
                    Text(call.peer.displayName, style = TitleText, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text(status(s), style = ItemText, color = Muted)
                }
                if (!ringing) WebButton("Voltar ao app", onApp, icon = Glyphs.ChevronLeft)
            }
            CallStage(
                stage = pictures,
                landscape = landscape,
                peers = call.peers,
                mySession = DirectCall.LOCAL,
                talking = { it.sessionId in s.speaking && !it.muted && !it.deafened },
                nameFor = { it.displayName },
                camOf = call::camOf,
                cameras = call.cameras,
                screen = call.screen,
                screenName = { call.peer.displayName },
                onPerson = onPerson,
                onFull = onFull,
                onFullCamera = onFullCamera,
                notice = s.notice,
                onDismissNotice = pictures::dismissNotice,
                followSpeaker = false,
                controls = {
                    CallControls(s.muted, s.deafened, s.camera, s.screenChosen, s.output, s.outputs,
                        onMute, onDeafen, onCamera, onScreen, { outputs = true }, onHangup, "desligar",
                        screenEnabled = s.phase == DirectCall.Phase.LIVE)
                },
                bar = if (ringing) ({
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
                        RoundButton("Recusar", Danger, Glyphs.HandsetArrowOut, onDecline)
                        RoundButton("Atender", Accent, Glyphs.HandsetArrowIn, onAnswer)
                    }
                }) else null,
            )
        }
    }
    if (outputs) OutputSheet(s.output, s.outputs, onOutput) { outputs = false }
}

/** What the call is doing, in words — and the clock once it is on. */
@Composable
private fun status(s: DirectCall.State): String {
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(s.since) {
        while (s.since > 0) {
            now = System.currentTimeMillis()
            delay(1000)
        }
    }
    return when (s.phase) {
        DirectCall.Phase.RINGING -> "está te ligando pelo harmony"
        DirectCall.Phase.CALLING -> "Chamando…"
        DirectCall.Phase.CONNECTING -> "Ligando o áudio…"
        DirectCall.Phase.RECONNECTING -> "Reconectando…"
        DirectCall.Phase.LIVE -> duration(now - s.since)
        DirectCall.Phase.IDLE -> ""
    }
}

@Composable
private fun RoundButton(label: String, color: Color, icon: androidx.compose.ui.graphics.vector.ImageVector, onClick: () -> Unit) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Box(
            Modifier.size(72.dp).clip(CircleShape).background(color).clickable(role = Role.Button, onClick = onClick)
                .semantics { contentDescription = label },
            contentAlignment = Alignment.Center,
        ) { Glyph(icon, null, size = 24) }
        Spacer(Modifier.height(8.dp))
        Text(label, style = PersonText)
    }
}
