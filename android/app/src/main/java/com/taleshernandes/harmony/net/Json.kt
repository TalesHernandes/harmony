package com.taleshernandes.harmony.net

import com.taleshernandes.harmony.model.Channel
import com.taleshernandes.harmony.model.CallAccepted
import com.taleshernandes.harmony.model.CallEnded
import com.taleshernandes.harmony.model.CallIncoming
import com.taleshernandes.harmony.model.CallInfo
import com.taleshernandes.harmony.model.CallPerson
import com.taleshernandes.harmony.model.CallSignal
import com.taleshernandes.harmony.model.CallSignalEvent
import com.taleshernandes.harmony.model.CallStateEvent
import com.taleshernandes.harmony.model.CallTaken
import com.taleshernandes.harmony.model.Candidate
import com.taleshernandes.harmony.model.Sdp
import com.taleshernandes.harmony.model.Attachment
import com.taleshernandes.harmony.model.Embed
import com.taleshernandes.harmony.model.Author
import com.taleshernandes.harmony.model.HistoryPage
import com.taleshernandes.harmony.model.Member
import com.taleshernandes.harmony.model.Message
import com.taleshernandes.harmony.model.Reaction
import com.taleshernandes.harmony.model.ReplyRef
import com.taleshernandes.harmony.model.Release
import com.taleshernandes.harmony.model.Releases
import com.taleshernandes.harmony.model.GatewayEvent
import com.taleshernandes.harmony.model.IceServer
import com.taleshernandes.harmony.model.Peer
import com.taleshernandes.harmony.model.TrackInfo
import com.taleshernandes.harmony.model.User
import com.taleshernandes.harmony.model.VoiceOffer
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import java.time.OffsetDateTime
import java.time.format.DateTimeParseException

/**
 * Reading the server's JSON. Missing fields read as empty rather than failing,
 * the way the web client treats them: a server one version ahead or behind
 * should still draw a list.
 */
object Json {
    fun roles(a: JSONArray?): List<com.taleshernandes.harmony.model.Role> = objects(a).map {
        com.taleshernandes.harmony.model.Role(it.getString("id"), it.getString("name"),
            it.optString("color"), it.optInt("position"), it.optBoolean("hoist"),
            strings(it.optJSONArray("permissions")).toSet(), strings(it.optJSONArray("members")))
    }
    fun roleSnapshot(body: String): com.taleshernandes.harmony.model.RoleSnapshot {
        val o = JSONObject(body)
        return com.taleshernandes.harmony.model.RoleSnapshot(roles(o.optJSONArray("roles")), strings(o.optJSONArray("colors")))
    }

    fun sessions(body: String): List<com.taleshernandes.harmony.model.AccountSession> =
        objects(JSONObject(body).optJSONArray("sessions")).map {
            com.taleshernandes.harmony.model.AccountSession(
                it.optString("id"), date(it.optString("created_at")) ?: 0,
                date(it.optString("last_seen_at")) ?: 0, date(it.optString("expires_at")) ?: 0,
                it.optString("user_agent"), it.optBoolean("current"),
            )
        }

    fun invite(o: JSONObject) = com.taleshernandes.harmony.model.Invite(
        o.optString("id"), o.optString("note"), date(o.optString("created_at")) ?: 0,
        date(o.optString("expires_at")) ?: 0, o.optBoolean("used"),
        if (o.isNull("created_by")) "" else o.optString("created_by"),
        if (o.isNull("used_by")) "" else o.optString("used_by"),
        if (o.isNull("code")) null else o.optString("code"),
        if (o.isNull("url")) null else o.optString("url"),
    )

    /** A successful response can still say this account cannot invite. */
    fun invites(body: String): List<com.taleshernandes.harmony.model.Invite>? {
        val o = JSONObject(body)
        return if (o.optBoolean("can_invite")) objects(o.optJSONArray("invites")).map(::invite) else null
    }

    fun nicknameBody(name: String): JSONObject = JSONObject().put("name", name)

    fun user(o: JSONObject): User {
        com.taleshernandes.harmony.model.Avatars.note(o.optString("username"), o.optString("avatar"))
        return User(
            username = o.optString("username"),
            displayName = o.optString("display_name"),
            isAdmin = o.optBoolean("is_admin"),
            email = o.optString("email"),
            emailVerified = o.optBoolean("email_verified", true),
            twofa = o.optString("twofa"),
        )
    }

    fun channel(o: JSONObject) = Channel(
        id = o.optString("id"),
        kind = o.optString("kind"),
        name = o.optString("name"),
        topic = o.optString("topic"),
        muted = o.optBoolean("muted"),
        unread = o.optInt("unread"),
        mentions = o.optInt("mentions"),
        lastMessageId = o.optLong("last_message_id"),
        lastReadId = o.optLong("last_read_id"),
        peer = o.optJSONObject("peer")?.let(::author),
        categoryId = if (o.isNull("category_id")) "" else o.optString("category_id"),
        position = o.optInt("position"),
    )

    private fun author(o: JSONObject) = Author(o.optString("username"), o.optString("display_name"))

    fun attachment(o: JSONObject) = Attachment(
        o.optString("id"), o.optString("name"), o.optString("mime"), o.optLong("size"),
        o.optInt("width"), o.optInt("height"), o.optString("url"), o.optBoolean("missing"),
    )

    fun message(o: JSONObject) = Message(
        id = o.optLong("id"),
        channelId = o.optString("channel_id"),
        author = author(o.optJSONObject("author") ?: JSONObject()),
        body = o.optString("body"),
        createdAt = date(o.optString("created_at")) ?: 0,
        editedAt = date(o.optString("edited_at")),
        deleted = o.optBoolean("deleted"),
        pinned = o.optBoolean("pinned"),
        kind = o.optString("kind"),
        attachments = objects(o.optJSONArray("attachments")).map(::attachment),
        embeds = objects(o.optJSONArray("embeds")).map {
            Embed(it.optString("url"), it.optString("kind"), it.optString("site"), it.optString("title"),
                it.optString("description"), it.optString("image"), it.optInt("width"), it.optInt("height"))
        },
        mentions = strings(o.optJSONArray("mentions")),
        reactions = objects(o.optJSONArray("reactions")).map {
            Reaction(it.optString("emoji"), strings(it.optJSONArray("users")))
        },
        replyTo = o.optJSONObject("reply_to")?.let {
            ReplyRef(it.optLong("id"), author(it.optJSONObject("author") ?: JSONObject()),
                it.optString("body"), it.optBoolean("deleted"))
        },
    )

    fun history(body: String): HistoryPage {
        val o = JSONObject(body)
        return HistoryPage(objects(o.optJSONArray("messages")).map(::message).sortedBy { it.id },
            o.optBoolean("has_more"), o.optBoolean("has_newer"))
    }

    fun members(body: String): List<Member> = objects(JSONObject(body).optJSONArray("members")).map {
        com.taleshernandes.harmony.model.Avatars.note(it.optString("username"), it.optString("avatar"))
        Member(it.optString("username"), it.optString("display_name"), it.optBoolean("is_admin"),
            if (it.isNull("nickname")) null else it.optString("nickname").ifEmpty { null })
    }

    private fun date(value: String): Long? = try {
        OffsetDateTime.parse(value).toInstant().toEpochMilli()
    } catch (_: DateTimeParseException) {
        null
    }

    fun peer(o: JSONObject) = Peer(
        sessionId = o.optString("session_id"),
        username = o.optString("username"),
        displayName = o.optString("display_name"),
        muted = o.optBoolean("muted"),
        deafened = o.optBoolean("deafened"),
        joinedAt = o.optLong("joined_at"),
        publishing = strings(o.optJSONArray("publishing")),
    )

    /** `GET /api/channels` → the channels, in the server's order. */
    fun channels(body: String): List<Channel> =
        objects(JSONObject(body).optJSONArray("channels")).map(::channel)

    fun layout(o: JSONObject) = GatewayEvent.Layout(
        objects(o.optJSONArray("categories")).map {
            com.taleshernandes.harmony.model.Category(it.optString("id"), it.optString("name"), it.optInt("position"))
        }, objects(o.optJSONArray("channels")).map {
            com.taleshernandes.harmony.model.ChannelPlacement(it.optString("id"),
                if (it.isNull("category_id")) "" else it.optString("category_id"), it.optInt("position"))
        })

    fun channelSnapshot(body: String): com.taleshernandes.harmony.model.ChannelSnapshot {
        val o = JSONObject(body)
        return com.taleshernandes.harmony.model.ChannelSnapshot(channels(body), layout(o).categories, o.optBoolean("can_manage"))
    }

    /** `GET /api/voice/rooms` → who is inside each room, by channel id. */
    fun rooms(body: String): Map<String, List<Peer>> {
        val rooms = JSONObject(body).optJSONObject("rooms") ?: return emptyMap()
        return rooms.keys().asSequence().associateWith { id ->
            objects(rooms.optJSONArray(id)).map(::peer)
        }
    }

    /** The join answer, or a `voice_offer` event's data (PROTOCOL-HARMONY §4.1). */
    fun voiceOffer(o: JSONObject): VoiceOffer {
        val mids = o.optJSONObject("send_mids")
        return VoiceOffer(
            sessionId = o.optString("session_id"),
            sdp = o.optString("sdp"),
            sendMids = mids?.keys()?.asSequence()?.associateWith { mids.optString(it) }.orEmpty(),
            tracks = objects(o.optJSONArray("tracks")).map {
                TrackInfo(
                    mid = it.optString("mid"),
                    username = it.optString("username"),
                    displayName = it.optString("display_name"),
                    kind = it.optString("kind"),
                    source = it.optString("source"),
                )
            },
            iceServers = iceServers(o),
            iceRestart = o.optBoolean("ice_restart", false),
        )
    }

    private fun iceServers(o: JSONObject): List<IceServer> = objects(o.optJSONArray("ice_servers")).map {
        // The server sends a list; WebRTC also allows a single URL string.
        val urls = it.optJSONArray("urls")?.let(::strings)
            ?: listOfNotNull(it.optString("urls").ifEmpty { null })
        IceServer(urls, it.optString("username").ifEmpty { null }, it.optString("credential").ifEmpty { null })
    }

    private fun callPerson(o: JSONObject) = CallPerson(o.optString("username"), o.optString("display_name"))

    fun call(o: JSONObject): CallInfo = CallInfo(
        id = o.optString("id"),
        from = callPerson(o.optJSONObject("from") ?: JSONObject()),
        to = callPerson(o.optJSONObject("to") ?: JSONObject()),
        iceServers = iceServers(o),
        polite = o.optBoolean("polite"),
        answered = o.optBoolean("answered"),
    )

    fun sdpJson(s: Sdp): JSONObject = JSONObject().put("type", s.type).put("sdp", s.sdp)

    fun candidateJson(c: Candidate): JSONObject = JSONObject()
        .put("candidate", c.candidate).put("sdpMid", c.sdpMid ?: JSONObject.NULL)
        .put("sdpMLineIndex", c.sdpMLineIndex)

    private fun callSignal(o: JSONObject): CallSignalEvent? {
        val payload = o.optJSONObject("payload") ?: return null
        val id = o.optString("id")
        val kind = o.optString("kind")
        val signal = when (kind) {
            "offer", "answer" -> {
                val sdp = payload.opt("sdp") as? String ?: return null
                if (sdp.isEmpty()) return null
                val type = if (payload.isNull("type")) kind else payload.optString("type")
                CallSignal.Description(id, Sdp(type, sdp))
            }
            "ice" -> CallSignal.Ice(id, Candidate(
                payload.optString("candidate"),
                if (payload.isNull("sdpMid")) null else payload.optString("sdpMid"),
                payload.optInt("sdpMLineIndex", 0),
            ))
            else -> return null
        }
        return CallSignalEvent(signal)
    }

    /** `{"user": {...}}`, the answer to a login and to `GET /api/me`. */
    /** The newest APK on the server, or null for anything that is not a sound answer. Never throws. */
    fun release(text: String): Release? = try {
        val o = JSONObject(text)
        val r = Release(
            versionCode = o.getInt("version_code"),
            versionName = o.getString("version_name"),
            size = o.getLong("size"),
            sha256 = o.getString("sha256"),
            url = o.getString("url"),
        )
        r.takeIf {
            it.versionCode > 0 && it.size > 0 && it.versionName.isNotBlank() &&
                Releases.validSha(it.sha256) && Releases.safePath(it.url)
        }
    } catch (e: Exception) {
        null
    }

    fun userOf(body: String): User = user(JSONObject(body).getJSONObject("user"))

    /** The `error` of an error answer, or null when the body is not one. */
    fun error(body: String): String? = try {
        JSONObject(body).optString("error").ifEmpty { null }
    } catch (_: JSONException) {
        null
    }

    /** One /ws message. Returns null for something that is not the envelope. */
    fun event(text: String): GatewayEvent? {
        val env = try {
            JSONObject(text)
        } catch (_: JSONException) {
            return null
        }
        val type = env.optString("type").ifEmpty { return null }
        val data = env.optJSONObject("data")
        return when (type) {
            // A message without a channel cannot be routed to a timeline.
            "message" -> data?.takeIf { it.optString("channel_id").isNotEmpty() }
                ?.let { GatewayEvent.MessageNew(message(it)) }
            "message_changed" -> data?.takeIf { it.optString("channel_id").isNotEmpty() }
                ?.let { GatewayEvent.MessageChanged(message(it)) }
            "read" -> data?.let { GatewayEvent.Read(it.optString("channel_id"), it.optLong("last_read_id")) }
            "typing" -> data?.let {
                GatewayEvent.TypingEvent(it.optString("channel_id"), it.optString("username"), it.optString("display_name"))
            }
            "voice_room" -> data?.let {
                GatewayEvent.VoiceRoom(it.optString("channel_id"), objects(it.optJSONArray("peers")).map(::peer))
            }
            "roles" -> data?.let { GatewayEvent.Roles(roles(it.optJSONArray("roles"))) }
            "layout" -> data?.let { layout(it) }
            "channel" -> data?.let { GatewayEvent.ChannelUpsert(channel(it)) }
            "channel_removed" -> data?.let { GatewayEvent.ChannelRemoved(it.optString("id")) }
            "voice_offer" -> data?.let { GatewayEvent.VoiceOfferEvent(voiceOffer(it)) }
            "call" -> data?.let {
                CallIncoming(it.optString("id"), callPerson(it.optJSONObject("from") ?: JSONObject()))
            }
            "call_accepted" -> data?.let {
                CallAccepted(it.optString("id"), callPerson(it.optJSONObject("peer") ?: JSONObject()))
            }
            "call_taken" -> data?.let { CallTaken(it.optString("id"), it.optString("by")) }
            "call_ended" -> data?.let { CallEnded(it.optString("id"), it.optString("reason")) }
            "call_signal" -> data?.let(::callSignal)
            "call_state" -> data?.let {
                CallStateEvent(it.optString("id"), it.optBoolean("mic"), it.optBoolean("cam"), it.optBoolean("screen"))
            }
            "presence" -> data?.let {
                if (it.has("avatar")) com.taleshernandes.harmony.model.Avatars.note(it.optString("username"), it.optString("avatar"))
                GatewayEvent.Presence(it.optString("username"), it.optString("state"))
            }
            // Somebody changed or removed their photo: the faces follow at once.
            "profile" -> data?.let { com.taleshernandes.harmony.model.Avatars.note(it.optString("username"), it.optString("avatar")); GatewayEvent.Other(type) }
            else -> GatewayEvent.Other(type)
        } ?: GatewayEvent.Other(type)
    }

    private fun objects(a: JSONArray?): List<JSONObject> =
        if (a == null) emptyList() else (0 until a.length()).mapNotNull { a.optJSONObject(it) }

    private fun strings(a: JSONArray?): List<String> =
        if (a == null) emptyList() else (0 until a.length()).map { a.optString(it) }
}
