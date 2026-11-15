package com.taleshernandes.harmony.media

import java.util.Locale

/** Numeric allowlist only: never serialize a stats report or an SDP into Diag. */
internal class ScreenDiagnostics {
    private val previous = mutableMapOf<String, Map<String, Any>>()
    private var lastAt: Long? = null

    fun clear() { previous.clear(); lastAt = null }

    fun due(now: Long): Boolean {
        val last = lastAt
        if (last != null && now - last < 10_000) return false
        lastAt = now
        return true
    }

    fun sample(key: String, values: Map<String, Any>): String {
        val old = previous.put(key, values.toMap())
        fun number(name: String) = (values[name] as? Number)?.toDouble()?.takeIf { it.isFinite() }
        fun delta(name: String): Double? {
            val a = number(name) ?: return null
            val b = (old?.get(name) as? Number)?.toDouble() ?: return null
            return (a - b).takeIf { it.isFinite() && it >= 0 }
        }
        val delay = delta("jitterBufferDelay")
        val count = delta("jitterBufferEmittedCount")
        val mean = if (delay != null && count != null && count > 0)
            String.format(Locale.ROOT, "%.1f", 1000 * delay / count) else "não encontrado"
        val fields = listOf("framesDecoded", "framesDropped", "framesReceived", "freezeCount",
            "frameWidth", "frameHeight", "estimatedPlayoutTimestamp", "packetsLost",
            "packetsReceived", "keyFramesDecoded", "totalDecodeTime", "jitterBufferDelay",
            "jitterBufferEmittedCount")
        return "jbIntervalMs=$mean " + fields.joinToString(" ") {
            "$it=${number(it)?.let { n -> String.format(Locale.ROOT, "%.3f", n).removeSuffix(".000") } ?: "não encontrado"}"
        }
    }
}

/** Compare the first stream ID, which WebRTC uses as its sync group. Never log the IDs. */
internal fun screenSyncGroup(sdp: String, videoMid: String?, audioMid: String?): String {
    if (videoMid == null || audioMid == null) return "não encontrado"
    val streams = mutableMapOf<String, String>()
    var mid: String? = null
    var first: String? = null
    fun save() { val m = mid; val s = first; if (m != null && s != null) streams[m] = s }
    for (line in sdp.lineSequence().map { it.trim() }) {
        if (line.startsWith("m=")) { save(); mid = null; first = null }
        if (line.startsWith("a=mid:")) mid = line.removePrefix("a=mid:")
        val msid = when {
            line.startsWith("a=msid:") -> line.removePrefix("a=msid:")
            line.startsWith("a=ssrc:") && " msid:" in line -> line.substringAfter(" msid:")
            else -> null
        }
        if (first == null && msid != null) first = msid.substringBefore(' ').takeIf { it.isNotEmpty() && it != "-" }
    }
    save()
    val video = streams[videoMid] ?: return "não encontrado"
    val audio = streams[audioMid] ?: return "não encontrado"
    return if (video == audio) "same" else "different"
}
