package com.taleshernandes.harmony.net

import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.model.Channel
import com.taleshernandes.harmony.model.CallInfo
import com.taleshernandes.harmony.model.Candidate
import com.taleshernandes.harmony.model.Sdp
import com.taleshernandes.harmony.model.Attachment
import com.taleshernandes.harmony.model.HistoryPage
import com.taleshernandes.harmony.model.Member
import com.taleshernandes.harmony.model.Message
import com.taleshernandes.harmony.model.Peer
import com.taleshernandes.harmony.model.Release
import com.taleshernandes.harmony.model.User
import com.taleshernandes.harmony.model.VoiceOffer
import okhttp3.Cookie
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.MultipartBody
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okio.source
import org.json.JSONObject
import java.io.IOException

const val SESSION_COOKIE = "harmony_session"

/** The server's address as the person typed it, reduced to scheme, host and port. */
object ServerUrl {
    /** Null when it cannot be an address at all. A missing scheme means https. */
    fun normalize(input: String): String? {
        val t = input.trim().trimEnd('/')
        if (t.isEmpty()) return null
        val withScheme = if ("://" in t) t else "https://$t"
        val url = withScheme.toHttpUrlOrNull() ?: return null
        return origin(url)
    }

    /**
     * What the /ws handshake must carry as `Origin`: exactly the harmony's own
     * origin (PROTOCOL-HARMONY §2). Default ports are left out, as a browser does.
     */
    fun origin(url: HttpUrl): String {
        val defaultPort = HttpUrl.defaultPort(url.scheme)
        val port = if (url.port == defaultPort) "" else ":${url.port}"
        return "${url.scheme}://${url.host}$port"
    }
}

/** The session is gone or was never there: the app goes back to the sign-in screen. */
class Unauthorized : IOException("unauthenticated")

/** The server said no, with its reason (`bad_credentials`, `rate_limited`, …). */
class ApiError(val status: Int, val reason: String?) : IOException("http $status ${reason.orEmpty()}")

sealed interface LoginResult {
    data class Ok(val user: User, val cookie: String) : LoginResult
    data class SecondFactor(val method: String, val challenge: String) : LoginResult
    data object BadCredentials : LoginResult
    data object RateLimited : LoginResult
    data class Failed(val why: String) : LoginResult
}

/**
 * The JSON API. Blocking calls, made off the main thread by the caller.
 *
 * No `Origin` is sent on writes: the server checks it only when present
 * (PROTOCOL-HARMONY §1), and an app is not a page.
 */
class Api(
    private val client: OkHttpClient,
    val baseUrl: String,
    private val cookie: () -> String?,
    private val userAgent: String,
    private val onEmailRequired: () -> Unit = {},
) {
    private val voiceClient = client.newBuilder()
        .connectTimeout(3, java.util.concurrent.TimeUnit.SECONDS)
        .readTimeout(3, java.util.concurrent.TimeUnit.SECONDS)
        .writeTimeout(3, java.util.concurrent.TimeUnit.SECONDS)
        .callTimeout(3, java.util.concurrent.TimeUnit.SECONDS)
        .build()

    /** Discard idle sockets belonging to the previous default network. */
    fun networkChanged() { client.connectionPool.evictAll() }

    fun login(username: String, password: String): LoginResult {
        return authenticate("/api/login", JSONObject().put("username", username).put("password", password))
    }

    fun login2FA(challenge: String, code: String): LoginResult =
        authenticate("/api/login/2fa", JSONObject().put("challenge", challenge).put("code", code))

    private fun authenticate(path: String, data: JSONObject): LoginResult {
        val body = data.toString()
        val req = Request.Builder()
            .url(baseUrl + path)
            .header("User-Agent", userAgent)
            .post(body.toRequestBody(JSON))
            .build()
        return try {
            client.newCall(req).execute().use { res ->
                val text = res.body.string()
                when {
                    res.isSuccessful -> {
                        val url = req.url
                        val c = Cookie.parseAll(url, res.headers).firstOrNull { it.name == SESSION_COOKIE }
                        loginResponse(text, c?.value).also {
                            Diag.log(when (it) {
                                is LoginResult.SecondFactor -> "login: 2fa pedido (${it.method})"
                                is LoginResult.Ok -> "login: ok"
                                else -> "login: resposta sem sessão"
                            })
                        }
                    }
                    res.code == 401 -> LoginResult.BadCredentials.also { Diag.log("login: 401") }
                    res.code == 429 -> LoginResult.RateLimited.also { Diag.log("login: 429") }
                    else -> {
                        Diag.log("login: http ${res.code}")
                        LoginResult.Failed(securityMessage(Json.error(text)))
                    }
                }
            }
        } catch (e: Exception) {
            Diag.log("login: sem conexão (${e.javaClass.simpleName})")
            LoginResult.Failed("Sem conexão com o servidor.")
        }
    }

    fun me(): User = Json.userOf(get("/api/me"))
    fun meStatus(): AccountStatus = accountStatus(get("/api/me"))

    fun forgotPassword(identifier: String) {
        post("/api/password/forgot", JSONObject().put("identifier", identifier))
    }

    fun changeEmail(password: String, email: String) {
        post("/api/me/email", JSONObject().put("password", password).put("email", email))
    }

    fun verifyEmail(code: String): User =
        Json.userOf(post("/api/me/email/verify", JSONObject().put("code", code)))

    fun resendEmail() { post("/api/me/email/resend", JSONObject()) }

    fun changePassword(current: String, next: String) {
        post("/api/me/password", JSONObject().put("current", current).put("new", next))
    }

    fun factor(action: String, password: String, code: String): FactorResult {
        require(action in listOf("totp/setup", "totp/enable", "email/enable", "disable", "recovery"))
        val r = JSONObject(post("/api/me/2fa/$action", JSONObject().put("password", password).put("code", code)))
        val codes = r.optJSONArray("recovery_codes")
        return FactorResult(r.optJSONObject("user")?.let(Json::user), r.optString("secret"),
            r.optString("uri"), r.optBoolean("code_required"),
            if (codes == null) emptyList() else (0 until codes.length()).map { codes.getString(it) })
    }

    private fun accountUrl(vararg segments: String): HttpUrl =
        requireNotNull(baseUrl.toHttpUrlOrNull()).newBuilder().addPathSegment("api")
            .apply { segments.forEach { addPathSegment(it) } }.build()

    fun sessions(): List<com.taleshernandes.harmony.model.AccountSession> = Json.sessions(get("/api/sessions"))

    fun revokeSession(id: String) {
        send(Request.Builder().url(accountUrl("sessions", id)).delete())
    }

    fun revokeOthers(): Int = JSONObject(post("/api/sessions/revoke-others", JSONObject())).optInt("revoked")

    fun invites(): List<com.taleshernandes.harmony.model.Invite>? = try {
        Json.invites(get("/api/invites"))
    } catch (e: ApiError) {
        if (e.status == 403 || e.status == 404) null else throw e
    }

    fun createInvite(note: String): com.taleshernandes.harmony.model.Invite =
        Json.invite(JSONObject(post("/api/invites", JSONObject().put("note", note))))

    fun revokeInvite(id: String) {
        send(Request.Builder().url(accountUrl("invites", id)).delete())
    }

    fun personPreferences(): List<com.taleshernandes.harmony.model.PersonPreference> = parsePersonPreferences(get("/api/me/blocks"))

    fun personPreference(username: String, block: Boolean, enabled: Boolean): List<com.taleshernandes.harmony.model.PersonPreference> {
        val request = Request.Builder().url(accountUrl("me", if (block) "blocks" else "mutes", username))
        if (enabled) request.put("".toRequestBody(JSON)) else request.delete()
        return parsePersonPreferences(send(request))
    }

    private fun parsePersonPreferences(body: String): List<com.taleshernandes.harmony.model.PersonPreference> {
        val list = JSONObject(body).getJSONArray("items")
        return (0 until list.length()).map { i -> val p = list.getJSONObject(i)
            com.taleshernandes.harmony.model.PersonPreference(p.getString("username"), p.optBoolean("muted"), p.optBoolean("blocked")) }
    }

    fun nickname(username: String, name: String): String = JSONObject(send(
        Request.Builder().url(accountUrl("people", username, "nickname"))
            .put(Json.nicknameBody(name).toString().toRequestBody(JSON)),
    )).getString("name")

    fun roles(): com.taleshernandes.harmony.model.RoleSnapshot = try {
        Json.roleSnapshot(get("/api/roles"))
    } catch (e: Exception) {
        Diag.log("roles: fetch error ${if (e is ApiError) "${e.status} ${e.reason}" else e.javaClass.simpleName}")
        throw e
    }

    fun roleAction(action: String, method: String, id: String? = null, member: String? = null, body: JSONObject = JSONObject()): String {
        require(method in listOf("POST", "PATCH", "DELETE", "PUT"))
        val segments = mutableListOf("roles")
        if (action == "order") segments.add("order")
        else if (id != null) segments.add(id)
        if (member != null) { segments.add("members"); segments.add(member) }
        Diag.log("roles: $action")
        return try {
            send(Request.Builder().url(accountUrl(*segments.toTypedArray())).method(method,
                if (method == "DELETE") null else body.toString().toRequestBody(JSON)))
        } catch (e: Exception) {
            Diag.log("roles: $action error ${if (e is ApiError) "${e.status} ${e.reason}" else e.javaClass.simpleName}")
            throw e
        }
    }

    fun channels(): List<Channel> = channelSnapshot().channels
    fun channelSnapshot() = Json.channelSnapshot(get("/api/channels"))

    fun organize(method: String, path: String, body: JSONObject = JSONObject()) {
        require(method in listOf("POST", "PATCH", "DELETE", "PUT"))
        Diag.log("organize: $method $path")
        try {
            send(Request.Builder().url(baseUrl + path).method(method,
                if (method == "DELETE") null else body.toString().toRequestBody(JSON)))
        } catch (e: Exception) {
            Diag.log("organize: error ${if (e is ApiError) "${e.status} ${e.reason}" else e.javaClass.simpleName}")
            throw e
        }
    }

    fun pins(channelId: String): List<Message> = Json.history(get("/api/channels/$channelId/pins")).messages.sortedByDescending { it.id }

    fun pin(messageId: Long, on: Boolean): Message {
        Diag.log("pin: $messageId $on")
        return try {
            Json.message(JSONObject(send(Request.Builder().url("$baseUrl/api/messages/$messageId/pin")
                .method(if (on) "POST" else "DELETE", if (on) "{}".toRequestBody(JSON) else null))))
        } catch (e: Exception) {
            Diag.log("pin: error ${if (e is ApiError) "${e.status} ${e.reason}" else e.javaClass.simpleName}")
            throw e
        }
    }

    /** Searches all history this session may read, in the server's result order. */
    fun search(query: String): List<SearchHit> {
        val url = requireNotNull(baseUrl.toHttpUrlOrNull()).newBuilder()
            .addPathSegment("api").addPathSegment("search").addQueryParameter("q", query).build()
        return searchHits(send(Request.Builder().url(url).get()))
    }

    /** Only versioned Twemoji art from this API's own server, never an arbitrary URL. */
    fun twemoji(name: String): ByteArray {
        require(name.matches(Regex("[0-9a-f]{1,6}(?:-[0-9a-f]{1,6})*")) && name.length <= 200)
        val url = requireNotNull(baseUrl.toHttpUrlOrNull()).newBuilder()
            .addPathSegment("static").addPathSegment("assets").addPathSegment("twemoji-15")
            .addPathSegment("$name.svg").build()
        val request = Request.Builder().url(url).get().header("User-Agent", userAgent)
        cookie()?.let { request.header("Cookie", "$SESSION_COOKIE=$it") }
        client.newCall(request.build()).execute().use { response ->
            checkResponse(response)
            val limit = 256 * 1024
            if (response.body.contentLength() > limit) throw IOException("SVG too large")
            val result = java.io.ByteArrayOutputStream()
            response.body.byteStream().use { input ->
                val buffer = ByteArray(8192)
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    if (result.size() + count > limit) throw IOException("SVG too large")
                    result.write(buffer, 0, count)
                }
            }
            return result.toByteArray()
        }
    }

    fun voiceRooms(): Map<String, List<Peer>> = Json.rooms(get("/api/voice/rooms"))

    private fun channelUrl(channelId: String, action: String): HttpUrl =
        requireNotNull(baseUrl.toHttpUrlOrNull()).newBuilder()
            .addPathSegment("api").addPathSegment("channels").addPathSegment(channelId)
            .addPathSegment(action).build()

    fun history(channelId: String, before: Long? = null, limit: Int? = null,
                around: Long? = null, after: Long? = null): HistoryPage {
        require(listOfNotNull(before, around, after).size <= 1) { "Use only one history cursor" }
        val url = channelUrl(channelId, "messages").newBuilder()
        before?.let { url.addQueryParameter("before", it.toString()) }
        around?.let { url.addQueryParameter("around", it.toString()) }
        after?.let { url.addQueryParameter("after", it.toString()) }
        limit?.let { url.addQueryParameter("limit", it.toString()) }
        return Json.history(send(Request.Builder().url(url.build()).get()))
    }

    fun post(channelId: String, body: String, attachments: List<String> = emptyList(), replyTo: Long? = null): Message {
        val data = JSONObject().put("body", body).put("attachments", org.json.JSONArray(attachments))
            .put("reply_to", replyTo ?: 0L)
        return Json.message(JSONObject(send(Request.Builder().url(channelUrl(channelId, "messages"))
            .post(data.toString().toRequestBody(JSON)))))
    }

    fun edit(messageId: Long, body: String): Message = Json.message(JSONObject(send(
        Request.Builder().url("$baseUrl/api/messages/$messageId")
            .patch(JSONObject().put("body", body).toString().toRequestBody(JSON)),
    )))

    fun delete(messageId: Long): Message = Json.message(JSONObject(send(
        Request.Builder().url("$baseUrl/api/messages/$messageId").delete(),
    )))

    fun react(messageId: Long, emoji: String, on: Boolean): Message = Json.message(JSONObject(
        post("/api/messages/$messageId/reactions", JSONObject().put("emoji", emoji).put("on", on)),
    ))

    fun markRead(channelId: String, lastReadId: Long) {
        send(Request.Builder().url(channelUrl(channelId, "read"))
            .post(JSONObject().put("last_read_id", lastReadId).toString().toRequestBody(JSON)))
    }

    fun typing(channelId: String) {
        send(Request.Builder().url(channelUrl(channelId, "typing")).post("{}".toRequestBody(JSON)))
    }

    fun openDm(username: String): Channel = Json.channel(JSONObject(
        post("/api/dms", JSONObject().put("username", username)),
    ))

    fun members(): List<Member> = Json.members(get("/api/members"))

    /**
     * One message, by id, or null if it is not there any more. The history
     * is newest-first below `before`, so the page of one just under id+1 is
     * the message itself when it exists.
     */
    fun message(channelId: String, id: Long): Message? =
        history(channelId, before = id + 1, limit = 1).messages.firstOrNull { it.id == id }

    /** What the server has for this person to be told (notify.go), as raw JSON. */
    fun pendingNotices(): String = get("/api/notifications/pending")

    /**
     * Where the app is: on screen ("active") or behind it ("hidden"), and
     * whether a call is on. The server counts the five minutes to away itself.
     */
    fun presence(): Map<String, String> {
        val body = JSONObject(get("/api/presence"))
        body.optJSONObject("avatars")?.let { a -> a.keys().forEach { u -> com.taleshernandes.harmony.model.Avatars.note(u, a.optString(u)) } }
        val entries = body.getJSONObject("presence")
        return entries.keys().asSequence().associateWith { entries.getString(it) }
    }

    fun presence(state: String, busy: Boolean) {
        post("/api/presence", JSONObject().put("state", state).put("busy", busy))
    }

    /** This phone's Firebase address, tied to this session. Answers how many devices the person has. */
    fun registerPush(token: String): Int =
        JSONObject(post("/api/push/fcm", JSONObject().put("token", token))).optInt("devices")

    fun upload(channelId: String, name: String, mime: String, bytes: ByteArray): Attachment =
        uploadPart(channelId, name, bytes.toRequestBody(mime.toMediaType()))

    /**
     * The same upload, read from [open] while it is sent rather than held
     * whole in memory first: four 30 MB photos read at once are more than
     * some phones give an app. [size] is -1 when the phone does not know it.
     */
    fun upload(channelId: String, name: String, mime: String, size: Long, open: () -> java.io.InputStream): Attachment {
        val part = object : okhttp3.RequestBody() {
            override fun contentType() = mime.toMediaType()
            override fun contentLength() = size
            override fun writeTo(sink: okio.BufferedSink) {
                open().use { sink.writeAll(it.source()) }
            }
        }
        return uploadPart(channelId, name, part)
    }

    private fun uploadPart(channelId: String, name: String, part: okhttp3.RequestBody): Attachment {
        val body = MultipartBody.Builder().setType(MultipartBody.FORM)
            .addFormDataPart("file", name, part).build()
        return Json.attachment(JSONObject(send(Request.Builder().url(channelUrl(channelId, "uploads")).post(body))))
    }

    fun file(url: String): ByteArray {
        if (url.startsWith("/api/people/")) return avatar(url)
        if (url.startsWith("/api/previews/")) return preview(url)
        require(url.startsWith("/api/files/")) { "Expected an /api/files/ URL" }
        val resolved = requireNotNull((baseUrl + url).toHttpUrlOrNull())
        // Reject dot segments and encoded traversal after OkHttp normalization too.
        require(resolved.encodedPath.startsWith("/api/files/") &&
            resolved.pathSegments.size == 3 && resolved.pathSegments.last().matches(Regex("[A-Za-z0-9_-]{1,64}"))) {
            "Expected a file ID"
        }
        return sendBytes(Request.Builder().url(resolved).get())
    }

    /** A link card's picture, `/api/previews/<sha-256 hex>` (api/preview.go): the server's copy. */
    private fun preview(url: String): ByteArray {
        require(Regex("/api/previews/[0-9a-f]{64}").matches(url)) { "Expected a preview URL" }
        val resolved = requireNotNull((baseUrl + url).toHttpUrlOrNull())
        require(resolved.pathSegments.size == 3 && resolved.pathSegments[1] == "previews") { "Expected a preview URL" }
        return sendBytes(Request.Builder().url(resolved).get())
    }

    /** A person's photo, `/api/people/<username>/avatar?v=<version>` (Avatars.url), as the server stored it. */
    private fun avatar(url: String): ByteArray {
        require(Regex("/api/people/[A-Za-z0-9_.-]{1,64}/avatar[?]v=[A-Za-z0-9]{1,64}").matches(url)) { "Expected an avatar URL" }
        val resolved = requireNotNull((baseUrl + url).toHttpUrlOrNull())
        require(resolved.pathSegments.size == 4 && resolved.pathSegments[3] == "avatar") { "Expected an avatar URL" }
        return sendBytes(Request.Builder().url(resolved).get())
    }

    /** Replaces the own photo with these JPEG bytes; the answer is the new version. */
    fun setAvatar(jpeg: ByteArray): String {
        val v = JSONObject(send(Request.Builder().url(accountUrl("me", "avatar")).put(jpeg.toRequestBody("image/jpeg".toMediaType())))).optString("avatar")
        return v
    }

    fun removeAvatar() {
        send(Request.Builder().url(accountUrl("me", "avatar")).delete())
    }

    // ------------------------------------------------ voice (PROTOCOL-HARMONY §4)

    /** Joins a room and returns the server's first offer. 409 `room_full`, 404 `not_found`. */
    fun voiceJoin(channelId: String): VoiceOffer {
        val body = JSONObject().put("publish", org.json.JSONArray().put("mic"))
        return Json.voiceOffer(JSONObject(post("/api/voice/${channelId}/join", body)))
    }

    fun voiceAnswer(sessionId: String, sdp: String) {
        post("/api/voice/answer", JSONObject().put("session_id", sessionId).put("sdp", sdp))
    }

    fun voiceRestart(sessionId: String): VoiceOffer =
        Json.voiceOffer(JSONObject(post("/api/voice/restart", JSONObject().put("session_id", sessionId))))

    fun voiceCandidate(sessionId: String, candidate: String, sdpMid: String?, sdpMLineIndex: Int) {
        post(
            "/api/voice/candidate",
            JSONObject()
                .put("session_id", sessionId)
                .put("candidate", candidate)
                .put("sdp_mid", sdpMid ?: JSONObject.NULL)
                .put("sdp_mline_index", sdpMLineIndex),
        )
    }

    fun voiceState(sessionId: String, muted: Boolean, deafened: Boolean) {
        post(
            "/api/voice/state",
            JSONObject().put("session_id", sessionId).put("muted", muted).put("deafened", deafened),
        )
    }

    fun voiceLeave(sessionId: String) {
        post("/api/voice/leave", JSONObject().put("session_id", sessionId))
    }

    /** Asks for somebody's screen, or stops (PROTOCOL-HARMONY §4.6). The offer comes on /ws. */
    fun voiceWatch(sessionId: String, username: String, watching: Boolean) {
        post(
            "/api/voice/watch",
            JSONObject().put("session_id", sessionId).put("username", username).put("watching", watching),
        )
    }

    /** Which of the three pictures of somebody's screen this device wants: `alta`, `media`, `baixa`. */
    fun voiceLayer(sessionId: String, username: String, layer: String) {
        post(
            "/api/voice/layer",
            JSONObject().put("session_id", sessionId).put("username", username).put("layer", layer),
        )
    }

    fun voiceOutputDelay(sessionId: String, ms: Int) {
        post("/api/voice/output-delay", JSONObject().put("session_id", sessionId).put("ms", ms))
    }

    /** Starts sending something besides the microphone — the camera. The slot comes in an offer on /ws. */
    fun voicePublish(sessionId: String, sources: List<String>) {
        post("/api/voice/publish", JSONObject().put("session_id", sessionId).put("sources", org.json.JSONArray(sources)))
    }

    fun voiceUnpublish(sessionId: String, sources: List<String>) {
        post("/api/voice/unpublish", JSONObject().put("session_id", sessionId).put("sources", org.json.JSONArray(sources)))
    }

    // ------------------------------------------------ direct calls (PROTOCOL-HARMONY §7)

    private fun callUrl(id: String, action: String? = null): HttpUrl =
        requireNotNull(baseUrl.toHttpUrlOrNull()).newBuilder()
            .addPathSegment("api").addPathSegment("calls").addPathSegment(id)
            .apply { action?.let { addPathSegment(it) } }.build()

    private fun callPost(id: String, action: String, body: JSONObject): String =
        send(Request.Builder().url(callUrl(id, action)).post(body.toString().toRequestBody(JSON)))

    fun callStart(username: String): CallInfo = Json.call(JSONObject(
        post("/api/calls", JSONObject().put("username", username)),
    ))

    fun callGet(id: String): CallInfo = Json.call(JSONObject(send(Request.Builder().url(callUrl(id)).get())))

    fun callAccept(id: String): CallInfo = Json.call(JSONObject(callPost(id, "accept", JSONObject())))

    fun callHangup(id: String, reason: String) {
        callPost(id, "hangup", JSONObject().put("reason", reason))
    }

    fun callDescription(id: String, sdp: Sdp) {
        callPost(id, "signal", JSONObject().put("kind", sdp.type).put("payload", Json.sdpJson(sdp)))
    }

    fun callCandidate(id: String, c: Candidate) {
        callPost(id, "signal", JSONObject().put("kind", "ice").put("payload", Json.candidateJson(c)))
    }

    fun callState(id: String, mic: Boolean, cam: Boolean, screen: Boolean) {
        callPost(id, "state", JSONObject().put("mic", mic).put("cam", cam).put("screen", screen))
    }

    /** The diagnostic log, kept on the server for the owner (the Windows program's "Enviar diagnóstico"). */
    fun sendDiagnostic(report: String) {
        post("/api/diagnostics", JSONObject().put("report", report))
    }

    /** The newest Android APK on the server; null when none is published (404 no_app). */
    fun appRelease(): Release? = try {
        Json.release(get("/api/app/android"))
            ?: throw IOException("unreadable release answer")
    } catch (e: ApiError) {
        if (e.status == 404 && e.reason == "no_app") null else throw e
    }

    /**
     * Downloads [r] into [dest] with this session, checking size and sha256
     * as it is written. On any failure — network, size, hash, [cancelled] —
     * [dest] is deleted and the call throws: a partial file that looks
     * complete must never reach the installer.
     */
    fun downloadRelease(r: Release, dest: java.io.File, cancelled: () -> Boolean, onProgress: (Long, Long) -> Unit) {
        // 15 MB on a bad mobile network outlasts the 20 s the API waits for a
        // whole answer; here the wait is per read, so a slow but moving
        // download is never cut, and a stalled one still is.
        val slow = client.newBuilder().readTimeout(60, java.util.concurrent.TimeUnit.SECONDS).build()
        val b = Request.Builder().url(baseUrl + r.url).get().header("User-Agent", userAgent)
        cookie()?.let { b.header("Cookie", "$SESSION_COOKIE=$it") }
        var ok = false
        try {
            slow.newCall(b.build()).execute().use { res ->
                checkResponse(res)
                val digest = java.security.MessageDigest.getInstance("SHA-256")
                var done = 0L
                var lastTick = 0L
                val buf = ByteArray(64 * 1024)
                res.body.byteStream().use { input ->
                    dest.outputStream().use { out ->
                        while (true) {
                            if (cancelled()) throw java.util.concurrent.CancellationException("cancelled")
                            val n = input.read(buf)
                            if (n < 0) break
                            done += n
                            if (done > r.size) throw IOException("longer than announced (${r.size})")
                            digest.update(buf, 0, n)
                            out.write(buf, 0, n)
                            val now = System.nanoTime()
                            if (now - lastTick > 100_000_000L) {
                                lastTick = now
                                onProgress(done, r.size)
                            }
                        }
                        out.fd.sync()
                    }
                }
                if (done != r.size) throw IOException("got $done of ${r.size} bytes")
                val sha = digest.digest().joinToString("") { "%02x".format(it) }
                if (sha != r.sha256) throw IOException("sha256 mismatch")
                onProgress(done, r.size)
                ok = true
            }
        } finally {
            if (!ok) dest.delete()
        }
    }

    private fun post(path: String, body: JSONObject): String =
        send(Request.Builder().url(baseUrl + path).post(body.toString().toRequestBody(JSON)))

    /** Ends this session on the server. Failing is fine: the cookie is forgotten either way. */
    fun logout() {
        try {
            send(Request.Builder().url("$baseUrl/api/logout").post(ByteArray(0).toRequestBody(JSON)))
        } catch (e: IOException) {
            Diag.log("logout: ${e.javaClass.simpleName}")
        }
    }

    private fun get(path: String): String = send(Request.Builder().url(baseUrl + path).get())

    private fun send(b: Request.Builder): String = sendBytes(b).toString(Charsets.UTF_8)

    private fun sendBytes(b: Request.Builder): ByteArray {
        b.header("User-Agent", userAgent)
        cookie()?.let { b.header("Cookie", "$SESSION_COOKIE=$it") }
        val req = b.build()
        val transport = if (req.url.encodedPath in setOf("/api/voice/restart", "/api/voice/answer")) voiceClient else client
        transport.newCall(req).execute().use { res ->
            checkResponse(res)
            return res.body.bytes()
        }
    }

    private fun checkResponse(res: okhttp3.Response) {
        if (res.isSuccessful) return
        val reason = Json.error(res.body.string())
        if (res.code == 403 && reason == "email_required") {
            Diag.log("api: confirmação de e-mail exigida")
            onEmailRequired()
        }
        if (res.code == 401 && reason != "bad_credentials") throw Unauthorized()
        throw ApiError(res.code, reason)
    }

    private companion object {
        val JSON = "application/json".toMediaType()
    }
}
