package com.taleshernandes.harmony.ui

import android.Manifest
import android.app.PictureInPictureParams
import android.content.Intent
import android.content.res.Configuration
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.util.Rational
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import android.provider.OpenableColumns
import com.taleshernandes.harmony.Conversations
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.taleshernandes.harmony.HarmonyApp
import com.taleshernandes.harmony.BuildConfig
import com.taleshernandes.harmony.model.Member
import com.taleshernandes.harmony.Screen
import com.taleshernandes.harmony.update.Updater
import com.taleshernandes.harmony.diag.Crash
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.push.Notices
import com.taleshernandes.harmony.media.DirectCall
import com.taleshernandes.harmony.media.Engine
import android.widget.Toast
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color as UiColor
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle

class MainActivity : ComponentActivity() {
    // The watched screen full size, lying down; and whether the app is the
    // floating window over another one.
    private var fullScreen by mutableStateOf(false)
    // The camera shown in full screen, by mid; null is the watched live.
    private var fullCamera by mutableStateOf<String?>(null)
    private var floating by mutableStateOf(false)
    // Whether there is a screen worth floating: leaving the app while
    // watching one keeps it in the corner (plan A4).
    private var canFloat = false

    private var pendingRoom: String? = null
    private var pendingCall: Pair<String, String>? = null
    private var pendingUnmute = false
    private lateinit var permissions: PermissionFlow
    private var showXiaomi by mutableStateOf(false)
    // The voice room whose screen is open; shown only while inside it.
    private var roomFor by mutableStateOf<String?>(null)
    private var pipPortrait = false
    private var pipName by mutableStateOf("alguém")
    private val pipAction = object : android.content.BroadcastReceiver() {
        override fun onReceive(context: android.content.Context, intent: Intent) {
            if (!floating) return
            val voice = (application as HarmonyApp).core.voice
            val state = voice.state.value
            if (state.channelId == null || state.channelId != intent.getStringExtra("room")) return
            if (intent.action in listOf(PIP_SOUND, PIP_MIC, PIP_LEAVE)) Diag.log("pip action: ${intent.action?.substringAfterLast('.')}")
            when (intent.action) {
                PIP_SOUND -> state.watching?.takeIf { it == intent.getStringExtra("watching") }?.let { voice.toggleScreenSound(it) }
                PIP_MIC -> if (!state.deafened) toggleMic()
                PIP_LEAVE -> { voice.leave(); finishAndRemoveTask() }
            }
        }
    }

    private fun permissionComplete(kind: PermissionKind, listening: Boolean) {
        val core = (application as HarmonyApp).core
        when (kind) {
            PermissionKind.MICROPHONE -> {
                val room = pendingRoom
                val call = pendingCall
                pendingRoom = null
                pendingCall = null
                when {
                    pendingUnmute -> {
                        pendingUnmute = false
                        if (!listening) {
                            core.voice.state.value.channelId?.let {
                                com.taleshernandes.harmony.media.CallService.startMicrophone(this, it)
                            }
                        }
                    }
                    room != null -> core.joinVoice(room)
                    call != null -> core.placeCall(call.first, call.second)
                }
            }
            PermissionKind.CAMERA -> if (core.voice.state.value.channelId != null) core.voice.setCamera(true)
            PermissionKind.NOTIFICATIONS -> maybeXiaomi()
            PermissionKind.INSTALL -> Unit
        }
    }

    private fun toggleMic() {
        val voice = (application as HarmonyApp).core.voice
        if (voice.state.value.deafened) return
        if (!voice.state.value.muted) voice.setMuted(true)
        else {
            pendingUnmute = true
            pendingRoom = null
            pendingCall = null
            permissions.request(PermissionKind.MICROPHONE)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        permissions = PermissionFlow(this, ::permissionComplete) { kind ->
            if (kind == PermissionKind.MICROPHONE) {
                pendingRoom = null
                pendingCall = null
                pendingUnmute = false
            }
        }
        permissions.restore(savedInstanceState)
        showXiaomi = savedInstanceState?.getBoolean("xiaomi_guide") ?: false
        pipName = savedInstanceState?.getString("pip_name") ?: "alguém"
        pipPortrait = savedInstanceState?.getBoolean("pip_portrait") ?: false
        roomFor = savedInstanceState?.getString(ROOM_FOR)
        pendingUnmute = savedInstanceState?.getBoolean("pending_unmute") ?: false
        pendingCall = savedInstanceState?.getString("pending_call_user")?.let { it to savedInstanceState.getString("pending_call_name", it) }
        androidx.core.content.ContextCompat.registerReceiver(this, pipAction,
            android.content.IntentFilter().apply { addAction(PIP_SOUND); addAction(PIP_MIC); addAction(PIP_LEAVE) },
            androidx.core.content.ContextCompat.RECEIVER_NOT_EXPORTED)
        // Made anew while already floating: the callback that says so came
        // to the old one.
        floating = isInPictureInPictureMode
        // Turning the phone while the permission question is up rebuilds
        // this screen; the room that was tapped survives it.
        pendingRoom = savedInstanceState?.getString(PENDING_ROOM)
        // The picker is another screen; this one may be rebuilt meanwhile.
        attachTo = savedInstanceState?.getString(ATTACH_TO)
        // Opened by tapping a notification. Not "only when the screen is new":
        // a process Android ended in the background comes back through here
        // with its saved state and the new tap in the intent (seen on the
        // emulator, 26/09/2026). Each tap carries its own mark instead, and a
        // screen rebuilt by turning the phone finds that mark already handled.
        handledTap = savedInstanceState?.getLong(HANDLED_TAP) ?: 0L
        noticeTapped(intent)
        // The app is always dark, so the bars' icons are always light, whatever
        // the phone's own theme is.
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
        )
        setContent {
            HarmonyTheme {
                val core = (application as HarmonyApp).core
                val d by core.direct.state.collectAsState()
                // A call ringing, or placed, with the app open: its own screen
                // comes up (with the app closed, the notification brings it).
                LaunchedEffect(d.phase) {
                    val ringing = d.phase == DirectCall.Phase.RINGING || d.phase == DirectCall.Phase.CALLING
                    if (ringing && lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) openCall()
                }
                // What a call left to say while nobody was looking at it:
                // "Chamada perdida de João".
                LaunchedEffect(d.phase, d.notice) {
                    val n = d.notice
                    if (d.phase == DirectCall.Phase.IDLE && n != null) {
                        Toast.makeText(this@MainActivity, n, Toast.LENGTH_LONG).show()
                        core.direct.dismissNotice()
                    }
                }
                val voiceNotice by core.voice.state.collectAsState()
                LaunchedEffect(voiceNotice.notice, voiceNotice.channelId, roomFor) {
                    val notice = voiceNotice.notice
                    if (notice != null && (voiceNotice.channelId == null || roomFor != voiceNotice.channelId)) {
                        Toast.makeText(this@MainActivity, notice, Toast.LENGTH_LONG).show()
                        core.voice.dismissNotice()
                    }
                }
                val onCall = d.active
                val update by core.updater.state.collectAsState()
                val signedIn = core.screen.collectAsState().value is Screen.Rooms
                // Not over a video in full screen or in the corner, and not
                // before there is an account to download with.
                val updating = !onCall && signedIn && !floating && !fullScreen && update !is Updater.State.Idle
                // An island in the layout under each screen's header: the
                // call going on, or else the update (design screen 7).
                var crashed by remember { mutableStateOf(Crash.pending(this@MainActivity)) }
                val banner: @Composable () -> Unit = {
                    val report = crashed
                    if (report != null) Strip(Glyphs.CircleExclamation, "O app fechou sozinho da última vez",
                        "Mande o registro para acharmos o defeito", danger = true, action = "Enviar",
                        onAction = { share(report, "harmony — o app fechou"); Crash.forget(this@MainActivity); crashed = null })
                    else if (onCall && !floating && !fullScreen) CallStrip(d, onOpen = ::openCall)
                    else if (updating) UpdateStrip(update, onInstall = core.updater::install, onCancel = core.updater::cancel)
                }
                val roleState by core.rooms.collectAsState()
                androidx.compose.runtime.CompositionLocalProvider(LocalRoles provides roleState) { Main(banner) }
                if (!floating) {
                    permissions.Content()
                    val permissionPrompt by core.updater.permissionPrompt.collectAsState()
                    permissionPrompt?.let { denied ->
                        PermissionSheet(PermissionKind.INSTALL, denied, core.updater::openInstallSettings, core.updater::deferPermission)
                    }
                }
            }
        }
    }

    @Composable
    private fun Main(banner: @Composable () -> Unit) {
        val core = (application as HarmonyApp).core
        val screen by core.screen.collectAsState()
        when (val s = screen) {
            Screen.Loading -> LoadingScreen()
            is Screen.SignIn -> SignInScreen(s, onSubmit = core::signIn, onShareDiag = ::shareDiag, onForgot = core::openForgotPassword)
            is Screen.SecondFactor -> SecondFactorScreen(s, core::secondFactor, core::secondFactorRecovery) { core.backToSignIn(s.server) }
            is Screen.ForgotPassword -> ForgotPasswordScreen(s, core::forgotPassword) { core.backToSignIn(s.server) }
            is Screen.VerifyEmail -> EmailVerificationScreen(s, core::currentApi, core::emailDone, core::skipEmail, core::signOut, core::refreshEmailGate)
            is Screen.Rooms -> {
                DisposableEffect(Unit) {
                    onDispose {
                        setFull(false)
                        floatWhenLeaving(false)
                        roomFor = null
                        core.setCovered(false)
                    }
                }
                LaunchedEffect(permissions.kind) { if (permissions.kind == null) askNotificationsOnce() }
                val joinAsked by core.joinAsked.collectAsState()
                // A room's notification tapped: joined once the account is
                // known, through the same door as a tap on the room.
                LaunchedEffect(joinAsked, s.user) {
                    val room = joinAsked ?: return@LaunchedEffect
                    if (s.user == null) return@LaunchedEffect
                    core.joinHandled()
                    if (core.voice.state.value.channelId != room) join(room)
                }
                val rooms by core.rooms.collectAsState()
                val live by core.live.collectAsState()
                val webrtc by core.webrtc.collectAsState()
                val voice by core.voice.state.collectAsState()
                var settings by rememberSaveable(s.user?.username) { mutableStateOf(false) }
                var personFor by rememberSaveable(s.user?.username) { mutableStateOf<String?>(null) }
                // The list panel over the conversation, and the people sheet.
                var drawer by rememberSaveable(s.user?.username) { mutableStateOf(false) }
                var people by rememberSaveable(s.user?.username) { mutableStateOf(false) }
                var searching by rememberSaveable(s.user?.username) { mutableStateOf(false) }
                val roomOpen = roomFor != null && roomFor == voice.channelId
                // Android asks every time: a grant to record the screen is good for one start only.
                val askScreen = rememberScreenShareOptions(voice.channelId) { projection, options ->
                    core.voice.setScreen(true, projection, options)
                }
                val screenOn = voice.videos.any { it.isScreen && it.username == voice.watching }
                val progress by core.voice.screenProgress.collectAsState()
                val volumeRevision by core.voice.volumeRevision.collectAsState()
                val video = voice.videos.firstOrNull { it.isScreen && it.username == voice.watching }
                val members by core.chat.state.collectAsState()
                LaunchedEffect(video, members.members) {
                    if (video != null) pipName = nameOf(video.username, video.displayName, members.members)
                }
                val mayFloat = screenOn && !settings && personFor == null && !showXiaomi && permissions.kind == null
                val portrait = video?.let { progress[it.mid]?.portrait }
                LaunchedEffect(mayFloat, portrait, voice.muted, voice.deafened, voice.watching, voice.channelId, volumeRevision, floating) {
                    portrait?.let { pipPortrait = it }
                    floatWhenLeaving(mayFloat)
                    if (floating && voice.channelId == null) finishAndRemoveTask()
                }
                val reconnecting = voice.phase == com.taleshernandes.harmony.media.VoiceCall.Phase.RECONNECTING ||
                    voice.phase == com.taleshernandes.harmony.media.VoiceCall.Phase.CONNECTING
                val cameraOn = fullCamera?.let { mid -> voice.videos.any { it.mid == mid } } == true
                LaunchedEffect(screenOn, cameraOn, fullScreen, reconnecting) {
                    if (!screenOn && !cameraOn && fullScreen && !reconnecting) setFull(false)
                }
                val covered = floating || (fullScreen && (screenOn || cameraOn || reconnecting)) || settings || personFor != null || showXiaomi ||
                    roomOpen || drawer || people || searching
                LaunchedEffect(covered) { core.setCovered(covered) }
                if (floating || (fullScreen && (screenOn || cameraOn || reconnecting))) {
                    FullVideo(core.voice, voice, floating = floating, onExit = { setFull(false) },
                        onMute = ::toggleMic, members = members.members, lastName = pipName,
                        cameraMid = fullCamera.takeIf { cameraOn && !floating })
                    return
                }
                if (showXiaomi) {
                    XiaomiGuide(onClose = { showXiaomi = false }, context = this@MainActivity)
                    return
                }
                val chat by core.chat.state.collectAsState()
                val direct by core.direct.state.collectAsState()
                val openDm = { username: String ->
                    people = false
                    drawer = false
                    core.chat.openDm(username) { ch ->
                        // Known before the socket says so, or the screen
                        // would not find it to open.
                        core.knowChannel(ch)
                        runOnUiThread {
                            roomFor = null
                            core.chat.open(ch.id, ch.lastReadId)
                        }
                    }
                }
                val open = chat.open?.let { id -> rooms.channels.firstOrNull { it.id == id } }
                // Open the last available conversation, or #geral. UI preferences
                // belong to this account on this device, never to the network layer.
                val uiPrefs = remember { getSharedPreferences("ui", MODE_PRIVATE) }
                val lastKey = "conversation:${s.user?.username.orEmpty()}"
                LaunchedEffect(s.user?.username, rooms.channels, chat.open) {
                    if (s.user == null) return@LaunchedEffect
                    if (chat.open != null && open != null) {
                        uiPrefs.edit().putString(lastKey, open.id).apply()
                    } else {
                        val last = uiPrefs.getString(lastKey, null)
                        val next = rooms.channels.firstOrNull { it.id == last && !it.isVoice }
                            ?: rooms.textChannels.firstOrNull { it.name == "geral" }
                            ?: rooms.textChannels.firstOrNull()
                        if (next != null) core.chat.open(next.id, next.lastReadId)
                    }
                }
                val home = HomeChat(
                    texts = rooms.textChannels,
                    dms = rooms.dms,
                    members = chat.members,
                    presence = chat.presence,
                    onOpen = { c -> drawer = false; roomFor = null; core.chat.open(c.id, c.lastReadId) },
                )
                var managedChannel by remember { mutableStateOf<com.taleshernandes.harmony.model.Channel?>(null) }
                var managedCategory by remember { mutableStateOf<com.taleshernandes.harmony.model.Category?>(null) }
                var organizing by remember { mutableStateOf(false) }
                ChannelTools(rooms, core::currentApi, core::refreshChannels, managedChannel, managedCategory, organizing,
                    { managedChannel = null; managedCategory = null; organizing = false })
                val list = @Composable { m: Modifier, withBanner: Boolean ->
                    ListPanel(
                        channelState = rooms,
                        onManageChannel = { managedChannel = it },
                        onManageCategory = { managedCategory = it },
                        onOrganize = { organizing = true },
                        user = s.user,
                        chat = home,
                        openId = open?.id,
                        rooms = rooms.voiceRooms,
                        live = live,
                        voice = voice,
                        onRoom = { r ->
                            drawer = false
                            // Your own room opens; another is joined, and opens once in.
                            if (r.channel.id == voice.channelId) roomFor = r.channel.id else join(r.channel.id)
                        },
                        onPerson = { personFor = it },
                        onSettings = { drawer = false; settings = true },
                        modifier = m,
                        banner = if (withBanner) banner else ({}),
                    )
                }
                Box(Modifier.fillMaxSize()) {
                    when {
                        roomOpen -> RoomScreen(
                            room = rooms.voiceRooms.firstOrNull { it.channel.id == voice.channelId },
                            voice = core.voice,
                            state = voice,
                            members = chat.members,
                            onBack = { roomFor = null },
                            onList = { drawer = true },
                            onSearch = { searching = true },
                            onPeople = { people = !people },
                            peopleOpen = people,
                            onPerson = { personFor = it },
                            onMute = ::toggleMic,
                            onDeafen = { core.voice.setDeafened(!voice.deafened) },
                            onCamera = { camera(!voice.cameraChosen) },
                            onScreen = { if (voice.screenChosen) core.voice.setScreen(false) else askScreen() },
                            onOutput = core.voice::chooseOutput,
                            onLeave = core.voice::leave,
                            onFull = { fullCamera = null; setFull(true) },
                            onFullCamera = { mid -> fullCamera = mid; setFull(true) },
                            onDismissNotice = core.voice::dismissNotice,
                            banner = banner,
                        )
                        open != null && s.user != null -> Box(Modifier.fillMaxSize()) {
                            ChatScreen(
                                onPin = core.chat::pin,
                                onJumpPin = { core.chat.open(open.id, open.lastReadId, it) },
                                channel = open,
                                me = s.user,
                                state = chat,
                                draft = core.chat.drafts.get(open.id),
                                api = core::currentApi,
                                actions = ChatActions(
                                    onBack = { finish() },
                                    onSend = { body, reply, done -> core.chat.send(open.id, body, reply) { ok -> runOnUiThread { done(ok) } } },
                                    onEdit = { id, body, done -> core.chat.edit(id, body) { ok -> runOnUiThread { done(ok) } } },
                                    onDelete = core.chat::delete,
                                    onReact = core.chat::react,
                                    onOlder = core.chat::loadOlder,
                                    onNewer = core.chat::loadNewer,
                                    onLatest = core.chat::latest,
                                    onPositioned = core.chat::positioned,
                                    onTyped = { core.chat.typed(open.id) },
                                    onDraft = { core.chat.drafts.set(open.id, it) },
                                    onAttach = {
                                        attachTo = open.id
                                        pickAttachments.launch(arrayOf("image/*", "video/*", "audio/*", "application/pdf", "text/plain", "application/zip", "application/ogg"))
                                    },
                                    onUnattach = core.chat::unattach,
                                    onDismissNotice = core.chat::dismissNotice,
                                    onPerson = { personFor = it },
                                    onCall = open.peer?.takeIf { open.isDm }?.let { p ->
                                        { call(p.username, nameOf(p.username, p.displayName, chat.members)) }
                                    },
                                ),
                                onList = { drawer = true },
                                onPeople = { people = !people },
                                peopleOpen = people,
                                onSearch = { searching = true },
                                banner = banner,
                            )
                        }
                        // During the first channel fetch keep the same entry surface.
                        else -> LoadingScreen()
                    }
                    Drawer(drawer, onClose = { drawer = false }) { m -> list(m, false) }
                    if (people) {
                        PeopleSheet(chat.members.values.toList(), s.user?.username, chat.presence,
                            onPick = { people = false; personFor = it }, onDismiss = { people = false })
                    }
                    personFor?.let { username ->
                        val personRooms = rooms.voiceRooms.filter { r -> r.peers.any { it.username == username } }
                        val room = personRooms.firstOrNull { it.channel.id == voice.channelId } ?: personRooms.firstOrNull()
                        val peers = room?.peers.orEmpty().filter { it.username == username }
                        val author = open?.peer?.takeIf { it.username == username }
                            ?: chat.timelines.values.asSequence().flatMap { it.messages.asSequence() }
                                .firstOrNull { it.author.username == username }?.author
                            ?: rooms.dms.firstNotNullOfOrNull { it.peer?.takeIf { p -> p.username == username } }
                        val person = chat.members[username] ?: Member(username,
                            peers.firstOrNull()?.displayName ?: author?.displayName
                                ?: s.user?.takeIf { it.username == username }?.displayName ?: username, false, null)
                        val inMyRoom = room != null && room.channel.id == voice.channelId
                        val sharing = peers.any { it.sharingScreen }
                        PersonSheet(
                            person = person,
                            me = s.user,
                            isMe = username == s.user?.username,
                            state = if (room != null) "in_call" else chat.presence[username]
                                ?: if (username == s.user?.username) "online" else "offline",
                            api = core::currentApi,
                            onNickname = { core.chat.nicknameSaved(person, it) },
                            inMyRoom = inMyRoom,
                            sharing = sharing,
                            roomName = room?.channel?.name,
                            voiceVolume = core.voice.volumes.get(username, false),
                            screenVolume = core.voice.volumes.get(username, true),
                            onVolume = { screen, level -> core.voice.setVolume(username, screen, level) },
                            onClose = { personFor = null },
                            onMessage = { personFor = null; openDm(username) },
                            onCall = { personFor = null; call(username, nameOf(username, person.displayName, chat.members)) },
                            onJoinRoom = room?.takeUnless { inMyRoom }?.let { r ->
                                { personFor = null; join(r.channel.id) }
                            },
                            onWatch = if (inMyRoom && sharing) ({
                                personFor = null
                                if (voice.watching == username) core.voice.watch(null)
                                else {
                                    core.voice.watch(username)
                                    // The stage is in the room's screen.
                                    roomFor = voice.channelId
                                }
                            }) else null,
                            watching = voice.watching == username,
                        )
                    }
                    if (searching) SearchPanel(chat, rooms.channels, core::currentApi,
                        onOpen = { c, messageId ->
                            searching = false; drawer = false; roomFor = null
                            core.knowChannel(c)
                            core.chat.open(c.id, c.lastReadId, messageId)
                        },
                        onClose = { searching = false })
                    if (settings && s.user != null) {
                        val update by core.updater.state.collectAsState()
                        val lastChecked by core.updater.lastChecked.collectAsState()
                        val denoise = remember { Engine.denoise(applicationContext) }
                        var denoiseEnabled by remember { mutableStateOf(denoise.enabled) }
                        var denoiseLevel by remember { mutableStateOf(denoise.level) }
                        val denoiseStatus by denoise.state.collectAsState()
                        var echoDumpArmed by remember { mutableStateOf(com.taleshernandes.harmony.media.EchoProbe.armed(applicationContext)) }
                        SettingsScreen(
                            me = s.user,
                            roleMembers = chat.members.values.toList(),
                            onRoleSnapshot = core::roleSnapshot,
                            api = core::currentApi,
                            outputs = if (direct.active) direct.outputs else voice.outputs,
                            onSecurityUser = core::securityUser,
                            preferredOutput = if (direct.active) direct.output else voice.output,
                            onOutput = { if (direct.active) core.direct.chooseOutput(it) else core.voice.chooseOutput(it) },
                            denoiseEnabled = denoiseEnabled,
                            denoiseLevel = denoiseLevel,
                            denoiseStatus = denoiseStatus,
                            onDenoise = { denoise.configure(on = it); denoiseEnabled = denoise.enabled },
                            onDenoiseLevel = { denoise.configure(db = it); denoiseLevel = denoise.level },
                            videoEngine = webrtc,
                            appVersion = BuildConfig.VERSION_NAME,
                            update = update,
                            lastChecked = lastChecked,
                            onCheckUpdate = { core.updater.check("ajustes", force = true) },
                            onShareDiagnostics = ::shareDiag,
                            echoDumpArmed = echoDumpArmed,
                            onEchoDump = { com.taleshernandes.harmony.media.EchoProbe.arm(applicationContext, it); echoDumpArmed = it },
                            echoDumpReady = com.taleshernandes.harmony.media.EchoProbe.latest(applicationContext) != null,
                            onShareEchoDump = ::shareEchoDump,
                            onLogout = { core.signOut() },
                            onClose = { settings = false },
                            banner = banner,
                        )
                    }
                }
            }
        }
    }

    // The conversation the picked files are for: the picker is another
    // screen, and the answer comes back after it.
    private var attachTo: String? = null
    private val pickAttachments = registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        val channel = attachTo ?: return@registerForActivityResult
        val core = (application as HarmonyApp).core
        for (uri in uris) {
            val name = contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                if (c.moveToFirst()) c.getString(0) else null
            } ?: "arquivo"
            val mime = contentResolver.getType(uri) ?: "application/octet-stream"
            val size = contentResolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)?.use { c ->
                if (c.moveToFirst() && !c.isNull(0)) c.getLong(0) else -1L
            } ?: -1L
            core.chat.attach(channel, name, mime, size) { contentResolver.openInputStream(uri) }
        }
    }

    private fun camera(on: Boolean) {
        if (!on) (application as HarmonyApp).core.voice.setCamera(false)
        else permissions.request(PermissionKind.CAMERA)
    }

    /** Calls somebody from their conversation (A7). */
    private fun call(username: String, displayName: String) {
        val core = (application as HarmonyApp).core
        if (core.direct.state.value.phase != DirectCall.Phase.IDLE) {
            openCall()
            return
        }
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
            core.placeCall(username, displayName)
        } else {
            pendingCall = username to displayName
            pendingRoom = null
            pendingUnmute = false
            permissions.request(PermissionKind.MICROPHONE)
        }
    }

    private fun openCall() {
        startActivity(Intent(this, CallActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }

    private fun join(channelId: String) {
        roomFor = channelId
        pendingCall = null
        pendingUnmute = false
        pendingRoom = channelId
        permissions.request(PermissionKind.MICROPHONE)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        // What getIntent() answers from now on, so a screen rebuilt later
        // finds this tap and not the one that first opened it.
        setIntent(intent)
        noticeTapped(intent)
    }

    // The mark of the last notification tap acted on (Notices.EXTRA_TAP).
    private var handledTap = 0L

    private fun noticeTapped(i: Intent?) {
        val tap = i?.getLongExtra(Notices.EXTRA_TAP, 0L) ?: 0L
        if (tap == 0L || tap == handledTap) return
        handledTap = tap
        (application as HarmonyApp).core.noticeTapped(
            i?.getStringExtra(Notices.EXTRA_OPEN), i?.getStringExtra(Notices.EXTRA_JOIN),
        )
    }

    // Android 13 and later show no notification until the person allows it.
    // Asked once, on the first entry after signing in; the answer is theirs
    // to change later in Ajustes, and asking again at every opening would be
    // the app nagging.
    private fun askNotificationsOnce() {
        if (permissions.has(PermissionKind.NOTIFICATIONS)) { maybeXiaomi(); return }
        if ((application as HarmonyApp).core.joinAsked.value != null) return
        val prefs = getSharedPreferences("ui", MODE_PRIVATE)
        if (prefs.getBoolean(ASKED_NOTICES, false) || permissions.kind != null || pendingRoom != null || pendingCall != null) return
        prefs.edit().putBoolean(ASKED_NOTICES, true).apply()
        permissions.request(PermissionKind.NOTIFICATIONS)
    }

    private fun maybeXiaomi() {
        if (!isXiaomi() || !getSystemService(android.app.NotificationManager::class.java).areNotificationsEnabled()) return
        val prefs = getSharedPreferences("ui", MODE_PRIVATE)
        if (prefs.getBoolean("xiaomi_guide_seen", false)) return
        prefs.edit().putBoolean("xiaomi_guide_seen", true).apply()
        showXiaomi = true
    }

    override fun onResume() {
        super.onResume()
        permissions.resumed()
        (application as HarmonyApp).core.updater.resumeIfAllowed()
        if ((application as HarmonyApp).core.screen.value is Screen.Rooms) maybeXiaomi()
    }

    override fun onDestroy() {
        unregisterReceiver(pipAction)
        super.onDestroy()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString(PENDING_ROOM, pendingRoom)
        permissions.save(outState)
        outState.putBoolean("xiaomi_guide", showXiaomi)
        outState.putBoolean("pip_portrait", pipPortrait)
        outState.putString("pip_name", pipName)
        outState.putBoolean("pending_unmute", pendingUnmute)
        outState.putString("pending_call_user", pendingCall?.first)
        outState.putString("pending_call_name", pendingCall?.second)
        outState.putString(ATTACH_TO, attachTo)
        outState.putLong(HANDLED_TAP, handledTap)
        outState.putString(ROOM_FOR, roomFor)
    }

    // Whether anybody is looking at the rooms: the ring of who is talking is
    // only measured then.
    override fun onStart() {
        super.onStart()
        (application as HarmonyApp).core.voice.setWatching(true)
        (application as HarmonyApp).core.voice.setLooking(true)
        (application as HarmonyApp).core.setLooking(true)
    }

    override fun onStop() {
        (application as HarmonyApp).core.voice.setWatching(false)
        // Nobody can see a picture now; the smallest one is asked for
        // (Layers.kt), and the good one returns the moment the app does.
        (application as HarmonyApp).core.voice.setLooking(false)
        // What is unsent goes to storage now: Android may end the app in the
        // background without another word.
        (application as HarmonyApp).core.setLooking(false)
        super.onStop()
    }

    // ------------------------------------------------------------ full screen

    private fun setFull(on: Boolean) {
        Diag.log(if (on) "video: full screen entered" else "video: full screen left")
        fullScreen = on
        if (!on) fullCamera = null
        fullScreenWindow(on)
    }

    // ------------------------------------------------------------ floating window

    private fun pipParams(): PictureInPictureParams {
        val voice = (application as HarmonyApp).core.voice
        val state = voice.state.value
        val speaking = !state.muted && !state.deafened
        val silent = state.watching?.let { voice.volumes.get(it, true) == 0f } ?: false
        fun action(id: String, icon: Int, label: String) =
            pipButton(id, icon, label) { putExtra("room", state.channelId).putExtra("watching", state.watching) }
        return PictureInPictureParams.Builder()
            .setAspectRatio(if (pipPortrait) Rational(9, 16) else Rational(16, 9))
            .setActions(listOf(
                action(PIP_SOUND, if (silent) com.taleshernandes.harmony.R.drawable.pip_volume_mudo else com.taleshernandes.harmony.R.drawable.pip_volume,
                    if (silent) "Ligar som da live" else "Silenciar som da live").apply { isEnabled = state.watching != null },
                action(PIP_MIC, if (!speaking) com.taleshernandes.harmony.R.drawable.pip_microfone_mudo else com.taleshernandes.harmony.R.drawable.pip_microfone,
                    if (!speaking) "Ligar microfone" else "Desligar microfone").apply {
                        isEnabled = !state.deafened && (speaking || permissions.has(PermissionKind.MICROPHONE))
                    },
                action(PIP_LEAVE, com.taleshernandes.harmony.R.drawable.pip_sair_da_sala, "Sair da sala"),
            ))
            .apply { if (Build.VERSION.SDK_INT >= 31) { setAutoEnterEnabled(canFloat); setSeamlessResizeEnabled(true) } }
            .build()
    }

    private fun floatWhenLeaving(on: Boolean) {
        canFloat = on
        // Android 12 and later go into the floating window by themselves on
        // the way out — the smooth way, from the gesture — when told in advance.
        if (Build.VERSION.SDK_INT >= 26) {
            runCatching { setPictureInPictureParams(pipParams()) }
        }
    }

    override fun onUserLeaveHint() {
        super.onUserLeaveHint()
        // Before Android 12 the app has to ask at the moment of leaving.
        if (Build.VERSION.SDK_INT < 31 && canFloat) {
            runCatching { enterPictureInPictureMode(pipParams()) }
        }
    }

    override fun onPictureInPictureModeChanged(isInPictureInPictureMode: Boolean, newConfig: Configuration) {
        super.onPictureInPictureModeChanged(isInPictureInPictureMode, newConfig)
        floating = isInPictureInPictureMode
        Diag.log(if (isInPictureInPictureMode) "vídeo: quadro flutuante" else "vídeo: de volta do quadro flutuante")
    }

    /**
     * Hands the diagnostic log to whatever the person picks (WhatsApp, e-mail).
     * Capped well under the size an Intent can carry.
     */
    private fun shareDiag() {
        val core = (application as HarmonyApp).core
        share(Diag.dump(core.diagHeader()), "harmony — diagnóstico do Android")
    }

    /** The newest echo recording, as a file another app can read (Ajustes → Diagnóstico). */
    private fun shareEchoDump() {
        val file = com.taleshernandes.harmony.media.EchoProbe.latest(applicationContext)
        if (file == null) { Toast.makeText(this, "Nenhuma gravação pronta", Toast.LENGTH_SHORT).show(); return }
        val uri = androidx.core.content.FileProvider.getUriForFile(this, "$packageName.files", file)
        val send = Intent(Intent.ACTION_SEND)
            .setType("application/octet-stream")
            .putExtra(Intent.EXTRA_SUBJECT, "harmony — gravação do eco (${file.name})")
            .putExtra(Intent.EXTRA_STREAM, uri)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            // The share sheet reads the file too (its preview): the grant covers it only through ClipData.
            .apply { clipData = android.content.ClipData.newRawUri(file.name, uri) }
        Diag.log("eco: enviando ${file.name} (${file.length() / 1024} KB)")
        startActivity(Intent.createChooser(send, "Enviar a gravação do eco"))
    }

    private fun share(report: String, subject: String) {
        val text = if (report.length > MAX_SHARE) report.takeLast(MAX_SHARE) else report
        val send = Intent(Intent.ACTION_SEND)
            .setType("text/plain")
            .putExtra(Intent.EXTRA_SUBJECT, subject)
            .putExtra(Intent.EXTRA_TEXT, text)
        startActivity(Intent.createChooser(send, "Compartilhar diagnóstico"))
    }

    /** A direct call going on while the person uses the rest of the app: a tap goes back to it. */
    @Composable
    private fun CallStrip(d: DirectCall.State, onOpen: () -> Unit) {
        val name = d.peer?.name?.ifEmpty { null } ?: "alguém"
        Strip(
            Glyphs.Handset,
            if (d.phase == DirectCall.Phase.CALLING) "Chamando $name…" else "Em chamada com $name",
            "Toque para voltar à chamada",
            Modifier.semantics { contentDescription = "voltar à chamada" },
            action = "Voltar",
            onAction = onOpen,
            onClick = onOpen,
        )
    }

    private companion object {
        const val MAX_SHARE = 200_000
        const val PENDING_ROOM = "pending_room"
        const val PIP_SOUND = "com.taleshernandes.harmony.PIP_SOUND"
        const val PIP_MIC = "com.taleshernandes.harmony.PIP_MIC"
        const val PIP_LEAVE = "com.taleshernandes.harmony.PIP_LEAVE"
        const val ATTACH_TO = "attach_to"
        const val ASKED_NOTICES = "asked_notices"
        const val HANDLED_TAP = "handled_tap"
        const val ROOM_FOR = "room_for"
    }
}
