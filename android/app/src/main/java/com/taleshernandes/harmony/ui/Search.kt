package com.taleshernandes.harmony.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.taleshernandes.harmony.Conversations
import com.taleshernandes.harmony.model.Channel
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.SearchHit
import com.taleshernandes.harmony.net.Unauthorized
import com.taleshernandes.harmony.diag.Diag
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.Date

/** The web searches the server's full history and opens the hit's conversation. */
@Composable
internal fun SearchPanel(state: Conversations.State, channels: List<Channel>, api: () -> Api?, onOpen: (Channel, Long) -> Unit, onClose: () -> Unit) {
    val personal by com.taleshernandes.harmony.model.PersonalPreferences.state.collectAsState()
    var query by remember { mutableStateOf("") }
    var hits by remember { mutableStateOf<List<SearchHit>>(emptyList()) }
    var looking by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf("") }
    val client = api()
    LaunchedEffect(query, client) {
        hits = emptyList()
        error = ""
        val text = query.trim()
        looking = text.length >= 2
        if (!looking) return@LaunchedEffect
        delay(250)
        try {
            // withContext checks cancellation on return even though OkHttp's
            // synchronous call is blocking: an old answer can never win.
            hits = withContext(Dispatchers.IO) { (client ?: throw Unauthorized()).search(text) }
            looking = false
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Diag.log("search failed: ${e.javaClass.simpleName}")
            error = accountError(e)
            looking = false
        }
    }
    Sheet(onClose, title = "Procurar", expanded = true, scroll = false) {
        val close = LocalPanelDismiss.current
        SearchField(query, { query = it })
        val status = when {
            query.trim().length < 2 -> "Digite pelo menos duas letras."
            looking -> "Procurando…"
            error.isNotEmpty() -> error
            hits.isEmpty() -> "Nada com essa palavra."
            else -> ""
        }
        if (status.isNotEmpty()) Text(status,
            style = if (!looking && error.isEmpty() && query.trim().length >= 2) ItemText else MetaText,
            color = if (error.isNotEmpty()) DangerSoftFg else Muted,
            // Starts where the typed words start: the field's 12, the 20 glyph and its 12 (owner, 02/10/2026).
            modifier = Modifier.padding(start = 44.dp, end = 12.dp, top = 12.dp, bottom = 12.dp))
        Spacer(Modifier.height(8.dp))
        LazyColumn(Modifier.weight(1f, fill = false), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            items(hits, key = { it.message.id }) { result ->
                val hit = result.message
                val channel = result.channel ?: channels.firstOrNull { it.id == hit.channelId }
                Column(Modifier.fillMaxWidth().island(Surface2).clickable(enabled = channel != null, role = Role.Button) {
                    channel?.let { close { onOpen(it, hit.id); onClose() } }
                }.padding(12.dp).semantics { contentDescription = "resultado ${hit.id}" }) {
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Row(Modifier.weight(1f).alignByBaseline(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text(channel?.let {
                                if (it.isDm) it.peer?.let { peer -> nameOf(peer.username, peer.displayName, state.members) } ?: it.name
                                else "#${it.name}"
                            }.orEmpty(), style = MetaText.copy(fontWeight = FontWeight.SemiBold), maxLines = 1,
                                overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
                                modifier = Modifier.weight(1f, fill = false).alignByBaseline())
                            Text(nameOf(hit.author.username, hit.author.displayName, state.members), style = MetaText, color = Muted,
                                maxLines = 1, overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
                                modifier = Modifier.weight(1f, fill = false).alignByBaseline())
                        }
                        Text(remember(hit.createdAt) { SimpleDateFormat("dd/MM/yy", ptBr).format(Date(hit.createdAt)) },
                            style = MetaText, color = Muted, modifier = Modifier.alignByBaseline())
                    }
                    EmojiRun(if (personal.any { it.username == hit.author.username && it.blocked }) "Mensagem de pessoa bloqueada" else hit.body, api, BodyText, Modifier.padding(top = 2.dp))
                }
            }
        }
    }
}

/** Same field as the web, including taking focus once when the panel opens. */
@Composable
private fun SearchField(value: String, onValue: (String) -> Unit) {
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    BasicTextField(value, onValue, singleLine = true, textStyle = BodyText.copy(color = Foreground),
        cursorBrush = SolidColor(Foreground),
        modifier = Modifier.fillMaxWidth().height(48.dp).clip(Shape8).background(Surface2)
            .focusRequester(focus).semantics { contentDescription = "procurar mensagens" }, decorationBox = { input ->
            Row(Modifier.fillMaxSize().padding(horizontal = 12.dp), verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Glyph(Glyphs.Magnifier, null, size = 20)
                Box(Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
                    if (value.isEmpty()) Text("Uma palavra que estava na mensagem", style = BodyText, color = Muted,
                        maxLines = 1, overflow = androidx.compose.ui.text.style.TextOverflow.Clip)
                    input()
                }
            }
        })
}
