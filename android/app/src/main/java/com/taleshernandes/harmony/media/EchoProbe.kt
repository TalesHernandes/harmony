package com.taleshernandes.harmony.media

import android.content.Context
import android.media.AudioManager
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import com.taleshernandes.harmony.diag.Diag
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import kotlin.math.roundToInt
import org.webrtc.audio.PlaybackRoute

/**
 * What the echo canceller does on this phone, for the owner's echo report of
 * 25/10/2026: a friend on speaker sent the other side's voice back.
 *
 * Every few seconds of a call the Diag gets AEC3's own figures (ERL, ERLE)
 * next to the real output delay and the media volume. On request, the next
 * call's first minute is also recorded as an AEC dump: what played, what the
 * microphone heard and what the canceller sent. It holds both voices, so it is
 * opt-in, one call only, and stays in the app's own folder until pulled.
 */
object EchoProbe {
    private const val DUMP_SECONDS = 60L
    private const val DUMP_MAX_BYTES = 40 * 1024 * 1024
    private const val LOG_EVERY_MS = 5_000L
    private const val KEEP_DUMPS = 2
    private const val RESTART_GRACE_MS = 3_000L

    private val worker = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "harmony-echo-probe") }
    @Volatile private var context: Context? = null
    @Volatile private var recording: ScheduledFuture<*>? = null
    @Volatile private var loggedAt = Long.MIN_VALUE
    @Volatile private var missingLogged = false
    // A switch between media and call mode stops and starts the recorder;
    // the dump lives in the audio processor and goes on through it.
    @Volatile private var pendingStop: ScheduledFuture<*>? = null

    fun attach(context: Context) { this.context = context.applicationContext }

    private fun preferences(c: Context) = c.getSharedPreferences("audio", Context.MODE_PRIVATE)

    private fun folder(c: Context) = c.getExternalFilesDir("aecdump") ?: File(c.filesDir, "aecdump")

    /** The newest finished recording, to send from Ajustes; none while one is being written. */
    fun latest(c: Context): File? =
        if (recording != null) null
        else folder(c).listFiles()?.filter { it.length() > 0 }?.maxByOrNull { it.lastModified() }

    fun armed(c: Context): Boolean = preferences(c).getBoolean("aecdump_next", false)

    fun arm(c: Context, on: Boolean) {
        preferences(c).edit().putBoolean("aecdump_next", on).apply()
        Diag.log(if (on) "eco: a próxima chamada grava ${DUMP_SECONDS}s do cancelador" else "eco: gravação do cancelador desarmada")
    }

    /** The microphone opened (room or direct call): record if the owner asked for it. */
    fun captureStarted() = worker.execute {
        missingLogged = false
        pendingStop?.cancel(false); pendingStop = null
        val c = context ?: return@execute
        if (!armed(c) || recording != null) return@execute
        val f = Engine.existingFactory() ?: return@execute
        val dir = folder(c)
        dir.mkdirs()
        dir.listFiles()?.sortedByDescending { it.lastModified() }?.drop(KEEP_DUMPS - 1)?.forEach { it.delete() }
        val file = File(dir, "eco-${System.currentTimeMillis()}.aecdump")
        val fd = try {
            ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_CREATE or ParcelFileDescriptor.MODE_TRUNCATE or ParcelFileDescriptor.MODE_READ_WRITE).detachFd()
        } catch (e: Exception) {
            Diag.log("eco: não abriu o arquivo do dump (${e.javaClass.simpleName})")
            return@execute
        }
        // The native side owns the descriptor from here on and closes it.
        if (!f.startAecDump(fd, DUMP_MAX_BYTES)) {
            Diag.log("eco: o WebRTC recusou o dump")
            return@execute
        }
        preferences(c).edit().putBoolean("aecdump_next", false).apply()
        Diag.log("eco: gravando o cancelador em ${file.name}")
        recording = worker.schedule({ stop("fim de ${DUMP_SECONDS}s") }, DUMP_SECONDS, TimeUnit.SECONDS)
    }

    fun captureStopped() = worker.execute {
        if (recording == null) return@execute
        pendingStop?.cancel(false)
        pendingStop = worker.schedule({ pendingStop = null; stop("o microfone fechou") }, RESTART_GRACE_MS, TimeUnit.MILLISECONDS)
    }

    private fun stop(why: String) {
        val r = recording ?: return
        recording = null
        r.cancel(false)
        Engine.existingFactory()?.stopAecDump()
        Diag.log("eco: dump fechado ($why)")
    }

    /** One media-source stats entry of the microphone, read where the call reads its level. */
    fun sample(members: Map<String, Any>, now: Long = SystemClock.elapsedRealtime()) {
        if (loggedAt != Long.MIN_VALUE && now - loggedAt < LOG_EVERY_MS) return
        val erl = (members["echoReturnLoss"] as? Number)?.toDouble()
        val erle = (members["echoReturnLossEnhancement"] as? Number)?.toDouble()
        if (erl == null && erle == null) {
            if (!missingLogged) Diag.log("eco: o WebRTC não informa ERL/ERLE neste aparelho (não encontrado)")
            missingLogged = true
            loggedAt = now
            return
        }
        loggedAt = now
        val c = context
        val output = PlaybackRoute.output()
        val volume = c?.getSystemService(AudioManager::class.java)?.let {
            // Call mode plays on the call's own volume.
            val stream = if (it.mode == AudioManager.MODE_IN_COMMUNICATION) AudioManager.STREAM_VOICE_CALL else AudioManager.STREAM_MUSIC
            "${it.getStreamVolume(stream)}/${it.getStreamMaxVolume(stream)}"
        } ?: "não encontrado"
        val level = (members["audioLevel"] as? Number)?.toDouble()
        Diag.log("eco: ERL=${db(erl)} ERLE=${db(erle)} mic=${level?.let { "%.3f".format(java.util.Locale.ROOT, it) } ?: "-"} " +
            "saída=${if (output.latencyMs < 0) "não encontrado" else "${output.latencyMs}ms"} tipo=${output.deviceType} " +
            "volume=$volume dfn=${if (c != null && Engine.denoise(c).enabled) "ligado" else "desligado"}")
    }

    internal fun resetForTest() { loggedAt = Long.MIN_VALUE; missingLogged = false }

    private fun db(v: Double?) = v?.let { "${(it * 10).roundToInt() / 10.0}dB" } ?: "-"
}
