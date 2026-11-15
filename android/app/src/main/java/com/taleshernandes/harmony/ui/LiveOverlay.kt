package com.taleshernandes.harmony.ui

/** FullscreenControls in Call.tsx: 12dp inset, 44dp controls, 8dp gaps. */
internal object LiveOverlay {
    const val INSET = 12
    const val TARGET = 44
    const val GAP = 8

    /** Leave room for microphone and leave before sizing the live's slider. */
    fun volumeWidth(safeWidth: Float): Float =
        (safeWidth - 2 * INSET - 2 * TARGET - 2 * GAP).coerceIn(0f, 248f)
}
