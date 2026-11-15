package com.taleshernandes.harmony.ui

import androidx.compose.runtime.collectAsState

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.taleshernandes.harmony.media.Output
import com.taleshernandes.harmony.media.Stage
import com.taleshernandes.harmony.media.VoiceCall
import com.taleshernandes.harmony.model.Member
import com.taleshernandes.harmony.model.Peer
import com.taleshernandes.harmony.model.VoiceRoom
import kotlinx.coroutines.delay

/** The words the sheet uses for where the sound goes (the design's, not the engine's). */
internal fun outputName(o: Output): String = when (o) {
    Output.BLUETOOTH -> "Fone Bluetooth"
    Output.WIRED -> "Fone com fio"
    Output.EARPIECE -> "Fone do aparelho"
    Output.SPEAKER -> "Alto-falante"
}

internal fun outputGlyph(o: Output?): ImageVector = when (o) {
    Output.SPEAKER -> Glyphs.Megaphone
    Output.EARPIECE -> Glyphs.Smartphone
    else -> Glyphs.Headphones
}

/** "Por onde sai o som": lines of 56, the chosen one in `selected` with the accent check. */
@Composable
internal fun OutputSheet(current: Output?, present: List<Output>, onPick: (Output) -> Unit, onDismiss: () -> Unit) {
    Sheet(onDismiss = onDismiss, title = "Por onde sai o som") {
        val close = LocalPanelDismiss.current
        Column(Modifier.fillMaxWidth()) {
            for (o in present) {
                val on = o == current
                SheetRow(outputName(o), { close { onPick(o); onDismiss() } }, icon = outputGlyph(o), iconTint = if (on) Foreground else Muted,
                    selected = on, trailing = if (on) ({ Glyph(Glyphs.Check, "escolhida", tint = Accent) }) else null)
            }
        }
    }
}

/** A button of the call bar: 48, `selected` behind; on is a white glyph, off a muted one. */
@Composable
@OptIn(ExperimentalFoundationApi::class)
internal fun CallButton(icon: ImageVector, description: String, on: Boolean, onClick: () -> Unit, enabled: Boolean = true,
                        background: Color = Selected, tint: Color = if (on) Foreground else Muted,
                        onOutput: (() -> Unit)? = null, slashed: Boolean = false) {
    Box(Modifier.size(48.dp).clip(Shape8).background(background)
        .combinedClickable(enabled = enabled, role = Role.Button, onClick = onClick, onLongClick = onOutput)
        .semantics {
            contentDescription = description
            stateDescription = if (on) "ligado" else "desligado"
            onOutput?.let { action -> customActions = listOf(CustomAccessibilityAction("Saída de som") { action(); true }) }
        }, contentAlignment = Alignment.Center) { if (slashed) Deafened(20, DangerSoftFg) else Glyph(icon, null, tint, 20) }
}

/**
 * The call bar's six buttons, a room's and a direct call's: microphone,
 * hearing (a long press picks where the sound goes), camera, screen, the
 * output, and leaving in red.
 */
@Composable
internal fun CallControls(muted: Boolean, deafened: Boolean, camera: Boolean, screen: Boolean, output: Output?, outputs: List<Output>,
                          onMute: () -> Unit, onDeafen: () -> Unit, onCamera: () -> Unit, onScreen: () -> Unit,
                          onOutputs: () -> Unit, onLeave: () -> Unit, leaveLabel: String, screenEnabled: Boolean = true) {
    val speaking = !muted && !deafened
    CallButton(if (speaking) Glyphs.Microphone else Glyphs.MicrophoneSlash,
        if (muted) "ligar o microfone" else "desligar o microfone", speaking, onMute,
        enabled = !deafened, background = if (speaking) Selected else DangerSoft,
        tint = if (speaking) Foreground else DangerSoftFg)
    CallButton(Glyphs.Headphones, if (deafened) "voltar a ouvir" else "não ouvir ninguém",
        !deafened, onDeafen, background = if (deafened) DangerSoft else Selected,
        tint = if (deafened) DangerSoftFg else Foreground,
        onOutput = if (outputs.size > 1) onOutputs else null, slashed = deafened)
    CallButton(Glyphs.Camera, if (camera) "sua câmera" else "ligar a câmera", camera, onCamera)
    CallButton(Glyphs.Display, if (screen) "parar a tela" else "compartilhar a tela", screen, onScreen,
        enabled = screenEnabled || screen)
    CallButton(outputGlyph(output), "saída de áudio", true, onOutputs, enabled = outputs.isNotEmpty())
    CallButton(Glyphs.HandsetArrowOut, leaveLabel, true, onLeave, background = Danger, tint = Foreground)
}

/** Hours only when there are hours: 12:04, 1:12:04. */
internal fun duration(ms: Long): String {
    val t = (ms / 1000).coerceAtLeast(0)
    val h = t / 3600
    val m = (t % 3600) / 60
    val s = t % 60
    return if (h > 0) "%d:%02d:%02d".format(h, m, s) else "%d:%02d".format(m, s)
}

/**
 * The voice room (design screen 4): the header, the stage when there is a
 * picture, the seats, and the call bar. Leaving the screen does not leave
 * the room; only the red button does.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun RoomScreen(
    room: VoiceRoom?,
    voice: VoiceCall,
    state: VoiceCall.State,
    members: Map<String, Member>,
    onBack: () -> Unit,
    onList: () -> Unit,
    onSearch: () -> Unit,
    onPeople: () -> Unit,
    onPerson: (String) -> Unit,
    onMute: () -> Unit,
    onDeafen: () -> Unit,
    onCamera: () -> Unit,
    onOutput: (Output) -> Unit,
    onLeave: () -> Unit,
    onFull: () -> Unit,
    onDismissNotice: () -> Unit,
    onFullCamera: (String) -> Unit = {},
    onScreen: () -> Unit = {},
    peopleOpen: Boolean = false,
    banner: @Composable () -> Unit = {},
) {
    BackHandler(onBack = onBack)
    var outputs by remember { mutableStateOf(false) }
    val peers = room?.peers.orEmpty()
    val mine = peers.firstOrNull { it.sessionId == state.sessionId }
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(mine?.joinedAt) {
        while (mine != null) {
            now = System.currentTimeMillis()
            delay(1000)
        }
    }
    val status = when (state.phase) {
        VoiceCall.Phase.JOINING -> "Entrando…"
        VoiceCall.Phase.CONNECTING -> "Conectando…"
        VoiceCall.Phase.RECONNECTING -> "Reconectando…"
        else -> mine?.takeIf { it.joinedAt > 0 }?.let { duration(now - it.joinedAt) }
    }
    val screen = state.videos.firstOrNull { it.isScreen && it.username == state.watching }
    val local = if (state.camera) voice.localCamera() else null
    // A camera turned off on the web is a paused track, not an ended one: the
    // last frame stays on the receiver, and comes back with a rejoin. The
    // room's own word on who has a camera on is what decides (the web reads
    // the same flag), never the mere presence of a track.
    val cameraOn = peers.filter { it.camera }.mapTo(HashSet()) { it.username }
    val remoteCams = state.videos.filter { !it.isScreen && it.username in cameraOn }
    fun camOf(p: Peer): VoiceCall.Video? =
        if (p.sessionId == state.sessionId) local else remoteCams.firstOrNull { it.username == p.username }
    fun talking(p: Peer) = p.username in state.speaking && !p.muted && !p.deafened
    fun nameFor(p: Peer) = nameOf(p.username, p.displayName, members)
    // The person stopped sharing (or left): stop watching, or the stage stays
    // on "Abrindo a tela…" for a screen that is not coming (owner, 01/10/2026).
    // Only with the room known, so a reconnect's empty list does not count.
    val watchedSharing = state.watching != null && peers.any { it.username == state.watching && it.sharingScreen }
    LaunchedEffect(state.watching, watchedSharing, room != null) {
        if (state.watching != null && room != null && !watchedSharing) voice.watch(null)
    }
    fun watchOf(p: Peer): (() -> Unit)? =
        if (p.sharingScreen && p.sessionId != state.sessionId && state.watching != p.username) ({ voice.watch(p.username) }) else null

    val controls: @Composable () -> Unit = {
        CallControls(state.muted, state.deafened, state.cameraChosen, state.screenChosen, state.output, state.outputs,
            onMute, onDeafen, onCamera, onScreen, { outputs = true }, onLeave, "sair da chamada")
    }
    BoxWithConstraints(Modifier.fillMaxSize().conversationInsets()) {
        val landscape = maxWidth > maxHeight
        Column(Modifier.fillMaxSize()) {

            ConversationHeader(room?.channel?.name.orEmpty(), "voice", onList, onSearch, onPeople, peopleOpen = peopleOpen)
            banner()
            CallStage(
                stage = voice,
                landscape = landscape,
                peers = peers,
                mySession = state.sessionId,
                talking = ::talking,
                nameFor = ::nameFor,
                camOf = ::camOf,
                cameras = remoteCams + listOfNotNull(local),
                screen = screen,
                screenName = { nameOf(it.username, it.displayName, members) },
                onPerson = onPerson,
                onFull = onFull,
                onFullCamera = onFullCamera,
                notice = state.notice,
                onDismissNotice = onDismissNotice,
                controls = controls,
                screenComing = state.watching?.takeIf { watchedSharing }?.let { members[it]?.nickname ?: members[it]?.displayName ?: it },
                watchOf = ::watchOf,
                onStopWatching = { voice.watch(null) },
            )
        }
    }
    if (outputs) OutputSheet(state.output, state.outputs, onOutput) { outputs = false }
}

/**
 * The middle of a call: the stage when there is a picture, the seats, the
 * notice and the call bar. A voice room and a direct call each say who is in
 * it and what each one shows, and get the same screen (owner, 25/10/2026: the
 * 1:1 "como na chamada normal"). [cameras] is every camera on, the others'
 * first; [screenComing] the name of a screen asked for and not here yet;
 * [bar], when given, takes the call bar's place (the telephone ringing).
 * [followSpeaker]: the room puts whoever talks on the big camera; a direct
 * call keeps the other person there (owner, 29/10/2026: "você deve ficar em
 * evidência pra mim e eu para você").
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
internal fun ColumnScope.CallStage(
    stage: Stage,
    landscape: Boolean,
    peers: List<Peer>,
    mySession: String?,
    talking: (Peer) -> Boolean,
    nameFor: (Peer) -> String,
    camOf: (Peer) -> VoiceCall.Video?,
    cameras: List<VoiceCall.Video>,
    screen: VoiceCall.Video?,
    screenName: (VoiceCall.Video) -> String,
    onPerson: (String) -> Unit,
    onFull: () -> Unit,
    onFullCamera: (String) -> Unit,
    notice: String?,
    onDismissNotice: () -> Unit,
    controls: @Composable () -> Unit,
    screenComing: String? = null,
    watchOf: (Peer) -> (() -> Unit)? = { null },
    onStopWatching: (() -> Unit)? = null,
    bar: (@Composable () -> Unit)? = null,
    followSpeaker: Boolean = true,
) {
    val staging = screen != null || screenComing != null
    val sharedStage: @Composable (Modifier) -> Unit = { stageModifier ->
        Box(stageModifier.clip(Shape8).background(Surface2), contentAlignment = Alignment.Center) {
            if (screen != null) {
                VideoView(stage, screen, Modifier.fillMaxSize(), onWidth = { stage.drawnAt(screen.username, it) })
                // Over the picture, not on it: the renderer is a view of its
                // own and may keep a touch to itself.
                Box(Modifier.matchParentSize().clickable(onClick = onFull).semantics { contentDescription = "tela cheia" })
                Pill("${screenName(screen)} · tela", Glyphs.DisplayPulse, Modifier.align(Alignment.BottomStart).padding(8.dp), iconTint = Foreground)
                Row(Modifier.align(Alignment.TopEnd).padding(8.dp),
                    verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    onStopWatching?.let { stop ->
                        Row(Modifier.height(24.dp).clip(Shape8).background(Background)
                            .clickable(role = Role.Button, onClick = stop)
                            .semantics { contentDescription = "parar de assistir" }
                            .padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                            Text("Parar de assistir", style = MetaText.copy(fontWeight = FontWeight.Medium), color = Foreground)
                        }
                    }
                    TileVideoControl(Glyphs.ChevronsExpandUpRight, "tela cheia", onFull)
                }
            } else {
                Text("Abrindo a tela de $screenComing…", style = MetaText, color = Muted)
            }
        }
    }
    val seat: @Composable (Peer) -> Unit = { p ->
        Seat(p, nameFor(p), p.sessionId == mySession, talking(p), camOf(p), stage, onPerson, watchOf(p))
    }
    if (landscape) {
        Row(Modifier.weight(1f).fillMaxWidth().padding(8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Box(Modifier.weight(1f).fillMaxHeight()) {
                if (staging) sharedStage(Modifier.fillMaxSize())
                else SeatGrid(peers, Modifier.fillMaxSize(), seat)
            }
            Column(Modifier.width(192.dp).fillMaxHeight(),
                verticalArrangement = if (staging) Arrangement.spacedBy(8.dp)
                    else Arrangement.spacedBy(8.dp, Alignment.CenterVertically)) {
                if (staging) SeatGrid(peers, Modifier.weight(1f)) { p ->
                    SmallSeat(p, nameFor(p), p.sessionId == mySession, talking(p), camOf(p), stage, Modifier.fillMaxSize(), onPerson)
                }
                notice?.let { Text(it, style = MetaText, color = Muted, maxLines = 2) }
                if (bar != null) bar()
                else FlowRow(Modifier.fillMaxWidth().island().padding(8.dp), maxItemsInEachRow = 3,
                    horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) { controls() }
            }
        }
    } else {
        Box(Modifier.weight(1f).fillMaxWidth().padding(start = 12.dp, end = 12.dp, top = 12.dp, bottom = 8.dp)) {
            when {
                // A screen being watched: 16:9 on the full width, the seats share the rest.
                staging -> Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    sharedStage(Modifier.fillMaxWidth().aspectRatio(16f / 9f))
                    SeatGrid(peers, Modifier.weight(1f), seat)
                }
                // A camera: it takes the height, and the seats become one row of 96.
                cameras.isNotEmpty() -> Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    val speaker = if (followSpeaker) peers.firstOrNull { talking(it) && camOf(it) != null } else null
                    val staged = speaker?.let(camOf) ?: cameras.first()
                    val stagedPeer = speaker ?: peers.firstOrNull { camOf(it)?.mid == staged.mid }
                    val ring = stagedPeer?.let { talking(it) } == true
                    key(staged.mid) {
                        Box(
                            Modifier.weight(1f).fillMaxWidth().clip(Shape8).background(Surface2),
                        ) {
                            VideoView(stage, staged, Modifier.fillMaxSize(), mirror = staged.mid == stage.ownCamera)
                            // A camera opens in full screen like a live: a tap on it, or the button.
                            Box(Modifier.matchParentSize().clickable { onFullCamera(staged.mid) }
                                .semantics { contentDescription = "câmera em tela cheia" })
                            if (ring) Box(Modifier.matchParentSize().border(2.dp, Success, Shape8))
                            if (staged.mid == stage.ownCamera) FlipCamera(stage, Modifier.align(Alignment.TopStart))
                            stagedPeer?.let(watchOf)?.let {
                                WatchButton(it, Modifier.align(Alignment.BottomCenter).padding(bottom = 40.dp))
                            }
                            Pill("${stagedPeer?.let(nameFor) ?: staged.displayName} · câmera", null,
                                Modifier.align(Alignment.BottomStart).padding(8.dp))
                            IconButton48(Glyphs.ChevronsExpandUpRight, "tela cheia", { onFullCamera(staged.mid) },
                                Modifier.align(Alignment.TopEnd).padding(8.dp), background = Surface1)
                        }
                    }
                    // With cameras on, the faces are small: whoever is sharing
                    // gets a full-width way in, instead of profile → Assistir.
                    for (p in peers) if (p != stagedPeer) watchOf(p)?.let { go ->
                        key("watch-" + p.sessionId) {
                            Row(Modifier.fillMaxWidth().height(44.dp).clip(Shape8).background(Selected)
                                .clickable(role = Role.Button, onClick = go)
                                .semantics { contentDescription = "assistir à tela de ${nameFor(p)}" },
                                horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
                                verticalAlignment = Alignment.CenterVertically) {
                                Glyph(Glyphs.Display, null, Foreground, 20)
                                Text("Assistir à tela de ${nameFor(p)}", style = MetaText.copy(fontWeight = FontWeight.Medium),
                                    color = Foreground, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            }
                        }
                    }
                    BoxWithConstraints(Modifier.fillMaxWidth().height(96.dp)) {
                        val w = (maxWidth - 24.dp) / 4
                        Row(Modifier.fillMaxSize().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            for (p in peers) key(p.sessionId) {
                                SmallSeat(p, nameFor(p), p.sessionId == mySession, talking(p) && p != stagedPeer,
                                    camOf(p)?.takeIf { it.mid != staged.mid }, stage, Modifier.width(w).fillMaxHeight(), onPerson)
                            }
                        }
                    }
                }
                else -> SeatGrid(peers, Modifier.fillMaxSize(), seat)
            }
        }
        notice?.let {
            Strip(Glyphs.CircleExclamation, it, null, danger = true, action = "OK", actionMain = false, onAction = onDismissNotice)
        }
        if (bar != null) Box(Modifier.fillMaxWidth().padding(start = 12.dp, end = 12.dp, bottom = 12.dp)) { bar() }
        else BoxWithConstraints(Modifier.fillMaxWidth().padding(start = 12.dp, end = 12.dp, bottom = 12.dp)
            .island().padding(8.dp)) {
            // Keep 48 dp targets and the exit visible at 360 dp. Narrower windows
            // can scroll the controls without shrinking their touch targets.
            Row(Modifier.horizontalScroll(rememberScrollState()).width(maxOf(maxWidth, 320.dp)),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically) {
                controls()
            }
        }
    }
}

/** Two to a row, the rows sharing the height. */
@Composable
internal fun SeatGrid(peers: List<Peer>, modifier: Modifier, seat: @Composable (Peer) -> Unit) {
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (peers.size == 1) {
            val p = peers.single()
            key(p.sessionId) { Box(Modifier.fillMaxSize()) { seat(p) } }
            return@Column
        }
        for (pair in peers.chunked(2)) {
            Row(Modifier.weight(1f).fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                for (p in pair) key(p.sessionId) { Box(Modifier.weight(1f).fillMaxHeight()) { seat(p) } }
                if (pair.size == 1) Spacer(Modifier.weight(1f))
            }
        }
    }
}

/** A name in a pill of 24 on the background: the only kind of text allowed over a picture. */
@Composable
private fun Pill(text: String, icon: ImageVector?, modifier: Modifier, iconTint: Color = Muted, peer: Peer? = null) {
    Row(
        modifier.height(24.dp).clip(Shape8).background(Background).padding(horizontal = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        icon?.let { Glyph(it, null, tint = iconTint, size = 12) }
        peer?.let { StateMarks(it) }
        Text(text, style = MetaText.copy(fontWeight = FontWeight.Medium), maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

/** The marks on a person's name chip, as the web's: hearing or microphone off, in red. */
@Composable
private fun StateMarks(p: Peer) {
 val personal by com.taleshernandes.harmony.model.PersonalPreferences.state.collectAsState()
 MicMark(p.deafened, p.muted, 12)
 if (personal.any { it.username == p.username && (it.muted || it.blocked) }) Glyph(Glyphs.MicrophoneSlash, "Silenciado por você", tint = Muted, size = 12)
}

/** A seat: surface, a face of 56 (or the person's camera), the name in a pill; the ring when talking. */
@Composable
internal fun Seat(p: Peer, name: String, me: Boolean, talking: Boolean, cam: VoiceCall.Video?, voice: Stage, onPerson: (String) -> Unit,
                 onWatch: (() -> Unit)? = null) {
    Box(
        Modifier.fillMaxSize().clip(Shape8).background(if (cam != null) Surface2 else Surface1)
            .clickable(role = Role.Button) { onPerson(p.username) }
            .semantics { contentDescription = "$name${if (talking) " falando" else ""}" },
        contentAlignment = Alignment.Center,
    ) {
        if (cam != null) {
            VideoView(voice, cam, Modifier.fillMaxSize(), mirror = cam.mid == voice.ownCamera)
            if (cam.mid == voice.ownCamera) FlipCamera(voice, Modifier.align(Alignment.TopStart))
            onWatch?.let { WatchButton(it, Modifier.align(Alignment.BottomCenter).padding(bottom = 40.dp)) }
        } else if (onWatch != null) {
            // The face and the button centred together, 16 apart (the web's 4
            // read as one lump on the phone — owner, 01/10/2026).
            Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(16.dp)) {
                Avatar(name, 56, username = p.username)
                WatchButton(onWatch)
            }
        } else {
            Avatar(name, 56, username = p.username)
        }
        Pill(name + if (me) " (você)" else "", null, Modifier.align(Alignment.BottomStart).padding(8.dp), peer = p)
        if (talking) Box(Modifier.matchParentSize().border(2.dp, Success, Shape8))
    }
}

/**
 * The web's "Assistir" (Call.tsx WATCH, at phone width): 36 high, 12 across,
 * the display glyph and the word, on the control grey. A screen is not sent
 * to anybody who has not asked for it (D4s), so this is the way in.
 */
@Composable
private fun WatchButton(onWatch: () -> Unit, modifier: Modifier = Modifier) {
    Row(modifier.height(36.dp).clip(Shape8).background(Selected)
        .clickable(role = Role.Button, onClick = onWatch).semantics { contentDescription = "assistir à tela" }
        .padding(horizontal = 12.dp), verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Glyph(Glyphs.Display, null, Foreground, 20)
        Text("Assistir", style = MetaText.copy(fontWeight = FontWeight.Medium), color = Foreground)
    }
}

/** A seat in the row under a camera: 96 high, a face of 40, the short name at 12. */
@Composable
private fun SmallSeat(p: Peer, name: String, me: Boolean, talking: Boolean, cam: VoiceCall.Video?, voice: Stage,
                      modifier: Modifier, onPerson: (String) -> Unit) {
    BoxWithConstraints(
        modifier.clip(Shape8).background(if (cam != null) Surface2 else Surface1)
            .clickable(role = Role.Button) { onPerson(p.username) }
            .semantics { contentDescription = "$name${if (talking) " falando" else ""}" },
        contentAlignment = Alignment.Center,
    ) {
        val compact = maxHeight < 80.dp
        if (cam != null) {
            VideoView(voice, cam, Modifier.fillMaxSize(), mirror = cam.mid == voice.ownCamera)
            if (cam.mid == voice.ownCamera) FlipCamera(voice, Modifier.align(Alignment.TopStart))
            Pill(if (me) "Você" else name.substringBefore(' '), null, Modifier.align(Alignment.BottomStart).padding(4.dp), peer = p)
        } else {
            Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Avatar(name, if (compact) 24 else 40, username = p.username)
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp),
                    modifier = Modifier.padding(horizontal = 4.dp)) {
                    StateMarks(p)
                    Text(if (me) "Você" else name.substringBefore(' '), fontSize = 12.sp, lineHeight = 16.sp,
                        fontWeight = FontWeight.Medium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
        }
        if (talking) Box(Modifier.matchParentSize().border(2.dp, Success, Shape8))
    }
}

/** Front or back camera, on the corner of the own picture; only on phones that have both. */
@Composable
private fun FlipCamera(voice: Stage, modifier: Modifier) {
    if (!remember { voice.canSwitchCamera() }) return
    Box(modifier.padding(8.dp).size(32.dp).clip(CircleShape).background(Surface1)
        .clickable(role = Role.Button, onClick = voice::switchCamera)
        .semantics { contentDescription = "trocar de câmera" }, contentAlignment = Alignment.Center) {
        Glyph(Glyphs.ArrowRotateRight, null, tint = Foreground, size = 16)
    }
}
