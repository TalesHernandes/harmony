package com.taleshernandes.harmony.media

import android.content.Context
import com.taleshernandes.harmony.diag.Diag
import org.webrtc.MediaConstraints
import org.webrtc.MediaStreamTrack
import org.webrtc.PeerConnection
import org.webrtc.PeerConnectionFactory
import org.webrtc.RtpTransceiver
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * A1's proof that the WebRTC engine links and runs on this phone: load the
 * native library, build a factory, open a connection with an audio slot, and
 * have the engine write an offer. No network, no microphone. If this fails,
 * nothing in A2 onwards can work, which is why it runs at every start and its
 * answer is on the screen.
 */
object WebRtcProbe {
    const val LIBRARY = "Shiguredo libwebrtc 155.8059.1.0 (M155)"

    /**
     * [run], unless it killed the app last time. A native abort cannot be
     * caught — it takes the process and the in-memory log with it — so a mark
     * is written before the probe and removed after. Finding the mark means
     * the last start died inside the probe: this start skips it and says so,
     * and the rest of the app stays usable. The first release build did
     * exactly this on the owner's phone (24/09/2026): it closed on every open
     * and there was no log to share.
     */
    fun guarded(context: Context): String {
        val prefs = context.getSharedPreferences("probe", Context.MODE_PRIVATE)
        val build = context.packageManager.getPackageInfo(context.packageName, 0).longVersionCompat()
        if (prefs.getLong(KEY_RUNNING, -1) == build) {
            Diag.log("webrtc: o app fechou durante o teste na última abertura; teste pulado nesta versão")
            return "falhou: o app fechou durante o teste (pulado)"
        }
        prefs.edit().putLong(KEY_RUNNING, build).commit()
        val result = run(context)
        prefs.edit().remove(KEY_RUNNING).commit()
        return result
    }

    private const val KEY_RUNNING = "running_version"

    @Suppress("DEPRECATION")
    private fun android.content.pm.PackageInfo.longVersionCompat(): Long =
        if (android.os.Build.VERSION.SDK_INT >= 28) longVersionCode else versionCode.toLong()

    /** Blocking; call it off the main thread. Returns a line for the screen. */
    fun run(context: Context): String {
        val t0 = System.nanoTime()
        return try {
            Engine.init(context)
            val factory = PeerConnectionFactory.builder().createPeerConnectionFactory()
            val config = PeerConnection.RTCConfiguration(emptyList()).apply {
                sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
            }
            val pc = factory.createPeerConnection(config, object : PeerConnection.Observer {
                override fun onSignalingChange(s: PeerConnection.SignalingState?) {}
                override fun onIceConnectionChange(s: PeerConnection.IceConnectionState?) {}
                override fun onIceConnectionReceivingChange(b: Boolean) {}
                override fun onIceGatheringChange(s: PeerConnection.IceGatheringState?) {}
                override fun onIceCandidate(c: org.webrtc.IceCandidate?) {}
                override fun onIceCandidatesRemoved(c: Array<out org.webrtc.IceCandidate>?) {}
                override fun onAddStream(s: org.webrtc.MediaStream?) {}
                override fun onRemoveStream(s: org.webrtc.MediaStream?) {}
                override fun onDataChannel(d: org.webrtc.DataChannel?) {}
                override fun onRenegotiationNeeded() {}
            }) ?: error("createPeerConnection devolveu null")
            pc.addTransceiver(
                MediaStreamTrack.MediaType.MEDIA_TYPE_AUDIO,
                RtpTransceiver.RtpTransceiverInit(RtpTransceiver.RtpTransceiverDirection.RECV_ONLY),
            )
            val done = CountDownLatch(1)
            var sdp: String? = null
            var err: String? = null
            pc.createOffer(object : SdpObserver {
                override fun onCreateSuccess(d: SessionDescription) {
                    sdp = d.description
                    done.countDown()
                }
                override fun onCreateFailure(e: String?) {
                    err = e
                    done.countDown()
                }
                override fun onSetSuccess() {}
                override fun onSetFailure(e: String?) {}
            }, MediaConstraints())
            val finished = done.await(5, TimeUnit.SECONDS)
            pc.dispose()
            factory.dispose()
            val ms = (System.nanoTime() - t0) / 1_000_000
            val offer = sdp
            when {
                !finished -> "falhou: a oferta não saiu em 5 s".also { Diag.log("webrtc: $it") }
                offer == null -> "falhou: $err".also { Diag.log("webrtc: $it") }
                else -> {
                    val opus = offer.contains("opus/48000")
                    Diag.log("webrtc: ok em $ms ms, oferta de ${offer.length} bytes, opus=$opus")
                    "ok ($ms ms)"
                }
            }
        } catch (t: Throwable) {
            // UnsatisfiedLinkError is the one this exists to catch.
            Diag.log("webrtc: falhou ${t.javaClass.simpleName}: ${t.message}")
            "falhou: ${t.javaClass.simpleName}"
        }
    }
}
