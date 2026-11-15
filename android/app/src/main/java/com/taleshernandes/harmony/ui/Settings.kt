package com.taleshernandes.harmony.ui

import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.offset
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import android.app.Activity
import android.app.Application
import android.app.NotificationManager
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.provider.Settings
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.clickable
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.taleshernandes.harmony.BuildConfig
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.media.Output
import com.taleshernandes.harmony.media.Mic
import com.taleshernandes.harmony.media.Microphones
import com.taleshernandes.harmony.media.Denoise
import com.taleshernandes.harmony.model.AccountSession
import com.taleshernandes.harmony.model.Invite
import com.taleshernandes.harmony.model.User
import com.taleshernandes.harmony.model.accountWhen
import com.taleshernandes.harmony.model.inviteState
import com.taleshernandes.harmony.model.updateCheckTime
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.ApiError
import com.taleshernandes.harmony.net.Unauthorized
import com.taleshernandes.harmony.update.Updater
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal fun accountError(e: Exception): String = when (e) {
    is Unauthorized -> "Entre de novo com a sua senha."
    is ApiError -> "O servidor respondeu ${e.status}."
    else -> "Sem conexão com o servidor."
}

/**
 * Settings use the web's mobile modal and horizontal tabs; the account's two lists open as their own screens. The host owns
 * saved preferences, media changes and the signed-in lifecycle.
 */
@Composable
fun SettingsScreen(
    me: User,
    roleMembers: List<com.taleshernandes.harmony.model.Member>,
    onRoleSnapshot: (com.taleshernandes.harmony.model.RoleSnapshot) -> Unit,
    api: () -> Api?,
    onSecurityUser: (User) -> Unit,
    outputs: List<Output>,
    preferredOutput: Output?,
    onOutput: (Output) -> Unit,
    denoiseEnabled: Boolean,
    denoiseLevel: Int,
    denoiseStatus: String,
    onDenoise: (Boolean) -> Unit,
    onDenoiseLevel: (Int) -> Unit,
    videoEngine: String,
    appVersion: String,
    update: Updater.State,
    lastChecked: Long?,
    onCheckUpdate: () -> Unit,
    onShareDiagnostics: () -> Unit,
    echoDumpArmed: Boolean,
    onEchoDump: (Boolean) -> Unit,
    echoDumpReady: Boolean,
    onShareEchoDump: () -> Unit,
    onLogout: suspend () -> Unit,
    onClose: () -> Unit,
    banner: @Composable () -> Unit = {},
    context: Context = LocalContext.current,
) {
    val preferences = remember(context) { context.getSharedPreferences("ui", Context.MODE_PRIVATE) }
    var tab by rememberSaveable { mutableIntStateOf(savedTab(preferences)) }
    var page by rememberSaveable { mutableStateOf("") }
    var callsGuide by rememberSaveable { mutableStateOf(false) }
    var outputSheet by remember { mutableStateOf(false) }
    var micSheet by remember { mutableStateOf(false) }
    remember(context) { Microphones.load(context) }
    val micChoice by Microphones.choice.collectAsState()
    // What is plugged in now, followed while Ajustes is open: a headset
    // connected here must make the row appear (Codex, 25/10/2026).
    var mics by remember(context) { mutableStateOf(presentMics(context)) }
    DisposableEffect(context) {
        val am = context.getSystemService(android.media.AudioManager::class.java)
        val watch = object : android.media.AudioDeviceCallback() {
            override fun onAudioDevicesAdded(added: Array<out android.media.AudioDeviceInfo>) { mics = presentMics(context) }
            override fun onAudioDevicesRemoved(removed: Array<out android.media.AudioDeviceInfo>) { mics = presentMics(context) }
        }
        am.registerAudioDeviceCallback(watch, android.os.Handler(android.os.Looper.getMainLooper()))
        onDispose { am.unregisterAudioDeviceCallback(watch) }
    }
    var asking by remember { mutableStateOf(false) }

    // The account's lists, loaded once here so the lines can say how many.
    var sessions by remember { mutableStateOf<List<AccountSession>>(emptyList()) }
    var invites by remember { mutableStateOf<List<Invite>?>(null) }
    var sessionError by remember { mutableStateOf("") }
    var inviteError by remember { mutableStateOf("") }
    var leaveError by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var loading by remember { mutableStateOf(true) }
    val currentApi by rememberUpdatedState(api)
    val scope = rememberCoroutineScope()
    fun connection() = currentApi() ?: throw Unauthorized()
    suspend fun loadSessions() { sessions = withContext(Dispatchers.IO) { connection().sessions() } }
    suspend fun loadInvites() { invites = withContext(Dispatchers.IO) { connection().invites() } }
    fun runAction(prefix: String, error: (String) -> Unit, action: suspend () -> Unit) {
        if (busy) return
        busy = true
        scope.launch {
            error("")
            try { action() } catch (e: CancellationException) { throw e } catch (e: Exception) {
                error(prefix + accountError(e))
                Diag.log("account action: ${e.javaClass.simpleName}")
            } finally { busy = false }
        }
    }
    var photoError by remember { mutableStateOf("") }
    var framing by remember { mutableStateOf<android.graphics.Bitmap?>(null) }
    val avatarVersions by com.taleshernandes.harmony.model.Avatars.versions.collectAsState()
    val hasPhoto = (avatarVersions[me.username] ?: "").isNotEmpty()
    val photoPicker = androidx.activity.compose.rememberLauncherForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.PickVisualMedia()) { uri ->
        // Picked: decoded here and framed in the editor; only the framed square goes up.
        if (uri != null) runAction("", { photoError = it }) {
            try {
                framing = withContext(Dispatchers.IO) { loadPickedPhoto(context, uri) }
            } catch (e: CancellationException) { throw e } catch (e: Exception) { photoError = photoErrorText(e) }
        }
    }
    framing?.let { photo ->
        PhotoEditor(photo, onCancel = { framing = null }, onSave = { jpeg ->
            framing = null
            runAction("", { photoError = it }) {
                try {
                    val version = withContext(Dispatchers.IO) { connection().setAvatar(jpeg) }
                    com.taleshernandes.harmony.model.Avatars.note(me.username, version)
                } catch (e: CancellationException) { throw e } catch (e: Exception) { photoError = photoErrorText(e) }
            }
        })
    }
    LaunchedEffect(me.username) {
        try { loadInvites() } catch (e: CancellationException) { throw e } catch (e: Exception) {
            inviteError = accountError(e)
            Diag.log("account invites: ${e.javaClass.simpleName}")
        }
        try { loadSessions() } catch (e: CancellationException) { throw e } catch (e: Exception) {
            sessionError = accountError(e)
            Diag.log("account sessions: ${e.javaClass.simpleName}")
        } finally { loading = false }
    }
    val account = Account(
        me = me, sessions = sessions, invites = invites, sessionError = sessionError, inviteError = inviteError,
        busy = busy, loading = loading,
        reloadSessions = { runAction("", { sessionError = it }) { loadSessions() } },
        reloadInvites = { runAction("", { inviteError = it }) { loadInvites() } },
        revokeSession = { id ->
            runAction("Não desconectou: ", { sessionError = it }) {
                withContext(Dispatchers.IO) { connection().revokeSession(id) }
                loadSessions()
            }
        },
        revokeOthers = {
            runAction("Não desconectou os outros: ", { sessionError = it }) {
                withContext(Dispatchers.IO) { connection().revokeOthers() }
                loadSessions()
            }
        },
        createInvite = { note, done ->
            runAction("", { inviteError = it }) {
                val fresh = withContext(Dispatchers.IO) { connection().createInvite(note) }
                done(fresh)
                loadInvites()
            }
        },
        revokeInvite = { id, done ->
            runAction("Não cancelou: ", { inviteError = it }) {
                withContext(Dispatchers.IO) { connection().revokeInvite(id) }
                done()
                loadInvites()
            }
        },
    )

    if (callsGuide && isXiaomi()) {
        XiaomiGuide(onClose = { callsGuide = false }, context = context)
        return
    }
    BackHandler { if (page.isNotEmpty()) page = "" else onClose() }
    val manageRoles = com.taleshernandes.harmony.model.permissionsOf(me.username, me.isAdmin, LocalRoles.current.roles).contains("manage_roles")
    LaunchedEffect(manageRoles) { if (!manageRoles && tab == 4) tab = 2 }
    when (page) {
        "security" -> { AccountSecurityPage(me, api, onSecurityUser) { page = "" }; return }
        "aparelhos" -> { DevicesPage(account) { page = "" }; return }
        "convites" -> { InvitesPage(account) { page = "" }; return }
    }

    Sheet(onClose, title = "Ajustes", expanded = true, scroll = false) {
        Column(Modifier.fillMaxSize()) {
            // Keep existing preference ids; managers gain the web's fifth tab.
            Row(Modifier.fillMaxWidth().then(if (manageRoles) Modifier.horizontalScroll(rememberScrollState()) else Modifier),
                horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                val tabs = listOf(Triple(0, "Voz e vídeo", Glyphs.Microphone), Triple(1, "Avisos", Glyphs.Bell),
                    Triple(2, "Conta", Glyphs.Person)) +
                    (if (manageRoles) listOf(Triple(4, "Cargos", Glyphs.Persons)) else emptyList()) +
                    listOf(Triple(3, "Diagnóstico", Glyphs.FileText))
                for ((index, label, icon) in tabs) {
                    Column((if (manageRoles) Modifier.width(96.dp) else Modifier.weight(1f)).height(56.dp).clip(Shape8)
                        .background(if (tab == index) Selected else Color.Transparent)
                        .clickable(role = Role.Button) { tab = index; preferences.edit().putInt("settings_tab", index).apply() }
                        .semantics { selected = tab == index; contentDescription = label },
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(4.dp, Alignment.CenterVertically)) {
                        Glyph(icon, null, if (tab == index) Foreground else Muted, 16)
                        Text(label, style = MetaText.copy(fontWeight = FontWeight.Medium), maxLines = 1,
                            color = if (tab == index) Foreground else Muted)
                    }
                }
            }
            banner()
            Column(Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState())
                .padding(start = 8.dp, end = 8.dp, top = 16.dp, bottom = 8.dp)) {
                when (tab) {
                    0 -> {
                        // One island, as the other settings: the switch line (the
                        // name of the option at the size of the other lines, the
                        // engine under it), and the strength under it while it is
                        // on, on the same rail as a person's volume. The checkbox,
                        // the 13 sp label and Material's slider read as three
                        // different apps (owner, 02/10/2026).
                        Text("Filtro de ruído".uppercase(ptBr), style = LabelText, color = Muted)
                        Spacer(Modifier.height(8.dp))
                        Column(Modifier.fillMaxWidth().clip(Shape8).background(Surface2)) {
                            Row(Modifier.fillMaxWidth().heightIn(min = 56.dp)
                                .toggleable(value = denoiseEnabled, role = Role.Switch, onValueChange = onDenoise)
                                .padding(horizontal = 12.dp, vertical = 8.dp),
                                verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                                Column(Modifier.weight(1f)) {
                                    Text("Tirar ruído de fundo", style = ItemText.copy(fontSize = 15.sp, lineHeight = 22.sp))
                                    Text("DeepFilterNet", style = MetaText, color = Muted)
                                }
                                androidx.compose.material3.Switch(checked = denoiseEnabled, onCheckedChange = null)
                            }
                            if (denoiseEnabled) LevelLine("Força", denoiseLevel, 10..100, "força do filtro de ruído",
                                onChange = { onDenoiseLevel(Denoise.normalizeLevel(it)) },
                                onReset = { onDenoiseLevel(Denoise.DEFAULT_LEVEL) })
                        }
                        if (denoiseEnabled && denoiseStatus.isNotEmpty()) {
                            Row(Modifier.fillMaxWidth().padding(start = 12.dp, end = 12.dp, top = 8.dp),
                                horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                Box(Modifier.padding(top = 2.dp)) { Glyph(Glyphs.CircleExclamation, null, Warning, 14) }
                                Text(denoiseStatus, style = MetaText, color = Muted, modifier = Modifier.weight(1f))
                            }
                        }
                        Spacer(Modifier.height(16.dp))
                        // Only expose controls backed by native actions. Routing
                        // needs a live call with more than one available output.
                        if (outputs.size > 1) {
                            SettingsChoice("Saída de som", preferredOutput?.let(::outputName) ?: "Padrão do sistema",
                                onClick = { outputSheet = true })
                            Spacer(Modifier.height(16.dp))
                        }
                        // The microphone (owner, 25/10/2026): the phone's unless another is chosen.
                        if (mics.size > 1 || micChoice != Mic.PHONE) {
                            SettingsChoice("Microfone", micName(micChoice), onClick = { micSheet = true })
                            Spacer(Modifier.height(16.dp))
                        }
                    }
                    1 -> Group("Avisos") { Notices(context, onXiaomi = { callsGuide = true }) }
                    2 -> Group("Conta") {
                        PersonalList(api)
                        SettingRow(Glyphs.Person, "Conta e segurança", "E-mail, senha e verificação em duas etapas", onClick = { page = "security" })
                        SettingRow(Glyphs.Picture, "Foto de perfil", photoError.ifEmpty { if (hasPhoto) "Trocar a foto" else "Escolher uma foto" },
                            onClick = if (busy) null else ({ photoPicker.launch(androidx.activity.result.PickVisualMediaRequest(androidx.activity.result.contract.ActivityResultContracts.PickVisualMedia.ImageOnly)) }),
                            nav = false)
                        if (hasPhoto) SettingRow(Glyphs.Xmark, "Remover a foto", null, onClick = if (busy) null else ({
                            runAction("", { photoError = it }) {
                                try {
                                    withContext(Dispatchers.IO) { connection().removeAvatar() }
                                    com.taleshernandes.harmony.model.Avatars.note(me.username, "")
                                } catch (e: CancellationException) { throw e } catch (e: Exception) { photoError = photoErrorText(e) }
                            }
                        }), nav = false)
                        SettingRow(Glyphs.Persons, "Aparelhos conectados",
                            when {
                                sessionError.isNotEmpty() -> sessionError
                                loading -> "Carregando…"
                                sessions.size == 1 -> "Só este aparelho"
                                else -> "${sessions.size} aparelhos"
                            },
                            onClick = { page = "aparelhos" })
                        if (me.isAdmin && (invites != null || inviteError.isNotEmpty())) {
                            val open = invites.orEmpty().count { !it.used }
                            SettingRow(Glyphs.PersonPlus, "Convites",
                                when {
                                    inviteError.isNotEmpty() -> inviteError
                                    open == 0 -> "Nenhum aberto"
                                    open == 1 -> "1 aberto"
                                    else -> "$open abertos"
                                },
                                onClick = { page = "convites" })
                        }
                        SettingRow(Glyphs.ArrowRightFromSquare, "Sair desta conta", leaveError.ifEmpty { null },
                            onClick = if (busy) null else ({ asking = true }), nav = false, tint = DangerSoftFg)
                    }
                    4 -> if (manageRoles) RolesPanel(me, roleMembers, api, onRoleSnapshot)
                    3 -> Group("Diagnóstico") {
                        SettingRow(Glyphs.CircleInfo, "Versão do app",
                            "$appVersion (${BuildConfig.VERSION_CODE}) · ${updateWords(update)} · ${updateCheckTime(lastChecked)}", nav = false)
                        GroupButton(Glyphs.ArrowRotateRight, "Procurar atualização", onCheckUpdate)
                        // Not in the design: which WebRTC engine answered, as the old Diagnóstico tab said.
                        SettingRow(Glyphs.Display, "Motor de vídeo", videoEngine, nav = false)
                        SettingRow(Glyphs.FileText, "Registro", "Copiar ou compartilhar", onClick = onShareDiagnostics)
                        // Echo report of 25/10/2026: the canceller's own recording, one call, opt-in (it holds both voices).
                        Row(Modifier.fillMaxWidth().heightIn(min = 56.dp)
                            .toggleable(value = echoDumpArmed, role = Role.Switch, onValueChange = onEchoDump)
                            .padding(horizontal = 12.dp, vertical = 8.dp),
                            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                            Column(Modifier.weight(1f)) {
                                Text("Gravar o eco da próxima chamada", style = ItemText.copy(fontSize = 15.sp, lineHeight = 22.sp))
                                Text("1 minuto, fica só neste aparelho", style = MetaText, color = Muted)
                            }
                            androidx.compose.material3.Switch(checked = echoDumpArmed, onCheckedChange = null)
                        }
                        if (echoDumpReady) SettingRow(Glyphs.FileText, "Enviar a gravação do eco",
                            "Por WhatsApp ou e-mail; tem as duas vozes", onClick = onShareEchoDump)
                    }
                }
            }
        }
    }
    if (outputSheet) OutputSheet(preferredOutput, outputs, onOutput) { outputSheet = false }
    if (micSheet) MicSheet(micChoice, mics, { Microphones.choose(context, it) }) { micSheet = false }
    if (asking) {
        Confirm(
            title = "Sair desta conta?",
            body = "Neste aparelho, o que você deixou escrito e não mandou vai embora junto. Para voltar, entre de novo com a sua senha.",
            confirm = "Sair",
            onConfirm = {
                asking = false
                runAction("Não saiu: ", { leaveError = it }) { onLogout() }
            },
            onDismiss = { asking = false },
        ) {
            Row(Modifier.fillMaxWidth().island(Surface2).padding(12.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(me.displayName, style = PersonText, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text("@${me.username}", style = MetaText, color = Muted, maxLines = 1)
            }
        }
    }
}

/**
 * The tab Ajustes opened on last time. 0.10 to 0.12 kept it as a word ("av"),
 * 0.13 as a number, and getInt on the word threw — Ajustes closed the app on
 * every phone that had opened it before 0.13 (the owner's Redmi, 02/10/2026).
 */
internal fun savedTab(preferences: android.content.SharedPreferences): Int =
    try { preferences.getInt("settings_tab", 0).coerceIn(0, 3) } catch (e: ClassCastException) {
        preferences.edit().remove("settings_tab").apply()
        0
    }

/** Choice geometry follows the measured web: label 16, gap 8, field 40. */
@Composable
private fun SettingsChoice(label: String, value: String, onClick: () -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(label.uppercase(ptBr), style = LabelText, color = Muted)
        Row(Modifier.fillMaxWidth().height(40.dp).clip(Shape8).background(Surface2)
            .clickable(role = Role.Button, onClick = onClick)
            .semantics { contentDescription = label }.padding(horizontal = 12.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(value, style = BodyText.copy(fontSize = 15.sp, lineHeight = 22.sp,
                fontWeight = FontWeight.Medium, letterSpacing = (-0.225).sp),
                modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis,
                color = Foreground)
            Glyph(Glyphs.ChevronDown, null, Muted, 12)
        }
    }
}

private fun updateWords(s: Updater.State): String = when (s) {
    Updater.State.Idle -> "em dia"
    is Updater.State.Available -> "a ${s.release.versionName} está disponível"
    is Updater.State.NeedsPermission -> "a ${s.release.versionName} espera a sua licença"
    is Updater.State.Downloading -> "baixando a ${s.release.versionName}"
    is Updater.State.Installing -> "instalando a ${s.release.versionName}"
    is Updater.State.Failed -> "a ${s.release.versionName} não ${if (s.byFile) "instalou" else "baixou"}"
}

/** What the two account screens read and do; the loading lives in [SettingsScreen]. */
private class Account(
    val me: User,
    val sessions: List<AccountSession>,
    val invites: List<Invite>?,
    val sessionError: String,
    val inviteError: String,
    val busy: Boolean,
    val loading: Boolean,
    val reloadSessions: () -> Unit,
    val reloadInvites: () -> Unit,
    val revokeSession: (String) -> Unit,
    val revokeOthers: () -> Unit,
    val createInvite: (note: String, done: (Invite) -> Unit) -> Unit,
    val revokeInvite: (id: String, done: () -> Unit) -> Unit,
)

/** Native settings keep the web's uppercase labels and secondary boxes. */
@Composable
internal fun Group(title: String, content: @Composable ColumnScope.() -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(title.uppercase(ptBr), style = LabelText, color = Muted)
        Column(Modifier.fillMaxWidth().island(Surface2).padding(4.dp), content = content)
    }
}

/** A line: at least 56, padding 6 8 6 12, a muted 20 glyph, the name and its support, the chevron when it goes somewhere. */
@Composable
internal fun SettingRow(
    icon: ImageVector,
    name: String,
    support: String?,
    onClick: (() -> Unit)? = null,
    nav: Boolean = onClick != null,
    tint: Color = Foreground,
    action: @Composable (() -> Unit)? = null,
) {
    Row(
        Modifier.fillMaxWidth().heightIn(min = 56.dp).clip(Shape8)
            .then(if (onClick != null) Modifier.clickable(role = Role.Button, onClick = onClick) else Modifier)
            .padding(start = 12.dp, end = 8.dp, top = 6.dp, bottom = 6.dp)
            .semantics { contentDescription = name },
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Glyph(icon, null, tint = if (tint == Foreground) Muted else tint)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(name, style = ItemText, color = tint)
            if (!support.isNullOrEmpty()) Text(support, style = MetaText, color = Muted)
        }
        // The chevron sits 4 further in than the row's 8.
        if (nav) Box(Modifier.padding(end = 4.dp)) { Glyph(Glyphs.ChevronRight, null, tint = Muted) }
        action?.invoke()
    }
}

/** A button inside a group: 48, the full width, surface secondary, padding 4 8 8 around it. */
@Composable
private fun GroupButton(icon: ImageVector, label: String, onClick: () -> Unit, enabled: Boolean = true) {
    Button48(label, onClick, Modifier.fillMaxWidth().padding(start = 8.dp, end = 8.dp, top = 4.dp, bottom = 8.dp),
        color = Surface2, enabled = enabled, icon = icon, style = BodyText.copy(fontWeight = FontWeight.Medium))
}

/**
 * Notificações opens Android's own screen for the app, where sound and
 * vibration of each kind live; the switches are re-read on the way back.
 */
@Composable
private fun Notices(context: Context, onXiaomi: () -> Unit) {
    val manager = context.getSystemService(NotificationManager::class.java)
    var revision by remember { mutableIntStateOf(0) }
    var error by remember { mutableStateOf("") }
    DisposableEffect(context) {
        val app = context.applicationContext as Application
        val listener = object : Application.ActivityLifecycleCallbacks {
            override fun onActivityResumed(activity: Activity) { revision++ }
            override fun onActivityCreated(activity: Activity, state: Bundle?) = Unit
            override fun onActivityStarted(activity: Activity) = Unit
            override fun onActivityPaused(activity: Activity) = Unit
            override fun onActivityStopped(activity: Activity) = Unit
            override fun onActivitySaveInstanceState(activity: Activity, state: Bundle) = Unit
            override fun onActivityDestroyed(activity: Activity) = Unit
        }
        app.registerActivityLifecycleCallbacks(listener)
        onDispose { app.unregisterActivityLifecycleCallbacks(listener) }
    }
    val enabled = remember(revision) { manager?.areNotificationsEnabled() == true }
    SettingRow(Glyphs.Bell, "Notificações",
        error.ifEmpty { "${if (enabled) "Ligadas" else "Desligadas"} · som e vibração nos Ajustes do Android" },
        onClick = {
            try {
                context.startActivity(Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                    .putExtra(Settings.EXTRA_APP_PACKAGE, context.packageName)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                error = ""
            } catch (e: Exception) {
                error = "Não abriu os ajustes do Android."
                Diag.log("notification settings: ${e.javaClass.simpleName}")
            }
        })
    // TODO(design): "Só menções e diretas" — there is no such preference on the server or in the app.
    if (isXiaomi()) {
        // TODO(design): "Faltam 2 de 3 chaves" — Android does not tell the app which Xiaomi switches are on.
        SettingRow(Glyphs.Smartphone, "Chamada com o celular bloqueado", "Três chaves da Xiaomi, uma vez só", onClick = onXiaomi)
    }
}

/** A sub-screen of Ajustes: the header with back, and a column that scrolls. */
@Composable
internal fun Page(title: String, onBack: () -> Unit, content: @Composable ColumnScope.() -> Unit) {
    Sheet(onBack, title = title, expanded = true) {
        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp), content = content)
    }
}

@Composable
private fun DevicesPage(a: Account, onBack: () -> Unit) {
    Page("Aparelhos conectados", onBack) {
        if (a.sessionError.isNotEmpty()) {
            Text(a.sessionError, style = MetaText, color = DangerSoftFg, modifier = Modifier.padding(horizontal = 12.dp))
            Button48("Tentar de novo", a.reloadSessions, Modifier.fillMaxWidth(), color = Surface2, enabled = !a.busy && !a.loading)
        }
        if (a.loading) Text("Carregando…", style = MetaText, color = Muted, modifier = Modifier.padding(horizontal = 12.dp))
        for (session in a.sessions) {
            Column(Modifier.fillMaxWidth().island().padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text(if (session.current) "Este aparelho" else session.userAgent.ifEmpty { "Aparelho" }, style = ItemText,
                        maxLines = 2, overflow = TextOverflow.Ellipsis)
                    Text("Entrou ${accountWhen(session.createdAt)} · visto ${accountWhen(session.lastSeenAt)}", style = MetaText, color = Muted)
                }
                if (!session.current) {
                    Button48("Desconectar", { a.revokeSession(session.id) }, Modifier.fillMaxWidth(), color = DangerSoft,
                        textColor = DangerSoftFg, enabled = !a.busy && !a.loading, style = SmallButtonText)
                }
            }
        }
        Button48("Desconectar todos os outros", a.revokeOthers, Modifier.fillMaxWidth(), color = Surface2,
            enabled = !a.busy && !a.loading, style = SmallButtonText)
    }
}

@Composable
private fun InvitesPage(a: Account, onBack: () -> Unit) {
    var fresh by remember { mutableStateOf<Invite?>(null) }
    var note by rememberSaveable { mutableStateOf("") }
    var copied by remember { mutableStateOf(false) }
    val clipboard = LocalClipboardManager.current
    Page("Convites", onBack) {
        if (a.inviteError.isNotEmpty()) {
            Text(a.inviteError, style = MetaText, color = DangerSoftFg, modifier = Modifier.padding(horizontal = 12.dp))
            Button48("Tentar de novo", a.reloadInvites, Modifier.fillMaxWidth(), color = Surface2, enabled = !a.busy && !a.loading)
        }
        val list = a.invites ?: return@Page
        Column(Modifier.fillMaxWidth().island().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Cada convite serve uma vez e vale por sete dias.", style = MetaText.copy(lineHeight = 20.sp), color = Muted)
            Labelled("para quem é · só você vê") {
                Field(note, { if (it.codePointCount(0, it.length) <= 60) note = it }, "anotação do convite",
                    placeholder = "Uma anotação sua", enabled = !a.busy)
                Text("Fica só nesta lista, para você lembrar de quem é cada convite. Quem recebe não vê.", style = MetaText, color = Muted)
            }
            Button48("Gerar convite", {
                val submitted = note
                a.createInvite(submitted) { made ->
                    fresh = made
                    copied = false
                    note = ""
                }
            }, Modifier.fillMaxWidth(), enabled = !a.busy && !a.loading, icon = Glyphs.PersonPlus)
        }
        fresh?.url?.takeIf { it.isNotEmpty() }?.let { url ->
            Column(Modifier.fillMaxWidth().island().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                SectionLabel("o que você acabou de gerar")
                Text(url, style = PersonText)
                Button48(if (copied) "Copiado" else "Copiar", { clipboard.setText(AnnotatedString(url)); copied = true },
                    Modifier.fillMaxWidth(), color = Surface2, style = SmallButtonText)
                Text("Link inteiro: dá para colar numa conversa sem explicar o que fazer com ele.", style = MetaText, color = Muted)
            }
        }
        if (list.isNotEmpty()) {
            SectionLabel("seus convites — ${list.size}", Modifier.padding(start = 12.dp, end = 12.dp, top = 8.dp))
            for (invite in list) {
                Column(Modifier.fillMaxWidth().island().padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text(invite.note.ifEmpty { "Sem anotação" }, style = ItemText, color = if (invite.used) Muted else Foreground)
                        Text(inviteState(invite, a.me.username), style = MetaText, color = Muted)
                    }
                    if (!invite.used) {
                        Button48("Cancelar", { a.revokeInvite(invite.id) { if (fresh?.id == invite.id) fresh = null } },
                            Modifier.fillMaxWidth(), color = DangerSoft, textColor = DangerSoftFg,
                            enabled = !a.busy && !a.loading, style = SmallButtonText)
                    }
                }
            }
        }
    }
}

/**
 * A level on the rail a person's volume uses (Person.kt VolumeLine): a 6 dp
 * rail, the foreground square for the thumb, the value at the end — touching
 * the value puts it back to the default. Steps of 5.
 */
@Composable
private fun LevelLine(label: String, value: Int, range: IntRange, description: String,
                      onChange: (Int) -> Unit, onReset: () -> Unit) {
    val span = (range.last - range.first).toFloat()
    val current by androidx.compose.runtime.rememberUpdatedState(onChange)
    var trackWidth by remember { androidx.compose.runtime.mutableIntStateOf(0) }
    fun pick(fraction: Float) {
        val v = (range.first + (fraction.coerceIn(0f, 1f) * span / 5f).let { kotlin.math.round(it) } * 5).toInt()
        if (v != value) current(v)
    }
    val at = ((value - range.first) / span).coerceIn(0f, 1f)
    Row(Modifier.fillMaxWidth().height(48.dp).padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(label, style = ItemText.copy(fontSize = 15.sp, lineHeight = 22.sp), modifier = Modifier.width(56.dp), maxLines = 1)
        Box(
            Modifier.weight(1f).height(44.dp)
                .onSizeChanged { trackWidth = it.width }
                .pointerInput(range) {
                    awaitEachGesture {
                        val down = awaitFirstDown()
                        if (size.width > 0) pick(down.position.x / size.width)
                        drag(down.id) { change ->
                            if (size.width > 0) pick(change.position.x / size.width)
                            change.consume()
                        }
                    }
                }
                .semantics {
                    contentDescription = description
                    progressBarRangeInfo = androidx.compose.ui.semantics.ProgressBarRangeInfo(value.toFloat(),
                        range.first.toFloat()..range.last.toFloat(), (span / 5f).toInt() - 1)
                    setProgress { pick((it - range.first) / span); true }
                },
            contentAlignment = Alignment.CenterStart,
        ) {
            Box(Modifier.fillMaxWidth().height(6.dp).clip(CircleShape).background(Surface3)) {
                Box(Modifier.fillMaxWidth(at).height(6.dp).clip(CircleShape).background(Accent))
            }
            Box(
                Modifier.offset { androidx.compose.ui.unit.IntOffset(((trackWidth - 16.dp.toPx()) * at).toInt(), 0) }
                    .size(16.dp).clip(androidx.compose.foundation.shape.RoundedCornerShape(6.dp)).background(Foreground),
            )
        }
        Text("$value dB", style = MetaText, color = Muted, textAlign = androidx.compose.ui.text.style.TextAlign.End, maxLines = 1,
            modifier = Modifier.width(48.dp).clickable(role = Role.Button, onClick = onReset)
                .semantics { contentDescription = "Voltar ao padrão" })
    }
}


internal fun micName(m: Mic): String = when (m) {
    Mic.PHONE -> "Microfone do celular"
    Mic.WIRED -> "Fone com fio"
    Mic.USB -> "Microfone USB"
    Mic.BLUETOOTH -> "Fone Bluetooth"
}

/** The inputs plugged in now, the phone's always first. */
private fun presentMics(context: Context): List<Mic> {
    val am = context.getSystemService(android.media.AudioManager::class.java)
    val found = am.getDevices(android.media.AudioManager.GET_DEVICES_INPUTS).mapNotNull { Microphones.kind(it.type) }.toSet()
    return Mic.entries.filter { it == Mic.PHONE || it in found }
}

/**
 * Which microphone the calls hear. Bluetooth says what it costs: while it is
 * chosen the headset leaves its music profile, in every app, until another is.
 */
@Composable
private fun MicSheet(current: Mic, present: List<Mic>, onPick: (Mic) -> Unit, onDismiss: () -> Unit) {
    Sheet(onDismiss = onDismiss, title = "Qual microfone a chamada usa") {
        val close = LocalPanelDismiss.current
        Column(Modifier.fillMaxWidth()) {
            for (m in present) {
                val on = m == current
                SheetRow(micName(m), { close { onPick(m); onDismiss() } }, icon = if (m == Mic.BLUETOOTH || m == Mic.WIRED) Glyphs.Headphones else Glyphs.Microphone,
                    iconTint = if (on) Foreground else Muted, selected = on,
                    trailing = if (on) ({ Glyph(Glyphs.Check, "escolhido", tint = Accent) }) else null)
            }
            if (Mic.BLUETOOTH in present) Text(
                "Com o microfone do fone Bluetooth, o som no fone vira qualidade de ligação enquanto ele estiver escolhido, em qualquer app.",
                style = MetaText, color = Muted, modifier = Modifier.padding(start = 12.dp, end = 12.dp, top = 8.dp, bottom = 4.dp))
        }
    }
}
