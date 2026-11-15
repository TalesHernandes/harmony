package com.taleshernandes.harmony

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import com.taleshernandes.harmony.net.NetworkChanges
import android.os.Build
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.media.CallService
import com.taleshernandes.harmony.media.DirectCall
import com.taleshernandes.harmony.media.VoiceCall
import com.taleshernandes.harmony.media.WebRtcProbe
import com.taleshernandes.harmony.model.CallPerson
import com.taleshernandes.harmony.model.GatewayEvent
import com.taleshernandes.harmony.model.RoomsState
import com.taleshernandes.harmony.model.User
import com.taleshernandes.harmony.update.Updater
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.Gateway
import com.taleshernandes.harmony.net.KeystoreVault
import com.taleshernandes.harmony.net.LoginResult
import com.taleshernandes.harmony.net.Saved
import com.taleshernandes.harmony.net.ServerUrl
import com.taleshernandes.harmony.net.Unauthorized
import com.taleshernandes.harmony.net.ApiError
import com.taleshernandes.harmony.push.Fetched
import com.taleshernandes.harmony.push.Messaging
import com.taleshernandes.harmony.push.NoticeRules
import com.taleshernandes.harmony.push.Notices
import com.taleshernandes.harmony.push.Ringing
import com.taleshernandes.harmony.push.Wake
import com.google.firebase.messaging.FirebaseMessaging
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient
import java.util.concurrent.TimeUnit

const val DEFAULT_SERVER = "https://harmony.taleshernandes.com"

sealed interface Screen {
    data object Loading : Screen
    data class SignIn(val server: String, val error: String? = null, val busy: Boolean = false) : Screen
    data class SecondFactor(val server: String, val method: String, val challenge: String,
        val error: String? = null, val busy: Boolean = false, val recovery: Boolean = false) : Screen
    data class ForgotPassword(val server: String, val sent: Boolean = false,
        val error: String? = null, val busy: Boolean = false) : Screen
    data class VerifyEmail(val user: User?, val required: Boolean, val error: String? = null) : Screen
    /** [user] is null while the server has not been reached since the app opened. */
    data class Rooms(val user: User?) : Screen
}

/**
 * The app's state, one per process. It outlives the activity, so turning the
 * phone does not sign anybody out or reopen the socket.
 */
class Core(private val context: Context) {
    private val vault = KeystoreVault(context)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val client = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(20, TimeUnit.SECONDS)
        // The server pings every 25 s; this is the phone noticing a dead
        // network by itself, which a switch from Wi-Fi to 4G produces silently.
        .pingInterval(30, TimeUnit.SECONDS)
        // The cookie is put on each request by hand, and OkHttp keeps a
        // hand-set header across a redirect to another host. The server never
        // redirects, so a redirect is refused rather than followed.
        .followRedirects(false)
        .followSslRedirects(false)
        .build()
    // For fetching a notification's text: Firebase gives the app a few
    // seconds, so a server that does not answer must not use all of them.
    private val wakeClient = client.newBuilder().callTimeout(5, TimeUnit.SECONDS).build()
    private val userAgent =
        "harmony-android/${BuildConfig.VERSION_NAME} (Android ${Build.VERSION.RELEASE}; ${Build.MANUFACTURER} ${Build.MODEL})"

    private val _screen = MutableStateFlow<Screen>(Screen.Loading)
    val screen: StateFlow<Screen> = _screen
    private val _rooms = MutableStateFlow(RoomsState())
    val rooms: StateFlow<RoomsState> = _rooms
    private val _live = MutableStateFlow(false)
    val live: StateFlow<Boolean> = _live
    private val _webrtc = MutableStateFlow("testando…")
    val webrtc: StateFlow<String> = _webrtc

    @Volatile private var api: Api? = null
    @Volatile private var emailSkipped = false
    @Volatile private var emailRequired = false
    @Volatile private var accountUser: User? = null
    // The session signed in, for fetching a notification's text with the
    // short-timeout client, and for telling whether it is still current.
    @Volatile private var saved: Saved? = null

    /** The API of the signed-in session, for pictures the screen fetches itself. */
    fun currentApi(): Api? = api
    private var gateway: Gateway? = null

    val voice = VoiceCall(context) { api }

    val chat = Conversations(context, scope, { api }, ::myName) { channelId, lastReadId ->
        _rooms.update { it.markRead(channelId, lastReadId) }
    }

    val notices = Notices(context)
    val ringing = Ringing(context)
    private val sounds = com.taleshernandes.harmony.media.Sounds(context)

    /** The new version, when the server has one (A8). */
    val updater = Updater(context, scope, { api }, { diagHeader() })

    /** The direct call (A7): one person ringing another, phone to computer. */
    val direct = DirectCall(context, ::callApi, object : DirectCall.Hooks {
        override fun awaitLive(timeoutMs: Long): Boolean {
            // Answering is opening the app, even over the lock screen: the
            // socket carries the other end's offer, and opens now.
            val g = gateway ?: return false
            g.start()
            val end = System.currentTimeMillis() + timeoutMs
            while (!_live.value && System.currentTimeMillis() < end) Thread.sleep(100)
            return _live.value
        }

        override fun ringing(callId: String, from: CallPerson, at: Long) = this@Core.ringing.show(callId, from, at)

        override fun ringOver(callId: String) = this@Core.ringing.cancel()

        override fun leaveRoom() {
            if (voice.state.value.channelId == null) return
            Diag.log("chamada direta: saindo da sala")
            voice.leave()
            // The room hands the phone's audio back (mode, focus, output) on
            // its own thread; the call must not take it before that, or the
            // room's hand-back lands on top of the call's.
            // Up to a join stuck on the network (VoiceCall's own timeouts).
            val end = System.currentTimeMillis() + 12_000
            while (voice.state.value.phase != VoiceCall.Phase.OFF && System.currentTimeMillis() < end) Thread.sleep(50)
        }
    }, voice.volumes)

    // Whether the app is on screen, and whether a video covers it: a message
    // arriving in the open conversation while nobody can see it is unread.
    // It starts false: the process may be started by a notification with no
    // screen at all, and the activity says so the moment it is shown.
    @Volatile private var looking = false
    // Whether the app has been on screen at all in this process.
    @Volatile private var shown = false
    @Volatile private var covered = false

    private fun myName(): String? = (_screen.value as? Screen.Rooms)?.user?.username

    /** The conversation being read right now, for counting what is unread. */
    private fun reading(): String? = if (visible()) chat.state.value.open else null

    /** A channel the app learned of from an answer rather than from the socket. */
    fun knowChannel(c: com.taleshernandes.harmony.model.Channel) {
        synchronized(lock) {
            _rooms.update { r -> if (r.channels.any { it.id == c.id }) r else r.apply(GatewayEvent.ChannelUpsert(c)) }
        }
    }

    fun setLooking(on: Boolean) {
        val reopened = on && !looking
        looking = on
        if (reopened && emailSkipped) {
            emailSkipped = false
            val a = api
            if (a != null && accountUser?.emailVerified == false) requireEmail(a, emailRequired)
        }
        if (on && !shown) {
            shown = true
            gateway?.start()
        }
        if (on) {
            updater.resumeIfAllowed()
            updater.check("aberto")
        }
        chat.setLooking(on && !covered)
        if (on) chat.drafts.unpersist() else chat.drafts.persist()
        seen()
    }

    /** A video in full screen or in the floating window hides the conversation. */
    fun setCovered(on: Boolean) {
        covered = on
        chat.setLooking(looking && !on)
        seen()
    }

    // Whether the conversations can be seen: the app on screen and no video
    // over it. The floating window keeps the app started while another app has
    // the screen, and a person watching a stream in the corner is not reading
    // messages — they must be told of them (Codex audit, 26/09/2026).
    private fun visible() = looking && !covered

    private fun seen() {
        val v = visible()
        presence.update { it.copy(looking = v) }
        if (v) chat.state.value.open?.let { notices.cancel(it, Long.MAX_VALUE) }
    }

    // ------------------------------------------------------------ presence

    // What the server is told about this phone: conversations in view or not,
    // and in a call or not. The server decides from it whether a notification
    // is worth sending (notify.go: nobody looking at the app is woken). [n]
    // only forces a resend: a reopened socket starts over from what it said.
    private data class Report(val looking: Boolean, val busy: Boolean, val n: Int)
    private val presence = MutableStateFlow(Report(looking = false, busy = false, n = 0))

    /** The app's own sounds follow the room and the direct call (media/Sounds). */
    private fun playSounds() {
        sounds.warm()
        scope.launch {
            combine(voice.state, _rooms, _screen) { v, r, sc -> Triple(v, r, (sc as? Screen.Rooms)?.user?.username) }
                .collect { (v, r, me) ->
                    sounds.voice(v)
                    val id = v.channelId ?: return@collect
                    if (me != null) sounds.room(id, r.peers[id].orEmpty(), me, v.phase != VoiceCall.Phase.LIVE)
                }
        }
        scope.launch { direct.state.collect { sounds.direct(it) } }
    }

    private fun reportPresence() {
        // One collector, so the POSTs go out in order. collectLatest: a newer
        // state ends the retries of an older one, after waiting for a POST
        // already on the wire. A POST that fails is tried again: one lost
        // "hidden" would leave the server sure the person is looking, and
        // swallowing every notification, for as long as the socket lives.
        scope.launch {
            presence.collectLatest { r ->
                var wait = PRESENCE_RETRY_FIRST_MS
                while (true) {
                    val a = api ?: return@collectLatest
                    // Not connected: the next socket to open sends it again.
                    if (!_live.value) return@collectLatest
                    try {
                        a.presence(if (r.looking) "active" else "hidden", r.busy)
                        return@collectLatest
                    } catch (_: Unauthorized) {
                        return@collectLatest
                    } catch (e: Exception) {
                        Diag.log("presença falhou: ${e.javaClass.simpleName}, de novo em $wait ms")
                    }
                    delay(wait)
                    wait = minOf(wait * 2, PRESENCE_RETRY_MAX_MS)
                }
            }
        }
        scope.launch {
            // In a room or on a direct call: online for as long as it lasts.
            combine(voice.state, direct.state) { v, d -> v.channelId != null || d.active }
                .distinctUntilChanged()
                .collect { busy -> presence.update { it.copy(busy = busy) } }
        }
        // A conversation opened in view is read: its notification goes.
        scope.launch {
            chat.state.map { it.open }.distinctUntilChanged().collect { id ->
                if (id != null && visible()) notices.cancel(id, Long.MAX_VALUE)
            }
        }
    }

    // ------------------------------------------------------------ notifications

    // The session a registration was last confirmed for, with the address it
    // registered ("epoch:token"). A new address from Firebase, or a new
    // session, is registered again; a failure leaves it for the next resync.
    @Volatile private var pushRegistered: String? = null
    // One registration at a time, each reading the current session when its
    // turn comes: a slow one from the account just left cannot land after the
    // new account's and hand the phone back to the old one (audit, 26/09/2026).
    private val pushLock = Mutex()

    private fun registerPush() {
        if (!Messaging.available(context)) {
            Diag.log("avisos: este build não tem o Firebase")
            return
        }
        FirebaseMessaging.getInstance().token.addOnCompleteListener { t ->
            if (!t.isSuccessful) {
                // No Google Play services, or no network to reach them.
                Diag.log("avisos: sem endereço do Firebase (${t.exception?.javaClass?.simpleName})")
                return@addOnCompleteListener
            }
            val token = t.result ?: return@addOnCompleteListener
            scope.launch { sendToken(token) }
        }
    }

    private suspend fun sendToken(token: String) = pushLock.withLock {
        val a = api ?: return@withLock
        val mine = epoch
        val key = "$mine:$token"
        if (pushRegistered == key) return@withLock
        try {
            val devices = a.registerPush(token)
            if (mine == epoch) pushRegistered = key
            Diag.log("avisos: aparelho registrado ($devices no total)")
        } catch (e: ApiError) {
            Diag.log(if (e.status == 503) "avisos: o servidor está sem o Firebase" else "avisos: registro recusado (${e.status})")
        } catch (e: Exception) {
            Diag.log("avisos: registro falhou (${e.javaClass.simpleName})")
        }
    }

    /** Firebase handed this phone a new address. */
    fun pushTokenChanged(token: String) {
        scope.launch { sendToken(token) }
    }

    // Showing a notification and taking one away happen on different threads
    // (Firebase's, the socket's, the screen's). Under one lock, a notification
    // fetched for a session cannot land after that session ended, nor after
    // its message was read while it was being fetched (audit, 26/09/2026).
    private val noticeLock = Any()
    // How far each conversation is known to be read (socket and lists).
    private val readUpTo = HashMap<String, Long>()

    /** Read up to [lastReadId], here or on another device. */
    private fun readTo(channelId: String, lastReadId: Long) = synchronized(noticeLock) {
        readUpTo[channelId] = maxOf(readUpTo[channelId] ?: 0L, lastReadId)
        notices.cancel(channelId, lastReadId)
    }

    /**
     * Firebase woke the phone. Runs on Firebase's thread and blocks it while
     * the text is fetched from our server — that is the design (Wake.kt):
     * Google carried only numbers.
     */
    fun onWake(data: Map<String, String>, priority: String = "") {
        val w = Wake.parse(data)
        if (w == null) {
            Diag.log("aviso ilegível")
            return
        }
        // How long Google and the phone took, measured instead of guessed:
        // [at] is when the server raised it (its clock; phones keep theirs
        // within a second). The server itself sends in under half a second.
        val late = if (w.at > 0) " ${fmtSecs(System.currentTimeMillis() - w.at)} depois de enviado" else ""
        Diag.log("aviso chegou: ${w.kind}$late$priority")
        if (w.kind == Wake.CALL) return wakeCall(w)
        if (w.kind == Wake.WATCH) return watchAlert(w)
        if (visible() && chat.state.value.open == w.channelId) return
        // Started by the notification with no screen, the session may not be
        // loaded yet: read it here. Always through the short-timeout client:
        // Firebase gives only a few seconds.
        val session = saved ?: vault.load()
        if (session == null) {
            Diag.log("aviso sem sessão: ignorado")
            return
        }
        val a = wakeApi(session)
        val notice = try {
            when (w.kind) {
                Wake.RING -> NoticeRules.ring(w, fetchOrNull { a.pendingNotices() }, System.currentTimeMillis())
                else -> NoticeRules.message(w, fetchMessage(a, w), myName())
            }
        } catch (_: Unauthorized) {
            Diag.log("aviso: a sessão não vale mais")
            null
        }
        if (notice == null) {
            Diag.log("aviso: nada a mostrar (lido, apagado ou vencido)")
            return
        }
        synchronized(noticeLock) {
            // Signed out, or into another account, while it was fetched.
            if ((saved ?: vault.load())?.cookie != session.cookie) return
            if (visible() && chat.state.value.open == notice.channelId) return
            if (notice.messageId > 0 && notice.messageId <= (readUpTo[notice.channelId] ?: 0L)) return
            notices.show(notice)
            if (w.at > 0) Diag.log("aviso na tela ${fmtSecs(System.currentTimeMillis() - w.at)} depois de enviado")
        }
    }

    /**
     * The watchman on the other machine says harmony stopped answering, or came
     * back (cmd/harmony/watch.go). The phone asks the server itself before
     * saying "fora do ar": in September the watchman saw a week of 502s while
     * everybody was using the service, and a phone that can reach the server
     * says that instead.
     */
    private fun watchAlert(w: Wake) {
        val base = (saved ?: vault.load())?.baseUrl
        if (base == null) {
            Diag.log("vigia: sem sessão, ignorado")
            return
        }
        val answers = try {
            wakeClient.newCall(okhttp3.Request.Builder().url("$base/healthz").build()).execute().use { it.code == 200 }
        } catch (_: Exception) {
            false
        }
        Diag.log("vigia: servidor ${w.state}; daqui ${if (answers) "responde" else "não responde"}")
        val (title, text) = when {
            w.state == Wake.UP -> "O harmony voltou" to "O vigia voltou a falar com o servidor."
            !answers -> "O harmony está fora do ar" to "Nem o vigia nem este celular conseguem falar com o servidor."
            else -> "O vigia não alcança o harmony" to "Daqui o servidor responde; pode ser o caminho até o vigia."
        }
        notices.showServer(title, text, if (w.at > 0) w.at else System.currentTimeMillis())
    }

    /**
     * Somebody is calling, and the phone was asleep. Google carried only the
     * call's id; who is calling comes from our server, with this phone's
     * session. Without an answer the phone still rings — "Alguém" — and the
     * name arrives once the ringing phone asks again (DirectCall).
     */
    private fun wakeCall(w: Wake) {
        if (w.at > 0 && System.currentTimeMillis() - w.at > DirectCall.RING_MS) {
            Diag.log("chamada: o aviso chegou depois de a chamada acabar")
            return
        }
        val session = saved ?: vault.load()
        if (session == null) {
            Diag.log("chamada sem sessão: ignorada")
            return
        }
        val a = wakeApi(session)
        val info = try {
            a.callGet(w.callId)
        } catch (_: Unauthorized) {
            Diag.log("chamada: a sessão não vale mais")
            return
        } catch (e: ApiError) {
            // 404: over already (given up, or answered and hung up).
            Diag.log("chamada: já acabou (${e.status})")
            return
        } catch (e: Exception) {
            Diag.log("chamada: busca falhou (${e.javaClass.simpleName})")
            null
        }
        if (info?.answered == true) {
            Diag.log("chamada: já atendida em outro aparelho")
            return
        }
        // Signed out, or into another account, while it was fetched.
        if ((saved ?: vault.load())?.cookie != session.cookie) return
        direct.called(w.callId, info?.from ?: CallPerson("", ""), w.at)
    }

    /**
     * The API for the call: the app's, or — in a process just started by a
     * notification, before the app's own is loaded — one made from the
     * stored session. Recusar pressed on a phone that was asleep must reach
     * the server, or the other side rings on to nobody (audit, 26/09/2026).
     */
    private fun callApi(): Api? = api ?: (saved ?: vault.load())?.let { st ->
        wakeApi(st)
    }

    /** Background notification/call fetches must honor the same gate as the visible API. */
    private fun wakeApi(session: Saved): Api =
        Api(wakeClient, session.baseUrl, { session.cookie }, userAgent) {
            scope.launch {
                if ((saved ?: vault.load())?.cookie != session.cookie) return@launch
                val active = api
                if (active == null) enter(session, null) else requireEmail(active)
            }
        }

    /**
     * The call screen was opened for [id] — by the ringing notification,
     * which can outlive the process Android ended meanwhile. The call is
     * asked for again, and [then] hears (on any thread) whether it is ringing
     * here now. Answers false when it is over.
     */
    fun resumeCall(id: String, then: (Boolean) -> Unit) {
        if (direct.ringingNow(id)) return then(true)
        scope.launch {
            val info = try {
                callApi()?.callGet(id)
            } catch (e: Exception) {
                Diag.log("chamada: retomar falhou (${e.javaClass.simpleName})")
                null
            }
            if (info == null || info.answered) {
                ringing.cancel()
                return@launch then(false)
            }
            Diag.log("chamada retomada depois de o app ter sido encerrado")
            direct.called(id, info.from, 0)
            val end = System.currentTimeMillis() + 2000
            while (!direct.ringingNow(id) && System.currentTimeMillis() < end) delay(50)
            then(direct.ringingNow(id))
        }
    }

    /** Calls somebody. The microphone permission is the screen's to ask for, before this. */
    fun placeCall(username: String, displayName: String) {
        direct.ring(CallPerson(username, displayName))
        // Now, from the tap (see joinVoice).
        CallService.start(context)
    }

    private fun fmtSecs(ms: Long) = String.format(java.util.Locale.ROOT, "%.1f s", ms / 1000.0)

    // A message that is gone and a fetch that failed are different answers:
    // the first says nothing, the second says "Nova mensagem".
    private fun fetchMessage(a: Api, w: Wake): Fetched {
      return try {
        // An old FCM may arrive after a block. If the private list cannot be
        // checked, stay quiet rather than displaying the generic fallback.
        val preferences = try { a.personPreferences() } catch (e: Unauthorized) {
            throw e
        } catch (_: Exception) {
            return Fetched.Got(null, com.taleshernandes.harmony.model.Channel(w.channelId, "", "", ""))
        }

        val channel = a.channels().firstOrNull { it.id == w.channelId }
        if (channel == null) {
            // Not in that conversation any more.
            Fetched.Got(null, com.taleshernandes.harmony.model.Channel(w.channelId, "", "", ""))
        } else {
            val message = a.message(w.channelId, w.messageId)
            Fetched.Got(message?.takeUnless { m -> preferences.any { it.username == m.author.username && it.blocked } }, channel)
        }
    } catch (e: Unauthorized) {
        throw e
    } catch (e: Exception) {
        Diag.log("aviso: busca falhou (${e.javaClass.simpleName})")
        Fetched.Failed
      }
    }

    private fun <T> fetchOrNull(get: () -> T): T? = try {
        get()
    } catch (e: Unauthorized) {
        throw e
    } catch (e: Exception) {
        Diag.log("aviso: busca falhou (${e.javaClass.simpleName})")
        null
    }

    // ------------------------------------------------------------ tapping a notification

    // A conversation asked for before the channel list has arrived. Every tap
    // replaces it, including one answered at once: an older tap still waiting
    // must not open on top of the newer one when the list lands.
    @Volatile private var pendingOpen: String? = null
    private val _joinAsked = MutableStateFlow<String?>(null)
    /** A room a notification asked to join; the screen joins it (the microphone is its to ask for). */
    val joinAsked: StateFlow<String?> = _joinAsked

    fun noticeTapped(open: String?, join: String?) {
        pendingOpen = null
        if (open != null) {
            Diag.log("aviso tocado: abrir conversa")
            val c = _rooms.value.channels.firstOrNull { it.id == open }
            if (c != null) chat.open(c.id, c.lastReadId) else pendingOpen = open
        }
        if (join != null) {
            Diag.log("aviso tocado: entrar na sala")
            _joinAsked.value = join
        }
    }

    fun joinHandled() {
        _joinAsked.value = null
    }

    // Events that arrive while a resync is fetching. The fetched lists may be
    // older than them, so they are applied again on top once the lists land.
    private val lock = Any()
    private var heldDuringResync: MutableList<GatewayEvent>? = null
    // Which resync is the latest: an older one landing after a newer one
    // would put back a list that is already out of date.
    private var resyncs = 0
    // Which signed-in session is current. Work started for one session
    // (a fetch, a retry) must not touch the screen of the next one.
    @Volatile private var epoch = 0
    private var retryWait = RESYNC_FIRST_MS

    fun start() {
        Diag.log("app aberto: $userAgent")
        scope.launch { _webrtc.value = WebRtcProbe.guarded(context) }
        notices.createChannels()
        ringing.createChannel()
        Diag.log("chamada em tela cheia: ${if (ringing.fullScreenAllowed()) "liberada" else "NÃO liberada pelo Android"}")
        reportPresence()
        playSounds()
        watchNetwork()
        scope.launch {
            val saved = vault.load()
            if (saved == null) {
                _screen.value = Screen.SignIn(vault.lastBaseUrl() ?: DEFAULT_SERVER)
            } else {
                Diag.log("sessão guardada encontrada")
                enter(saved, user = null)
            }
        }
    }

    fun signIn(serverInput: String, username: String, password: String) {
        val base = ServerUrl.normalize(serverInput)
        if (base == null) {
            _screen.value = Screen.SignIn(serverInput, "Endereço do servidor inválido.")
            return
        }
        if (username.isBlank() || password.isEmpty()) {
            _screen.value = Screen.SignIn(serverInput, "Preencha usuário e senha.")
            return
        }
        _screen.value = Screen.SignIn(serverInput, busy = true)
        scope.launch {
            val probe = Api(client, base, { null }, userAgent)
            when (val r = probe.login(username.trim(), password)) {
                is LoginResult.SecondFactor ->
                    _screen.value = Screen.SecondFactor(base, r.method, r.challenge)
                is LoginResult.Ok -> {
                    val saved = Saved(base, r.cookie)
                    vault.save(saved)
                    enter(saved, r.user)
                }
                LoginResult.BadCredentials ->
                    _screen.value = Screen.SignIn(serverInput, com.taleshernandes.harmony.net.securityMessage("bad_credentials"))
                LoginResult.RateLimited ->
                    _screen.value = Screen.SignIn(serverInput, com.taleshernandes.harmony.net.securityMessage("rate_limited"))
                is LoginResult.Failed ->
                    _screen.value = Screen.SignIn(serverInput, r.why)
            }
        }
    }

    fun backToSignIn(server: String) {
        Diag.log("login: voltar")
        _screen.value = Screen.SignIn(server)
    }

    fun secondFactorRecovery() {
        val s = _screen.value as? Screen.SecondFactor ?: return
        if (!s.busy && s.method == "totp") {
            Diag.log("login: alternar recuperação")
            _screen.value = s.copy(recovery = !s.recovery, error = null)
        }
    }

    fun secondFactor(code: String) {
        val s = _screen.value as? Screen.SecondFactor ?: return
        if (s.busy || code.isBlank()) return
        _screen.value = s.copy(busy = true, error = null)
        scope.launch {
            val r = Api(client, s.server, { null }, userAgent).login2FA(s.challenge, code)
            if ((_screen.value as? Screen.SecondFactor)?.challenge != s.challenge) return@launch
            when (r) {
                is LoginResult.Ok -> {
                    val session = Saved(s.server, r.cookie)
                    vault.save(session)
                    enter(session, r.user)
                }
                else -> _screen.value = s.copy(error = when (r) {
                    LoginResult.BadCredentials -> com.taleshernandes.harmony.net.securityMessage("bad_credentials")
                    LoginResult.RateLimited -> com.taleshernandes.harmony.net.securityMessage("rate_limited")
                    is LoginResult.Failed -> r.why
                    else -> com.taleshernandes.harmony.net.securityMessage("invalid_code")
                })
            }
        }
    }

    fun openForgotPassword(server: String) {
        val base = ServerUrl.normalize(server)
        if (base == null) { _screen.value = Screen.SignIn(server, "Endereço do servidor inválido."); return }
        Diag.log("password forgot: abrir")
        _screen.value = Screen.ForgotPassword(base)
    }

    fun forgotPassword(identifier: String) {
        val s = _screen.value as? Screen.ForgotPassword ?: return
        if (s.busy || identifier.isBlank()) return
        _screen.value = s.copy(busy = true, error = null)
        scope.launch {
            val next = try {
                Api(client, s.server, { null }, userAgent).forgotPassword(identifier.trim())
                Diag.log("password forgot: pedido aceito")
                s.copy(sent = true)
            } catch (e: Exception) {
                Diag.log("password forgot: ${e.javaClass.simpleName}")
                s.copy(error = com.taleshernandes.harmony.net.securityError(e))
            }
            if (_screen.value is Screen.ForgotPassword) _screen.value = next
        }
    }

    /** Called by the transport, including calls made by other screens. */
    private fun requireEmail(a: Api, required: Boolean = true) {
        if (a !== api) return
        Diag.log("email gate: servidor exige confirmação")
        emailRequired = required
        emailSkipped = false
        gateway?.stop()
        gateway = null
        _live.value = false
        synchronized(lock) { ++resyncs; heldDuringResync = null }
        _screen.value = Screen.VerifyEmail(accountUser, required)
        refreshEmailGate()
    }

    fun refreshEmailGate() {
        val a = api ?: return
        val mine = epoch
        scope.launch {
            try {
                val status = a.meStatus()
                if (mine != epoch || a !== api) return@launch
                if (_screen.value !is Screen.VerifyEmail) return@launch
                accountUser = status.user
                emailRequired = emailRequired || status.emailRequired
                if (status.user.emailVerified) emailDone(status.user)
                else _screen.value = Screen.VerifyEmail(status.user, emailRequired)
            } catch (_: Unauthorized) { if (mine == epoch) forget() }
            catch (e: Exception) {
                Diag.log("email gate: ${e.javaClass.simpleName}")
                if (mine == epoch && _screen.value is Screen.VerifyEmail) _screen.value = Screen.VerifyEmail(accountUser, emailRequired,
                    com.taleshernandes.harmony.net.securityError(e))
            }
        }
    }

    fun securityUser(user: User) {
        if (api == null || accountUser?.username != user.username) return
        accountUser = user
        if (_screen.value is Screen.Rooms) _screen.value = Screen.Rooms(user)
    }

    fun emailDone(user: User) {
        if (!user.emailVerified) return
        Diag.log("email gate: confirmado")
        securityUser(user)
        val session = saved ?: return
        val a = api ?: return
        activate(session, a, epoch, user)
    }

    fun skipEmail() {
        val s = _screen.value as? Screen.VerifyEmail ?: return
        if (s.required || s.user == null) return
        Diag.log("email gate: agora não")
        emailSkipped = true
        activate(saved ?: return, api ?: return, epoch, s.user)
    }

    fun signOut() {
        val a = api
        Diag.log("sair")
        // A direct call is put down with this session first: revoked, the
        // hangup would be refused and the other side left on a call with
        // nobody, busy for the server's twelve hours (audit, 26/09/2026).
        direct.hangup { if (a != null) scope.launch { a.logout() } }
        forget()
    }

    /** The header of a shared diagnostic log: which phone, which build, which server. */
    fun diagHeader(): String =
        "harmony Android ${BuildConfig.VERSION_NAME} — ${Build.MANUFACTURER} ${Build.MODEL}, " +
            "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})\n" +
            "servidor: ${api?.baseUrl ?: "-"}; ao vivo: ${_live.value}; webrtc: ${_webrtc.value} — ${WebRtcProbe.LIBRARY}"

    private fun enter(saved: Saved, user: User?) {
        val mine = ++epoch
        this.saved = saved
        emailSkipped = false
        emailRequired = false
        accountUser = user
        lateinit var a: Api
        a = Api(client, saved.baseUrl, { saved.cookie }, userAgent) { requireEmail(a) }
        api = a
        // No wait on /api/me here: the app opens on the rooms even offline, and
        // the resync that runs on every connect reads /api/me and turns to the
        // gate. Only a sign-in that already says "not confirmed" stops first.
        if (user != null && !user.emailVerified) {
            Diag.log("email gate: confirmação pendente")
            _screen.value = Screen.VerifyEmail(user, false)
            refreshEmailGate()
        } else activate(saved, a, mine, user)
    }

    private fun activate(saved: Saved, a: Api, mine: Int, user: User?) {
        if (mine != epoch || a !== api) return
        // The volumes are this account's from the start: a direct call can
        // come before any room (the room also says so when it is joined).
        user?.let { voice.volumes.forAccount(it.username) }
        _screen.value = Screen.Rooms(user)
        val g = Gateway(client, saved.baseUrl, saved.cookie, object : Gateway.Listener {
            override fun onOpen() {
                _live.value = true
                retryWait = RESYNC_FIRST_MS
                presence.update { it.copy(n = it.n + 1) }
                resync(a, mine)
                // A deploy restarts the server, so the socket coming back is
                // the earliest news of a new version there is.
                updater.check("reconectou")
            }

            override fun onEvent(ev: GatewayEvent) {
                if (ev.type == "person_preferences") { resync(a, mine); return }

                if (ev is GatewayEvent.Roles) Diag.log("roles: event, ${ev.roles.size} roles")
                voice.onEvent(ev)
                direct.onEvent(ev)
                // Read here or on another device: the phone stops saying so.
                if (ev is GatewayEvent.Read) readTo(ev.channelId, ev.lastReadId)
                synchronized(lock) {
                    heldDuringResync?.add(ev)
                    _rooms.update { it.apply(ev, myName(), reading()) }
                }
                chat.onEvent(ev)
            }

            override fun onDown() {
                _live.value = false
            }

            override fun onUnauthorized() {
                if (mine == epoch) forget()
            }
        }, hidden = { !visible() })
        gateway?.stop()
        gateway = g
        // Only once somebody has seen the app. A process started by a
        // notification in a pocket fetches what it needs on its own (onWake);
        // opening the socket there made the person show as online to their
        // friends without having opened anything (the owner, 26/09/2026).
        if (shown) g.start() else Diag.log("sem tela: conexão fica para quando o app abrir")
    }

    fun roleSnapshot(snapshot: com.taleshernandes.harmony.model.RoleSnapshot) {
        synchronized(lock) {
            _rooms.update { it.copy(roleColors = snapshot.colors).apply(GatewayEvent.Roles(snapshot.roles), myName(), reading()) }
        }
    }

    private fun resync(a: Api, mine: Int) {
        val n = synchronized(lock) {
            heldDuringResync = mutableListOf()
            ++resyncs
        }
        scope.launch {
            try {
                val status = a.meStatus()
                if (mine != epoch) return@launch
                if (_screen.value !is Screen.Rooms || n != synchronized(lock) { resyncs }) return@launch
                accountUser = status.user
                emailRequired = status.emailRequired
                if (!status.user.emailVerified && (!emailSkipped || emailRequired)) {
                    requireEmail(a, status.emailRequired)
                    return@launch
                }
                val me = status.user
                val personal = a.personPreferences()
                val snapshot = a.channelSnapshot()
                val channels = snapshot.channels
                val rooms = a.voiceRooms()
                val ranks = a.roles()
                if (mine != epoch) return@launch
                if (_screen.value !is Screen.Rooms) return@launch
                synchronized(lock) {
                    if (n != resyncs || mine != epoch) return@launch
                    com.taleshernandes.harmony.model.PersonalPreferences.replace(personal)
                    _screen.value = Screen.Rooms(me)
                    val held = heldDuringResync.orEmpty()
                    heldDuringResync = null
                    _rooms.update {
                        val fresh = it.resync(channels, rooms).copy(categories = snapshot.categories, canManage = snapshot.canManage,
                            roles = ranks.roles, roleColors = ranks.colors, roleOwner = me.isAdmin)
                        // A message the fetched list already counted must not
                        // be counted again on top of it.
                        val newest = channels.associate { c -> c.id to c.lastMessageId }
                        held.filterNot { ev -> ev is GatewayEvent.MessageNew && ev.message.id <= (newest[ev.message.channelId] ?: 0) }
                            .fold(fresh) { s, ev -> s.apply(ev, myName(), reading()) }
                    }
                }
                chat.resynced()
                pendingOpen?.let { id ->
                    pendingOpen = null
                    _rooms.value.channels.firstOrNull { it.id == id }?.let { c -> chat.open(c.id, c.lastReadId) }
                }
                // A read that happened while the socket was closed arrives
                // only in the list: its notification goes now. Only where
                // nothing is left unread — a process started afresh does not
                // know which message a notification from before is showing.
                for (c in channels) if (c.unread == 0) readTo(c.id, c.lastReadId)
                registerPush()
                Diag.log("em dia: ${channels.count { it.isVoice }} salas, ${rooms.values.sumOf { it.size }} pessoas em voz")
                // A call that was on while the socket was closed may have
                // missed a renegotiation: it checks itself against the list.
                voice.reconcile(rooms)
            } catch (_: Unauthorized) {
                Diag.log("401 ao buscar: sessão não vale mais")
                if (mine == epoch) forget()
            } catch (e: Exception) {
                // IOException, or an unreadable answer. The socket may stay
                // open for hours, so waiting for it to reopen is not a retry.
                synchronized(lock) { if (n == resyncs) heldDuringResync = null }
                Diag.log("buscar falhou: ${e.javaClass.simpleName}")
                val wait = retryWait
                retryWait = minOf(retryWait * 2, RESYNC_MAX_MS)
                delay(wait)
                if (mine == epoch && _live.value && n == synchronized(lock) { resyncs }) resync(a, mine)
            }
        }
    }

    /** Joins a voice room. The microphone permission is the screen's to ask for, before this. */
    fun refreshChannels() { api?.let { resync(it, epoch) } }

    fun joinVoice(channelId: String) {
        val me = (_screen.value as? Screen.Rooms)?.user?.username ?: return
        if (direct.state.value.phase != DirectCall.Phase.IDLE) {
            // One call at a time: the direct call is put down by the person,
            // not by walking into a room.
            voice.notice("Você está numa chamada direta. Desligue antes de entrar numa sala.")
            return
        }
        voice.join(channelId, me)
        // Now, from the tap: Android lets a microphone service start only
        // while the app is on screen. It stops itself when the call ends.
        CallService.start(context)
    }

    /** The session is over, for whatever reason: back to the sign-in screen. */
    private fun forget() {
        epoch++
        voice.leave()
        direct.hangup()
        com.taleshernandes.harmony.model.PersonalPreferences.clear()
        chat.forget()
        gateway?.stop()
        gateway = null
        val base = api?.baseUrl
        api = null
        accountUser = null
        emailSkipped = false
        emailRequired = false
        synchronized(noticeLock) {
            saved = null
            vault.clear()
            readUpTo.clear()
            notices.cancelAll()
        }
        pushRegistered = null
        pendingOpen = null
        _joinAsked.value = null
        _live.value = false
        _rooms.value = RoomsState()
        _screen.value = Screen.SignIn(base ?: vault.lastBaseUrl() ?: DEFAULT_SERVER)
    }

    private companion object {
        const val RESYNC_FIRST_MS = 2000L
        const val RESYNC_MAX_MS = 30_000L
        const val PRESENCE_RETRY_FIRST_MS = 1000L
        const val PRESENCE_RETRY_MAX_MS = 30_000L
    }

    private fun watchNetwork() {
        val cm = context.getSystemService(ConnectivityManager::class.java) ?: return
        cm.registerDefaultNetworkCallback(object : ConnectivityManager.NetworkCallback() {
            // Every default-network replacement matters, even without onLost.
            private val changes = NetworkChanges<Network>()

            override fun onAvailable(network: Network) {
                val moved = changes.available(network)
                if (moved) {
                    api?.networkChanged()
                    Diag.log("default network changed; restart immediately")
                    // The socket on the dead network looks open until a ping
                    // fails, 30 s later; everything said meanwhile is lost.
                    gateway?.reconnect()
                } else {
                    Diag.log("rede disponível")
                    gateway?.nudge()
                }
                voice.networkChanged(moved)
                direct.networkChanged(moved)
            }

            override fun onLost(network: Network) {
                Diag.log("rede perdida")
                if (changes.lost(network)) {
                    api?.networkChanged()
                    gateway?.reconnect()
                    voice.networkChanged(true)
                    direct.networkChanged(true)
                }
            }

            override fun onCapabilitiesChanged(network: Network, capabilities: NetworkCapabilities) {
                val next = (0..7).filter { capabilities.hasTransport(it) }.toSet()
                val ready = capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)
                if (changes.capabilities(network, next, ready)) {
                    api?.networkChanged()
                    Diag.log("network capabilities changed; recover transports immediately")
                    gateway?.reconnect()
                    voice.networkChanged(true)
                    direct.networkChanged(true)
                } else if (changes.gainedValidation(network)) {
                    // Validation alone is only a nudge for an already broken call.
                    gateway?.nudge()
                    voice.validationChanged()
                    direct.networkChanged(false)
                }
            }
        })
    }
}
