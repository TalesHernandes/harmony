package com.taleshernandes.harmony.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.text.Normalizer
import com.taleshernandes.harmony.net.Api

internal data class EmojiGroup(val name: String, val entries: List<Pair<String, String>>)

private fun plainEmoji(text: String): String = Normalizer.normalize(text.lowercase(ptBr), Normalizer.Form.NFD)
    .replace(Regex("\\p{M}+"), "")

/** The web's 420dp panel anchored above the composer, without a veil. */
@Composable
@OptIn(ExperimentalFoundationApi::class)
internal fun EmojiPanel(onPick: (String) -> Unit, onClose: () -> Unit, api: () -> Api?) {
    val configuration = LocalConfiguration.current
    val density = LocalDensity.current
    val context = LocalContext.current
    val preferences = remember(context) { context.getSharedPreferences("ui", android.content.Context.MODE_PRIVATE) }
    val sections = remember {
        val used = preferences.getString("emoji_recent", "").orEmpty().split('\n').filter { it.isNotEmpty() }.take(12)
        if (used.isEmpty()) EmojiGroups else listOf(EmojiGroup("usados aqui", used.map { e -> e to "" })) + EmojiGroups
    }
    fun pick(emoji: String) {
        val previous = preferences.getString("emoji_recent", "").orEmpty().split('\n').filter { it.isNotEmpty() && it != emoji }
        preferences.edit().putString("emoji_recent", (listOf(emoji) + previous).take(12).joinToString("\n")).apply()
        onPick(emoji)
    }
    val width = (configuration.screenWidthDp - 40).coerceAtMost(384)
    var query by remember { mutableStateOf("") }
    var selected by remember { mutableIntStateOf(0) }
    var jump by remember { mutableIntStateOf(-1) }
    val list = rememberLazyListState()
    val rail = rememberLazyListState()
    // Lazy lists may precompose the next row. Only rows inside the viewport
    // fetch pictures; precomposed rows hold the same empty boxes.
    val visibleRows by remember(list) { derivedStateOf { list.layoutInfo.visibleItemsInfo.map { it.index }.toSet() } }
    val visibleTabs by remember(rail) { derivedStateOf { rail.layoutInfo.visibleItemsInfo.map { it.index }.toSet() } }
    val cellSize = minOf(28f, (width - 96) / 8f * .78f).dp
    val groups = remember(query) {
        val q = plainEmoji(query.trim())
        if (q.isEmpty()) sections else listOf(EmojiGroup("encontrados", EmojiGroups.flatMap { it.entries }
            .filter { q in plainEmoji(it.second) || q == it.first }.distinctBy { it.first }))
    }
    val starts = remember(groups) {
        var row = 0
        groups.map { group -> row.also { if (group.entries.isNotEmpty()) row += 1 + (group.entries.size + 7) / 8 } }
    }
    LaunchedEffect(groups) { list.scrollToItem(0) }
    LaunchedEffect(groups, jump) {
        if (query.isBlank() && jump >= 0) {
            list.scrollToItem(starts.getOrElse(jump) { 0 })
            jump = -1
        }
    }
    LaunchedEffect(list, starts, query) {
        snapshotFlow { list.firstVisibleItemIndex }.collect { at ->
            if (query.isBlank()) selected = starts.indexOfLast { it <= at }.coerceAtLeast(0)
        }
    }
    Sheet(onClose, title = "Reagir", expanded = true, scroll = false, width = 420) {
        val close = LocalPanelDismiss.current
        Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            WebSearchField(query, { query = it }, "procurar emoji", "Fogo, coracao, risada", height = 44, size = 16,
                gap = 8, style = BodyText.copy(fontSize = 15.sp, lineHeight = 22.sp), clearLabel = "limpar busca de emoji")
            Row(Modifier.weight(1f), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                LazyColumn(Modifier.width(32.dp).fillMaxHeight(), state = rail, verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    items(sections.size) { i ->
                        val group = sections[i]
                        Box(Modifier.size(32.dp).clip(Shape8).background(if (selected == i && query.isBlank()) Selected else androidx.compose.ui.graphics.Color.Transparent)
                            .clickable(role = Role.Button) { query = ""; selected = i; jump = i }
                            .semantics { contentDescription = group.name }, contentAlignment = Alignment.Center) {
                            if (group.name == "usados aqui") Glyph(Glyphs.Clock, null, Muted, 18)
                            else Twemoji(group.entries.first().first, 18.dp, api, hold = i !in visibleTabs)
                        }
                    }
                }
                LazyColumn(Modifier.weight(1f).padding(end = 12.dp), state = list) {
                    if (query.isNotBlank() && groups.single().entries.isEmpty()) item {
                        Column(Modifier.padding(vertical = 16.dp)) {
                            Text("Nenhum emoji com esse nome", style = ItemText.copy(fontSize = 15.sp, lineHeight = 22.sp))
                            Text("Tente uma palavra só, como abacaxi ou pizza.", style = MetaText, color = Muted)
                        }
                    }
                    for ((groupIndex, group) in groups.withIndex()) {
                        if (group.entries.isEmpty()) continue
                        stickyHeader { Text(group.name.uppercase(ptBr), style = LabelText, color = Muted,
                            modifier = Modifier.fillMaxWidth().background(Surface1).padding(top = 8.dp, bottom = 4.dp)) }
                        for ((rowIndex, row) in group.entries.chunked(8).withIndex()) {
                            val itemIndex = starts[groupIndex] + 1 + rowIndex
                            item {
                                Row(Modifier.fillMaxWidth().padding(bottom = 4.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                                    for ((emoji, _) in row) {
                                        Box(Modifier.weight(1f).aspectRatio(1f).clip(Shape8)
                                            .webClickable(role = Role.Button) { close { pick(emoji) } }.semantics { contentDescription = emoji }, contentAlignment = Alignment.Center) {
                                            Twemoji(emoji, cellSize, api, hold = itemIndex !in visibleRows)
                                        }
                                    }
                                    repeat(8 - row.size) { Spacer(Modifier.weight(1f)) }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
