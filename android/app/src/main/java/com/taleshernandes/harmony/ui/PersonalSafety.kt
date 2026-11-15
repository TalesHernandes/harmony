package com.taleshernandes.harmony.ui

import androidx.compose.runtime.*
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Text
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.taleshernandes.harmony.model.PersonalPreferences
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.diag.Diag
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
internal fun PersonalActions(username: String, api: () -> Api?) {
 val items by PersonalPreferences.state.collectAsState()
 val p = items.firstOrNull { it.username == username }
 var asking by remember { mutableStateOf(false) }
 var busy by remember { mutableStateOf(false) }
 var error by remember { mutableStateOf("") }
 val scope = rememberCoroutineScope()
 fun change(block: Boolean, enabled: Boolean) {
  if (busy) return
  busy = true
  scope.launch {
   try {
    val next = withContext(Dispatchers.IO) { (api() ?: throw com.taleshernandes.harmony.net.Unauthorized()).personPreference(username, block, enabled) }
    PersonalPreferences.replace(next)
    Diag.log("personal preference changed")
    asking = false
    error = ""
   } catch (e: kotlinx.coroutines.CancellationException) { throw e }
   catch (e: Exception) { error = accountError(e) }
   finally { busy = false }
  }
 }
 Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
  WebButton(if (p?.muted == true) "Tirar o silêncio" else "Silenciar", { change(false, p?.muted != true) }, Modifier.fillMaxWidth(), enabled = !busy, centered = false, icon = Glyphs.MicrophoneSlash, padding = 12, gap = 12)
  WebButton(if (p?.blocked == true) "Desbloquear" else "Bloquear", { if (p?.blocked == true) change(true,false) else asking = true }, Modifier.fillMaxWidth(), enabled = !busy, centered = false, icon = Glyphs.Person, padding = 12, gap = 12)
  if (error.isNotEmpty()) Text(error, style = MetaText, color = DangerSoftFg)
 }
 if (asking) Confirm("Bloquear esta pessoa?", "Você deixa de ouvir a voz e a tela dela, as mensagens ficam recolhidas e não haverá novas mensagens privadas nem chamadas entre vocês. Ela não será avisada.", "Bloquear", { change(true,true) }, { if (!busy) asking = false })
}

@Composable
internal fun PersonalList(api: () -> Api?) {
 val items by PersonalPreferences.state.collectAsState()
 Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
  SectionLabel("Pessoas silenciadas e bloqueadas")
  if (items.isEmpty()) Text("Você não silenciou nem bloqueou ninguém.", style = MetaText, color = Muted)
  items.forEach { p -> Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
   Text("@${p.username}", style = BodyText, color = Foreground)
   PersonalActions(p.username, api)
  } }
 }
}

@Composable
internal fun BlockedConversation(username: String, api: () -> Api?) {
 var busy by remember { mutableStateOf(false) }
 var error by remember { mutableStateOf("") }
 val scope = rememberCoroutineScope()
 Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
  Text("Você bloqueou esta pessoa. Desbloqueie para conversar.", style = MetaText, color = Muted)
  WebButton("Desbloquear", {
   busy = true
   scope.launch {
    try {
     PersonalPreferences.replace(withContext(Dispatchers.IO) { (api() ?: throw com.taleshernandes.harmony.net.Unauthorized()).personPreference(username, true, false) })
     Diag.log("personal preference changed")
    } catch (e: kotlinx.coroutines.CancellationException) { throw e }
    catch (e: Exception) { error = accountError(e) }
    finally { busy = false }
   }
  }, Modifier.fillMaxWidth(), enabled = !busy, centered = false, icon = Glyphs.Person, padding = 12, gap = 12)
  if (error.isNotEmpty()) Text(error, style = MetaText, color = DangerSoftFg)
 }
}
