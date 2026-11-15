package com.taleshernandes.harmony.ui

import androidx.compose.runtime.collectAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.sp
import androidx.compose.ui.unit.dp
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.media.Volumes
import com.taleshernandes.harmony.model.Member
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.Unauthorized
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.roundToInt

internal fun personStateLabel(state: String): String = when (state) {
    "online" -> "Online"
    "away" -> "Ausente"
    "in_call" -> "Em chamada"
    else -> "Offline"
}

/**
 * The person's panel (design screen 5), in the sheet. A null action means
 * unavailable; the host checks room access before supplying it.
 */
@Composable
fun PersonSheet(
    person: Member,
    me: com.taleshernandes.harmony.model.User?,
    roles: List<com.taleshernandes.harmony.model.Role> = LocalRoles.current.roles,
    isMe: Boolean,
    state: String,
    api: () -> Api?,
    onNickname: (String) -> Unit,
    inMyRoom: Boolean,
    sharing: Boolean,
    roomName: String?,
    voiceVolume: Float,
    screenVolume: Float,
    onVolume: (screen: Boolean, level: Float) -> Unit,
    onClose: () -> Unit,
    onMessage: (() -> Unit)? = null,
    onCall: (() -> Unit)? = null,
    onJoinRoom: (() -> Unit)? = null,
    onWatch: (() -> Unit)? = null,
    /** Already watching this person's screen: the same place stops it. */
    watching: Boolean = false,
) {
    WebModal(onClose, title = "Perfil") {
        key(person.username) {
            var saved by remember(person.nickname) { mutableStateOf(person.nickname.orEmpty()) }
            var draft by rememberSaveable(person.nickname) { mutableStateOf(person.nickname.orEmpty()) }
            var busy by remember { mutableStateOf(false) }
            var error by remember { mutableStateOf("") }
            val scope = rememberCoroutineScope()
            val currentApi by rememberUpdatedState(api)
            val currentNickname by rememberUpdatedState(onNickname)
            val name = (if (isMe) "" else saved).ifEmpty { person.displayName.ifEmpty { person.username } }
            val changed = draft.trim() != saved
            fun save() {
                if (busy || !changed) return
                busy = true
                val submitted = draft.trim()
                scope.launch {
                    try {
                        val result = withContext(Dispatchers.IO) {
                            (currentApi() ?: throw Unauthorized()).nickname(person.username, submitted)
                        }
                        saved = result
                        draft = result
                        error = ""
                        currentNickname(result)
                    } catch (e: CancellationException) { throw e } catch (e: Exception) {
                        error = "Não salvou: " + accountError(e)
                        Diag.log("nickname: ${e.javaClass.simpleName}")
                    } finally { busy = false }
                }
            }
            Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                // The face centred on the three lines beside it (owner, 02/10/2026), and a
                // tap on a photo shows it big.
                var big by remember { mutableStateOf(false) }
                val versions by com.taleshernandes.harmony.model.Avatars.versions.collectAsState()
                val hasPhoto = !versions[person.username].isNullOrEmpty()
                if (big) BigPhoto(api, person.username, name) { big = false }
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    Avatar(person.displayName.ifEmpty { person.username }, 56, face = Selected, username = person.username,
                        modifier = if (hasPhoto) Modifier.clip(CircleShape).clickable(role = Role.Button) { big = true }
                            .semantics { contentDescription = "ver a foto de $name" } else Modifier)
                    Column(Modifier.weight(1f)) {
                        Text(name, color = roleColor(com.taleshernandes.harmony.model.colorOf(person.username, roles)), style = AuthorText.copy(fontSize = 18.sp), maxLines = 1, overflow = TextOverflow.Ellipsis)
                        // Keep their real name beside your private nickname so
                        // a renamed friend remains identifiable.
                        Text("@${person.username}" + if (saved.isNotEmpty()) " · ${person.displayName}" else "",
                            style = MetaText, color = Muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Row(Modifier.padding(top = 4.dp), horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
                            Box(Modifier.size(8.dp).clip(CircleShape).background(presenceColor(state)))
                            Text(personStateLabel(state), style = MetaText, color = Muted)
                        }
                    }
                }
                RoleChips(person.username, person.isAdmin, me, roles, api)
                Text(if (roomName.isNullOrEmpty()) "Não está em nenhuma sala de voz." else "Em $roomName" + if (sharing) " — compartilhando a tela" else "",
                    style = MetaText, color = Muted)
                if (!isMe) {
                    PersonalActions(person.username, api)
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        onMessage?.let { WebButton("Conversar no privado", it, Modifier.fillMaxWidth(), icon = Glyphs.Comment,
                            centered = false, padding = 12, gap = 12,
                            style = PersonText.copy(fontSize = 14.sp, lineHeight = 21.sp, letterSpacing = 0.sp)) }
                        if (sharing) onWatch?.let { WebButton(if (watching) "Parar de assistir" else "Assistir à tela de $name", it,
                            Modifier.fillMaxWidth(), icon = Glyphs.Display, centered = false, padding = 12, gap = 12) }
                        if (!roomName.isNullOrEmpty()) onJoinRoom?.let { WebButton("Entrar em $roomName", it, Modifier.fillMaxWidth(),
                            icon = Glyphs.Headphones, centered = false, padding = 12, gap = 12) }
                    }
                    if (inMyRoom) {
                        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            SectionLabel("volume — só para você", Modifier.padding(bottom = 4.dp))
                            VolumeLine(Glyphs.Volume, "Voz", voiceVolume) { onVolume(false, it) }
                            if (sharing) VolumeLine(Glyphs.Display, "Tela", screenVolume, "Som da tela") { onVolume(true, it) }
                        }
                    }
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        SectionLabel("Como você chama essa pessoa")
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                            Field(draft, { if (it.codePointCount(0, it.length) <= 32) draft = it }, "Como você chama essa pessoa",
                                Modifier.weight(1f), placeholder = person.displayName, enabled = !busy, height = 40,
                                style = BodyText.copy(fontSize = 15.sp, lineHeight = 22.sp, letterSpacing = (-0.225).sp),
                                options = KeyboardOptions(imeAction = ImeAction.Done), actions = KeyboardActions(onDone = { save() }))
                            WebButton("Salvar", { save() }, height = 40, padding = 12, enabled = !busy && changed, background = Surface2,
                                style = MetaText.copy(fontWeight = FontWeight.Medium))
                        }
                        Text("Só você vê esse nome.", style = MetaText, color = Muted)
                        if (error.isNotEmpty()) Text(error, style = MetaText, color = DangerSoftFg)
                    }
                }
            }
        }
    }
}

/**
 * One volume, for this person, for you: a line of 44, the slider's box its
 * target. Steps of 5 with a light tick at every 10; the host keeps and
 * applies it through Volumes.
 */
@Composable
private fun VolumeLine(icon: ImageVector, label: String, level: Float, description: String = label, onChange: (Float) -> Unit) {
    val max = Volumes.MAX
    var value by remember(level) { mutableFloatStateOf(level.coerceIn(0f, max)) }
    var trackWidth by remember { mutableIntStateOf(0) }
    val haptic = LocalHapticFeedback.current
    val currentChange by rememberUpdatedState(onChange)
    fun pick(target: Float) {
        val stepped = ((target.coerceIn(0f, max) / STEP).roundToInt() * STEP).coerceIn(0f, max)
        if (stepped == value) return
        if ((stepped * 100).roundToInt() / 10 != (value * 100).roundToInt() / 10) {
            haptic.performHapticFeedback(HapticFeedbackType.TextHandleMove)
        }
        value = stepped
        currentChange(stepped)
    }
    Row(Modifier.fillMaxWidth().padding(top = 8.dp).height(44.dp).island(Surface2).padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(label, style = MetaText, modifier = Modifier.width(64.dp), maxLines = 1)
        Box(
            Modifier.weight(1f).height(44.dp)
                .onSizeChanged { trackWidth = it.width }
                .pointerInput(max) {
                    awaitEachGesture {
                        val down = awaitFirstDown()
                        if (size.width > 0) pick(down.position.x / size.width * max)
                        drag(down.id) { change ->
                            if (size.width > 0) pick(change.position.x / size.width * max)
                            change.consume()
                        }
                    }
                }
                .semantics {
                    contentDescription = "volume $description"
                    progressBarRangeInfo = ProgressBarRangeInfo(value, 0f..max, (max / STEP).roundToInt() - 1)
                    setProgress { pick(it); true }
                },
            contentAlignment = Alignment.CenterStart,
        ) {
            Box(Modifier.fillMaxWidth().height(6.dp).clip(CircleShape).background(Surface3)) {
                Box(Modifier.fillMaxWidth(value / max).height(6.dp).clip(CircleShape).background(Accent))
            }
            Box(
                Modifier.offset { IntOffset(((trackWidth - 16.dp.toPx()) * (value / max)).roundToInt(), 0) }
                    .size(16.dp).clip(androidx.compose.foundation.shape.RoundedCornerShape(6.dp)).background(Foreground),
            )
        }
        Text("${(value * 100).roundToInt()}%", style = MetaText, color = Muted, textAlign = TextAlign.End,
            modifier = Modifier.width(40.dp).clickable(role = Role.Button) { pick(1f) }, maxLines = 1)
    }
}

private const val STEP = 0.05f
