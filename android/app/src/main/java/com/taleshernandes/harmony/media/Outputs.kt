package com.taleshernandes.harmony.media

/** Where the sound of a call can go, in the words the screen uses. */
enum class Output(val label: String) {
    BLUETOOTH("Bluetooth"),
    WIRED("Fone com fio"),
    EARPIECE("No ouvido"),
    SPEAKER("Viva-voz"),
}

enum class PlaybackMode { MEDIA, CALL }

fun microphoneIsOpen(available: Boolean, muted: Boolean, deafened: Boolean = false): Boolean =
    available && !muted && !deafened

/**
 * Call mode only where it buys something (owner, 29/10/2026: "vamos ter que
 * voltar ao modo ligação"). Out of the loudspeaker or the earpiece, with the
 * microphone open, the sound comes back into the microphone: the call's own
 * canceller removes the call, but only the phone's canceller, which runs in
 * call mode, hears every other app (an Instagram video went to the other
 * side whole). With anything on the head nothing comes back, and a muted
 * microphone sends nothing: there the sound stays media — its full volume
 * for a live (why 0.25 left call mode) and Bluetooth in music quality, never
 * the telephone link.
 */
fun playbackMode(output: Output?, microphoneOpen: Boolean): PlaybackMode =
    if (microphoneOpen && (output == Output.SPEAKER || output == Output.EARPIECE)) PlaybackMode.CALL
    else PlaybackMode.MEDIA

/**
 * Which output a call uses, as a rule with no Android in it.
 *
 * Left alone, the best one present wins: whatever the person put on their
 * head, and otherwise the loudspeaker — a group room is not held to the ear.
 * A choice made by hand sticks until that output goes away, or until a
 * headset arrives: connecting one mid-call is itself a choice, the one the
 * plan names ("trocar sozinho ao conectar um fone").
 */
class OutputPolicy(private val phoneLike: Boolean = false) {
    var manual: Output? = null
        private set

    /**
     * A direct call is held to the ear, like a telephone ([phoneLike]) —
     * until there is a picture on it: a camera or a shared screen is looked
     * at, not listened to, and then the loudspeaker wins again.
     */
    var video = false

    fun choose(o: Output) {
        manual = o
    }

    /** Called with what is present now and what was present before; returns the output to use. */
    fun pick(present: Set<Output>, before: Set<Output> = present): Output? {
        val arrived = present - before
        if (arrived.any { it.isHeadset }) manual = null
        if (manual != null && manual !in present) manual = null
        val order = if (phoneLike && !video) PHONE else PREFERENCE
        return manual ?: order.firstOrNull { it in present }
    }

    private val Output.isHeadset get() = this == Output.BLUETOOTH || this == Output.WIRED

    private companion object {
        val PREFERENCE = listOf(Output.BLUETOOTH, Output.WIRED, Output.SPEAKER, Output.EARPIECE)
        val PHONE = listOf(Output.BLUETOOTH, Output.WIRED, Output.EARPIECE, Output.SPEAKER)
    }
}
