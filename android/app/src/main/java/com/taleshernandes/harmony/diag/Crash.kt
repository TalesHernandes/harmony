package com.taleshernandes.harmony.diag

import android.content.Context
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter

/**
 * What the app was doing when it closed by itself, kept for the next launch.
 *
 * The diagnostic log lives only in memory, so a crash took it along — and a
 * crash in Ajustes (2026-10-01) also took the only button that shares it. The
 * handler writes the log and the stack trace to one file before the process
 * dies; the next launch offers to send it and then forgets it.
 */
object Crash {
    private const val FILE = "last-crash.txt"
    private const val MAX = 150_000

    fun install(context: Context, header: () -> String) {
        val file = File(context.filesDir, FILE)
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            runCatching {
                val trace = StringWriter().also { error.printStackTrace(PrintWriter(it)) }.toString()
                Diag.log("crash on ${thread.name}: ${error.javaClass.name}")
                file.writeText((Diag.dump(header()) + "\n--- fechou aqui ---\n" + trace).takeLast(MAX))
            }
            previous?.uncaughtException(thread, error)
        }
    }

    /** The report of the last crash, if there is one not yet sent or dismissed. */
    fun pending(context: Context): String? =
        runCatching { File(context.filesDir, FILE).takeIf { it.exists() }?.readText() }.getOrNull()

    fun forget(context: Context) {
        runCatching { File(context.filesDir, FILE).delete() }
    }
}
