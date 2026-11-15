package com.taleshernandes.harmony.diag

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The in-memory diagnostic log, the app's version of the web's diaglog.
 *
 * It exists from the first day because every hard bug in this project was
 * found in a log taken from the device that had it. It lives only in memory and
 * leaves the phone only when the owner presses "compartilhar".
 *
 * What goes in: what the app did and what the server answered, by type and
 * count. What never goes in: message text, passwords, cookies (CLAUDE.md).
 */
object Diag {
    private const val MAX_LINES = 1500

    private val lines = ArrayDeque<String>(MAX_LINES)
    private val clock = SimpleDateFormat("HH:mm:ss.SSS", Locale.ROOT)

    /** Mirrors each line somewhere else as it is written (logcat, on the device). */
    @Volatile
    var echo: ((String) -> Unit)? = null

    fun log(msg: String) {
        val line = synchronized(this) {
            val l = "${clock.format(Date())} $msg"
            if (lines.size == MAX_LINES) lines.removeFirst()
            lines.addLast(l)
            l
        }
        echo?.invoke(line)
    }

    /** The whole log, oldest first, under a header saying which device wrote it. */
    fun dump(header: String): String = synchronized(this) {
        buildString {
            append(header).append('\n')
            for (l in lines) append(l).append('\n')
        }
    }

    internal fun clearForTest() = synchronized(this) { lines.clear() }
}
