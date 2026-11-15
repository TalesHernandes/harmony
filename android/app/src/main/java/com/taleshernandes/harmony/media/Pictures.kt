package com.taleshernandes.harmony.media

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import org.webrtc.VideoSink

private val AlwaysFront = MutableStateFlow(true)

/**
 * Whatever holds pictures a screen can draw: a voice room, or a direct call.
 * The screen hands in a renderer by the picture's id and takes it back; the
 * track itself never leaves the call (CLAUDE.md), so a renderer can never be
 * left on a track the call has already disposed of.
 */
interface Pictures {
    fun attach(mid: String, sink: VideoSink)
    fun detach(mid: String, sink: VideoSink)

    /**
     * The own camera is the front one, so its picture is drawn mirrored like a
     * mirror; the back one is not.
     */
    val frontCamera: StateFlow<Boolean> get() = AlwaysFront
}

/**
 * What the call's screen pieces need beyond the pictures: the stage, the
 * seats and the full screen are one set (ui/Rooms.kt, ui/Video.kt), fed by a
 * voice room or a direct call alike, so the two look and behave the same
 * (owner, 25/10/2026: "a chamada 1:1 funcionar como na chamada normal").
 */
interface Stage : Pictures {
    /** The id of this phone's own camera picture. */
    val ownCamera: String
    /** More than one camera to choose from: the button to switch shows only then. */
    fun canSwitchCamera(): Boolean
    fun switchCamera()

    /** Each person's voice and screen sound, for this phone only — one store for room and call. */
    val volumes: Volumes
    /** Moves on every volume change, so what shows a level reads it again. */
    val volumeRevision: StateFlow<Int>
    fun setVolume(username: String, screen: Boolean, level: Float)
    /** The live's sound off, or back to where it was. */
    fun toggleScreenSound(username: String) = setVolume(username, true, volumes.toggled(username))

    /** How wide somebody's screen is drawn, in real pixels; a room picks the layer to ask for by it. */
    fun drawnAt(username: String, widthPx: Int) {}
    /**
     * Whether each screen's frames are moving, for the floating window; null
     * where nobody counts them (a direct call), so no picture is said stalled.
     */
    val screenProgress: StateFlow<Map<String, ScreenProgress>>? get() = null
}
