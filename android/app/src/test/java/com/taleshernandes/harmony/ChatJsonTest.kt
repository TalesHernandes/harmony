package com.taleshernandes.harmony

import com.taleshernandes.harmony.model.*
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.ApiError
import com.taleshernandes.harmony.net.Json
import com.taleshernandes.harmony.net.Unauthorized
import okhttp3.*
import okhttp3.ResponseBody.Companion.toResponseBody
import okio.Buffer
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ChatJsonTest {
    // The fields and optional members emitted by api/chat.go:messageJSON.
    private val full = """{"id":42,"channel_id":"c","author":{"username":"sol","display_name":"Sol"},
        "body":"hello @tales","created_at":"2026-09-25T12:00:00Z","edited_at":"2026-09-25T12:01:00Z",
        "attachments":[{"id":"f1","name":"photo.png","mime":"image/png","size":1234,"width":40,
          "height":30,"url":"/api/files/f1","missing":true}],
        "mentions":["tales"],"reactions":[{"emoji":"👍","users":["sol","tales"]}],"deleted":false,
        "reply_to":{"id":40,"author":{"username":"tales","display_name":"Tales"},"body":"question","deleted":false}}"""

    @Test fun fullMessageAndOptionalKind() {
        val m = Json.message(JSONObject(full))
        assertEquals(42L, m.id)
        assertEquals("c", m.channelId)
        assertEquals(Author("sol", "Sol"), m.author)
        assertEquals("hello @tales", m.body)
        assertEquals(1790337600000L, m.createdAt)
        assertEquals(1790337660000L, m.editedAt)
        assertFalse(m.deleted)
        assertEquals("", m.kind)
        assertEquals(Attachment("f1", "photo.png", "image/png", 1234, 40, 30, "/api/files/f1", true), m.attachments.single())
        assertTrue(m.attachments.single().isImage)
        assertEquals(listOf("tales"), m.mentions)
        assertEquals(listOf(Reaction("👍", listOf("sol", "tales"))), m.reactions)
        assertEquals(ReplyRef(40, Author("tales", "Tales"), "question", false), m.replyTo)
        assertEquals("call", Json.message(JSONObject(full).put("kind", "call")).kind)
    }

    @Test fun tombstoneAndDeletedReply() {
        val m = Json.message(JSONObject("""{"id":42,"channel_id":"c",
            "author":{"username":"sol","display_name":"Sol"},"body":"",
            "created_at":"2026-09-25T12:00:00Z","deleted":true,
            "deleted_at":"2026-09-25T13:00:00Z","deleted_by":"tales",
            "attachments":[],"mentions":[],"reactions":[],
            "reply_to":{"id":40,"author":{"username":"tales","display_name":"Tales"},"body":"","deleted":true}}"""))
        assertTrue(m.deleted)
        assertTrue(m.replyTo!!.deleted)
        assertTrue(m.attachments.isEmpty())
        assertNull(m.editedAt)
    }

    @Test fun missingFieldsAndDatesAreTolerated() {
        val m = Json.message(JSONObject())
        assertEquals(0L, m.createdAt)
        assertNull(m.editedAt)
        assertEquals("", m.kind)
        assertEquals(Author("", ""), m.author)
        assertTrue(m.attachments.isEmpty() && m.mentions.isEmpty() && m.reactions.isEmpty())
        assertNull(m.replyTo)
        assertFalse(Json.attachment(JSONObject()).missing)
        assertFalse(Json.attachment(JSONObject().put("mime", "application/pdf")).isImage)
        assertEquals(Channel("", "", "", ""), Json.channel(JSONObject()))
        assertEquals(HistoryPage(emptyList(), false), Json.history("{}"))
        assertEquals(emptyList<Member>(), Json.members("{}"))
        assertEquals(m.createdAt, Json.message(JSONObject().put("created_at", "bad")).createdAt)
        assertEquals(1790337600123L, Json.message(JSONObject().put("created_at", "2026-09-25T09:00:00.123-03:00")).createdAt)
    }

    @Test fun historyIsAlwaysAscending() {
        val p = Json.history("""{"messages":[{"id":3},{"id":1},{"id":2}],"has_more":true}""")
        assertEquals(listOf(1L, 2L, 3L), p.messages.map { it.id })
        assertTrue(p.hasMore)
    }

    @Test fun dmAndMembers() {
        val c = Json.channel(JSONObject("""{"id":"d","kind":"dm","name":"","topic":"","muted":true,
            "unread":3,"mentions":2,"last_message_id":42,"last_read_id":39,
            "peer":{"username":"sol","display_name":"Sol"}}"""))
        assertTrue(c.isDm)
        assertFalse(c.isText)
        assertEquals(Author("sol", "Sol"), c.peer)
        assertEquals(listOf(3, 2), listOf(c.unread, c.mentions))
        assertEquals(listOf(42L, 39L), listOf(c.lastMessageId, c.lastReadId))
        assertTrue(c.muted)
        assertEquals(listOf(Member("sol", "Sol", false, "Sun"), Member("tales", "Tales", true, null)),
            Json.members("""{"members":[{"username":"sol","display_name":"Sol","is_admin":false,"nickname":"Sun"},
                {"username":"tales","display_name":"Tales","is_admin":true}]}"""))
    }

    @Test fun allChatEvents() {
        assertEquals(GatewayEvent.MessageNew(Json.message(JSONObject(full))), Json.event("""{"type":"message","data":$full}"""))
        assertEquals(GatewayEvent.MessageChanged(Json.message(JSONObject(full))), Json.event("""{"type":"message_changed","data":$full}"""))
        assertEquals(GatewayEvent.Read("c", 42), Json.event("""{"type":"read","data":{"channel_id":"c","last_read_id":42}}"""))
        assertEquals(GatewayEvent.TypingEvent("c", "sol", "Sol"), Json.event("""{"type":"typing","data":{"channel_id":"c","username":"sol","display_name":"Sol"}}"""))
        for (type in listOf("message", "message_changed", "read", "typing")) {
            assertNotNull(Json.event("""{"type":"$type","data":{}}"""))
            assertEquals(GatewayEvent.Other(type), Json.event("""{"type":"$type"}"""))
        }
    }

    @Test fun apiUsesServerRoutesBodiesCookieAndBinaryFiles() {
        val requests = mutableListOf<Request>()
        val bytes = byteArrayOf(0, 1, -1, -128)
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val req = chain.request()
            requests.add(req)
            val response = when {
                req.url.encodedPath.startsWith("/api/files/") || req.url.encodedPath.startsWith("/api/previews/") -> bytes.toResponseBody()
                req.url.encodedPath == "/api/members" -> """{"members":[]}""".toResponseBody()
                req.method == "GET" -> """{"messages":[],"has_more":false}""".toResponseBody()
                else -> full.toResponseBody()
            }
            Response.Builder().request(req).protocol(Protocol.HTTP_1_1).code(200).message("OK").body(response).build()
        }.build()
        val api = Api(client, "https://example.test", { "test-session" }, "test-agent")
        api.history("c/encoded", 42, 20)
        assertEquals("/api/channels/c%2Fencoded/messages", requests.last().url.encodedPath)
        assertEquals("42", requests.last().url.queryParameter("before"))
        assertEquals("20", requests.last().url.queryParameter("limit"))
        fun body() = JSONObject(Buffer().also { requests.last().body!!.writeTo(it) }.readUtf8())
        api.post("c", "hello", listOf("f1"), 40)
        assertEquals("POST", requests.last().method)
        assertEquals("hello", body().getString("body"))
        assertEquals("f1", body().getJSONArray("attachments").getString(0))
        assertEquals(40L, body().getLong("reply_to"))
        api.edit(42, "edit")
        assertEquals("PATCH", requests.last().method)
        assertEquals("/api/messages/42", requests.last().url.encodedPath)
        assertEquals("edit", body().getString("body"))
        api.delete(42)
        assertEquals("DELETE", requests.last().method)
        api.react(42, "👍", true)
        assertEquals("/api/messages/42/reactions", requests.last().url.encodedPath)
        assertTrue(body().getBoolean("on"))
        api.markRead("c", 42)
        assertEquals("/api/channels/c/read", requests.last().url.encodedPath)
        assertEquals(42L, body().getLong("last_read_id"))
        api.typing("c")
        assertEquals("/api/channels/c/typing", requests.last().url.encodedPath)
        api.openDm("sol")
        assertEquals("/api/dms", requests.last().url.encodedPath)
        assertEquals("sol", body().getString("username"))
        assertTrue(api.members().isEmpty())
        api.upload("c", "photo.png", "image/png", bytes)
        assertEquals("/api/channels/c/uploads", requests.last().url.encodedPath)
        val multipart = requests.last().body as MultipartBody
        assertEquals("multipart/form-data", multipart.type.toString())
        assertEquals("form-data; name=\"file\"; filename=\"photo.png\"", multipart.part(0).headers!!["Content-Disposition"])
        assertArrayEquals(bytes, Buffer().also { multipart.part(0).body.writeTo(it) }.readByteArray())
        assertArrayEquals(bytes, api.file("/api/files/f1"))
        assertArrayEquals(bytes, api.file("/api/previews/" + "ab".repeat(32)))
        for (req in requests) {
            assertEquals("harmony_session=test-session", req.header("Cookie"))
            assertEquals("test-agent", req.header("User-Agent"))
        }
        val count = requests.size
        for (url in listOf("https://elsewhere.test/api/files/f1", "/api/me", "/api/files/../me", "/api/files/%2e%2e/me",
            "/api/previews/../me", "/api/previews/" + "AB".repeat(32), "/api/previews/abc")) {
            assertThrows(IllegalArgumentException::class.java) { api.file(url) }
        }
        assertEquals(count, requests.size)
    }

    @Test fun apiPreservesErrorsAndAcceptsEmptyTypingResponse() {
        var code = 204
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1).code(code).message("test")
                .body((if (code == 204) "" else """{"error":"message_limit"}""").toResponseBody()).build()
        }.build()
        val api = Api(client, "https://example.test", { null }, "test")
        api.typing("c")
        code = 401
        assertThrows(Unauthorized::class.java) { api.history("c") }
        code = 429
        val err = assertThrows(ApiError::class.java) { api.post("c", "hello") }
        assertEquals(429, err.status)
        assertEquals("message_limit", err.reason)
    }
}
