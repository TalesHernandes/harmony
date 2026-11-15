package com.taleshernandes.harmony.ui

import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.taleshernandes.harmony.model.*
import com.taleshernandes.harmony.net.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

internal fun channelActionError(e: Exception): String = when ((e as? ApiError)?.reason) {
    "stale_layout" -> "A lista de canais mudou. Ela foi recarregada; tente de novo."
    "pin_limit" -> "Este canal já tem 250 mensagens fixadas. Desafixe uma para fixar outra."
    else -> accountError(e)
}

private data class Naming(val title: String, val path: String, val method: String, val kind: String = "", val initial: String = "")

@Composable
internal fun ChannelTools(state: RoomsState, api: () -> Api?, refresh: () -> Unit,
    channel: Channel?, category: Category?, organizing: Boolean, onDismiss: () -> Unit) {
    val scope = rememberCoroutineScope()
    var naming by remember { mutableStateOf<Naming?>(null) }
    var deleting by remember { mutableStateOf<Pair<String, String>?>(null) }
    var moving by remember { mutableStateOf<Channel?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    var draft by remember(organizing) { mutableStateOf(ChannelLayout.from(state)) }
    fun act(method: String, path: String, body: JSONObject = JSONObject(), done: () -> Unit = {}) {
        if (busy || !state.canManage) return
        busy = true
        scope.launch {
            try {
                withContext(Dispatchers.IO) { (api() ?: throw Unauthorized()).organize(method, path, body) }
                refresh(); done()
            } catch (e: CancellationException) { throw e
            } catch (e: Exception) {
                if ((e as? ApiError)?.reason == "stale_layout") {
                    moving = null; naming = null; deleting = null
                    refresh(); onDismiss()
                }
                error = channelActionError(e)
            } finally { busy = false }
        }
    }
    LaunchedEffect(state.canManage) {
        if (!state.canManage) { naming = null; deleting = null; moving = null; onDismiss() }
    }
    if (state.canManage && channel != null && naming == null && deleting == null && moving == null) {
        Sheet(onDismiss, title = channel.name) {
            SheetRow("Renomear canal", { naming = Naming("Renomear canal", "/api/channels/${channel.id}", "PATCH", initial = channel.name) })
            SheetRow("Mover para pasta…", { moving = channel })
            SheetRow("Apagar canal", { deleting = "/api/channels/${channel.id}" to
                if (channel.isVoice) "A sala some para todo mundo." else "O canal some para todo mundo, com as mensagens. Elas não são apagadas do servidor: se foi engano, dá para trazer de volta." })
        }
    }
    if (state.canManage && category != null && naming == null && deleting == null) {
        Sheet(onDismiss, title = category.name) {
            SheetRow("Renomear categoria", { naming = Naming("Renomear categoria", "/api/categories/${category.id}", "PATCH", initial = category.name) })
            SheetRow("Apagar categoria", { deleting = "/api/categories/${category.id}" to "Apagar a categoria não apaga canal nenhum. Os canais vão para o topo." })
        }
    }
    if (state.canManage && organizing && naming == null) {
        Sheet(onDismiss, title = "Organizar canais", expanded = true, scroll = false) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                WebButton("Nova categoria", { naming = Naming("Nova categoria", "/api/categories", "POST") }, enabled = !busy)
                WebButton("Novo canal", { naming = Naming("Novo canal", "/api/channels", "POST", "text") }, enabled = !busy)
            }
            Text("Arraste pelas alças para mudar a ordem. Para trocar de pasta, use o menu do canal.", style = MetaText, color = Muted, modifier = Modifier.padding(vertical = 12.dp))
            LazyColumn(Modifier.weight(1f)) {
                item { Text("Pastas", style = ColumnLabelText, color = Muted) }
                items(draft.categories, key = { "folder:" + it.id }) { folder ->
                    DragLine(folder.name, !busy) { step ->
                        val from = draft.categories.indexOfFirst { it.id == folder.id }
                        val to = from + step
                        if (to in draft.categories.indices) draft = draft.reorder(from, to, true)
                    }
                }
                for (folder in listOf<Category?>(null) + draft.categories) {
                    item { Text(folder?.name ?: "Sem categoria", style = ColumnLabelText, color = Muted, modifier = Modifier.padding(top = 12.dp)) }
                    for (voice in listOf(false, true)) {
                        val siblings = draft.channels.filter { placement -> placement.categoryId == folder?.id.orEmpty() &&
                            state.channels.firstOrNull { it.id == placement.id }?.isVoice == voice }
                        items(siblings, key = { it.id }) { placement ->
                            val c = state.channels.firstOrNull { it.id == placement.id }
                            DragLine((if (voice) "Voz · " else "#") + c?.name.orEmpty(), !busy) { step ->
                                val index = siblings.indexOfFirst { it.id == placement.id }
                                val next = siblings.getOrNull(index + step)
                                if (next != null) {
                                    val from = draft.channels.indexOfFirst { it.id == placement.id }
                                    val to = draft.channels.indexOfFirst { it.id == next.id }
                                    draft = draft.reorder(from, to, false)
                                }
                            }
                        }
                    }
                }
            }
            WebButton(if (busy) "Salvando…" else "Salvar ordem", { act("PUT", "/api/layout", draft.body(), onDismiss) },
                Modifier.fillMaxWidth(), enabled = !busy)
        }
    }
    naming?.takeIf { state.canManage }?.let { n ->
        var name by remember(n) { mutableStateOf(n.initial) }
        var kind by remember(n) { mutableStateOf(n.kind) }
        Sheet({ if (!busy) { naming = null; onDismiss() } }, title = n.title) {
            WebSearchField(name, { name = it }, "Nome", "Nome")
            if (n.kind.isNotEmpty()) Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                WebButton(if (kind == "text") "✓ Texto" else "Texto", { kind = "text" })
                WebButton(if (kind == "voice") "✓ Voz" else "Voz", { kind = "voice" })
            }
            WebButton("Salvar", {
                val body = JSONObject().put("name", name.trim())
                if (kind.isNotEmpty()) body.put("kind", kind)
                act(n.method, n.path, body) { naming = null; onDismiss() }
            }, Modifier.fillMaxWidth().padding(top = 12.dp), enabled = !busy && name.trim().isNotEmpty())
        }
    }
    moving?.takeIf { state.canManage }?.let { c ->
        Sheet({ moving = null; onDismiss() }, title = "Mover para pasta…") {
            for (folder in listOf<Category?>(null) + state.categories.sortedBy { it.position }) {
                SheetRow(folder?.name ?: "Sem categoria", {
                    act("PUT", "/api/layout", ChannelLayout.from(state).moveChannel(c.id, folder?.id.orEmpty()).body()) {
                        moving = null; onDismiss()
                    }
                })
            }
        }
    }
    deleting?.takeIf { state.canManage }?.let { (path, body) ->
        Confirm("Apagar ${channel?.name ?: category?.name.orEmpty()}?", body, "Apagar", {
            if (!busy) act("DELETE", path) { deleting = null; onDismiss() }
        }, { if (!busy) { deleting = null; onDismiss() } })
    }
    error?.let { text -> Sheet({ error = null }, title = "Não foi possível salvar") { Text(text, style = BodyText) } }
}

@Composable
private fun DragLine(label: String, enabled: Boolean, onStep: (Int) -> Unit) {
    val action by rememberUpdatedState(onStep)
    val threshold = with(LocalDensity.current) { 48.dp.toPx() }
    Row(Modifier.fillMaxWidth().height(48.dp), verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(48.dp).semantics { contentDescription = "Arrastar $label" }
            .pointerInput(enabled, threshold) {
                if (!enabled) return@pointerInput
                var distance = 0f
                detectVerticalDragGestures(onDragStart = { distance = 0f }, onVerticalDrag = { change, dy ->
                    change.consume(); distance += dy
                    if (distance >= threshold) { action(1); distance -= threshold }
                    if (distance <= -threshold) { action(-1); distance += threshold }
                })
            }, contentAlignment = Alignment.Center) { Glyph(Glyphs.Bars, null, Muted, 20) }
        Text(label, style = ItemText, maxLines = 1)
    }
}
