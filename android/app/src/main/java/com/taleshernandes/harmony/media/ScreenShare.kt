package com.taleshernandes.harmony.media

import org.webrtc.RtpParameters
import kotlin.math.min

/** One ordinary, RID-less stream; the SFU forwards it without a layer ladder. */
internal object ScreenShare {
    const val FPS = 60
    // media.ts SCREEN_CEILING['720']['60']; a ceiling, never a target rate.
    const val MAX_BPS = 12_000_000

    /** libwebrtc puts the current send codec first in GetRtpSendParameters. */
    fun senderCodec(params: RtpParameters, chosen: String): RtpParameters.Codec {
        val sending = params.codecs.firstOrNull() ?: error("No negotiated screen codec")
        check(sending.name.equals(chosen, true)) { "Screen sender did not select requested codec" }
        return sending
    }

    /** Both caps are needed: ScreenCapturerAndroid ignores the capture FPS. */
    fun tune(params: RtpParameters, options: ScreenOptions = ScreenOptions()) {
        check(params.encodings.size == 1) { "Screen requires one encoding" }
        params.encodings.single().apply {
            maxBitrateBps = options.maxBps
            maxFramerate = options.framesPerSecond
        }
        params.degradationPreference = RtpParameters.DegradationPreference.MAINTAIN_FRAMERATE
    }

    data class Size(val width: Int, val height: Int)

    /** Keep the phone's aspect ratio, in either orientation, within the chosen 16:9 box. */
    fun size(width: Int, height: Int, heightCap: Int = 720): Size {
        require(width > 0 && height > 0)
        require(heightCap in listOf(480, 720, 1080))
        val scale = min(1.0, min(heightCap * 16.0 / 9 / maxOf(width, height), heightCap.toDouble() / minOf(width, height)))
        fun even(value: Int) = maxOf(2, (value * scale).toInt() / 2 * 2)
        return Size(even(width), even(height))
    }

    /** H.264 when this engine can encode it; VP8 is the explicit fallback. */
    fun codec(names: List<String>): String? = when {
        names.any { it.equals("H264", ignoreCase = true) } -> "H264"
        names.any { it.equals("VP8", ignoreCase = true) } -> "VP8"
        else -> null
    }

    /**
     * An answerer's send codec comes from the remote offer, not from its own
     * answer preferences (libwebrtc BaseChannel::SetRemoteContent_w).
     * Reorder only the named screen m-line, keeping every payload/profile and
     * attribute: narrowing the first video answer can narrow the SFU's codec
     * set for later camera/receive slots too (sfu/codecs.go).
     */
    fun preferCodec(sdp: String, mid: String, chosen: String): String {
        require(chosen == "H264" || chosen == "VP8")
        val separator = if (sdp.contains("\r\n")) "\r\n" else "\n"
        val lines = sdp.split(separator).toMutableList()
        val starts = lines.indices.filter { lines[it].startsWith("m=") }
        val sections = starts.mapIndexed { i, start -> start until (starts.getOrNull(i + 1) ?: lines.size) }
        val section = sections.singleOrNull { range -> range.any { lines[it] == "a=mid:$mid" } }
            ?: error("Screen mid not uniquely present in offer")
        val fields = lines[section.first].split(Regex("\\s+"))
        check(fields.size > 3 && fields[0] == "m=video") { "Screen mid is not video" }
        val formats = fields.drop(3)
        val names = HashMap<String, String>()
        val repair = HashMap<String, String>()
        val rtpmap = Regex("^a=rtpmap:(\\d+) ([^/ ]+)/.*$", RegexOption.IGNORE_CASE)
        val fmtp = Regex("^a=fmtp:(\\d+) (.*)$", RegexOption.IGNORE_CASE)
        val apt = Regex("(?:^|;\\s*)apt=(\\d+)(?:;|$)", RegexOption.IGNORE_CASE)
        for (i in section) {
            rtpmap.matchEntire(lines[i])?.let { names[it.groupValues[1]] = it.groupValues[2] }
            fmtp.matchEntire(lines[i])?.let { line ->
                apt.find(line.groupValues[2])?.let { repair[line.groupValues[1]] = it.groupValues[1] }
            }
        }
        val primary = formats.filter { names[it].equals(chosen, ignoreCase = true) }
        check(primary.isNotEmpty()) { "Chosen screen codec is absent from offer" }
        val preferred = primary.flatMap { pt ->
            listOf(pt) + formats.filter { names[it].equals("rtx", true) && repair[it] == pt }
        }
        val order = preferred + formats.filterNot { it in preferred }
        if (order == formats) return sdp
        lines[section.first] = (fields.take(3) + order).joinToString(" ")
        return lines.joinToString(separator)
    }
}

/** Same selectors as the web; automatic on a phone resolves to 720p60. */
data class ScreenOptions(val quality: String = "auto", val fps: String = "auto", val sound: Boolean = true) {
    init {
        require(quality in listOf("auto", "1080", "720", "480"))
        require(fps in listOf("auto", "60", "30", "15"))
    }
    val height get() = if (quality == "auto") 720 else quality.toInt()
    val framesPerSecond get() = if (fps == "auto") ScreenShare.FPS else fps.toInt()
    val maxBps get() = when (height) {
        1080 -> when (framesPerSecond) { 60 -> 15_000_000; 30 -> 12_000_000; else -> 9_000_000 }
        720 -> when (framesPerSecond) { 60 -> ScreenShare.MAX_BPS; 30 -> 9_000_000; else -> 6_000_000 }
        else -> 6_000_000
    }
}

/** Invalidates permission/service replies and unpublish retries from an old choice. */
internal class ScreenChoice {
    var turn = 0
        private set
    var wanted = false
        private set

    fun choose(on: Boolean): Int {
        wanted = on
        return ++turn
    }

    fun accepts(replyTurn: Int): Boolean = wanted && turn == replyTurn

    fun shouldPublish(session: String?, publishedSession: String?): Boolean =
        wanted && session != null && session != publishedSession
}
