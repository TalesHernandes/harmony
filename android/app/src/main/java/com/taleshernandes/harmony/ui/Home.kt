package com.taleshernandes.harmony.ui

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.MutableTransitionState
import androidx.compose.animation.core.rememberTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.remember
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.ExperimentalFoundationApi
import com.taleshernandes.harmony.model.*
import androidx.compose.ui.platform.LocalContext
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.taleshernandes.harmony.media.VoiceCall
import com.taleshernandes.harmony.model.Channel
import com.taleshernandes.harmony.model.Member
import com.taleshernandes.harmony.model.Peer
import com.taleshernandes.harmony.model.User
import com.taleshernandes.harmony.model.VoiceRoom

/** What the list needs from the chat: the channels with their badges, and where a tap goes. */
class HomeChat(
    val texts: List<Channel>,
    val dms: List<Channel>,
    val members: Map<String, Member>,
    val presence: Map<String, String>,
    val onOpen: (Channel) -> Unit,
)

/** Keep the approved list islands on their own background, without a sheet's chrome or gutter. */
@Composable
internal fun Drawer(open: Boolean, onClose: () -> Unit, panel: @Composable (Modifier) -> Unit) {
    val configuration = androidx.compose.ui.platform.LocalConfiguration.current
    val width = drawerWidth(configuration.screenWidthDp, configuration.screenHeightDp)
    val state = remember { MutableTransitionState(false) }
    state.targetState = open
    val animation = rememberTransition(state, label = "drawer")
    val fade = tween<Float>(180, easing = FastOutSlowInEasing)
    BackHandler(enabled = state.currentState || state.targetState, onBack = onClose)
    Box(Modifier.fillMaxSize()) {
        animation.AnimatedVisibility(visible = { it }, enter = fadeIn(fade), exit = fadeOut(fade)) {
            Box(Modifier.fillMaxSize().background(Scrim)
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null, onClick = onClose)
                .semantics { contentDescription = "fechar" })
        }
        Box(Modifier.fillMaxSize().safeDrawingPadding()) {
            animation.AnimatedVisibility(visible = { it }, modifier = Modifier.align(Alignment.CenterStart),
                enter = slideInHorizontally(tween(180, easing = FastOutSlowInEasing)) { -it } + fadeIn(fade),
                exit = slideOutHorizontally(tween(180, easing = FastOutSlowInEasing)) { -it } + fadeOut(fade)) {
                Box(Modifier.width(width.dp).fillMaxHeight().padding(vertical = 8.dp)
                    .clip(RoundedCornerShape(topEnd = 8.dp, bottomEnd = 8.dp)).background(Background)
                    .pointerInput(onClose) {
                        val enough = 48.dp.toPx()
                        var moved = 0f
                        var dismissed = false
                        detectHorizontalDragGestures(
                            onDragStart = { moved = 0f; dismissed = false },
                            onHorizontalDrag = { change, dx ->
                                moved += dx
                                if (!dismissed && moved < -enough) {
                                    change.consume()
                                    dismissed = true
                                    onClose()
                                }
                            })
                    }) { panel(Modifier.fillMaxSize()) }
            }
        }
    }
}

internal fun drawerWidth(width: Int, height: Int): Int = if (width > height) 320 else 256

/**
 * The list itself: the list island and, under it, the person's piece. Fills
 * the screen when no conversation is open, and the drawer when one is.
 */
@Composable
internal fun ListPanel(
    user: User?,
    chat: HomeChat,
    openId: String?,
    rooms: List<VoiceRoom>,
    live: Boolean,
    voice: VoiceCall.State,
    onRoom: (VoiceRoom) -> Unit,
    onPerson: (String) -> Unit,
    onSettings: () -> Unit,
    modifier: Modifier = Modifier,
    banner: @Composable () -> Unit = {},
    channelState: RoomsState = RoomsState(),
    onManageChannel: (Channel) -> Unit = {},
    onManageCategory: (Category) -> Unit = {},
    onOrganize: () -> Unit = {},
) {
    val prefs = LocalContext.current.getSharedPreferences("channel-folders", android.content.Context.MODE_PRIVATE)
    var closed by remember { mutableStateOf(prefs.getStringSet("closed", emptySet())!!.toSet()) }
    var tab by rememberSaveable { mutableIntStateOf(0) }
    Column(modifier.padding(bottom = 8.dp)) {
        banner()
        if (!live) Strip(Glyphs.CircleExclamation, "Reconectando…", "A lista pode estar desatualizada.")
        Row(Modifier.fillMaxWidth().height(64.dp).padding(horizontal = 24.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("harmony", style = TitleText, modifier = Modifier.weight(1f))
                if (channelState.canManage) WebIcon(Glyphs.Bars, "Organizar canais", onOrganize)
        }
        Tabs(
                tab,
                mentions = chat.texts.filter { !it.muted }.sumOf { it.mentions },
                unread = chat.dms.filter { !it.muted }.sumOf { it.unread },
                onTab = { tab = it },
        )
        Spacer(Modifier.height(8.dp))
        Column(Modifier.weight(1f).fillMaxWidth().padding(horizontal = 8.dp).island()) {
            LazyColumn(Modifier.weight(1f).fillMaxWidth(), contentPadding = PaddingValues(top = 8.dp, bottom = 8.dp),
                verticalArrangement = Arrangement.spacedBy(2.dp)) {
                if (tab == 1) {
                    // No "Nova conversa" row (the owner, 17/10/2026): a
                    // conversation starts from the person, in the people list.
                    if (chat.dms.isEmpty()) {
                        item {
                            Text("Nenhuma conversa ainda. Comece uma pelo botão de pessoas, no alto.", style = MetaText.copy(lineHeight = 20.sp),
                                color = Muted, modifier = Modifier.padding(16.dp))
                        }
                    }
                    items(chat.dms, key = { it.id }) { c ->
                        DmEntry(c, chat.members, chat.presence, selected = c.id == openId, onOpen = { chat.onOpen(c) }, onPerson = onPerson)
                    }
                    return@LazyColumn
                }
                for (section in channelState.sections(closed, openId, voice.channelId)) {
                    val folder = section.category
                    item(key = "section:" + (folder?.id ?: "top")) {
                        if (folder == null) ListHeading("Canais") else {
                            FolderHeading(folder, folder.id in closed, {
                                closed = if (folder.id in closed) closed - folder.id else closed + folder.id
                                prefs.edit().putStringSet("closed", closed).apply()
                            }, if (channelState.canManage) ({ onManageCategory(folder) }) else null)
                        }
                    }
                    for (c in section.channels) {
                        val manage = if (channelState.canManage) ({ onManageChannel(c) }) else null
                        if (!c.isVoice) item(key = c.id) {
                            ChannelEntry(c, c.id == openId, { chat.onOpen(c) }, manage)
                        } else {
                            if (folder == null && c.id == section.channels.firstOrNull { it.isVoice }?.id) {
                                item(key = "voice-heading") { ListHeading("Voz", Modifier.padding(top = 6.dp)) }
                            }
                            val room = rooms.firstOrNull { it.channel.id == c.id } ?: VoiceRoom(c, emptyList())
                            item(key = c.id) { RoomEntry(room, c.id == voice.channelId, { onRoom(room) }, manage) }
                            items(room.peers, key = { "peer:" + c.id + ":" + it.sessionId }) { p ->
                                PeerEntry(p, nameOf(p.username, p.displayName, chat.members), p.sessionId == voice.sessionId,
                                    p.username in voice.speaking && !p.muted && !p.deafened, { onPerson(p.username) })
                            }
                        }
                    }
                }
            }
        }
        Spacer(Modifier.height(8.dp))
        PersonPiece(user, onSettings)
    }
}

/** "Servidor" and "Mensagens", 36 high, gap 4; the count only on the closed one. */
@Composable
private fun Tabs(tab: Int, mentions: Int, unread: Int, onTab: (Int) -> Unit) {
    Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp).island().padding(8.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        for ((i, label) in listOf("Servidor", "Mensagens").withIndex()) {
            val on = i == tab
            Row(
                (if (tab == 0 && unread > 0 && i == 0) Modifier.width(103.3.dp) else Modifier.weight(1f))
                    .height(36.dp).clip(Shape8).background(if (on) Selected else Color.Transparent)
                    .webClickable(role = Role.Tab) { onTab(i) }
                    .semantics { contentDescription = label; selected = on },
                horizontalArrangement = Arrangement.spacedBy(6.dp, Alignment.CenterHorizontally),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(label, style = PersonText.copy(fontWeight = if (on) FontWeight.SemiBold else FontWeight.Medium),
                    color = if (on) Foreground else Muted, maxLines = 1)
                if (!on) {
                    if (i == 0 && mentions > 0) MentionChip(mentions)
                    if (i == 1 && unread > 0) CountPill(unread)
                }
            }
        }
    }
}

@Composable
private fun ListHeading(label: String, modifier: Modifier = Modifier) {
    // Native creation is unavailable. Keep the heading height and omit the target.
    Row(modifier.fillMaxWidth().height(32.dp).padding(start = 16.dp, end = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(label.uppercase(ptBr), style = ColumnLabelText, color = Muted, modifier = Modifier.weight(1f))
    }
}

/**
 * A line of the list, the whole island wide: its row sits 8 in, and when it is
 * the open one it goes to `selected` with the accent bar on the island's edge.
 */
@Composable
@OptIn(ExperimentalFoundationApi::class)
private fun ListLine(
    selected: Boolean,
    height: Int,
    description: String,
    onClick: () -> Unit,
    onLong: (() -> Unit)? = null,
    start: Int = 8,
    gap: Int = 6,
    content: @Composable RowScope.() -> Unit,
) {
    Box(
        Modifier.fillMaxWidth().height(height.dp)
            .then(if (selected) Modifier.drawBehind { edgeBar() } else Modifier)
            .padding(horizontal = 8.dp),
    ) {
        Row(
            Modifier.fillMaxSize().clip(Shape8).background(if (selected) Selected else Color.Transparent)
                .combinedClickable(role = Role.Button, onClick = onClick, onLongClick = onLong).padding(start = start.dp, end = 8.dp)
                .semantics { contentDescription = description },
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(gap.dp),
            content = content,
        )
    }
}

/** A text channel: unread is said by weight and white; mentions by the @ chip; silenced by the bell. */
@Composable
private fun ChannelEntry(c: Channel, selected: Boolean, onOpen: () -> Unit, onLong: (() -> Unit)? = null) {
    val lit = (c.unread > 0 && !c.muted) || selected
    // The # sits close to the name: half the gap of a face and a name (owner, 02/10/2026).
    ListLine(selected, 36, "#${c.name}", onOpen, onLong = onLong, gap = 3) {
        Glyph(Glyphs.Hashtag, null, tint = if (lit) Foreground else Muted, size = 18)
        Text(c.name, style = ItemText.copy(fontWeight = if (lit) FontWeight.SemiBold else FontWeight.Medium),
            color = if (lit) Foreground else Muted, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
        if (c.muted) Glyph(Glyphs.BellSlash, "silenciado", tint = Muted, size = 16)
        else if (c.mentions > 0) MentionChip(c.mentions)
    }
}

/** A voice room: 36, headphones and the name on the same line as text channels. */
@Composable
private fun RoomEntry(room: VoiceRoom, mine: Boolean, onClick: () -> Unit, onLong: (() -> Unit)? = null) {
    val busy = room.peers.isNotEmpty() || mine
    ListLine(mine, 36, room.channel.name, onClick, onLong = onLong) {
        Glyph(Glyphs.Headphones, null, tint = if (mine) Foreground else Muted, size = 18)
        Text(room.channel.name, style = ItemText.copy(fontWeight = if (busy) FontWeight.SemiBold else FontWeight.Medium),
            color = if (busy) Foreground else Muted, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
    }
}

/** Somebody in a room: 36, indent 32, a face of 28 with the ring when talking, and their state in a glyph. */
@Composable
private fun PeerEntry(p: Peer, name: String, me: Boolean, talking: Boolean, onTap: () -> Unit) {
    ListLine(false, 36, "$name${if (talking) " falando" else ""}", onTap, start = 32, gap = 6) {
        Avatar(name, 28, speaking = talking, username = p.username)
        Text(name + if (me) " (você)" else "", style = PersonText, color = if (talking) Foreground else Muted,
            maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
        Row(horizontalArrangement = Arrangement.spacedBy(4.dp), verticalAlignment = Alignment.CenterVertically) {
            if (p.sharingScreen) Glyph(Glyphs.DisplayPulse, "transmitindo", tint = Muted, size = 14)
            if (p.camera) Glyph(Glyphs.Camera, "câmera", tint = Muted, size = 14)
            MicMark(p.deafened, p.muted, 14)
        }
    }
}

/** A direct conversation: 40, the face with the presence dot, the name, the count or the bell. */
@Composable
private fun DmEntry(c: Channel, members: Map<String, Member>, presence: Map<String, String>, selected: Boolean,
                    onOpen: () -> Unit, onPerson: (String) -> Unit) {
    val name = c.peer?.let { nameOf(it.username, it.displayName, members) } ?: c.name
    val lit = (c.unread > 0 && !c.muted) || selected
    // Native closing is unavailable; the name and unread count keep their row positions.
    Box(Modifier.fillMaxWidth().height(40.dp)) {
        ListLine(selected, 40, name, onOpen, start = 8, gap = 6) {
            // No clip here: the presence dot hangs past the face's lower-right and a rounded clip of the button cut it (0.19).
            Box(Modifier.size(32.dp).webClickable { c.peer?.let { onPerson(it.username) } }
                .semantics { contentDescription = "perfil de $name" }, contentAlignment = Alignment.Center) {
                Avatar(name, 32, username = c.peer?.username, dot = presenceColor(c.peer?.let { presence[it.username] }),
                    face = if (presence[c.peer?.username] == "online") Surface3 else Surface2,
                    ink = if (presence[c.peer?.username] == "online") Foreground else Muted)
            }
            Text(name, style = ItemText.copy(fontWeight = if (lit) FontWeight.SemiBold else FontWeight.Medium),
                color = if (lit) Foreground else Muted, maxLines = 1, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f))
            if (c.muted) Glyph(Glyphs.BellSlash, "silenciada", tint = Muted, size = 14)
            else if (c.unread > 0) CountPill(c.unread)
        }
    }
}

/** The person at the foot of the list: 48, and the way into Ajustes. */
@Composable
private fun PersonPiece(user: User?, onSettings: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 8.dp).height(48.dp).island().padding(start = 16.dp, end = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        val name = user?.displayName?.ifEmpty { null } ?: user?.username ?: "…"
        Avatar(name, 32, dot = Success, username = user?.username)
        Column(Modifier.weight(1f)) {
            Text(name, style = PersonText, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        WebIcon(Glyphs.Gear, "ajustes", onSettings, size = 20, background = Surface2, enabled = user != null)
    }
}

/** The web's 240dp right panel, including you, grouped by presence. */
@Composable
internal fun PeopleSheet(members: List<Member>, me: String?, presence: Map<String, String>, onPick: (String) -> Unit, onDismiss: () -> Unit) {
    Sheet(onDismiss, title = "Pessoas · ${members.size}", expanded = true, scroll = false,
        width = 240, edge = PanelEdge.End) {
        val close = LocalPanelDismiss.current
        Column(Modifier.fillMaxSize()) {
            val groups = com.taleshernandes.harmony.model.memberGroups(members, me, presence, LocalRoles.current.roles)
            LazyColumn {
                for ((label, group) in groups) {
                    if (group.isEmpty()) continue
                    item {
                        Text("${label.uppercase(ptBr)} · ${group.size}", style = ColumnLabelText, color = Muted,
                            modifier = Modifier.fillMaxWidth().padding(start = 8.dp, end = 8.dp, top = 16.dp, bottom = 2.dp))
                    }
                    items(group, key = { it.username }) { p ->
                        val name = p.nickname?.ifEmpty { null } ?: p.displayName.ifEmpty { p.username }
                        val online = (presence[p.username] ?: if (p.username == me) "online" else "offline") in listOf("online", "in_call", "away")
                        Row(Modifier.fillMaxWidth().padding(top = 2.dp).height(48.dp).clip(Shape8)
                            .webClickable(role = Role.Button) { close { onPick(p.username) } }.padding(horizontal = 8.dp)
                            .semantics { contentDescription = name }, verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            Avatar(name, 32, username = p.username, dot = presenceColor(presence[p.username] ?: if (p.username == me) "online" else "offline"),
                                face = if (online) Surface3 else Surface2, ink = if (online) Foreground else Muted)
                            Text(name, style = ItemText, color = roleNameColor(p.username, if (online) Foreground else Muted),
                                maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                        }
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun FolderHeading(folder: Category, closed: Boolean, onToggle: () -> Unit, onLong: (() -> Unit)?) {
    Text((if (closed) "› " else "⌄ ") + folder.name, style = ColumnLabelText, color = Muted,
        modifier = Modifier.fillMaxWidth().combinedClickable(onClick = onToggle, onLongClick = onLong).padding(12.dp))
}
