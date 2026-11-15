package com.taleshernandes.harmony.net

import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.model.GatewayEvent
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

/** The wait before the next reconnect: doubling from 0.5 s to 5 s, as the web does (gateway.ts). */
class Backoff(private val first: Long = 500, private val max: Long = 5_000) {
    private var next = first

    fun next(): Long = next.also { next = minOf(next * 2, max) }

    fun reset() {
        next = first
    }
}

/**
 * The live connection, `/ws`. It only receives (PROTOCOL-HARMONY §2).
 *
 * On every open — the first one included — [Listener.onOpen] asks the app to
 * fetch again, because whatever happened while the socket was closed did not
 * arrive on it. A 401 on the handshake means the session is gone, and that is
 * the one failure that does not retry.
 */
class Gateway(
    private val client: OkHttpClient,
    private val baseUrl: String,
    private val cookie: String,
    private val listener: Listener,
    /**
     * Whether the app is behind the screen as the socket opens. Said in the
     * address, so the server never counts a phone in a pocket as looking —
     * not even for the moment before the presence POST lands (gateway.go).
     */
    private val hidden: () -> Boolean = { false },
) {
    interface Listener {
        fun onOpen()
        fun onEvent(ev: GatewayEvent)
        fun onDown()
        fun onUnauthorized()
    }

    private val timer = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "gateway-retry") }
    private val backoff = Backoff()
    private var ws: WebSocket? = null
    private var retry: ScheduledFuture<*>? = null
    // Stopped until start(): a gateway made and not started yet must not be
    // opened by a network callback's nudge (Core defers the start, A6).
    private var stopped = true

    /** Opens the socket. A second call does nothing: two callers may race to it (Core). */
    @Synchronized
    fun start() {
        if (!stopped || timer.isShutdown) return
        stopped = false
        open()
    }

    @Synchronized
    fun stop() {
        stopped = true
        retry?.cancel(false)
        ws?.close(1000, null)
        ws = null
        timer.shutdownNow()
    }

    /** Try now instead of waiting out the backoff — the network just came back. */
    @Synchronized
    fun nudge() {
        if (stopped || ws != null) return
        retry?.cancel(false)
        backoff.reset()
        Diag.log("ws network available: retry immediately")
        open()
    }

    /** Drop the socket and open a new one now: the network under it is gone. */
    @Synchronized
    fun reconnect() {
        if (stopped) return
        retry?.cancel(false)
        val old = ws
        // Forgotten first, so that its failure, when it comes, is not
        // mistaken for the new socket's.
        ws = null
        old?.cancel()
        backoff.reset()
        Diag.log("ws network changed: reconnect immediately")
        listener.onDown()
        open()
    }

    @Synchronized
    private fun open() {
        if (stopped) return
        val wsUrl = baseUrl.replaceFirst("http", "ws") + "/ws" + if (hidden()) "?state=hidden" else ""
        val req = Request.Builder()
            .url(wsUrl)
            .header("Cookie", "$SESSION_COOKIE=$cookie")
            // The server refuses a handshake from any other origin (gateway.go).
            .header("Origin", baseUrl)
            .build()
        val socket = client.newWebSocket(req, Handler())
        ws = socket
    }

    @Synchronized
    private fun lost(socket: WebSocket, why: String) {
        if (ws !== socket) return
        ws = null
        listener.onDown()
        if (stopped) return
        val wait = backoff.next()
        Diag.log("ws fechado ($why), tentando de novo em $wait ms")
        retry = timer.schedule({ open() }, wait, TimeUnit.MILLISECONDS)
    }

    private inner class Handler : WebSocketListener() {
        override fun onOpen(webSocket: WebSocket, response: Response) {
            synchronized(this@Gateway) {
                if (ws !== webSocket) return
                backoff.reset()
            }
            Diag.log("ws aberto")
            listener.onOpen()
        }

        override fun onMessage(webSocket: WebSocket, text: String) {
            // A socket already replaced or stopped may still be delivering.
            if (synchronized(this@Gateway) { ws !== webSocket }) return
            val ev = Json.event(text)
            if (ev == null) {
                Diag.log("ws mensagem ilegível")
                return
            }
            Diag.log("ws ${ev.type}")
            listener.onEvent(ev)
        }

        override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
            webSocket.close(1000, null)
            lost(webSocket, "servidor fechou, $code")
        }

        override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
            if (response?.code == 401) {
                Diag.log("ws 401: sessão não vale mais")
                synchronized(this@Gateway) {
                    if (ws !== webSocket) return
                    ws = null
                    stopped = true
                }
                listener.onUnauthorized()
                return
            }
            lost(webSocket, response?.let { "http ${it.code}" } ?: t.javaClass.simpleName)
        }
    }
}
