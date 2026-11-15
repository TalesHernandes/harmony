package com.taleshernandes.harmony.ui

import androidx.compose.runtime.collectAsState

import android.content.ClipData
import android.content.ClipboardManager
import android.text.format.DateFormat
import androidx.activity.compose.BackHandler
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.foundation.gestures.scrollBy
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Text
import androidx.compose.material3.minimumInteractiveComponentSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.layout
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.LinkAnnotation
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.TextLinkStyles
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.LineHeightStyle
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withLink
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.taleshernandes.harmony.Conversations
import com.taleshernandes.harmony.model.Attachment
import com.taleshernandes.harmony.model.Channel
import com.taleshernandes.harmony.model.Embed
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.foundation.Canvas
import com.taleshernandes.harmony.model.Member
import com.taleshernandes.harmony.model.Message
import com.taleshernandes.harmony.model.Timeline
import com.taleshernandes.harmony.model.User
import com.taleshernandes.harmony.net.Api
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import java.util.Calendar

/** What the person does to messages, from the conversation screen. */
class ChatActions(
    val onBack: () -> Unit,
    val onSend: (body: String, replyTo: Long?, done: (Boolean) -> Unit) -> Boolean,
    val onEdit: (id: Long, body: String, done: (Boolean) -> Unit) -> Unit,
    val onDelete: (Long) -> Unit,
    val onReact: (Long, String, Boolean) -> Unit,
    val onOlder: () -> Unit,
    val onTyped: () -> Unit,
    val onDraft: (String) -> Unit,
    val onAttach: () -> Unit,
    val onUnattach: (String) -> Unit,
    val onDismissNotice: () -> Unit,
    val onPerson: (String) -> Unit,
    /** Calls the other person; only in a direct conversation. */
    val onCall: (() -> Unit)? = null,
    val onNewer: () -> Unit = {},
    val onLatest: () -> Unit = {},
    val onPositioned: (Conversations.Position) -> Unit = {},
)

/**
 * The conversation (design screen 3): the full-width header, the list straight on
 * the background, anchored at the bottom, and the island to write in.
 * [onList] opens the list panel, from the bars button or a drag that starts in
 * the middle of the conversation (the edge is the system's back gesture).
 */
@Composable
fun ChatScreen(
    channel: Channel,
    onJumpPin: (Long) -> Unit = {},
    onPin: (Long, Boolean) -> Unit = { _, _ -> },
    me: User,
    state: Conversations.State,
    draft: String,
    api: () -> Api?,
    actions: ChatActions,
    onList: () -> Unit,
    onPeople: () -> Unit,
    onSearch: () -> Unit,
    peopleOpen: Boolean = false,
    banner: @Composable () -> Unit = {},
) {
    BackHandler(onBack = actions.onBack)
    val timeline = state.timelines[channel.id] ?: Timeline()
    val members = state.members
    val roles = LocalRoles.current.roles
    val manageMessages = "manage_messages" in com.taleshernandes.harmony.model.permissionsOf(me.username, me.isAdmin, roles)
    var pinsOpen by remember(channel.id) { mutableStateOf(false) }
    var replyTo by remember(channel.id) { mutableStateOf<Message?>(null) }
    var editing by remember(channel.id) { mutableStateOf<Message?>(null) }
    var acting by remember { mutableStateOf<Message?>(null) }
    var removing by remember { mutableStateOf<Message?>(null) }
    var looking by remember { mutableStateOf<Pair<Attachment, Message>?>(null) }
    val density = LocalDensity.current

    Column(Modifier.fillMaxSize().conversationInsets().imePadding()) {
        ConversationHeader(channel.peer?.takeIf { channel.isDm }?.let { nameOf(it.username, it.displayName, members) } ?: channel.name,
            channel.kind, onList, onSearch, onPeople, actions.onCall, peopleOpen, onPins = { pinsOpen = true })
        banner()
        Column(
            Modifier.weight(1f).fillMaxWidth().pointerInput(Unit) {
                // The list opens from a drag to the right that starts in the
                // middle, never from the edge.
                val edge = with(density) { 48.dp.toPx() }
                val enough = with(density) { 64.dp.toPx() }
                var from = 0f
                var moved = 0f
                detectHorizontalDragGestures(
                    onDragStart = { from = it.x; moved = 0f },
                    onHorizontalDrag = { change, dx ->
                        if (from < edge || from > size.width - edge) return@detectHorizontalDragGestures
                        moved += dx
                        if (moved > enough) {
                            change.consume()
                            moved = Float.NEGATIVE_INFINITY
                            onList()
                        }
                    },
                )
            },
        ) {
            Box(Modifier.weight(1f).fillMaxWidth()) {
                MessageList(
                    channelId = channel.id,
                    me = me,
                    timeline = timeline,
                    readAtOpen = state.readAtOpen,
                    loadingOlder = state.loadingOlder,
                    loadingNewer = state.loadingNewer,
                    position = state.position?.takeIf { it.channelId == channel.id },
                    members = members,
                    api = api,
                    onOlder = actions.onOlder,
                    onNewer = actions.onNewer,
                    onLatest = actions.onLatest,
                    onPositioned = actions.onPositioned,
                    onLong = { acting = it },
                    onReact = actions.onReact,
                    onPicture = { f, m -> looking = f to m },
                    onPerson = actions.onPerson,
                )
            }
            TypingLine(state.typing[channel.id].orEmpty())
        }
        state.notice?.let {
            Strip(Glyphs.CircleExclamation, it, null, danger = true, action = "OK", actionMain = false, onAction = actions.onDismissNotice)
        }
        val personal by com.taleshernandes.harmony.model.PersonalPreferences.state.collectAsState()
        if (channel.isDm && personal.any { it.username == channel.peer?.username && it.blocked }) {
            BlockedConversation(channel.peer!!.username, api)
        } else Composer(
            channel = channel,
            draft = draft,
            replyTo = replyTo,
            editing = editing,
            pending = state.pending,
            uploading = state.uploading,
            members = members,
            sending = channel.id in state.sending,
            api = api,
            actions = actions,
            onCancelReply = { replyTo = null },
            onCancelEdit = { editing = null },
            onSent = { replyTo = null; editing = null },
        )
    }

    acting?.let { selected ->
        val m = timeline.messages.firstOrNull { it.id == selected.id } ?: selected
        MessageSheet(
            m = m,
            me = me,
            api = api,
            onDismiss = { acting = null },
            onReact = { emoji ->
                val mine = m.reactions.any { it.emoji == emoji && me.username in it.users }
                actions.onReact(m.id, emoji, !mine)
            },
            onReply = { replyTo = m; editing = null },
            onEdit = { editing = m; replyTo = null },
            onRemove = { removing = m },
            canRemove = com.taleshernandes.harmony.model.canDeleteMessage(me, roles, channel, m.author.username),
            onPin = if (com.taleshernandes.harmony.model.canPin(manageMessages, channel) && !m.deleted && m.kind.isEmpty()) ({ onPin(m.id, !m.pinned) }) else null,
        )
    }
    removing?.let { m ->
        Confirm(
            title = "Apagar esta mensagem?",
            body = "Ela sai para todo mundo nesta conversa, e isso não se desfaz. Fica no lugar a linha de mensagem removida.",
            confirm = "Apagar",
            onConfirm = { removing = null; actions.onDelete(m.id) },
            onDismiss = { removing = null },
        )
    }
    if (pinsOpen) PinsPanel(channel.id, state.pinRevision, api, onJumpPin) { pinsOpen = false }
    looking?.let { (f, m) -> Lightbox(f, nameOf(m.author.username, m.author.displayName, members), api) { looking = null } }
}

/** A question before something that cannot be undone: an island over the scrim, two buttons of 48. */
@Composable
internal fun Confirm(title: String, body: String, confirm: String, onConfirm: () -> Unit, onDismiss: () -> Unit,
                     extra: @Composable () -> Unit = {}) {
    Sheet(onDismiss, title = title) {
        val close = LocalPanelDismiss.current
        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(16.dp)) {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(body, style = PersonText.copy(lineHeight = 22.sp), color = Muted)
            }
            extra()
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button48("Cancelar", { close(onDismiss) }, Modifier.weight(1f), color = Surface2, style = SmallButtonText)
                Button48(confirm, { close(onConfirm) }, Modifier.weight(1f), color = DangerSoft, textColor = DangerSoftFg, style = SmallButtonText)
            }
        }
    }
}

/** One line of the list, worked out in reading order and drawn bottom-up. */
private data class Row_(
    val m: Message,
    val newDay: String?,
    val grouped: Boolean,
    val firstUnread: Boolean,
    /** A following line of a block shows its hour at the end, only the last of each minute. */
    val trailingTime: Boolean,
    /** Mentions of you in a row of one block share one box and one bar, with no gap between them. */
    val mentionAbove: Boolean = false,
    val mentionBelow: Boolean = false,
)

private fun rowsOf(messages: List<Message>, readAtOpen: Long, me: String): List<Row_> {
    val out = ArrayList<Row_>(messages.size)
    var previous: Message? = null
    var previousDay: String? = null
    var unreadShown = false
    for (m in messages) {
        val d = dayOf(m.createdAt)
        val newDay = d != previousDay
        previousDay = d
        val p = previous
        // Same person, within five minutes, no day break: one block (Messages.tsx).
        val grouped = !newDay && p != null && p.author.username == m.author.username &&
            m.createdAt - p.createdAt < 5 * 60_000 && m.kind.isEmpty() && p.kind.isEmpty()
        val firstUnread = !unreadShown && readAtOpen > 0 && m.id > readAtOpen && m.author.username != me
        if (firstUnread) unreadShown = true
        out += Row_(m, if (newDay) d else null, grouped, firstUnread, false)
        previous = m
    }
    // The block's head already says its minute; a following line says a new
    // one, once, on the last line that has it.
    var headMinute = -1L
    for (i in out.indices) {
        val r = out[i]
        val minute = r.m.createdAt / 60_000
        if (!r.grouped || r.m.replyTo != null) { headMinute = minute; continue }
        val next = out.getOrNull(i + 1)
        val lastOfMinute = next == null || !next.grouped || next.m.createdAt / 60_000 != minute
        if (lastOfMinute && minute != headMinute) out[i] = r.copy(trailingTime = true)
    }
    // A line that continues a block and names you, under another that does,
    // joins its box: one tinted piece, as on the web, not a stack of cards.
    for (i in 1 until out.size) {
        val r = out[i]
        val p = out[i - 1]
        if (r.grouped && r.m.replyTo == null && r.newDay == null && !r.firstUnread && mentions(p.m, me) && mentions(r.m, me)) {
            out[i] = r.copy(mentionAbove = true)
            out[i - 1] = p.copy(mentionBelow = true)
        }
    }
    return out
}

private fun mentions(m: Message, me: String) = m.kind.isEmpty() && me in m.mentions

@Composable
private fun MessageList(
    channelId: String,
    me: User,
    timeline: Timeline,
    readAtOpen: Long,
    loadingOlder: Boolean,
    loadingNewer: Boolean,
    position: Conversations.Position?,
    members: Map<String, Member>,
    api: () -> Api?,
    onOlder: () -> Unit,
    onNewer: () -> Unit,
    onLatest: () -> Unit,
    onPositioned: (Conversations.Position) -> Unit,
    onLong: (Message) -> Unit,
    onReact: (Long, String, Boolean) -> Unit,
    onPicture: (Attachment, Message) -> Unit,
    onPerson: (String) -> Unit,
) {
    if (!timeline.loaded) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { Text("Carregando…", style = MetaText, color = Muted) }
        return
    }
    if (timeline.messages.isEmpty()) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Text("Ninguém falou nada aqui ainda.", style = ItemText, color = Muted)
        }
        return
    }
    val rows = remember(timeline.messages, readAtOpen) { rowsOf(timeline.messages, readAtOpen, me.username).asReversed() }
    val list = key(channelId) { rememberLazyListState() }
    var highlighted by remember(channelId) { mutableStateOf<Long?>(null) }
    val highlight = remember(channelId) { Animatable(0f) }
    var centering by remember(channelId) { mutableStateOf(false) }
    val messageHeights = remember(channelId) { mutableMapOf<Long, Int>() }
    // Keep the visible message still when an after-page inserts rows at item 0.
    var newerAnchor by remember(channelId) { mutableStateOf<Triple<Long, Int, Long?>?>(null) }
    var newerStalled by remember(channelId) { mutableStateOf(false) }
    // A new result in the same conversation is a new request too. Pagination
    // does not restart this effect, and consuming it does not cancel the fade.
    var request by remember(channelId) { mutableStateOf<Conversations.Position?>(null) }
    LaunchedEffect(position) { if (position != null) request = position }
    LaunchedEffect(request) {
        val target = request ?: return@LaunchedEffect
        centering = true
        highlighted = null
        highlight.snapTo(0f)
        try {
            if (target.messageId == null) {
                list.scrollToItem(0)
            } else {
                val index = rows.indexOfFirst { it.m.id == target.messageId }
                if (index >= 0) {
                    list.scrollToItem(index + 1) // the bottom spacer is item 0
                    val item = snapshotFlow {
                        list.layoutInfo.visibleItemsInfo.firstOrNull { it.key == target.messageId }
                    }.first { it != null }!!
                    val layout = list.layoutInfo
                    val center = (layout.viewportStartOffset + layout.viewportEndOffset) / 2f
                    // In reverse layout a positive scroll moves the row down.
                    val messageHeight = messageHeights[target.messageId] ?: item.size
                    list.scrollBy(center - (item.offset + item.size - messageHeight / 2f))
                    highlighted = target.messageId
                    highlight.snapTo(1f)
                }
            }
        } finally {
            centering = false
            onPositioned(target)
        }
        if (highlighted != null) {
            delay(2700)
            highlight.animateTo(0f, tween(300))
            highlighted = null
        }
    }
    LaunchedEffect(timeline.newestId, loadingNewer, position) {
        val anchor = newerAnchor ?: return@LaunchedEffect
        if (position != null) {
            newerAnchor = null
            newerStalled = false
        } else if (!loadingNewer) {
            newerStalled = timeline.newestId == anchor.third
            if (timeline.newestId != anchor.third) {
                val index = rows.indexOfFirst { it.m.id == anchor.first }
                if (index >= 0) {
                    list.scrollToItem(index + 1)
                    val item = snapshotFlow {
                        list.layoutInfo.visibleItemsInfo.firstOrNull { it.key == anchor.first }
                    }.first { it != null }!!
                    list.scrollBy((anchor.second - item.offset).toFloat())
                }
            }
            newerAnchor = null
        }
    }
    // Drawn from the bottom: the newest is item 0, and a message arriving
    // while the reader is at the bottom keeps them there without any help.
    val nearTop by remember(list) { derivedStateOf { (list.layoutInfo.visibleItemsInfo.lastOrNull()?.index ?: 0) >= list.layoutInfo.totalItemsCount - 5 } }
    LaunchedEffect(list, timeline.oldestId, timeline.hasMore, position, centering) {
        snapshotFlow { nearTop }.collect { if (it && position == null && !centering) onOlder() }
    }
    LaunchedEffect(list, timeline.newestId, timeline.hasNewer, position, centering, newerAnchor) {
        snapshotFlow { (list.layoutInfo.visibleItemsInfo.firstOrNull()?.index ?: Int.MAX_VALUE) <= 4 }
            .collect {
                if (!it) newerStalled = false
                if (it && position == null && !centering && timeline.hasNewer && newerAnchor == null && !newerStalled) {
                    list.layoutInfo.visibleItemsInfo.firstOrNull { item -> item.key is Long }?.let { item ->
                        newerAnchor = Triple(item.key as Long, item.offset, timeline.newestId)
                        onNewer()
                    }
                }
            }
    }
    BoxWithConstraints(Modifier.fillMaxSize()) {
        // Space at the two boundaries lets even the first/last hit reach the
        // center. Returning to recent messages restores the usual bottom edge.
        val searchWindow = (position ?: request)?.messageId != null
        LazyColumn(state = list, reverseLayout = true, modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(vertical = if (searchWindow) maxHeight / 2 else 0.dp)) {
            item(key = "bottom") { Spacer(Modifier.height(8.dp)) }
            items(rows, key = { it.m.id }) { r ->
                Column {
                    r.newDay?.let { DayLabel(it) }
                    if (r.firstUnread) UnreadLabel()
                    Box(Modifier.fillMaxWidth().onSizeChanged { messageHeights[r.m.id] = it.height }.drawBehind {
                        if (r.m.id == highlighted) drawRoundRect(AccentSoft.copy(alpha = AccentSoft.alpha * highlight.value),
                            cornerRadius = CornerRadius(8.dp.toPx()))
                    }) {
                        if (r.m.kind == "call") {
                            CallNotice(r.m, me.username, members)
                        } else if (r.m.kind == "pin") {
                            PinNotice(r.m, me.username, members)
                        } else {
                            MessageRow(r, me, members, api, onLong, onReact, onPicture, onPerson)
                        }
                    }
                }
            }
            item(key = "top") {
                Text(
                    when {
                        timeline.hasMore -> if (loadingOlder) "Carregando…" else ""
                        else -> "Começo da conversa."
                    },
                    style = MetaText,
                    color = Muted,
                    modifier = Modifier.fillMaxWidth().padding(vertical = 16.dp),
                    textAlign = TextAlign.Center,
                )
            }
        }
        if (timeline.hasNewer || searchWindow) {
            WebButton("Ir para as recentes", onLatest,
                modifier = Modifier.align(Alignment.BottomCenter).padding(8.dp))
            if (loadingNewer) Text("Carregando…", style = MetaText, color = Muted,
                modifier = Modifier.align(Alignment.TopCenter).island().padding(8.dp))
        }
    }
}

/** Web DayLine: 6dp vertically, 12dp between the label and either rule. */
@Composable
private fun DayLabel(day: String) {
    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Box(Modifier.weight(1f).height(1.dp).background(Color(0xFF28282C)))
        Text(day.uppercase(ptBr), style = LabelText, color = Foreground, maxLines = 1)
        Box(Modifier.weight(1f).height(1.dp).background(Color(0xFF28282C)))
    }
}

/** Where the unread start: a label of 20 in danger soft, on the left, with no line. */
@Composable
private fun UnreadLabel() {
    Row(Modifier.fillMaxWidth().padding(start = 8.dp, top = 16.dp, bottom = 4.dp)) {
        Box(Modifier.height(20.dp).clip(CircleShape).background(DangerSoft).padding(horizontal = 8.dp), contentAlignment = Alignment.Center) {
            Text("NÃO LIDAS", style = LabelText, color = DangerSoftFg)
        }
    }
}

@Composable
private fun CallNotice(m: Message, me: String, members: Map<String, Member>) {
    val mine = m.author.username == me
    val who = nameOf(m.author.username, m.author.displayName, members)
    val rejected = m.body == "rejected"
    val text = when {
        rejected && mine -> "$who recusou a chamada"
        rejected -> "Você recusou a chamada"
        mine -> "Você ligou — ninguém atendeu"
        else -> "Chamada perdida de $who"
    }
    Row(
        Modifier.padding(start = 56.dp, end = 8.dp, top = 8.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Glyph(if (mine) Glyphs.HandsetArrowOut else Glyphs.HandsetArrowIn, null, tint = Muted, size = 16)
        Text(text, style = MetaText, color = if (rejected || mine) Muted else DangerSoftFg)
        Text(clock(m.createdAt), style = MetaText, color = Muted)
    }
}

/** The line a new pin leaves (Messages.tsx PinNotice): the words come from the kind. */
@Composable
private fun PinNotice(m: Message, me: String, members: Map<String, Member>) {
    val who = if (m.author.username == me) "Você" else nameOf(m.author.username, m.author.displayName, members)
    Row(
        Modifier.padding(start = 56.dp, end = 8.dp, top = 8.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Glyph(Glyphs.Pin, null, tint = Muted, size = 16)
        Text("$who fixou uma mensagem.", style = MetaText, color = Muted)
        Text(clock(m.createdAt), style = MetaText, color = Muted)
    }
}

/**
 * The accent bar of 3 on the left edge, the whole height, radius 0 9999 9999 0:
 * "open" and "for you". A bar that goes on into the next line keeps that end square.
 */
internal fun DrawScope.edgeBar(roundTop: Boolean = true, roundBottom: Boolean = true) {
    val w = 3.dp.toPx()
    val r = CornerRadius(w / 2, w / 2)
    drawPath(
        Path().apply {
            addRoundRect(RoundRect(0f, 0f, w, size.height,
                topRightCornerRadius = if (roundTop) r else CornerRadius.Zero,
                bottomRightCornerRadius = if (roundBottom) r else CornerRadius.Zero))
        },
        Accent,
    )
}

/**
 * A chip seen at 32 that takes a press at 48: the target grows to 48, but the
 * layout keeps the 32, so the chips sit where the design puts them. The part
 * of the target outside the 32 still takes the press (nothing above clips it).
 */
internal fun Modifier.seenAt32(): Modifier = layout { m, c ->
    val p = m.measure(c)
    val bleed = ((p.height - 32.dp.roundToPx()) / 2).coerceAtLeast(0)
    layout(p.width, p.height - 2 * bleed) { p.place(0, -bleed) }
}.minimumInteractiveComponentSize()

@OptIn(ExperimentalFoundationApi::class, ExperimentalLayoutApi::class)
@Composable
private fun MessageRow(
    r: Row_,
    me: User,
    members: Map<String, Member>,
    api: () -> Api?,
    onLong: (Message) -> Unit,
    onReact: (Long, String, Boolean) -> Unit,
    onPicture: (Attachment, Message) -> Unit,
    onPerson: (String) -> Unit,
) {
    val m = r.m
    val personal by com.taleshernandes.harmony.model.PersonalPreferences.state.collectAsState()
    var shown by remember(m.id) { mutableStateOf(false) }
    if (personal.any { it.username == m.author.username && it.blocked } && !shown) {
        Row(Modifier.fillMaxWidth().padding(12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Mensagem de pessoa bloqueada", color = Muted, style = MetaText, modifier = Modifier.weight(1f))
            WebButton("Mostrar", { shown = true }, height = 36)
        }
        return
    }
    val mentioned = me.username in m.mentions
    val head = !r.grouped || m.replyTo != null
    val name = nameOf(m.author.username, m.author.displayName, members)
    val haptic = LocalHapticFeedback.current
    val long = { if (!m.deleted) { haptic.performHapticFeedback(HapticFeedbackType.LongPress); onLong(m) } }

    // The gap above a line joined to the mention over it is inside the box.
    val above = if (r.mentionAbove || r.newDay != null) 0.dp else if (head) 12.dp else 0.dp
    Column(Modifier.fillMaxWidth().padding(top = above)) {
        m.replyTo?.let { q ->
            Row(
                Modifier.padding(start = 56.dp, end = 8.dp, bottom = 2.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Glyph(Glyphs.ArrowShapeTurnUpLeft, null, tint = Muted, size = 12)
                Text(nameOf(q.author.username, q.author.displayName, members), style = MetaText.copy(fontWeight = FontWeight.SemiBold), color = Muted, maxLines = 1)
                EmojiRun(
                    if (personal.any { it.username == q.author.username && it.blocked }) "Mensagem de pessoa bloqueada" else if (q.deleted) "Mensagem removida" else q.body.ifEmpty { "Imagem" },
                    api = api, style = MetaText.copy(fontStyle = if (q.deleted) FontStyle.Italic else FontStyle.Normal),
                    color = Muted, maxLines = 1, overflow = TextOverflow.Ellipsis,
                )
            }
        }
        // Tint covers padding, never the block gap. Both rows keep a 48dp
        // avatar column so every message, including a mention, starts at x=64.
        val outer = if (mentioned) Modifier.drawBehind { edgeBar(!r.mentionAbove, !r.mentionBelow) } else Modifier
        val box = if (mentioned) {
            val top = if (r.mentionAbove) 0.dp else 8.dp
            val bottom = if (r.mentionBelow) 0.dp else 8.dp
            Modifier.background(AccentSoft, RoundedCornerShape(top, top, bottom, bottom))
                .padding(horizontal = 8.dp, vertical = 4.dp)
        } else Modifier.padding(horizontal = 8.dp, vertical = 4.dp)
        Box(outer.fillMaxWidth().padding(horizontal = 8.dp)) {
            Row(
                Modifier.fillMaxWidth().then(box)
                    .webCombinedClickable(enabled = !m.deleted, onClick = {}, onLongClick = long)
                    .semantics { contentDescription = "mensagem ${m.id}" },
            ) {
                Box(Modifier.width(48.dp)) {
                    // The face goes to the person; a long press on it is still
                    // a long press on the message. 48 of target around the 36.
                    if (head) Box(
                        Modifier.size(48.dp).webCombinedClickable(onLongClick = long, onClick = { onPerson(m.author.username) })
                            .semantics { contentDescription = "perfil de $name" },
                    ) { Avatar(name, 36, Modifier.padding(top = 6.dp), username = m.author.username) }
                }
                Column(Modifier.weight(1f)) {
                    if (head) {
                        // Name and hour on one baseline, as the design's align-items: baseline.
                        Row(Modifier.height(24.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text(name, color = roleNameColor(m.author.username), style = AuthorText, maxLines = 1, overflow = TextOverflow.Ellipsis,
                                modifier = Modifier.weight(1f, fill = false).alignByBaseline()
                                    .webClickable(role = Role.Button) { onPerson(m.author.username) }
                                    .semantics { contentDescription = name })
                            Text(clock(m.createdAt), style = MetaText, color = Muted, maxLines = 1, modifier = Modifier.alignByBaseline())
                        }
                    }
                    if (m.deleted) {
                        Text("Mensagem removida", style = BodyText, color = Muted, fontStyle = FontStyle.Italic)
                    } else {
                        if (m.body.isNotEmpty() || m.editedAt != null || r.trailingTime) {
                            MessageText(m.body, me.username, m.editedAt != null, if (r.trailingTime) hour(m.createdAt) else null, api)
                        }
                        PictureGrid(m.attachments.filter { it.isImage && !it.missing }, api, onLong = long) { onPicture(it, m) }
                        for (f in m.attachments.filter { !it.isImage && !it.missing }) FileLine(f)
                        for (f in m.attachments.filter { it.missing }) {
                            Text("${f.name} — arquivo perdido", style = MetaText, color = Muted)
                        }
                        for (e in m.embeds) EmbedCard(e, api)
                        if (m.reactions.isNotEmpty()) {
                            FlowRow(Modifier.padding(top = 4.dp), horizontalArrangement = Arrangement.spacedBy(4.dp),
                                verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                for (re in m.reactions) {
                                    val mine = me.username in re.users
                                    // Same 24dp chip and 8dp inset as the web.
                                    Box(contentAlignment = Alignment.Center) {
                                        Row(
                                            Modifier.height(24.dp).clip(Shape8)
                                                .background(if (mine) AccentSoft else Surface2)
                                                .then(if (mine) Modifier.border(1.dp, Accent, Shape8) else Modifier)
                                                .webClickable(role = Role.Button) { onReact(m.id, re.emoji, !mine) }
                                                .padding(horizontal = 8.dp)
                                                .semantics { contentDescription = "reação ${re.emoji} ${re.users.size}" },
                                            verticalAlignment = Alignment.CenterVertically,
                                            horizontalArrangement = Arrangement.spacedBy(4.dp),
                                        ) {
                                            Twemoji(re.emoji, 16.dp, api)
                                            Text("${re.users.size}", style = MetaText.copy(fontWeight = FontWeight.SemiBold),
                                                color = if (mine) AccentSoftFg else Muted)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

/**
 * The body: mentions and links lit, a mention in a box of accent soft with
 * radius 4 drawn behind the text (a span cannot round its background), and
 * the hour at the end of a following line.
 */
@Composable
private fun MessageText(text: String, me: String, edited: Boolean, time: String?, api: () -> Api?) {
    val built = remember(text, me, edited, time) { body(text, me, edited, time) }
    val emoji = remember(built.first) { inlineEmoji(built.first) }
    var layout by remember { mutableStateOf<TextLayoutResult?>(null) }
    Text(
        emoji.first,
        inlineContent = emojiContent(emoji.second, 22f, api),
        // Inline pictures must not grow the web's 24dp line and push every
        // older message upward in this bottom-anchored history.
        style = BodyText.copy(lineHeightStyle = LineHeightStyle(
            LineHeightStyle.Alignment.Center, LineHeightStyle.Trim.None, LineHeightStyle.Mode.Fixed)),
        color = Foreground,
        onTextLayout = { layout = it },
        modifier = Modifier.drawBehind {
            val l = layout ?: return@drawBehind
            // The design's padding 1 4 around the name's own box.
            val dx = 4.dp.toPx()
            val dy = 1.dp.toPx()
            for (range in built.second) {
                if (range.last >= l.layoutInput.text.length) continue
                val a = l.getBoundingBox(range.first)
                val b = l.getBoundingBox(range.last)
                if (l.getLineForOffset(range.first) != l.getLineForOffset(range.last)) continue
                val line = l.getLineForOffset(range.first)
                val top = l.getLineTop(line) + dy
                val bottom = l.getLineBottom(line) - dy
                drawRoundRect(AccentSoft, Offset(a.left - dx, top), Size(b.right - a.left + 2 * dx, bottom - top), CornerRadius(8.dp.toPx()))
            }
        },
    )
}

/** Up to 280 wide, square, gap 4, two to a row. */
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun PictureGrid(pictures: List<Attachment>, api: () -> Api?, onLong: () -> Unit, onOpen: (Attachment) -> Unit) {
    if (pictures.isEmpty()) return
    val maxPx = with(LocalDensity.current) { 280.dp.roundToPx() }
    Column(Modifier.padding(top = 6.dp).widthIn(max = 280.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        for (pair in pictures.chunked(2)) {
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                for (f in pair) {
                    RemotePicture(
                        api = api,
                        url = f.url,
                        maxPx = if (pictures.size == 1) maxPx else maxPx / 2,
                        description = f.name,
                        modifier = Modifier.weight(1f).aspectRatio(1f).clip(Shape8)
                            .webCombinedClickable(onLongClick = onLong, onClick = { onOpen(f) }),
                    )
                }
                if (pair.size == 1 && pictures.size > 1) Spacer(Modifier.weight(1f))
            }
        }
    }
}

private val EmbedTitle = PersonText.copy(fontSize = 15.sp, lineHeight = 22.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 0.sp)

/**
 * The web's link card (Messages.tsx Embeds): 432 at most, the site over the
 * title over the description; a video or a wide picture under them at the
 * card's width, anything else an 80 square at the side. The card opens the
 * page in the browser; nothing plays here.
 */
@Composable
private fun EmbedCard(e: Embed, api: () -> Api?) {
    val uri = LocalUriHandler.current
    val wide = e.image.isNotEmpty() && (e.kind == "video" || e.width >= 400)
    val widePx = with(LocalDensity.current) { 408.dp.roundToPx() }
    val thumbPx = with(LocalDensity.current) { 80.dp.roundToPx() }
    Row(
        Modifier.padding(top = 4.dp).widthIn(max = 432.dp).fillMaxWidth().clip(Shape8).background(Surface2)
            .webClickable(role = Role.Button) { runCatching { uri.openUri(e.url) } }
            .padding(12.dp)
            .semantics { contentDescription = "prévia ${e.title.ifEmpty { e.url }}" },
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            if (e.site.isNotEmpty()) Text(e.site, style = MetaText, color = Muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (e.title.isNotEmpty()) Text(e.title, style = EmbedTitle, color = AccentSoftFg, maxLines = 2, overflow = TextOverflow.Ellipsis)
            if (e.description.isNotEmpty()) Text(e.description, style = MetaText, color = Muted, maxLines = 3, overflow = TextOverflow.Ellipsis)
            if (wide) {
                val ratio = if (e.width > 0 && e.height > 0) (e.width.toFloat() / e.height).coerceAtLeast(408f / 288f) else 16f / 9f
                Box(Modifier.padding(top = 4.dp).fillMaxWidth().aspectRatio(ratio).clip(Shape8).background(Surface3), contentAlignment = Alignment.Center) {
                    RemotePicture(api = api, url = e.image, maxPx = widePx, description = "", modifier = Modifier.fillMaxSize())
                    if (e.kind == "video") Canvas(Modifier.size(56.dp)) {
                        drawCircle(Color.Black.copy(alpha = 0.6f))
                        val u = size.width / 56f
                        drawPath(Path().apply { moveTo(23 * u, 18 * u); lineTo(39 * u, 28 * u); lineTo(23 * u, 38 * u); close() }, Color.White)
                    }
                }
            }
        }
        if (!wide && e.image.isNotEmpty()) {
            RemotePicture(api = api, url = e.image, maxPx = thumbPx, description = "", modifier = Modifier.size(80.dp).clip(Shape8).background(Surface3))
        }
    }
}

@Composable
private fun FileLine(f: Attachment) {
    Row(
        Modifier.padding(top = 6.dp).island().padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Glyph(Glyphs.FileText, null, tint = Muted)
        Column {
            Text(f.name, style = PersonText, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(sizeOf(f.size) + " · abra no navegador para baixar", style = MetaText, color = Muted)
        }
    }
}

@Composable
private fun Lightbox(f: Attachment, author: String, api: () -> Api?, onClose: () -> Unit) {
    val maxPx = with(LocalDensity.current) { 1600.dp.roundToPx() }.coerceAtMost(2048)
    Sheet(onClose, title = author, expanded = true, scroll = false) {
        // Pinch to zoom, drag to look around, double tap for 2x or back; only
        // the X closes (owner, 01/10/2026: a tap on the picture closed it in
        // the middle of looking). Transform only, as everything that moves.
        var scale by remember { mutableFloatStateOf(1f) }
        var offset by remember { mutableStateOf(Offset.Zero) }
        var box by remember { mutableStateOf(IntSize.Zero) }
        fun clamp(o: Offset, k: Float): Offset {
            val mx = box.width * (k - 1f) / 2f
            val my = box.height * (k - 1f) / 2f
            return Offset(o.x.coerceIn(-mx, mx), o.y.coerceIn(-my, my))
        }
        Box(
            Modifier.fillMaxSize().background(Background).onSizeChanged { box = it }
                .pointerInput(Unit) {
                    detectTapGestures(onDoubleTap = { at ->
                        if (scale > 1f) { scale = 1f; offset = Offset.Zero } else {
                            scale = 2f
                            offset = clamp(Offset(box.width / 2f - at.x, box.height / 2f - at.y) * 2f, 2f)
                        }
                    })
                }
                .pointerInput(Unit) {
                    detectTransformGestures { centroid, pan, zoom, _ ->
                        val k = (scale * zoom).coerceIn(1f, 5f)
                        val c = Offset(centroid.x - box.width / 2f, centroid.y - box.height / 2f)
                        // Keep the point under the fingers under the fingers.
                        offset = clamp((offset - c) * (k / scale) + c + pan, k)
                        scale = k
                    }
                },
            contentAlignment = Alignment.Center,
        ) {
            RemotePicture(api, f.url, maxPx, f.name,
                Modifier.fillMaxSize().graphicsLayer {
                    scaleX = scale; scaleY = scale; translationX = offset.x; translationY = offset.y
                }, scale = ContentScale.Fit)

        }
    }
}

@Composable
private fun TypingLine(names: List<String>) {
    if (names.isEmpty()) return
    val text = when (names.size) {
        1 -> "${names[0]} está digitando…"
        2 -> "${names[0]} e ${names[1]} estão digitando…"
        else -> "${names.size} pessoas estão digitando…"
    }
    // An idle channel has no reserved row: the web lets history reach the composer.
    Text(text, style = MetaText, color = Muted, maxLines = 1, overflow = TextOverflow.Ellipsis,
        modifier = Modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, bottom = 4.dp))
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun Composer(
    channel: Channel,
    draft: String,
    replyTo: Message?,
    editing: Message?,
    pending: List<Attachment>,
    uploading: Int,
    members: Map<String, Member>,
    sending: Boolean,
    api: () -> Api?,
    actions: ChatActions,
    onCancelReply: () -> Unit,
    onCancelEdit: () -> Unit,
    onSent: () -> Unit,
) {
    // The box's text is the draft, kept by the app; editing borrows the box
    // and gives the draft back when it is done.
    var value by remember(channel.id) { mutableStateOf(TextFieldValue(draft, TextRange(draft.length))) }
    // An edit is saved or not; a message is sending or not — that one is
    // kept by the app, so leaving and coming back cannot send it twice.
    var saving by remember(channel.id) { mutableStateOf(false) }
    val busy = saving || sending
    // What was in the box when an edit borrowed it, given back afterwards.
    // Changed only when editing starts or ends, never on the first frame:
    // an effect that sets the box on mount eats what is typed before it runs
    // (the web's D4l).
    var beforeEdit by remember(channel.id) { mutableStateOf<TextFieldValue?>(null) }
    var editingId by remember(channel.id) { mutableStateOf<Long?>(null) }
    if (editing?.id != editingId) {
        val e = editing
        if (e != null) {
            if (beforeEdit == null) beforeEdit = value
            value = TextFieldValue(e.body, TextRange(e.body.length))
        } else {
            value = beforeEdit ?: value
            beforeEdit = null
        }
        editingId = e?.id
    }
    var emoji by remember { mutableStateOf(false) }
    val people = members.values.toList()
    val mention = remember(value) { mentionAt(value) }
    val suggestions = remember(mention, people) {
        val q = mention?.second?.lowercase() ?: return@remember emptyList()
        people.filter { it.username.lowercase().startsWith(q) || it.displayName.lowercase().startsWith(q) }.take(5)
    }
    val place = if (channel.isDm) "Conversar com ${channel.peer?.let { nameOf(it.username, it.displayName, members) }?.substringBefore(' ') ?: channel.name}"
        else "Escrever em #${channel.name}"
    fun put(next: TextFieldValue) {
        value = next
        if (editing == null) actions.onDraft(next.text)
    }

    Box(Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, bottom = 16.dp)) {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            if (suggestions.isNotEmpty()) {
                Column(Modifier.fillMaxWidth().island().padding(4.dp)) {
                    for (s in suggestions) {
                        val shownName = s.nickname?.ifEmpty { null } ?: s.displayName.ifEmpty { s.username }
                        Row(
                            Modifier.fillMaxWidth().height(48.dp).clip(Shape8).webClickable {
                                val (start, _) = mention!!
                                val t = value.text
                                val end = value.selection.start
                                val next = t.substring(0, start) + "@" + s.username + " " + t.substring(end)
                                put(TextFieldValue(next, TextRange(start + s.username.length + 2)))
                            }.padding(horizontal = 12.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(10.dp),
                        ) {
                            Avatar(shownName, 24, username = s.username)
                            Text(shownName, style = ItemText, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                            Text("@${s.username}", style = MetaText, color = Muted, maxLines = 1)
                        }
                    }
                }
            }
            Column(Modifier.fillMaxWidth().island().padding(6.dp)) {
                if (replyTo != null || editing != null) {
                    Row(
                        Modifier.fillMaxWidth().height(44.dp).padding(start = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        // TODO(design): the kit has no pencil; editing goes without a glyph.
                        if (editing == null) Glyph(Glyphs.ArrowShapeTurnUpLeft, null, tint = Muted, size = 16)
                        Text(
                            if (editing != null) AnnotatedString("Editando a mensagem")
                            else buildAnnotatedString {
                                append("Respondendo a ")
                                withStyle(SpanStyle(color = Foreground, fontWeight = FontWeight.SemiBold)) {
                                    append(nameOf(replyTo!!.author.username, replyTo.author.displayName, members))
                                }
                            },
                            style = MetaText, color = Muted, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis,
                        )
                        Box(
                            Modifier.size(44.dp).clip(Shape8).webClickable(role = Role.Button) { if (editing != null) onCancelEdit() else onCancelReply() }
                                .semantics { contentDescription = "cancelar" },
                            contentAlignment = Alignment.Center,
                        ) { Glyph(Glyphs.Xmark, null, tint = Muted, size = 16) }
                    }
                }
                if (pending.isNotEmpty() || uploading > 0) {
                    Row(
                        Modifier.fillMaxWidth().padding(start = 8.dp, end = 8.dp, top = 4.dp, bottom = 4.dp),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        for (f in pending) {
                            // The whole picture is the target that takes it out.
                            Box(
                                Modifier.size(56.dp).clip(Shape8).webClickable(role = Role.Button) { actions.onUnattach(f.id) }
                                    .semantics { contentDescription = "tirar ${f.name}" },
                            ) {
                                if (f.isImage) {
                                    RemotePicture(api, f.url, 160, f.name, Modifier.matchParentSize())
                                } else {
                                    Box(Modifier.matchParentSize().background(Surface2), contentAlignment = Alignment.Center) {
                                        Glyph(Glyphs.FileText, null, tint = Muted)
                                    }
                                }
                                Box(
                                    Modifier.align(Alignment.TopEnd).padding(4.dp).size(20.dp).clip(CircleShape).background(Background),
                                    contentAlignment = Alignment.Center,
                                ) { Glyph(Glyphs.Xmark, null, size = 12) }
                            }
                        }
                        if (uploading > 0) Text("Enviando $uploading…", style = MetaText, color = Muted)
                    }
                }
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    if (editing == null) {
                        WebIcon(Glyphs.File, "mandar arquivo", actions.onAttach, target = 36, size = 16, tint = Muted, background = Color.Transparent)
                    } else {
                        Spacer(Modifier.width(8.dp))
                    }
                    WebIcon(Glyphs.FaceSmile, "emoji", { emoji = !emoji }, target = 36, size = 16, tint = Muted, background = Color.Transparent)
                    Box(Modifier.weight(1f).heightIn(min = 36.dp).padding(horizontal = 4.dp, vertical = 6.dp)) {
                        if (value.text.isEmpty()) {
                            Text(place, style = BodyText, color = Muted, maxLines = 1, overflow = TextOverflow.Clip)
                        }
                        BasicTextField(
                            value = value,
                            onValueChange = { v ->
                                val grew = v.text != value.text
                                value = if (v.text.length > Conversations.MAX_BODY) v.copy(text = v.text.take(Conversations.MAX_BODY)) else v
                                if (grew && editing == null) {
                                    actions.onDraft(value.text)
                                    if (value.text.isNotEmpty()) actions.onTyped()
                                }
                            },
                            textStyle = BodyText.copy(color = Foreground),
                            cursorBrush = SolidColor(Foreground),
                            maxLines = 6,
                            keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Sentences),
                            modifier = Modifier.fillMaxWidth().semantics { contentDescription = "escrever mensagem" },
                        )
                    }
                    val filled = value.text.isNotBlank() || (editing == null && pending.isNotEmpty())
                    val canSend = !busy && (editing != null || uploading == 0) && filled
                    WebButton(
                        if (editing != null) "Salvar" else "Enviar",
                        {
                            val e = editing
                            if (e != null) {
                                saving = true
                                actions.onEdit(e.id, value.text) { ok ->
                                    saving = false
                                    if (ok) onCancelEdit()
                                }
                            } else {
                                val sent = value.text
                                actions.onSend(sent, replyTo?.id) { ok ->
                                    if (ok) {
                                        // Only what was sent: a next line typed
                                        // while this one travelled stays.
                                        if (value.text == sent) value = TextFieldValue("")
                                        onSent()
                                    }
                                }
                            }
                        },
                        height = 36,
                        background = if (canSend) Accent else Selected,
                        enabled = canSend,
                    )
                }
            }
        }
        if (emoji) {
            // The same insertion path as before: replacing the selection preserves
            // the draft and puts the cursor immediately after the chosen emoji.
            EmojiPanel(api = api, onClose = { emoji = false }, onPick = { e ->
                val t = value.text
                val s = value.selection
                val next = t.substring(0, s.min) + e + t.substring(s.max)
                put(TextFieldValue(next, TextRange(s.min + e.length)))
                emoji = false
            })
        }
    }
}

/** The "@nam" being typed at the cursor, as (where the @ is, what follows it). */
private fun mentionAt(v: TextFieldValue): Pair<Int, String>? {
    val end = v.selection.start
    if (end != v.selection.end || end == 0) return null
    val t = v.text
    var i = end - 1
    while (i >= 0 && (t[i].isLetterOrDigit() || t[i] == '_' || t[i] == '.' || t[i] == '-')) i--
    if (i < 0 || t[i] != '@') return null
    if (i > 0 && !t[i - 1].isWhitespace()) return null
    return i to t.substring(i + 1, end)
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun MessageSheet(
    m: Message,
    me: User,
    api: () -> Api?,
    onDismiss: () -> Unit,
    onReact: (String) -> Unit,
    onReply: () -> Unit,
    onEdit: () -> Unit,
    onRemove: () -> Unit,
    onPin: (() -> Unit)? = null,
    canRemove: Boolean = false,
) {
    val context = LocalContext.current
    var picker by remember(m.id) { mutableStateOf(false) }
    Sheet(onDismiss = onDismiss, title = "Mensagem") {
        val close = LocalPanelDismiss.current
        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            // The quick ones, and whatever is already on the message: pressing
            // one that is yours takes it back, the same as the chip under it.
            val quick = (QUICK + m.reactions.map { it.emoji }).distinct()
            FlowRow(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                for (e in quick) {
                    val mine = m.reactions.any { it.emoji == e && me.username in it.users }
                    Box(
                        Modifier.size(48.dp).clip(Shape8).background(if (mine) AccentSoft else Surface2)
                            .webClickable(role = Role.Button, hover = Selected) { close { onReact(e); onDismiss() } }.semantics { contentDescription = e },
                        contentAlignment = Alignment.Center,
                    ) { Twemoji(e, 24.dp, api) }
                }
            }
            // Keep all actions together; only the emoji strip has an 8dp gap.
            Column {
                Box {
                    MessageSheetAction("Reagir", "reagir", { picker = true }, Glyphs.FaceSmile)
                    if (picker) EmojiPanel(api = api, onClose = { picker = false }, onPick = {
                        close { onReact(it); picker = false; onDismiss() }
                    })
                }
                onPin?.let { pin -> MessageSheetAction(if (m.pinned) "Desafixar" else "Fixar", "fixar mensagem", { close { pin(); onDismiss() } }, Glyphs.Pin) }
                MessageSheetAction("Responder", "responder", { close { onReply(); onDismiss() } }, Glyphs.ArrowShapeTurnUpLeft)
                if (m.body.isNotEmpty()) {
                    MessageSheetAction("Copiar texto", "copiar texto", {
                        context.getSystemService(ClipboardManager::class.java)?.setPrimaryClip(ClipData.newPlainText("mensagem", m.body))
                        close { onDismiss() }
                    }, Glyphs.Copy)
                }
                if (m.author.username == me.username) MessageSheetAction("Editar", "editar", { close { onEdit(); onDismiss() } }, Glyphs.Pencil)
                if (canRemove) {
                    MessageSheetAction("Remover", "remover", { close { onRemove(); onDismiss() } }, Glyphs.TrashBin, tint = DangerSoftFg)
                }
            }
        }
    }
}

@Composable
private fun MessageSheetAction(text: String, label: String, onClick: () -> Unit,
                               icon: androidx.compose.ui.graphics.vector.ImageVector? = null, tint: Color = Foreground) {
    Row(Modifier.fillMaxWidth().height(56.dp).clip(Shape8).webClickable(role = Role.Button, hover = if (tint == DangerSoftFg) DangerSoft else Surface3, onClick = onClick)
        .padding(horizontal = 12.dp).semantics { contentDescription = label },
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
        icon?.let { Glyph(it, null, tint) }
        Text(text, style = ItemText, color = tint)
    }
}

// ------------------------------------------------------------ text

private val QUICK = listOf("👍", "❤️", "😂", "😮", "😢", "🔥", "🎉", "👀")
private val MENTION = Regex("@([A-Za-z0-9_.-]+)")
private val LINK = Regex("https?://[^\\s<>\"]+")

/**
 * Mentions lit the web's way (Messages.tsx Body) and links pressable; the
 * ranges of the mentions come back so their box can be drawn behind them.
 */
private fun body(text: String, me: String, edited: Boolean, time: String?): Pair<AnnotatedString, List<IntRange>> {
    val boxes = ArrayList<IntRange>()
    val s = buildAnnotatedString {
        data class Span(val range: IntRange, val link: Boolean, val name: String)
        val spans = (MENTION.findAll(text).map { Span(it.range, false, it.groupValues[1]) } +
            LINK.findAll(text).map { Span(it.range, true, it.value.trimEnd('.', ',', ')', '!', '?')) })
            .sortedBy { it.range.first }
        var at = 0
        for (sp in spans) {
            if (sp.range.first < at) continue
            append(text.substring(at, sp.range.first))
            if (sp.link) {
                withLink(LinkAnnotation.Url(sp.name, TextLinkStyles(SpanStyle(color = AccentSoftFg, textDecoration = TextDecoration.Underline)))) {
                    append(sp.name)
                }
                at = sp.range.first + sp.name.length
            } else {
                val start = length
                val mine = sp.name.equals(me, ignoreCase = true) || sp.name.equals("todos", ignoreCase = true)
                withStyle(SpanStyle(color = if (mine) Foreground else AccentSoftFg, fontWeight = FontWeight.Medium)) { append("@" + sp.name) }
                boxes += start until length
                at = sp.range.last + 1
            }
        }
        append(text.substring(at))
        if (edited) {
            withStyle(SpanStyle(color = Muted, fontSize = 13.sp)) { append(" (editada)") }
        }
        if (time != null) {
            withStyle(SpanStyle(color = Muted, fontSize = 13.sp)) { append((if (length > 0) " " else "") + time) }
        }
    }
    return s to boxes
}

internal fun nameOf(username: String, displayName: String, members: Map<String, Member>): String =
    members[username]?.nickname?.ifEmpty { null } ?: displayName.ifEmpty { username }

private fun hour(ms: Long): String = DateFormat.format("HH:mm", Calendar.getInstance().apply { timeInMillis = ms }).toString()

/** The hour for today, "Ontem" for yesterday, the date before that (Messages.tsx clock). */
private fun clock(ms: Long): String {
    val c = Calendar.getInstance().apply { timeInMillis = ms }
    val time = DateFormat.format("HH:mm", c).toString()
    val today = Calendar.getInstance()
    val yesterday = Calendar.getInstance().apply { add(Calendar.DAY_OF_YEAR, -1) }
    return when {
        sameDay(c, today) -> time
        sameDay(c, yesterday) -> "Ontem $time"
        else -> DateFormat.format("dd/MM", c).toString() + " " + time
    }
}

private fun sameDay(a: Calendar, b: Calendar) =
    a.get(Calendar.YEAR) == b.get(Calendar.YEAR) && a.get(Calendar.DAY_OF_YEAR) == b.get(Calendar.DAY_OF_YEAR)

/** "quinta-feira, 11 de setembro", the measured web day label. */
private fun dayOf(ms: Long): String =
    java.text.SimpleDateFormat("EEEE, dd 'de' MMMM", ptBr).format(java.util.Date(ms))

private fun sizeOf(bytes: Long): String = when {
    bytes >= 1 shl 20 -> String.format(ptBr, "%.1f MB", bytes / 1048576.0)
    bytes >= 1024 -> "${bytes / 1024} KB"
    else -> "$bytes B"
}
