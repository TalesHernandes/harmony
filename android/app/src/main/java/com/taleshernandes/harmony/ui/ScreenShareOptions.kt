package com.taleshernandes.harmony.ui

import android.content.Intent
import android.os.Build
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import com.taleshernandes.harmony.media.ScreenOptions

/** Choose first, then request Android's one-use projection consent. */
@Composable
internal fun rememberScreenShareOptions(room: String?, onResult: (Intent, ScreenOptions) -> Unit): () -> Unit {
    val context = LocalContext.current
    val preferences = remember(context) { context.getSharedPreferences("screen", android.content.Context.MODE_PRIVATE) }
    var open by remember { mutableStateOf(false) }
    var quality by rememberSaveable { mutableStateOf(preferences.getString("quality", "auto")
        ?.takeIf { it in listOf("auto", "1080", "720", "480") } ?: "auto") }
    var fps by rememberSaveable { mutableStateOf(preferences.getString("fps", "auto")
        ?.takeIf { it in listOf("auto", "60", "30", "15") } ?: "auto") }
    var sound by rememberSaveable { mutableStateOf(preferences.getBoolean("sound", true)) }
    var pendingRoom by rememberSaveable { mutableStateOf<String?>(null) }
    var pendingQuality by rememberSaveable { mutableStateOf("auto") }
    var pendingFps by rememberSaveable { mutableStateOf("auto") }
    var pendingSound by rememberSaveable { mutableStateOf(true) }
    val currentRoom = rememberUpdatedState(room)
    val result = rememberUpdatedState(onResult)
    val askAndroid = rememberScreenShare { projection ->
        val requestedRoom = pendingRoom
        pendingRoom = null
        if (projection != null && requestedRoom != null && requestedRoom == currentRoom.value) {
            result.value(projection, ScreenOptions(pendingQuality, pendingFps, pendingSound))
        }
    }
    LaunchedEffect(room) { open = false }
    if (open && room != null) Sheet(onDismiss = { open = false }, title = "Compartilhar a tela") {
        val close = LocalPanelDismiss.current
        Column(Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(16.dp)) {
            ScreenOption("Qualidade", quality,
                listOf("auto" to "Automática", "1080" to "1080p", "720" to "720p", "480" to "480p")) { quality = it }
            ScreenOption("Quadros por segundo", fps,
                listOf("auto" to "Automático", "60" to "60 fps", "30" to "30 fps", "15" to "15 fps")) { fps = it }
            if (quality == "auto" || fps == "auto") Text(
                "No celular, o automático usa ${if (quality == "auto") "720p" else "${quality}p"} e ${if (fps == "auto") "60" else fps} fps.",
                style = MetaText, color = Muted)
            if (Build.VERSION.SDK_INT >= 29) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically) {
                    Text("Compartilhar som", style = ItemText, modifier = Modifier.weight(1f))
                    Switch(checked = sound, onCheckedChange = { sound = it })
                }
            } else Text("O som do celular precisa do Android 10 ou mais recente.", style = MetaText, color = Muted)
            Button48("Compartilhar a tela", {
                pendingRoom = room
                pendingQuality = quality
                pendingFps = fps
                pendingSound = sound && Build.VERSION.SDK_INT >= 29
                preferences.edit().putString("quality", quality).putString("fps", fps).putBoolean("sound", sound).apply()
                close { open = false; askAndroid() }
            }, Modifier.fillMaxWidth())
        }
    }
    return { if (room != null && pendingRoom == null) open = true }
}

@Composable
private fun ScreenOption(label: String, value: String, options: List<Pair<String, String>>, choose: (String) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(label, style = LabelText, color = Muted)
        Box {
            Row(Modifier.fillMaxWidth().background(Surface2, Shape8)
                .clickable(role = Role.Button) { expanded = true }.padding(12.dp)) {
                Text(options.first { it.first == value }.second, style = ItemText, modifier = Modifier.weight(1f))
                Glyph(Glyphs.ChevronDown, null, Muted, 12)
            }
            if (expanded) Sheet({ expanded = false }, title = label) {
                val close = LocalPanelDismiss.current
                options.forEach { (id, title) ->
                    SheetRow(title, { close { choose(id); expanded = false } }, selected = id == value)
                }
            }
        }
    }
}
