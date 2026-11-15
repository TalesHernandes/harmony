package com.taleshernandes.harmony.media

/** Hearing off temporarily owns mute; restoring hearing restores the person's choice. */
internal data class DirectControls(val muted: Boolean = false, val deafened: Boolean = false, val beforeDeafen: Boolean = false) {
    fun mute(on: Boolean) = if (deafened) this else copy(muted = on)
    fun deafen(on: Boolean): DirectControls = when {
        on == deafened -> this
        on -> copy(muted = true, deafened = true, beforeDeafen = muted)
        else -> copy(muted = beforeDeafen, deafened = false)
    }
}
