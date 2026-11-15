package com.taleshernandes.harmony.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.autofill.ContentType
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import com.taleshernandes.harmony.Screen

/** Dark and only dark, with the design's tokens; no dynamic colour. */
@Composable
fun HarmonyTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = darkColorScheme(
            primary = Accent,
            onPrimary = Foreground,
            background = Background,
            onBackground = Foreground,
            surface = Background,
            onSurface = Foreground,
            surfaceVariant = Surface1,
            onSurfaceVariant = Muted,
            surfaceContainer = Surface1,
            surfaceContainerHigh = Surface1,
            surfaceContainerLow = Surface1,
            error = Danger,
        ),
        typography = Typography(bodyLarge = TextStyle(fontFamily = Inter, fontSize = 16.sp, lineHeight = 24.sp, letterSpacing = (-0.02).em)),
    ) {
        Surface(modifier = Modifier.fillMaxSize(), color = Background, contentColor = Foreground, content = content)
    }
}

@Composable
fun LoadingScreen() {
    Box(Modifier.fillMaxSize().safeDrawingPadding(), contentAlignment = Alignment.Center) { CircularProgressIndicator(color = Accent) }
}

/** The address as the person reads it: no scheme, no trailing slash. */
private fun shown(server: String) = server.trim().removePrefix("https://").removePrefix("http://").trimEnd('/')

@Composable
fun SignInScreen(
    state: Screen.SignIn,
    onSubmit: (server: String, username: String, password: String) -> Unit,
    onShareDiag: () -> Unit,
    onForgot: (String) -> Unit,
) {
    var server by rememberSaveable { mutableStateOf(state.server) }
    var username by rememberSaveable { mutableStateOf("") }
    // Not rememberSaveable: a password has no business in the saved-state bundle.
    var password by remember { mutableStateOf("") }
    // "Trocar" opens the server's own island under the card.
    var changing by rememberSaveable { mutableStateOf(false) }
    val canSubmit = !state.busy && username.isNotEmpty() && password.isNotEmpty()
    val submit = { if (canSubmit) onSubmit(server, username, password) }
    val serverFocus = remember { FocusRequester() }
    LaunchedEffect(changing) { if (changing) serverFocus.requestFocus() }
    BackHandler(enabled = changing) { changing = false }

    BoxWithConstraints(Modifier.fillMaxSize().safeDrawingPadding().imePadding()) {
        Column(
            Modifier.fillMaxWidth().verticalScroll(rememberScrollState())
                .padding(horizontal = 24.dp).padding(top = ((maxHeight - 342.dp) / 2).coerceAtLeast(24.dp), bottom = 24.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            val width = Modifier.widthIn(max = 480.dp).fillMaxWidth()
            Column(width) {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text("harmony", style = TitleText)
                    Text("Entre com a sua conta.", style = MetaText, color = Muted)
                }
                Spacer(Modifier.height(24.dp))
                Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
                    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text("Nome ou e-mail", style = MetaText, color = Muted)
                        Field(username, { username = it }, "Nome ou e-mail", height = 44, background = Surface1, enabled = !state.busy, autofill = ContentType.Username,
                            options = KeyboardOptions(autoCorrectEnabled = false, keyboardType = KeyboardType.Ascii, imeAction = ImeAction.Next))
                    }
                    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text("Senha", style = MetaText, color = Muted)
                        Field(password, { password = it }, "Senha", height = 44, background = Surface1, enabled = !state.busy, secret = true, autofill = ContentType.Password,
                            options = KeyboardOptions(keyboardType = KeyboardType.Password, imeAction = ImeAction.Done),
                            actions = KeyboardActions(onDone = { submit() }))
                    }
                }
                if (state.error != null) Text(state.error, style = MetaText.copy(lineHeight = 20.sp), color = DangerSoftFg)
                Spacer(Modifier.height(16.dp))
                WebButton(if (state.busy) "Entrando…" else "Entrar", { submit() }, Modifier.fillMaxWidth(), enabled = canSubmit,
                    background = if (canSubmit) Accent else Selected)
                // The web's text link (Auth.tsx: mt-4, text-meta, muted), not a
                // second button stuck to "Entrar" (the owner, 30/10/2026). 40dp
                // of target with the text 16dp under the button, as on the web.
                Spacer(Modifier.height(4.dp))
                Box(
                    Modifier.height(40.dp).webClickable(enabled = !state.busy, role = Role.Button) { onForgot(server) },
                    contentAlignment = Alignment.CenterStart,
                ) { Text("Esqueci a senha", style = MetaText, color = Muted) }
                Spacer(Modifier.height(24.dp))
                Text("Não existe cadastro aberto. Para entrar é preciso um convite do dono.", style = MetaText, color = Muted)
            }
            if (!changing) {
                // Almost nobody changes the server: one line, under everything.
                Row(width.height(48.dp).padding(start = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                    Glyph(Glyphs.Server, null, tint = Muted, size = 16)
                    Text(shown(server), style = MetaText.copy(lineHeight = 20.sp), color = Muted, maxLines = 1,
                        overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f).padding(start = 8.dp))
                    Box(
                        Modifier.height(48.dp).clickable(enabled = !state.busy, role = Role.Button) { changing = true }
                            .padding(horizontal = 12.dp).semantics { contentDescription = "trocar servidor" },
                        contentAlignment = Alignment.Center,
                    ) { Text("Trocar", style = MetaText.copy(lineHeight = 20.sp, fontWeight = FontWeight.SemiBold)) }
                }
            } else {
                Column(width.island().padding(start = 20.dp, end = 20.dp, top = 16.dp, bottom = 20.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    SectionLabel("endereço do servidor")
                    Field(server, { server = it }, "endereço do servidor", height = 44, enabled = !state.busy, focus = serverFocus,
                        options = KeyboardOptions(keyboardType = KeyboardType.Uri, autoCorrectEnabled = false, imeAction = ImeAction.Done),
                        actions = KeyboardActions(onDone = { submit() }))
                    Text("O mesmo endereço que você abre no navegador.", style = MetaText.copy(lineHeight = 20.sp), color = Muted)
                    // Not in the design: the diagnostic the owner asks for when
                    // signing in fails has to stay reachable before signing in.
                    Box(
                        Modifier.height(48.dp).clickable(role = Role.Button, onClick = onShareDiag),
                        contentAlignment = Alignment.CenterStart,
                    ) {
                        Text("Compartilhar diagnóstico", style = MetaText.copy(lineHeight = 20.sp, fontWeight = FontWeight.SemiBold), color = Muted)
                    }
                }
            }
        }
    }
}
