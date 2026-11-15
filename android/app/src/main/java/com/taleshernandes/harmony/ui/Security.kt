package com.taleshernandes.harmony.ui

import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.PersistableBundle
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.taleshernandes.harmony.Screen
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.model.User
import com.taleshernandes.harmony.net.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Secrets stay in memory, never in rememberSaveable or diagnostics. */
@Composable
private fun SecurityFrame(title: String, content: @Composable ColumnScope.() -> Unit) {
    Box(Modifier.fillMaxSize().safeDrawingPadding(), contentAlignment = Alignment.Center) {
        Column(Modifier.widthIn(max = 480.dp).fillMaxWidth().verticalScroll(rememberScrollState()).imePadding().padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)) {
            Text(title, style = TitleText)
            content()
        }
    }
}

/** True inside a Settings card, whose Surface2 would swallow a Surface2 field or button. */
private val LocalOnCard = staticCompositionLocalOf { false }

@Composable
private fun SecurityField(label: String, value: String, set: (String) -> Unit, busy: Boolean,
    secret: Boolean = false, type: KeyboardType = KeyboardType.Text) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(label, style = MetaText, color = Muted)
        val card = LocalOnCard.current
        Field(value, set, label, secret = secret, enabled = !busy, height = 44,
            background = if (card) Surface3 else Surface1, ringGap = if (card) Surface2 else Background,
            options = KeyboardOptions(autoCorrectEnabled = false,
                keyboardType = if (secret) KeyboardType.Password else type))
    }
}

@Composable
private fun SecurityButton(text: String, enabled: Boolean = true, quiet: Boolean = false, action: () -> Unit) {
    Button48(text, action, Modifier.fillMaxWidth(), enabled = enabled, color = if (!quiet) Accent else if (LocalOnCard.current) Surface3 else Surface2,
        style = SmallButtonText, maxLines = 2)
}

@Composable
private fun SecurityFeedback(error: String?, info: String = "") {
    if (!error.isNullOrEmpty()) Text(error, style = MetaText, color = DangerSoftFg)
    if (info.isNotEmpty()) Text(info, style = MetaText, color = Muted)
}

@Composable
fun SecondFactorScreen(s: Screen.SecondFactor, submit: (String) -> Unit, toggle: () -> Unit, back: () -> Unit) {
    var code by remember(s.challenge, s.recovery) { mutableStateOf("") }
    BackHandler(enabled = !s.busy, onBack = back)
    SecurityFrame("harmony") {
        Text("Entre com a sua conta.", style = MetaText, color = Muted)
        SecurityField(if (s.recovery) "Código de recuperação" else if (s.method == "email")
            "Código recebido por e-mail" else "Código do app", code, { code = it }, s.busy)
        SecurityFeedback(s.error)
        SecurityButton("Entrar", !s.busy && code.isNotBlank()) { submit(code) }
        Row(Modifier.fillMaxWidth().padding(top = 8.dp), horizontalArrangement = Arrangement.SpaceBetween) {
            if (s.method == "totp") TextLink(if (s.recovery) "Usar o código do app" else "Usar um código de recuperação", !s.busy, toggle)
            else Spacer(Modifier)
            TextLink("Voltar", !s.busy, back)
        }
    }
}

@Composable
fun ForgotPasswordScreen(s: Screen.ForgotPassword, submit: (String) -> Unit, back: () -> Unit) {
    var identifier by remember { mutableStateOf("") }
    BackHandler(enabled = !s.busy, onBack = back)
    SecurityFrame("Esqueci a senha") {
        if (s.sent) Text("Se a conta tem e-mail confirmado, enviaremos um link. Confira também o spam.", style = MetaText, color = Muted)
        else {
            SecurityField("Nome ou e-mail", identifier, { identifier = it }, s.busy)
            SecurityFeedback(s.error)
            SecurityButton("Enviar link", !s.busy && identifier.isNotBlank()) { submit(identifier) }
        }
        TextLink("Voltar para entrar", !s.busy, back)
    }
}

@Composable
fun EmailVerificationScreen(s: Screen.VerifyEmail, api: () -> Api?, done: (User) -> Unit,
    skip: () -> Unit, logout: () -> Unit, retry: () -> Unit) {
    var email by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var code by remember { mutableStateOf("") }
    var sent by remember(s.user?.username) { mutableStateOf(!s.user?.email.isNullOrEmpty()) }
    var changing by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf("") }
    var info by remember { mutableStateOf("") }
    val scope = rememberCoroutineScope()
    val currentApi by rememberUpdatedState(api)
    fun run(name: String, action: suspend (Api) -> Unit) {
        if (busy) return
        busy = true; error = ""
        scope.launch {
            Diag.log("email verification: $name")
            try { action(currentApi() ?: throw Unauthorized()) }
            catch (e: CancellationException) { throw e }
            catch (e: Exception) {
                error = securityError(e); Diag.log("email verification: ${e.javaClass.simpleName}")
                if (e is Unauthorized) retry()
            }
            finally { busy = false }
        }
    }
    SecurityFrame("Confirme seu e-mail") {
        if (s.user == null) {
            SecurityFeedback(s.error)
            SecurityButton("Tentar de novo", !busy, action = retry)
        } else {
            Text(if (sent && !changing) "Digite o código que enviamos para ${email.ifEmpty { s.user.email }}. Ele vale por 10 minutos; confira também o spam."
                else "Cadastre seu e-mail para recuperar a conta e receber avisos de segurança.", style = MetaText, color = Muted)
            if (!sent || changing) {
                SecurityField("E-mail", email, { email = it }, busy, type = KeyboardType.Email)
                SecurityField("Senha atual", password, { password = it }, busy, secret = true)
                SecurityButton("Enviar código", !busy && email.isNotBlank() && password.isNotEmpty()) {
                    val pass = password; val address = email
                    run("enviar") { a ->
                        withContext(Dispatchers.IO) { a.changeEmail(pass, address) }
                        password = ""; code = ""; sent = true; changing = false; info = "Código enviado."
                    }
                }
            } else {
                SecurityField("Código", code, { code = it }, busy, type = KeyboardType.Number)
                SecurityButton("Confirmar e-mail", !busy && code.isNotBlank()) {
                    val submitted = code
                    run("confirmar") { a ->
                        val user = withContext(Dispatchers.IO) { a.verifyEmail(submitted) }
                        code = ""; done(user)
                    }
                }
                SecurityButton("Reenviar", !busy, quiet = true) {
                    run("reenviar") { a -> withContext(Dispatchers.IO) { a.resendEmail() }; info = "Código reenviado." }
                }
                SecurityButton("Usar outro e-mail", !busy, quiet = true) {
                    Diag.log("email verification: outro endereço")
                    changing = true; code = ""; error = ""; info = ""
                }
            }
            SecurityFeedback(error.ifEmpty { s.error.orEmpty() }, info)
        }
        // As on the web: leaving is a quiet text line at the foot, not a sixth button.
        Row(Modifier.fillMaxWidth().padding(top = 8.dp), horizontalArrangement = Arrangement.SpaceBetween) {
            if (s.user != null && !s.required) TextLink("Agora não", !busy, skip) else Spacer(Modifier)
            TextLink("Sair da conta", !busy, logout)
        }
    }
}

@Composable
private fun TextLink(text: String, enabled: Boolean, action: () -> Unit) {
    Text(text, style = MetaText, color = Muted,
        modifier = Modifier.clip(Shape8).clickable(enabled = enabled, onClick = action).padding(horizontal = 4.dp, vertical = 12.dp))
}

/** Use the same platform clipboard as Chat.kt, with the Android sensitive flag. */
private fun copySecret(context: Context, text: String) {
    val clip = ClipData.newPlainText("harmony", text)
    if (Build.VERSION.SDK_INT >= 24) clip.description.extras = PersistableBundle().apply {
        putBoolean("android.content.extra.IS_SENSITIVE", true)
    }
    val clipboard = context.getSystemService(ClipboardManager::class.java)
        ?: throw IllegalStateException("Clipboard unavailable")
    clipboard.setPrimaryClip(clip)
    Diag.log("security clipboard: copiado")
}

@Composable
fun AccountSecurityPage(me: User, api: () -> Api?, onUser: (User) -> Unit, back: () -> Unit) {
    var user by remember { mutableStateOf(me) }
    var open by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var email by remember { mutableStateOf("") }
    var next by remember { mutableStateOf("") }
    var code by remember { mutableStateOf("") }
    var codeSent by remember { mutableStateOf(false) }
    var setup by remember { mutableStateOf<FactorResult?>(null) }
    var codes by remember { mutableStateOf<List<String>>(emptyList()) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf("") }
    var info by remember { mutableStateOf("") }
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val currentApi by rememberUpdatedState(api)
    val currentOnUser by rememberUpdatedState(onUser)
    fun clear() {
        password = ""; email = ""; next = ""; code = ""; codeSent = false; setup = null
    }
    fun toggle(which: String) {
        if (busy) return
        Diag.log("account security: abrir $which")
        open = if (open == which) "" else which
        clear(); error = ""; info = ""
    }
    fun run(name: String, action: suspend (Api) -> Unit) {
        if (busy) return
        busy = true; error = ""; info = ""
        scope.launch {
            Diag.log("account security: $name")
            try { action(currentApi() ?: throw Unauthorized()) }
            catch (e: CancellationException) { throw e }
            catch (e: Exception) { error = securityError(e); Diag.log("account security: ${e.javaClass.simpleName}") }
            finally { busy = false }
        }
    }
    fun factor(action: String) {
        val pass = password; val submitted = code
        run(action) { a ->
            val r = withContext(Dispatchers.IO) { a.factor(action, pass, submitted) }
            if (r.secret.isNotEmpty() && r.uri.isNotEmpty()) { setup = r; code = "" }
            else if (r.codeRequired) { codeSent = true; code = ""; info = "Enviamos um código para o seu e-mail." }
            else {
                r.user?.let { user = it; currentOnUser(it) }
                codes = r.recoveryCodes; open = ""; clear()
                info = if (action == "disable") "Verificação em duas etapas desligada." else "Verificação em duas etapas atualizada."
            }
        }
    }
    LaunchedEffect(Unit) {
        run("carregar") { a ->
            user = withContext(Dispatchers.IO) { a.me() }
            currentOnUser(user)
        }
    }
    BackHandler(enabled = !busy, onBack = back)
    Page("Conta e segurança", { if (!busy) back() }) {
        Group("Conta e segurança") { CompositionLocalProvider(LocalOnCard provides true) {
            SettingRow(Glyphs.Person, "E-mail", user.email.ifEmpty { "não cadastrado" } +
                if (!user.emailVerified) " · não confirmado" else "", nav = false, action = {
                Button48(if (open == "email") "Cancelar" else "Trocar", { toggle("email") }, color = Surface3,
                    enabled = !busy, style = SmallButtonText)
            })
            if (open == "email") {
                Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    if (!codeSent) {
                        SecurityField("Novo e-mail", email, { email = it }, busy, type = KeyboardType.Email)
                        SecurityField("Senha atual", password, { password = it }, busy, secret = true)
                        SecurityButton("Enviar código", !busy && email.isNotBlank() && password.isNotEmpty()) {
                            val pass = password; val address = email
                            run("trocar e-mail") { a ->
                                withContext(Dispatchers.IO) { a.changeEmail(pass, address) }
                                password = ""; codeSent = true; info = "Enviamos um código para o novo e-mail."
                            }
                        }
                    } else {
                        SecurityField("Código recebido", code, { code = it }, busy, type = KeyboardType.Number)
                        SecurityButton("Confirmar", !busy && code.isNotBlank()) {
                            val submitted = code
                            run("confirmar e-mail") { a ->
                                user = withContext(Dispatchers.IO) { a.verifyEmail(submitted) }
                                currentOnUser(user); open = ""; clear(); info = "E-mail trocado."
                            }
                        }
                        SecurityButton("Reenviar código", !busy, quiet = true) {
                            run("reenviar e-mail") { a -> withContext(Dispatchers.IO) { a.resendEmail() }; info = "Código reenviado." }
                        }
                    }
                }
            }
            SettingRow(Glyphs.Person, "Senha", "••••••••••", nav = false, action = {
                Button48(if (open == "password") "Cancelar" else "Trocar", { toggle("password") }, color = Surface3,
                    enabled = !busy, style = SmallButtonText)
            })
            if (open == "password") {
                Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    SecurityField("Senha atual", password, { password = it }, busy, secret = true)
                    SecurityField("Nova senha (pelo menos 10 caracteres)", next, { next = it }, busy, secret = true)
                    SecurityButton("Salvar nova senha", !busy && password.isNotEmpty() && next.length >= 10) {
                        val pass = password; val replacement = next
                        run("trocar senha") { a ->
                            withContext(Dispatchers.IO) { a.changePassword(pass, replacement) }
                            open = ""; clear(); info = "Senha trocada. Os outros aparelhos saíram da conta."
                        }
                    }
                }
            }
            SettingRow(Glyphs.Person, "Verificação em duas etapas", when (user.twofa) {
                "totp" -> "Por app autenticador"
                "email" -> "Por e-mail"
                else -> "Desligada"
            } + if (!user.emailVerified) " · confirme o e-mail antes" else "", nav = false, action = {
                Button48(if (open == "twofa") "Cancelar" else if (user.twofa.isEmpty()) "Ligar" else "Mudar",
                    { toggle("twofa") }, color = Surface3, enabled = !busy && user.emailVerified, style = SmallButtonText)
            })
            if (open == "twofa") {
                Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    val pending = setup
                    if (user.twofa.isEmpty() && pending == null) {
                        Text("Além da senha, entrar vai pedir um código. Escolha de onde ele vem.", style = MetaText, color = Muted)
                        SecurityField("Senha atual", password, { password = it }, busy, secret = true)
                        SecurityButton("Por app autenticador (recomendado)", !busy && password.isNotEmpty()) { factor("totp/setup") }
                        SecurityButton("Por e-mail", !busy && password.isNotEmpty(), quiet = true) { factor("email/enable") }
                    }
                    if (pending != null) {
                        Text("Digite esta chave no app autenticador (Google Authenticator, Aegis…). Vale por 10 minutos.", style = MetaText, color = Muted)
                        Text(groupSecret(pending.secret), style = BodyText)
                        SecurityButton("Copiar chave", !busy, quiet = true) {
                            try { copySecret(context, pending.secret); info = "Chave copiada." }
                            catch (e: Exception) { error = "Não foi possível copiar a chave."; Diag.log("security clipboard: ${e.javaClass.simpleName}") }
                        }
                        SecurityButton("Abrir no app autenticador", !busy, quiet = true) {
                            try {
                                require(Uri.parse(pending.uri).scheme == "otpauth")
                                context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(pending.uri)))
                                Diag.log("totp: abrir autenticador")
                            } catch (_: ActivityNotFoundException) {
                                error = "Nenhum app autenticador instalado. Copie a chave e adicione no app."
                                Diag.log("totp: nenhum autenticador")
                            } catch (e: Exception) {
                                error = "Não foi possível abrir o app autenticador. Copie a chave e adicione no app."
                                Diag.log("totp: ${e.javaClass.simpleName}")
                            }
                        }
                        SecurityField("Código que o app mostra", code, { code = it }, busy, type = KeyboardType.Number)
                        SecurityButton("Confirmar e ligar", !busy && code.isNotBlank()) { factor("totp/enable") }
                    }
                    if (user.twofa.isNotEmpty()) {
                        SecurityField("Senha atual", password, { password = it }, busy, secret = true)
                        if (user.twofa == "totp" || codeSent) SecurityField(if (user.twofa == "totp")
                            "Código do app ou de recuperação" else "Código recebido por e-mail", code, { code = it }, busy)
                        SecurityButton(if (user.twofa == "email" && !codeSent) "Enviar código para desligar" else "Desligar",
                            !busy && password.isNotEmpty() && (!(user.twofa == "totp" || codeSent) || code.isNotBlank())) { factor("disable") }
                        if (user.twofa == "totp") SecurityButton("Gerar novos códigos de recuperação", !busy && password.isNotEmpty(), quiet = true) { factor("recovery") }
                        Text("Para trocar de método, desligue e ligue de novo.", style = MetaText, color = Muted)
                    }
                }
            }
            if (codes.isNotEmpty()) {
                Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text("Códigos de recuperação", style = ItemText)
                    Text("Se perder o celular, cada um destes entra no lugar do código do app, uma vez. Guarde fora deste aparelho. Gerar novos invalida estes.", style = MetaText, color = Muted)
                    Text(codes.joinToString("\n"), style = BodyText)
                    SecurityButton("Copiar", !busy, quiet = true) {
                        try { copySecret(context, codes.joinToString("\n")); info = "Códigos copiados." }
                        catch (e: Exception) { error = "Não foi possível copiar os códigos."; Diag.log("security clipboard: ${e.javaClass.simpleName}") }
                    }
                    SecurityButton("Já guardei", !busy) { codes = emptyList(); Diag.log("recovery codes: guardados") }
                }
            }
            SecurityFeedback(error, info)
        } }
    }
}
