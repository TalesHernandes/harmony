package com.taleshernandes.harmony

import android.content.Context
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.model.Attachment
import com.taleshernandes.harmony.model.Channel
import com.taleshernandes.harmony.model.GatewayEvent
import com.taleshernandes.harmony.model.HistoryPage
import com.taleshernandes.harmony.model.Member
import com.taleshernandes.harmony.model.Message
import com.taleshernandes.harmony.model.Timeline
import com.taleshernandes.harmony.model.Typing
import com.taleshernandes.harmony.model.TypingSender
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.ApiError
import com.taleshernandes.harmony.ui.Pictures
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONObject
import java.io.IOException
import java.io.InputStream

/**
 * The text side of the app: which conversation is on screen, the messages
 * held for each, who is typing, and what is waiting to be sent.
 *
 * The rule is the rooms' (Core.resync): the socket keeps nothing while it is
 * closed, so every time it opens the conversation on screen is fetched again
 * and the others are forgotten, to be fetched when they are next opened.
 */
class Conversations(
    context: Context,
    private val scope: CoroutineScope,
    private val api: () -> Api?,
    private val me: () -> String?,
    /** The server took the read mark; the list's badge goes out with it. */
    private val onRead: (channelId: String, lastReadId: Long) -> Unit,
) {
    /** A one-shot list position, published only after its history window is ready. */
    data class Position(val channelId: String, val messageId: Long?, val generation: Int)

    data class State(
        val timelines: Map<String, Timeline> = emptyMap(),
        /** The conversation on screen, or null on the list. */
        val open: String? = null,
        /** The read mark the open conversation had when it was opened: the "novas" line. */
        val readAtOpen: Long = 0,
        val typing: Map<String, List<String>> = emptyMap(),
        /** Everybody on the server, for names and nicknames. */
        val members: Map<String, Member> = emptyMap(),
        val presence: Map<String, String> = emptyMap(),
        val loadingOlder: Boolean = false,
        val loadingNewer: Boolean = false,
        val position: Position? = null,
        /** Something the person should know, said once. */
        val notice: String? = null,
        val pinRevision: Long = 0,
        /** Pictures picked for the next message, uploaded already. */
        val pending: List<Attachment> = emptyList(),
        val uploading: Int = 0,
        /** Conversations with a message on its way: pressing send again there would send it twice. */
        val sending: Set<String> = emptySet(),
    )

    private val _state = MutableStateFlow(State())
    val state: StateFlow<State> = _state

    val drafts = Drafts(context)

    private val typing = Typing()
    private val typingSender = TypingSender()
    private var typingSweep: Job? = null
    @Volatile private var looking = true

    // Which signed-in session this is. Anything started for one session —
    // a fetch, a send, an upload — must not touch the next one's screen.
    @Volatile private var session = 0

    // Everything below is guarded by [lock]: the socket's thread, the
    // fetches' threads and the screen's all reach it.
    private val lock = Any()
    // Confirmed edits win over a member fetch that was already in flight.
    private val nicknames = mutableMapOf<String, String>()
    private var presenceEvents: MutableMap<String, String>? = null
    // Events for a conversation whose newest page is being fetched. The page
    // may be older than they are, so they are put back on top when it lands,
    // as the web does. The list is the fetch's own: a newer fetch replacing
    // it makes the older one's answer void.
    private val held = HashMap<String, MutableList<GatewayEvent>>()
    private val newerHeld = HashMap<String, MutableList<GatewayEvent>>()
    private val olderRequests = HashMap<String, Any>()
    // How many times each conversation was replaced wholesale. An older page
    // asked for before a replacement belongs to a list that is gone, and
    // joining it to the new one would leave a hole in the middle.
    private val generation = HashMap<String, Int>()
    // The read mark the server is known to have, per conversation — from the
    // list, from another device's `read`, and from this one's own. The app
    // only ever moves it forward from there, one request at a time.
    private val readMark = HashMap<String, Long>()
    private val readInFlight = HashSet<String>()
    // One upload at a time: each is streamed, but four at once is still four
    // connections fighting over a phone's uplink.
    private val uploads = Mutex()

    /** Opens a conversation: fetched if nothing of it is held, and read. */
    fun open(channelId: String, lastReadId: Long, messageId: Long? = null) {
        synchronized(lock) { readMark[channelId] = maxOf(readMark[channelId] ?: 0, lastReadId) }
        _state.update { it.copy(open = channelId, readAtOpen = lastReadId, pending = emptyList(),
            loadingOlder = false, loadingNewer = false, position = null) }
        val t = _state.value.timelines[channelId]
        if (messageId != null) fetchLatest(channelId, around = messageId)
        else if (t == null || !t.loaded || t.hasNewer) fetchLatest(channelId, positionLatest = true)
        else markRead()
    }

    fun latest() {
        _state.value.open?.let { fetchLatest(it, positionLatest = true) }
    }

    fun positioned(position: Position) {
        _state.update { if (it.position == position) it.copy(position = null) else it }
    }

    fun close() {
        _state.update { it.copy(open = null, pending = emptyList(), position = null) }
    }

    /**
     * Whether the open conversation can be seen: the app on screen and the
     * conversation not covered by a video. Unseen, it is not being read.
     */
    fun setLooking(on: Boolean) {
        looking = on
        if (on) markRead()
    }

    /** The socket (re)opened: whatever was held may have missed something. */
    fun resynced() {
        // Decided inside one update, so that a conversation opened at the
        // same moment is either kept and fetched here, or opened after and
        // fetched by open() — never dropped in between.
        var keep: String? = null
        synchronized(lock) {
            _state.update { s ->
                keep = s.open
                s.copy(timelines = s.timelines.filterKeys { it == s.open }, pinRevision = s.pinRevision + 1)
            }
            for (id in generation.keys.toList()) generation[id] = generation.getValue(id) + 1
        }
        keep?.let { fetchLatest(it, positionLatest = true) }
        if (_state.value.members.isEmpty()) fetchMembers()
        fetchPresence()
    }

    fun forget() {
        synchronized(lock) {
            session++
            nicknames.clear()
            presenceEvents = null
            held.clear()
            newerHeld.clear()
            olderRequests.clear()
            generation.clear()
            readMark.clear()
            readInFlight.clear()
        }
        typingSweep?.cancel()
        _state.value = State()
        drafts.clear()
        Pictures.clear()
        com.taleshernandes.harmony.model.Avatars.clear()
    }

    fun onEvent(ev: GatewayEvent) {
        when (ev) {
            is GatewayEvent.Presence -> synchronized(lock) {
                presenceEvents?.put(ev.username, ev.state)
                _state.update { it.copy(presence = it.presence + (ev.username to ev.state)) }
            }
            is GatewayEvent.MessageNew -> {
                val m = ev.message
                synchronized(typing) { typing.stopped(m.channelId, m.author.username) }
                timelineEvent(m.channelId, ev)
                _state.update { it.withTyping(m.channelId) }
                if (m.channelId == _state.value.open) markRead()
            }
            is GatewayEvent.MessageChanged -> {
                timelineEvent(ev.message.channelId, ev)
                _state.update { it.copy(pinRevision = it.pinRevision + 1) }
            }
            is GatewayEvent.Read -> synchronized(lock) {
                readMark[ev.channelId] = maxOf(readMark[ev.channelId] ?: 0, ev.lastReadId)
            }
            is GatewayEvent.TypingEvent -> {
                if (ev.username == me()) return
                synchronized(typing) { typing.saw(ev.channelId, ev.username, ev.displayName, now()) }
                _state.update { it.withTyping(ev.channelId) }
                sweepTyping()
            }
            else -> {}
        }
    }

    fun loadOlder() {
        val open = _state.value.open ?: return
        val a = api() ?: return
        val request = Any()
        val (mine, gen, before) = synchronized(lock) {
            val s = _state.value
            val t = s.timelines[open] ?: return
            if (s.open != open || s.loadingOlder || !t.hasMore || !t.loaded || open in held || open in olderRequests) return
            val before = t.oldestId ?: return
            olderRequests[open] = request
            _state.update { it.copy(loadingOlder = true) }
            Triple(session, generation[open] ?: 0, before)
        }
        scope.launch {
            val page = try {
                a.history(open, before = before)
            } catch (e: Exception) {
                Diag.log("histórico mais antigo falhou: ${e.javaClass.simpleName}")
                null
            }
            synchronized(lock) {
                if (mine != session) return@launch
                // Replaced while this was on its way: the page belongs to a list that is gone.
                if ((generation[open] ?: 0) != gen || olderRequests[open] !== request) return@launch
                olderRequests.remove(open)
                _state.update { st ->
                    val cur = st.timelines[open]
                    if (page != null && cur != null) {
                        st.copy(timelines = st.timelines + (open to cur.withOlder(page)),
                            loadingOlder = if (st.open == open) false else st.loadingOlder)
                    } else {
                        st.copy(loadingOlder = if (st.open == open) false else st.loadingOlder)
                    }
                }
            }
        }
    }

    /** Extends the contiguous retained window towards the live end. */
    fun loadNewer() {
        val open = _state.value.open ?: return
        val a = api() ?: return
        val buffer = mutableListOf<GatewayEvent>()
        val (mine, gen, after) = synchronized(lock) {
            val s = _state.value
            val t = s.timelines[open] ?: return
            if (s.open != open || s.loadingNewer || !t.hasNewer || !t.loaded || open in held || open in newerHeld) return
            val end = t.newestId ?: return
            newerHeld[open] = buffer
            _state.update { it.copy(loadingNewer = true) }
            Triple(session, generation[open] ?: 0, end)
        }
        scope.launch {
            try {
                val page = a.history(open, after = after)
                synchronized(lock) {
                    if (mine != session || generation[open] != gen || newerHeld[open] !== buffer) return@launch
                    newerHeld.remove(open)
                    _state.update { s ->
                        val t = s.timelines[open] ?: return@update s
                        val next = buffer.fold(t.withNewer(page)) { acc, ev -> acc.apply(ev) }
                        s.copy(timelines = s.timelines + (open to next),
                            loadingNewer = if (s.open == open) false else s.loadingNewer)
                    }
                }
                markRead()
            } catch (e: Exception) {
                Diag.log("newer history failed: ${e.javaClass.simpleName}")
                synchronized(lock) {
                    if (mine != session || newerHeld[open] !== buffer) return@launch
                    newerHeld.remove(open)
                    _state.update { if (it.open == open) it.copy(loadingNewer = false) else it }
                    if (_state.value.open == open) say(failure(e, "Não deu para carregar as mensagens mais recentes."))
                }
            }
        }
    }

    /** A key was pressed in the box: the others see "digitando…", at most every few seconds. */
    fun typed(channelId: String) {
        if (!typingSender.shouldSend(channelId, now())) return
        val a = api() ?: return
        scope.launch { runCatching { a.typing(channelId) } }
    }

    /**
     * Sends [body] with the pictures already uploaded. Returns false when
     * there is nothing to send, a picture is still uploading, or a send is
     * already on its way here. The draft is dropped only once the server has
     * the message, and only if it is still what was sent: somebody typing
     * the next line while the first one travels keeps it.
     */
    fun send(channelId: String, body: String, replyTo: Long?, onDone: (ok: Boolean) -> Unit): Boolean {
        val text = body.trim()
        val s0 = _state.value
        if (channelId in s0.sending || s0.uploading > 0) return false
        val files = s0.pending.map { it.id }
        if (text.isEmpty() && files.isEmpty()) return false
        val a = api() ?: return false
        val mine = session
        typingSender.reset(channelId)
        _state.update { it.copy(sending = it.sending + channelId) }
        scope.launch {
            try {
                val m = a.post(channelId, text, files, replyTo)
                if (mine != session) return@launch
                synchronized(lock) {
                    timelineEvent(channelId, GatewayEvent.MessageNew(m))
                    _state.update { s ->
                        s.copy(
                            pending = s.pending.filterNot { it.id in files },
                            sending = s.sending - channelId,
                        )
                    }
                }
                if (drafts.get(channelId).trim() == text) drafts.set(channelId, "")
                markRead()
                onDone(true)
            } catch (e: Exception) {
                if (mine != session) return@launch
                _state.update { it.copy(sending = it.sending - channelId) }
                say(failure(e, "Não deu para enviar."))
                onDone(false)
            }
        }
        return true
    }

    fun edit(id: Long, body: String, onDone: (ok: Boolean) -> Unit) {
        val a = api() ?: return
        val mine = session
        scope.launch {
            try {
                val m = a.edit(id, body.trim())
                if (mine != session) return@launch
                replace(m)
                onDone(true)
            } catch (e: Exception) {
                if (mine != session) return@launch
                say(failure(e, "Não deu para editar."))
                onDone(false)
            }
        }
    }

    fun pin(id: Long, on: Boolean) {
        val a = api() ?: return
        val mine = session
        scope.launch {
            try {
                val m = a.pin(id, on)
                if (mine == session) onEvent(GatewayEvent.MessageChanged(m))
            } catch (e: Exception) {
                if (mine == session) say(if ((e as? ApiError)?.reason == "pin_limit")
                    "Este canal já tem 250 mensagens fixadas. Desafixe uma para fixar outra."
                    else failure(e, "Não deu para alterar a mensagem fixada."))
            }
        }
    }

    fun delete(id: Long) {
        val a = api() ?: return
        val mine = session
        scope.launch {
            try {
                val m = a.delete(id)
                if (mine == session) replace(m)
            } catch (e: Exception) {
                if (mine == session) say(failure(e, "Não deu para apagar."))
            }
        }
    }

    fun react(id: Long, emoji: String, on: Boolean) {
        val a = api() ?: return
        val mine = session
        scope.launch {
            try {
                val m = a.react(id, emoji, on)
                if (mine == session) replace(m)
            } catch (e: Exception) {
                if (mine == session) say(failure(e, "Não deu para reagir."))
            }
        }
    }

    /**
     * Uploads one picked file now, so that pressing send only sends. It is
     * streamed from the phone's storage, never held whole in memory. [size]
     * is -1 when the phone does not know it; the server refuses what is too
     * big either way.
     */
    fun attach(channelId: String, name: String, mime: String, size: Long, open: () -> InputStream?) {
        val a = api() ?: return
        if (_state.value.pending.size + _state.value.uploading >= MAX_ATTACHMENTS) {
            say("No máximo $MAX_ATTACHMENTS arquivos por mensagem.")
            return
        }
        if (size > MAX_UPLOAD_BYTES) {
            say("Arquivo grande demais (o limite é 32 MB).")
            return
        }
        val mine = session
        _state.update { it.copy(uploading = it.uploading + 1) }
        scope.launch {
            try {
                val att = uploads.withLock {
                    a.upload(channelId, name, mime, size) { open() ?: throw IOException("unreadable") }
                }
                if (mine == session) {
                    _state.update { s -> if (s.open == channelId) s.copy(pending = s.pending + att) else s }
                }
            } catch (e: Exception) {
                if (mine == session) say(failure(e, "Não deu para enviar o arquivo."))
            } finally {
                if (mine == session) _state.update { it.copy(uploading = it.uploading - 1) }
            }
        }
    }

    fun unattach(id: String) {
        _state.update { s -> s.copy(pending = s.pending.filterNot { it.id == id }) }
    }

    fun dismissNotice() {
        _state.update { it.copy(notice = null) }
    }

    /** Publish the nickname only after PersonSheet's request succeeded. */
    fun nicknameSaved(person: Member, nickname: String) {
        synchronized(lock) {
            nicknames[person.username] = nickname
            _state.update { s ->
                val member = s.members[person.username] ?: person
                s.copy(members = s.members + (person.username to member.copy(nickname = nickname.ifEmpty { null })))
            }
        }
    }

    /** Opens (or finds) the direct conversation with somebody; [then] gets it. */
    fun openDm(username: String, then: (Channel) -> Unit) {
        val a = api() ?: return
        val mine = session
        scope.launch {
            try {
                val c = a.openDm(username)
                if (mine == session) then(c)
            } catch (e: Exception) {
                if (mine == session) say(failure(e, "Não deu para abrir a conversa."))
            }
        }
    }

    // ------------------------------------------------------------ inside

    private fun timelineEvent(channelId: String, ev: GatewayEvent) {
        synchronized(lock) {
            held[channelId]?.add(ev)
            newerHeld[channelId]?.add(ev)
            _state.update { s ->
                val t = s.timelines[channelId] ?: return@update s
                s.copy(timelines = s.timelines + (channelId to t.apply(ev)))
            }
        }
    }

    private fun fetchLatest(channelId: String, around: Long? = null, positionLatest: Boolean = false) {
        val a = api() ?: return
        val buffer = mutableListOf<GatewayEvent>()
        val (mine, gen) = synchronized(lock) {
            held[channelId] = buffer
            newerHeld.remove(channelId)
            olderRequests.remove(channelId)
            val gen = (generation[channelId] ?: 0) + 1
            generation[channelId] = gen
            _state.update { s ->
                val timelines = if (around != null) s.timelines + (channelId to Timeline()) else s.timelines
                if (s.open == channelId) s.copy(timelines = timelines, loadingOlder = false,
                    loadingNewer = false, position = null) else s.copy(timelines = timelines)
            }
            session to gen
        }
        scope.launch {
            try {
                val page: HistoryPage = a.history(channelId, around = around)
                synchronized(lock) {
                    // A newer fetch of the same conversation, or another session.
                    if (mine != session || generation[channelId] != gen || held[channelId] !== buffer) return@launch
                    held.remove(channelId)
                    _state.update { s ->
                        val t = if (around != null) Timeline().withAround(page)
                            else (s.timelines[channelId] ?: Timeline()).withLatest(page)
                        val position = if (s.open == channelId && (around != null || positionLatest))
                            Position(channelId, around, gen) else s.position
                        s.copy(timelines = s.timelines + (channelId to buffer.fold(t) { acc, ev -> acc.apply(ev) }),
                            position = position)
                    }
                }
                markRead()
            } catch (e: Exception) {
                Diag.log("histórico falhou: ${e.javaClass.simpleName}")
                synchronized(lock) {
                    if (mine != session || generation[channelId] != gen || held[channelId] !== buffer) return@launch
                    held.remove(channelId)
                    if (_state.value.open == channelId) {
                        say(if (around != null && e is ApiError && e.status == 404)
                            "Essa mensagem não está mais disponível nesta conversa."
                        else failure(e, "Não deu para carregar a conversa."))
                        if (around != null) fetchLatest(channelId, positionLatest = true)
                    }
                }
            }
        }
    }

    private fun fetchPresence() {
        val a = api() ?: return
        val buffer = mutableMapOf<String, String>()
        val mine = synchronized(lock) {
            presenceEvents = buffer
            session
        }
        scope.launch {
            try {
                val snapshot = a.presence()
                synchronized(lock) {
                    if (mine != session || presenceEvents !== buffer) return@launch
                    // Events received during the request win over its older snapshot.
                    _state.update { it.copy(presence = snapshot + buffer) }
                    presenceEvents = null
                }
            } catch (e: Exception) {
                synchronized(lock) { if (presenceEvents === buffer) presenceEvents = null }
                Diag.log("presence fetch failed: ${e.javaClass.simpleName}")
            }
        }
    }

    private fun fetchMembers() {
        val a = api() ?: return
        val mine = session
        scope.launch {
            runCatching { a.members() }.onSuccess { list ->
                synchronized(lock) {
                    if (mine == session) _state.update { s ->
                        s.copy(members = list.associate { member ->
                            member.username to if (member.username in nicknames)
                                member.copy(nickname = nicknames.getValue(member.username).ifEmpty { null }) else member
                        })
                    }
                }
            }
        }
    }

    /**
     * The conversation on screen is read up to its newest message. The mark
     * only moves forward from what the server is known to have, one request
     * per conversation at a time, and the list's badge follows once the
     * server has said yes.
     */
    private fun markRead() {
        if (!looking) return
        val s = _state.value
        val open = s.open ?: return
        if (s.timelines[open]?.let { !it.loaded || it.hasNewer } != false) return
        val newest = s.timelines[open]?.newestId ?: return
        val a = api() ?: return
        val mine = synchronized(lock) {
            if (newest <= (readMark[open] ?: 0) || open in readInFlight) return
            readInFlight += open
            session
        }
        scope.launch {
            val ok = try {
                a.markRead(open, newest)
                true
            } catch (e: Exception) {
                Diag.log("marcar lido falhou: ${e.javaClass.simpleName}")
                false
            }
            synchronized(lock) {
                if (mine != session) return@launch
                readInFlight -= open
                if (ok) readMark[open] = maxOf(readMark[open] ?: 0, newest)
            }
            if (ok) {
                onRead(open, newest)
                // Something newer may have arrived while this one travelled.
                markRead()
            }
        }
    }

    private fun replace(m: Message) = timelineEvent(m.channelId, GatewayEvent.MessageChanged(m))

    private fun State.withTyping(channelId: String): State =
        // The socket's thread writes it and the sweeper's reads it.
        copy(typing = typing + (channelId to synchronized(this@Conversations.typing) { this@Conversations.typing.names(channelId, now()) }))

    /** One timer for everybody typing; it stops once nobody is. */
    private fun sweepTyping() {
        if (typingSweep?.isActive == true) return
        typingSweep = scope.launch {
            while (isActive) {
                delay(1000)
                val ids = _state.value.typing.keys
                _state.update { s -> ids.fold(s) { acc, id -> acc.withTyping(id) } }
                if (_state.value.typing.values.all { it.isEmpty() }) break
            }
        }
    }

    private fun say(text: String) {
        _state.update { it.copy(notice = text) }
    }

    private fun failure(e: Exception, fallback: String): String = when {
        e is ApiError && e.reason == "delivery_failed" -> "Não foi possível entregar."
        e is ApiError && e.reason == "message_limit" -> "Muitas mensagens seguidas. Espere um minuto."
        e is ApiError && e.reason == "too_large" -> "Arquivo grande demais (o limite é 32 MB)."
        e is ApiError && (e.reason == "upload_limit" || e.reason == "upload_bytes_limit") ->
            "Limite de envios atingido. Tente mais tarde."
        e is ApiError && e.status == 404 -> "Essa mensagem ou conversa não existe mais."
        e is IOException && e !is ApiError -> "Sem conexão. $fallback"
        else -> fallback
    }.also { Diag.log("chat: ${e.javaClass.simpleName} ${(e as? ApiError)?.reason.orEmpty()}") }

    private fun now() = System.currentTimeMillis()

    companion object {
        const val MAX_ATTACHMENTS = 4 // store.MaxAttachmentsPerMessage
        const val MAX_UPLOAD_BYTES = 32L shl 20 // api.MaxUploadBytes
        const val MAX_BODY = 4000 // store.MaxMessageRunes
    }
}

/**
 * What was written and not sent, per conversation. Kept in memory while the
 * app runs, and written to the phone's storage only when the app leaves the
 * screen, because Android may end a process in the background without a word.
 *
 * Not encrypted, the web's decision (D4m): the messages themselves sit in the
 * clear in the server's database, and a draft on the person's own phone is
 * less exposed than that. Signing out erases them.
 */
class Drafts(context: Context) {
    private val prefs = context.getSharedPreferences("drafts", Context.MODE_PRIVATE)
    private val held = java.util.concurrent.ConcurrentHashMap<String, String>()

    init {
        // Read back once, and taken off the disk: from here on the memory is
        // the copy that counts.
        prefs.getString(KEY, null)?.let { raw ->
            runCatching {
                val o = JSONObject(raw)
                for (k in o.keys()) held[k] = o.optString(k)
            }
        }
        prefs.edit().remove(KEY).apply()
    }

    fun get(channelId: String): String = held[channelId].orEmpty()

    fun set(channelId: String, text: String) {
        if (text.isBlank()) held.remove(channelId) else held[channelId] = text
    }

    /** The app left the screen: whatever is unsent goes to storage. */
    fun persist() {
        if (held.isEmpty()) {
            prefs.edit().remove(KEY).apply()
            return
        }
        val o = JSONObject()
        for ((k, v) in held) o.put(k, v)
        prefs.edit().putString(KEY, o.toString()).apply()
    }

    /** Back on screen: storage is emptied again. */
    fun unpersist() {
        prefs.edit().remove(KEY).apply()
    }

    fun clear() {
        held.clear()
        prefs.edit().remove(KEY).apply()
    }

    private companion object {
        const val KEY = "drafts"
    }
}
