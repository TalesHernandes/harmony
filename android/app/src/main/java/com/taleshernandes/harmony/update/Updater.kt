package com.taleshernandes.harmony.update

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.Settings
import com.taleshernandes.harmony.BuildConfig
import com.taleshernandes.harmony.HarmonyApp
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.model.Release
import com.taleshernandes.harmony.model.Releases
import com.taleshernandes.harmony.net.Api
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import java.io.File
import java.util.concurrent.CancellationException

/**
 * A new version installed without a cable (A8).
 *
 * The server says what the newest APK is (PROTOCOL-HARMONY §8); the app
 * downloads it with its own session, checks it three ways — the server's size
 * and sha256 while it is written, and then, from the file itself, that it is
 * this app and a higher version than the one running — and hands it to
 * Android's installer, which asks the person to confirm. Nothing here ever
 * installs without that confirmation.
 *
 * The version the file claims is read from the file by the PackageManager,
 * not from the server's answer: the server's number comes from the file's
 * name, and a misnamed file must not become an update that installs the same
 * version forever.
 */
class Updater(
    private val context: Context,
    private val scope: CoroutineScope,
    private val api: () -> Api?,
    private val header: () -> String = { "" },
) {
    sealed interface State {
        data object Idle : State
        data class Available(val release: Release) : State
        /** Android wants the person to allow this app to install apps, once. */
        data class NeedsPermission(val release: Release) : State
        data class Downloading(val release: Release, val done: Long, val total: Long) : State
        data class Installing(val release: Release) : State
        /** [byFile]: the session installer gave up; offer Android's own, as a file opened from Download does. */
        data class Failed(val release: Release, val why: String, val byFile: Boolean = false) : State
    }

    private val _state = MutableStateFlow<State>(State.Idle)
    val state: StateFlow<State> = _state

    private val _lastChecked = MutableStateFlow<Long?>(null)
    val lastChecked: StateFlow<Long?> = _lastChecked
    // A check may finish after an install (or a newer check) has started.
    private var revision = 0L

    private val _permissionPrompt = MutableStateFlow<Boolean?>(null)
    val permissionPrompt: StateFlow<Boolean?> = _permissionPrompt
    private val permissionPrefs = context.getSharedPreferences("update_permission", Context.MODE_PRIVATE)
    private var awaitingSettings = false

    @Volatile private var lastCheck = 0L
    @Volatile private var cancelled = false

    private fun busy(s: State) = s is State.Downloading || s is State.Installing

    /**
     * Asks the server. Called when the app is shown, when the live connection
     * comes back (a deploy restarts the server, and the web learned that this
     * is the earliest news there is); a floor keeps a burst to one question.
     * No clock: a phone app goes behind and comes back many times a day.
     */
    @Synchronized
    fun check(why: String, force: Boolean = false) {
        val now = System.currentTimeMillis()
        if (!force && now - lastCheck < CHECK_FLOOR_MS) return
        if (busy(_state.value)) return
        val a = api() ?: return
        lastCheck = now
        _lastChecked.value = now
        val mine = ++revision
        scope.launch {
            val r = try {
                a.appRelease()
            } catch (e: Exception) {
                // Not knowing is never "there is something new".
                Diag.log("versão: não consegui perguntar ($why): ${e.message}")
                return@launch
            }
            synchronized(this@Updater) {
                if (mine != revision || busy(_state.value)) return@launch
                if (Releases.isNewer(BuildConfig.VERSION_CODE.toLong(), r)) {
                    r!!
                    val cur = _state.value
                    // A failure or a pending permission for this same version stays on screen.
                    val same = when (cur) {
                        is State.Failed -> cur.release == r
                        is State.NeedsPermission -> cur.release == r
                        else -> false
                    }
                    if (!same) {
                        // Android may reclaim the process while its settings screen is open.
                        if (permissionPrefs.getLong("release", -1) == r.versionCode.toLong()) {
                            _state.value = State.NeedsPermission(r)
                            if (canInstall()) install() else _permissionPrompt.value = true
                            return@launch
                        }
                        _permissionPrompt.value = null
                        awaitingSettings = false
                        Diag.log("versão: ${r.versionName} (${r.versionCode}) disponível, esta é ${BuildConfig.VERSION_NAME} ($why)")
                        _state.value = State.Available(r)
                    }
                } else {
                    if (_state.value !is State.Idle) Diag.log("versão: em dia (${BuildConfig.VERSION_NAME})")
                    _state.value = State.Idle
                    _permissionPrompt.value = null
                    awaitingSettings = false
                    permissionPrefs.edit().remove("release").apply()
                }
            }
        }
    }

    /** The person pressed "Atualizar" (or "Pelo arquivo", after the installer gave up). */
    @Synchronized
    fun install() {
        val s = _state.value
        val r = when (s) {
            is State.Available -> s.release
            is State.NeedsPermission -> s.release
            is State.Failed -> s.release
            else -> return
        }
        val byFile = s is State.Failed && s.byFile
        revision++
        if (!canInstall()) {
            Diag.log("versão: pedindo licença para instalar")
            _state.value = State.NeedsPermission(r)
            _permissionPrompt.value = false
            return
        }
        val a = api() ?: return
        permissionPrefs.edit().remove("release").apply()
        awaitingSettings = false
        _permissionPrompt.value = null
        cancelled = false
        _state.value = State.Downloading(r, 0, r.size)
        scope.launch { run(a, r, byFile) }
    }

    /** Only the sheet's Continue/Abrir Ajustes button opens Android's installer switch. */
    @Synchronized
    fun openInstallSettings() {
        val s = _state.value as? State.NeedsPermission ?: return
        val i = Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${context.packageName}"))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        try {
            awaitingSettings = true
            permissionPrefs.edit().putLong("release", s.release.versionCode.toLong()).apply()
            _permissionPrompt.value = null
            context.startActivity(i)
        } catch (e: Exception) {
            awaitingSettings = false
            _permissionPrompt.value = true
            Diag.log("install settings unavailable: ${e.javaClass.simpleName}")
        }
    }

    @Synchronized
    fun deferPermission() {
        revision++
        permissionPrefs.edit().remove("release").apply()
        _permissionPrompt.value = null
        awaitingSettings = false
        val s = _state.value as? State.NeedsPermission ?: return
        _state.value = State.Available(s.release)
    }

    /** Called when the app returns, including a return without turning the switch on. */
    @Synchronized
    fun resumeIfAllowed() {
        if (!awaitingSettings) return
        awaitingSettings = false
        if (_state.value !is State.NeedsPermission) return
        if (canInstall()) { _permissionPrompt.value = null; install() }
        else { _permissionPrompt.value = true; Diag.log("install permission denied") }
    }

    fun cancel() {
        cancelled = true
    }

    private fun canInstall(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.O || context.packageManager.canRequestPackageInstalls()

    private fun run(a: Api, r: Release, byFile: Boolean = false) {
        val dir = File(context.cacheDir, "update").apply { mkdirs() }
        val apk = File(dir, "harmony-${r.versionCode}.apk")
        // The file the installer gave up on is kept for the second way in.
        if (byFile && apk.exists() && verify(apk, r) == null) return openFile(apk, r)
        // Only one download ever sits here: an old one is 20 MB of nothing.
        dir.listFiles()?.forEach { it.delete() }
        var last = 0L
        try {
            a.downloadRelease(r, apk, { cancelled }) { done, total ->
                _state.value = State.Downloading(r, done, total)
                last = done
            }
        } catch (e: CancellationException) {
            Diag.log("versão: download cancelado em $last de ${r.size}")
            _state.value = State.Available(r)
            return
        } catch (e: Exception) {
            Diag.log("versão: download falhou em $last de ${r.size}: ${e.message}")
            _state.value = State.Failed(r, "O download não terminou. Tente de novo.")
            return
        }
        val why = verify(apk, r)
        if (why != null) {
            Diag.log("versão: arquivo recusado: $why")
            apk.delete()
            _state.value = State.Failed(r, "O arquivo baixado não é uma versão nova do harmony.")
            return
        }
        if (byFile) return openFile(apk, r)
        _state.value = State.Installing(r)
        // The file stays (cache/update, emptied by the next download and by
        // Updated) in case the installer gives up and the file way is tried.
        try {
            commit(apk, r)
        } catch (e: Exception) {
            Diag.log("versão: o instalador recusou: ${e.message}")
            _state.value = State.Failed(r, "O Android não aceitou a instalação.", byFile = true)
            report("o instalador recusou a sessão")
        }
    }

    /**
     * Android's package installer on the file, the way opening it from
     * Download does. A Samsung (25/10/2026) looped on the session installer
     * (confirm, nothing, the banner again) and the same APK sent as a file
     * installed. No answer comes back from this way.
     */
    private fun openFile(apk: File, r: Release) {
        val uri = androidx.core.content.FileProvider.getUriForFile(context, "${context.packageName}.files", apk)
        val i = Intent(Intent.ACTION_VIEW)
            .setDataAndType(uri, "application/vnd.android.package-archive")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
        try {
            Diag.log("versão: abrindo a ${r.versionName} no instalador do Android, pelo arquivo")
            context.startActivity(i)
            _state.value = State.Failed(r, "Se o Android não instalou, toque de novo.", byFile = true)
        } catch (e: Exception) {
            Diag.log("versão: o instalador pelo arquivo não abriu: ${e.javaClass.simpleName}")
            _state.value = State.Failed(r, "O Android não abriu o instalador.", byFile = true)
            report("o instalador pelo arquivo não abriu")
        }
    }

    /**
     * A failed install sends the diagnostic log to the server by itself, so the
     * owner reads why on a phone he cannot hold (25/10/2026: the in-memory log
     * of the Samsung that looped was gone by the time anyone asked). The log
     * never holds message text, passwords or e-mail; one send a minute at most
     * (the server's own limit).
     */
    private fun report(what: String) {
        val a = api() ?: return
        val source = try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
                context.packageManager.getInstallSourceInfo(context.packageName).installingPackageName ?: "nenhum"
            else @Suppress("DEPRECATION") context.packageManager.getInstallerPackageName(context.packageName) ?: "nenhum"
        } catch (e: Exception) { "não encontrado" }
        val text = Diag.dump(header() + "\natualização: $what; instalado por: $source; pode instalar apps: ${canInstall()}")
        scope.launch {
            try {
                a.sendDiagnostic(text)
                Diag.log("versão: registro enviado ao servidor")
            } catch (e: Exception) {
                Diag.log("versão: o registro não foi ao servidor (${e.javaClass.simpleName})")
            }
        }
    }

    /** Null when the file is this app at the version promised, and newer than this one. */
    private fun verify(apk: File, r: Release): String? {
        @Suppress("DEPRECATION")
        val info = context.packageManager.getPackageArchiveInfo(apk.path, 0)
            ?: return "o Android não reconhece o arquivo"
        if (info.packageName != context.packageName) return "é outro app (${info.packageName})"
        @Suppress("DEPRECATION")
        val code = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) info.longVersionCode else info.versionCode.toLong()
        if (code != r.versionCode.toLong()) return "versão $code, o servidor anunciou ${r.versionCode}"
        if (code <= BuildConfig.VERSION_CODE) return "versão $code não é mais nova que ${BuildConfig.VERSION_CODE}"
        return null
    }

    private fun commit(apk: File, r: Release) {
        val installer = context.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(context.packageName)
            setSize(apk.length())
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                // Always ask. Android could skip the question when this app
                // installed the previous version itself, but an update the
                // person did not see coming would restart the app under
                // their fingers, in the middle of whatever they were doing.
                setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_REQUIRED)
            }
        }
        val id = installer.createSession(params)
        installer.openSession(id).use { session ->
            session.openWrite("harmony.apk", 0, apk.length()).use { out ->
                apk.inputStream().use { it.copyTo(out) }
                session.fsync(out)
            }
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                // The installer writes its answer into this intent's extras.
                (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0)
            val pi = PendingIntent.getBroadcast(
                context, id,
                Intent(context, InstallResult::class.java).setPackage(context.packageName),
                flags,
            )
            Diag.log("versão: entregando a ${r.versionName} ao instalador (sessão $id)")
            session.commit(pi.intentSender)
        }
    }

    @Synchronized
    internal fun result(status: Int, message: String?, confirm: Intent?) {
        val r = (_state.value as? State.Installing)?.release
        when (status) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                Diag.log("versão: o Android pede confirmação")
                confirm?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)?.let {
                    try {
                        context.startActivity(it)
                    } catch (e: Exception) {
                        Diag.log("versão: não abriu a confirmação: ${e.message}")
                        if (r != null) _state.value = State.Failed(r, "O Android não mostrou a confirmação.")
                    }
                }
            }
            // The process is ended by the installer a moment after this.
            PackageInstaller.STATUS_SUCCESS -> Diag.log("versão: instalada")
            // Not only the person's "Cancelar": Play Protect and Samsung's
            // blocker end a session the same way, with no word on screen.
            PackageInstaller.STATUS_FAILURE_ABORTED -> {
                Diag.log("versão: instalação cancelada ($message)")
                if (r != null) _state.value = State.Failed(r, "O Android não instalou.", byFile = true)
                report("cancelada ($message)")
            }
            else -> {
                Diag.log("versão: instalação falhou ($status): $message")
                val again = status != PackageInstaller.STATUS_FAILURE_CONFLICT &&
                    status != PackageInstaller.STATUS_FAILURE_INCOMPATIBLE && status != PackageInstaller.STATUS_FAILURE_STORAGE
                if (r != null) _state.value = State.Failed(r, failure(status), byFile = again)
                report("falhou ($status: $message)")
            }
        }
    }

    private fun failure(status: Int): String = when (status) {
        PackageInstaller.STATUS_FAILURE_CONFLICT, PackageInstaller.STATUS_FAILURE_INCOMPATIBLE ->
            "Esta versão foi assinada com outra chave. Desinstale o harmony e instale a nova pela pasta Download."
        PackageInstaller.STATUS_FAILURE_STORAGE -> "Falta espaço no celular."
        PackageInstaller.STATUS_FAILURE_BLOCKED -> "O celular bloqueou a instalação."
        else -> "A instalação não terminou. Tente de novo."
    }

    companion object {
        private const val CHECK_FLOOR_MS = 60_000L
    }
}

/** Where Android's installer answers (the session's IntentSender). */
class InstallResult : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
        val message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)
        @Suppress("DEPRECATION")
        val confirm = if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
        else intent.getParcelableExtra(Intent.EXTRA_INTENT)
        (context.applicationContext as HarmonyApp).core.updater.result(status, message, confirm)
    }
}

/**
 * The new version is in. Android's installer ends the old process and leaves
 * the person on the home screen, having just pressed "Atualizar" in the app;
 * this says it worked and brings them back with one touch. (An activity cannot
 * be opened from here: Android forbids starting a screen from the background.)
 */
class Updated : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_MY_PACKAGE_REPLACED) return
        File(context.cacheDir, "update").listFiles()?.forEach { it.delete() }
        Diag.log("versão: agora ${BuildConfig.VERSION_NAME}")
        val nm = context.getSystemService(android.app.NotificationManager::class.java) ?: return
        if (!nm.areNotificationsEnabled()) return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && nm.getNotificationChannel(CHANNEL) == null) {
            nm.createNotificationChannel(
                android.app.NotificationChannel(CHANNEL, "Atualizações do app", android.app.NotificationManager.IMPORTANCE_DEFAULT)
                    .apply { description = "Quando uma versão nova do harmony termina de instalar" },
            )
        }
        val open = PendingIntent.getActivity(
            context, 0,
            Intent(context, com.taleshernandes.harmony.ui.MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val n = androidx.core.app.NotificationCompat.Builder(context, CHANNEL)
            .setSmallIcon(com.taleshernandes.harmony.R.drawable.notif_app_atualizado)
            .setContentTitle("harmony atualizado para a ${BuildConfig.VERSION_NAME}")
            .setContentText("Toque para abrir")
            .setContentIntent(open)
            .setAutoCancel(true)
            .setTimeoutAfter(10 * 60 * 1000L)
            .build()
        try {
            nm.notify(NOTICE_ID, n)
        } catch (e: SecurityException) {
            // Notifications not granted (Android 13+): nothing to say it with.
        }
    }

    private companion object {
        const val CHANNEL = "app_update"
        const val NOTICE_ID = 0x4a38
    }
}
