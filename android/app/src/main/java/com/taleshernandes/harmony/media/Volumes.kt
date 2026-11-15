package com.taleshernandes.harmony.media

import android.content.Context
import org.json.JSONObject

/**
 * How loud each person is, kept on the phone: their voice and the sound of
 * their screen apart, from 0 to 200 %, the way the web keeps them (media.ts
 * volumeKey: `username` for the voice, `username:tela` for the screen's
 * sound). Per account, so two people sharing a phone do not share taste.
 */
class Volumes(context: Context) {
    private val prefs = context.getSharedPreferences("volumes", Context.MODE_PRIVATE)
    private var account = ""
    private var levels = HashMap<String, Float>()
    // The screen sound's level before its button turned it off, to give back.
    private val audible = HashMap<String, Float>()

    @Synchronized
    fun forAccount(username: String) {
        if (username == account) return
        account = username
        levels = HashMap()
        audible.clear()
        val saved = prefs.getString(username, null) ?: return
        runCatching {
            val o = JSONObject(saved)
            for (k in o.keys()) levels[k] = o.getDouble(k).toFloat().coerceIn(0f, MAX)
        }
    }

    @Synchronized
    fun get(username: String, screen: Boolean): Float = levels[key(username, screen)] ?: 1f

    @Synchronized
    fun set(username: String, screen: Boolean, level: Float) {
        val v = level.coerceIn(0f, MAX)
        if (v == 1f) levels.remove(key(username, screen)) else levels[key(username, screen)] = v
        prefs.edit().putString(account, JSONObject(levels.mapValues { it.value.toDouble() }).toString()).apply()
    }

    /** The screen sound's level after a tap on its button: off, or back to where it was. */
    @Synchronized
    fun toggled(username: String): Float {
        val level = get(username, true)
        if (level > 0f) {
            audible[username] = level
            return 0f
        }
        return audible[username] ?: 1f
    }

    @Synchronized
    fun reset() {
        levels.clear()
        prefs.edit().remove(account).apply()
    }

    private fun key(username: String, screen: Boolean) = if (screen) "$username:tela" else username

    companion object {
        const val MAX = 2f
    }
}
