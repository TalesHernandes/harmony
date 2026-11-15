package com.taleshernandes.harmony.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.taleshernandes.harmony.model.Message
import com.taleshernandes.harmony.net.*
import com.taleshernandes.harmony.diag.Diag
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.CancellationException

@Composable
internal fun PinsPanel(channelId: String, revision: Long, api: () -> Api?, onJump: (Long) -> Unit, onClose: () -> Unit) {
    val personal by com.taleshernandes.harmony.model.PersonalPreferences.state.collectAsState()
    var messages by remember(channelId) { mutableStateOf<List<Message>>(emptyList()) }
    var error by remember(channelId) { mutableStateOf<String?>(null) }
    var loading by remember(channelId) { mutableStateOf(true) }
    val client = api()
    LaunchedEffect(channelId, revision, client) {
        try {
            error = null
            messages = withContext(Dispatchers.IO) { (client ?: throw Unauthorized()).pins(channelId) }
        } catch (e: CancellationException) { throw e
        } catch (e: Exception) {
            Diag.log("pins: error ${if (e is ApiError) "${e.status} ${e.reason}" else e.javaClass.simpleName}")
            error = channelActionError(e)
        } finally { loading = false }
    }
    Sheet(onClose, title = "Mensagens fixadas", expanded = true, scroll = false) {
        val close = LocalPanelDismiss.current
        Text("Até 250 mensagens por canal.", style = MetaText, color = Muted, modifier = Modifier.padding(bottom = 12.dp))
        if (loading) Text("Carregando…", style = BodyText)
        error?.let { Text(it, style = BodyText, color = DangerSoftFg) }
        if (!loading && error == null && messages.isEmpty()) Text("Nenhuma mensagem fixada.", style = BodyText, color = Muted)
        LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            items(messages, key = { it.id }) { m ->
                Column(Modifier.fillMaxWidth().island(Surface2).clickable { close { onJump(m.id); onClose() } }.padding(12.dp)) {
                    Text(m.author.displayName.ifEmpty { m.author.username }, style = ItemText)
                    Text(if (personal.any { it.username == m.author.username && it.blocked }) "Mensagem de pessoa bloqueada" else m.body.ifEmpty { if (m.attachments.isNotEmpty()) "Anexo" else "Mensagem" }, style = BodyText, maxLines = 5)
                }
            }
        }
    }
}
