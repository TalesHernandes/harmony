package com.taleshernandes.harmony.ui

import android.Manifest
import android.app.KeyguardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.taleshernandes.harmony.diag.Diag

internal enum class PermissionKind(val permission: String) {
    MICROPHONE(Manifest.permission.RECORD_AUDIO), CAMERA(Manifest.permission.CAMERA),
    NOTIFICATIONS(Manifest.permission.POST_NOTIFICATIONS), INSTALL("")
}

/** One explanation before the OS question, with the pending operation owned by the activity. */
internal class PermissionFlow(
    private val activity: ComponentActivity,
    private val complete: (PermissionKind, Boolean) -> Unit,
    private val cancelled: (PermissionKind) -> Unit = {},
) {
    private val prefs = activity.getSharedPreferences("permissions", Context.MODE_PRIVATE)
    var kind by mutableStateOf<PermissionKind?>(null)
        private set
    var denied by mutableStateOf(false)
        private set
    private var waiting by mutableStateOf(false)
    private var settings = false
    private val launcher = activity.registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        waiting = false
        val k = kind ?: return@registerForActivityResult
        Diag.log("permission ${k.name}: ${if (granted) "granted" else "denied"}")
        if (granted) finish(k, false) else {
            denied = true
            prefs.edit().putBoolean(k.name, true).apply()
        }
    }

    fun has(k: PermissionKind) = k == PermissionKind.NOTIFICATIONS && Build.VERSION.SDK_INT < 33 ||
        activity.checkSelfPermission(k.permission) == PackageManager.PERMISSION_GRANTED

    fun request(k: PermissionKind) {
        if (has(k)) { complete(k, false); return }
        Diag.log("permission explanation: ${k.name}")
        kind = k
        denied = prefs.getBoolean(k.name, false)
        waiting = false
    }

    fun save(out: Bundle) {
        out.putString("permission_kind", kind?.name)
        out.putBoolean("permission_denied", denied)
        out.putBoolean("permission_waiting", waiting)
        out.putBoolean("permission_settings", settings)
    }

    fun restore(saved: Bundle?) {
        kind = saved?.getString("permission_kind")?.let { PermissionKind.valueOf(it) }
        denied = saved?.getBoolean("permission_denied") ?: false
        waiting = saved?.getBoolean("permission_waiting") ?: false
        settings = saved?.getBoolean("permission_settings") ?: false
    }

    fun resumed() {
        if (!settings) return
        settings = false
        waiting = false
        val k = kind ?: return
        if (has(k)) finish(k, false) else denied = true
    }

    private fun finish(k: PermissionKind, listening: Boolean) {
        kind = null
        complete(k, listening)
    }

    private fun dismiss() {
        val k = kind ?: return
        kind = null
        cancelled(k)
    }

    @Composable fun Content() {
        val k = kind ?: return
        if (waiting) return
        PermissionSheet(k, denied, onPrimary = {
            if (denied) {
                openAppSettings(activity) { settings = true; waiting = true }
            } else {
                waiting = true
                launcher.launch(k.permission)
            }
        }, onSecondary = {
            if (denied && k == PermissionKind.MICROPHONE) finish(k, true) else dismiss()
        }, onDismiss = ::dismiss)
    }
}

private data class PermissionCopy(val icon: ImageVector, val title: String, val body: String, val path: String, val secondary: String)

@Composable
internal fun PermissionSheet(kind: PermissionKind, denied: Boolean, onPrimary: () -> Unit, onSecondary: () -> Unit, onDismiss: () -> Unit = onSecondary) {
    val copy = when (kind) {
        PermissionKind.MICROPHONE -> if (denied) PermissionCopy(Glyphs.MicrophoneSlash,
            "Sem microfone, você só escuta", "Tudo bem entrar assim. Quando quiser falar, é só liberar nos Ajustes.",
            "Apps › harmony › Permissões › Microfone › Permitir só durante o uso", "Entrar só ouvindo")
        else PermissionCopy(Glyphs.Microphone, "Pra falar na sala, o harmony precisa do microfone",
            "Ele só fica ligado enquanto você está numa sala ou numa chamada. No mudo, nada sai.", "", "Agora não")
        PermissionKind.CAMERA -> if (denied) PermissionCopy(Glyphs.Camera,
            "A câmera ficou bloqueada", "Você continua na chamada com voz. Pra aparecer, libere a câmera nos Ajustes.",
            "Apps › harmony › Permissões › Câmera › Permitir só durante o uso", "Seguir sem câmera")
        else PermissionCopy(Glyphs.Camera, "Pra ligar a câmera, o Android pergunta antes",
            "Ela só liga quando você toca nela e desliga quando você sai.", "", "Agora não")
        PermissionKind.NOTIFICATIONS -> if (denied) PermissionCopy(Glyphs.BellSlash,
            "Sem aviso, você só vê quando abrir o app", "Chamada também não toca. Dá pra ligar depois, quando quiser.",
            "Apps › harmony › Notificações › Permitir notificações", "Deixar assim")
        else PermissionCopy(Glyphs.Bell, "Quer saber quando alguém te chamar?",
            "Mensagem direta, menção e chamada chegam mesmo com o app fechado. Canal que você silenciou continua quieto.", "", "Agora não")
        PermissionKind.INSTALL -> if (denied) PermissionCopy(Glyphs.ArrowRotateRight,
            "A atualização ficou esperando", "Sem essa licença o harmony não consegue instalar a versão nova. A que você tem continua funcionando.",
            "Apps › Acesso especial › Instalar apps desconhecidos › harmony › Permitir desta fonte", "Depois")
        else PermissionCopy(Glyphs.ArrowRotateRight, "Pra se atualizar, o harmony precisa de uma licença",
            "É uma vez só. O Android vai abrir \"Instalar apps desconhecidos\": ligue \"Permitir desta fonte\" e volte pra cá.", "", "Depois")
    }
    Sheet(onDismiss, title = when (kind) {
        PermissionKind.MICROPHONE -> "Microfone"
        PermissionKind.CAMERA -> "Câmera"
        PermissionKind.NOTIFICATIONS -> "Notificações"
        PermissionKind.INSTALL -> "Atualização"
    }) {
        val close = LocalPanelDismiss.current
        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(16.dp)) {
            Box(Modifier.size(48.dp).background(Surface2, RoundedCornerShape(8.dp)), contentAlignment = Alignment.Center) {
                Glyph(copy.icon, null, size = 24)
            }
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(copy.title, style = ItemText)
                Text(copy.body, color = Muted, fontSize = 15.sp, lineHeight = 22.sp, letterSpacing = (-0.3).sp)
            }
            if (denied) Column(Modifier.fillMaxWidth().background(Surface2, RoundedCornerShape(8.dp)).padding(horizontal = 14.dp, vertical = 12.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("No Android", color = Muted, fontSize = 12.sp, lineHeight = 16.sp, letterSpacing = (-0.24).sp)
                Text(copy.path, fontSize = 14.sp, lineHeight = 20.sp, letterSpacing = (-0.28).sp, fontWeight = FontWeight.Medium)
            }
            Column(Modifier.padding(top = 4.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                PermissionButton(if (denied) "Abrir Ajustes" else "Continuar", { close(onPrimary) }, Accent)
                PermissionButton(copy.secondary, { close(onSecondary) }, Surface2)
            }
        }
    }
}

@Composable private fun PermissionButton(label: String, action: () -> Unit, color: Color) {
    androidx.compose.material3.Button(action, Modifier.fillMaxWidth().height(48.dp).semantics { contentDescription = label },
        shape = RoundedCornerShape(8.dp), elevation = null,
        colors = androidx.compose.material3.ButtonDefaults.buttonColors(containerColor = color, contentColor = Foreground)) {
        // The style in full: inside a Material button the default text is labelLarge, which is not Inter.
        Text(label, style = SmallButtonText.copy(fontWeight = if (color == Accent) FontWeight.SemiBold else FontWeight.Medium),
            maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

internal fun isXiaomi(manufacturer: String = Build.MANUFACTURER): Boolean =
    manufacturer.lowercase(java.util.Locale.ROOT) in setOf("xiaomi", "redmi", "poco")

/** App settings expose account information: unlock before leaving the call's lock-screen UI. */
internal fun openAppSettings(context: Context, opened: () -> Unit = {}) {
    val go = {
        runCatching {
            context.startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:${context.packageName}")))
            opened()
        }.onFailure { Diag.log("permission settings unavailable: ${it.javaClass.simpleName}") }
        Unit
    }
    val kg = context.getSystemService(KeyguardManager::class.java)
    if (context is ComponentActivity && kg.isKeyguardLocked) {
        kg.requestDismissKeyguard(context, object : KeyguardManager.KeyguardDismissCallback() {
            override fun onDismissSucceeded() = go()
        })
    } else go()
}

@Composable internal fun XiaomiGuide(onClose: () -> Unit, context: Context) {
    LaunchedEffect(Unit) { Diag.log("permissions: Xiaomi guide shown") }
    Sheet(onClose, title = "Chamadas na Xiaomi", expanded = true) {
        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(24.dp)) {
        Column(Modifier.padding(horizontal = 4.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Pra chamada tocar com o celular bloqueado", fontSize = 22.sp, lineHeight = 28.sp, letterSpacing = (-0.22).sp, fontWeight = FontWeight.SemiBold)
            Text("A Xiaomi segura os apps em segundo plano pra poupar bateria. São três chaves, coisa de um minuto, e você só faz uma vez.", color = Muted, fontSize = 15.sp, lineHeight = 22.sp, letterSpacing = (-0.3).sp)
        }
        listOf("Mostrar na tela de bloqueio" to "Ajustes › Apps › harmony › Outras permissões",
            "Abrir novas janelas em segundo plano" to "Na mesma tela, Outras permissões",
            "Bateria: Sem restrições" to "Ajustes › Apps › harmony › Energia › Economia de bateria").forEachIndexed { index, (title, path) ->
            Row(Modifier.fillMaxWidth().background(Surface1, RoundedCornerShape(8.dp)).padding(16.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Box(Modifier.size(28.dp).background(Surface2, RoundedCornerShape(8.dp)), contentAlignment = Alignment.Center) {
                    Text("${index + 1}", fontSize = 13.sp, letterSpacing = (-0.26).sp, fontWeight = FontWeight.SemiBold)
                }
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(title, fontSize = 15.sp, lineHeight = 22.sp, letterSpacing = (-0.3).sp, fontWeight = FontWeight.SemiBold)
                    Text(path, color = Muted, fontSize = 13.sp, lineHeight = 20.sp, letterSpacing = (-0.26).sp)
                }
            }
        }
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            PermissionButton("Abrir Ajustes do harmony", { openAppSettings(context) }, Accent)
            PermissionButton("Já fiz", onClose, Surface2)
        }
    }
    }
}
