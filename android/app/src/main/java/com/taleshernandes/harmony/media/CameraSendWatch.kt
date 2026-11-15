package com.taleshernandes.harmony.media

import com.taleshernandes.harmony.diag.Diag
import org.webrtc.RTCStatsReport

/**
 * Once a second, from a stats report: is the camera going out? Said five
 * seconds after it starts, and then only when it stops or starts moving
 * again, with the encoder that is doing the work. The room and the direct
 * call read their cameras with this one, so the two Diag lines are the same
 * (a 1:1 camera at 7–8 fps on 29/10/2026 had no line at all).
 */
class CameraSendWatch {
    private var frames = -1L
    private var framesAt = 0L
    private var moving = false
    private var saidAt = 0L

    /** The camera (re)started: measure from scratch. */
    fun reset() {
        frames = -1L
        moving = false
        saidAt = 0L
    }

    /** [mid] is the camera's send slot. */
    fun judge(report: RTCStatsReport, mid: String?, now: Long) {
        val out = report.statsMap.values.firstOrNull {
            it.type == "outbound-rtp" && it.members["kind"] == "video" && it.members["mid"] == mid
        } ?: return
        val encoded = (out.members["framesEncoded"] as? Number)?.toLong() ?: return
        if (frames < 0) {
            frames = encoded
            framesAt = now
            saidAt = now
            return
        }
        val secs = (now - framesAt) / 1000.0
        if (secs < 0.9) return
        val fps = (encoded - frames) / secs
        frames = encoded
        framesAt = now
        val nowMoving = fps >= 1
        val what = "${out.members["frameWidth"] ?: "?"}x${out.members["frameHeight"] ?: "?"} a ${"%.0f".format(fps)} q/s, " +
            "codificador ${out.members["encoderImplementation"] ?: "?"}" +
            (out.members["qualityLimitationReason"]?.let { if (it != "none") ", limitado por $it" else "" } ?: "")
        if (nowMoving != moving) {
            Diag.log(if (nowMoving) "vídeo: a câmera está mandando: $what" else "vídeo: a câmera parou de mandar: $what")
            moving = nowMoving
        } else if (saidAt != 0L && now - saidAt >= 5000) {
            Diag.log("vídeo: câmera mandando $what")
            saidAt = 0L
        }
    }
}
