package com.taleshernandes.harmony

import com.taleshernandes.harmony.model.inviteState
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.ApiError
import com.taleshernandes.harmony.net.Json
import com.taleshernandes.harmony.net.Unauthorized
import com.taleshernandes.harmony.ui.personStateLabel
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Request
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import okio.Buffer
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.time.Instant
import java.time.ZoneId

class AccountJsonTest {
    private val created = "2026-09-26T10:00:00Z"
    private val expires = "2026-10-03T10:00:00Z"
    private fun invite(id: String, by: String, used: Boolean, usedBy: String = "") =
        """{"id":"$id","note":"Para um amigo","created_at":"$created","expires_at":"$expires",
            "created_by":"$by","used":$used,"used_by":"$usedBy"}"""

    private class Server {
        val requests = mutableListOf<Request>()
        var code = 200
        var body = "{}"
        val api = Api(OkHttpClient.Builder().addInterceptor { chain ->
            requests += chain.request()
            Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1).code(code)
                .message("test").body(body.toResponseBody()).build()
        }.build(), "https://example.test", { "test-session" }, "test-agent")
        fun request() = requests.last()
        fun json() = JSONObject(Buffer().also { request().body!!.writeTo(it) }.readUtf8())
    }

    @Test fun sessionsReadAllServerFields() {
        val server = Server()
        server.body = """{"sessions":[{"id":"this","created_at":"$created","last_seen_at":"2026-09-26T11:00:00Z",
            "expires_at":"$expires","user_agent":"Android","current":true},
            {"id":"other","created_at":"$created","last_seen_at":"$created","expires_at":"$expires",
            "user_agent":"Browser","current":false}]}"""
        val sessions = server.api.sessions()
        assertEquals(2, sessions.size)
        val current = sessions.first()
        assertEquals("this", current.id)
        assertEquals(Instant.parse(created).toEpochMilli(), current.createdAt)
        assertEquals(Instant.parse("2026-09-26T11:00:00Z").toEpochMilli(), current.lastSeenAt)
        assertEquals(Instant.parse(expires).toEpochMilli(), current.expiresAt)
        assertEquals("Android", current.userAgent)
        assertTrue(current.current)
        assertFalse(sessions.last().current)
        assertEquals("GET", server.request().method)
        assertEquals("/api/sessions", server.request().url.encodedPath)
        assertTrue(Json.sessions("""{"sessions":null}""").isEmpty())
    }

    @Test fun invitesReadUsedUnusedAndCommandLineCreators() {
        val server = Server()
        server.body = """{"can_invite":true,"invites":[${invite("one", "me", false)},
            ${invite("two", "owner", true, "friend")},${invite("three", "", false)}]}"""
        val list = server.api.invites()!!
        assertEquals(3, list.size)
        assertFalse(list[0].used)
        assertEquals("me", list[0].createdBy)
        assertEquals("Para um amigo", list[0].note)
        assertEquals(Instant.parse(expires).toEpochMilli(), list[0].expiresAt)
        assertTrue(list[1].used)
        assertEquals("friend", list[1].usedBy)
        assertEquals("", list[2].createdBy)
        assertNull(list[0].code)
        assertNull(list[0].url)
        assertEquals("/api/invites", server.request().url.encodedPath)
        server.body = """{"can_invite":true,"invites":null}"""
        assertEquals(emptyList<Any>(), server.api.invites())
    }

    @Test fun onlyAuthorizationResponsesHideInvites() {
        val server = Server()
        for (status in listOf(403, 404)) {
            server.code = status
            server.body = """{"error":"not_allowed"}"""
            assertNull(server.api.invites())
        }
        server.code = 200
        server.body = """{"can_invite":false,"invites":[]}"""
        assertNull(server.api.invites())
        server.code = 401
        assertThrows(Unauthorized::class.java) { server.api.invites() }
        for (status in listOf(429, 500)) {
            server.code = status
            assertEquals(status, assertThrows(ApiError::class.java) { server.api.invites() }.status)
        }
    }

    @Test fun writesUseExactBodiesEscapedSegmentsAndSessionHeaders() {
        val server = Server()
        server.api.revokeSession("device/with ?#")
        assertEquals("DELETE", server.request().method)
        assertEquals("/api/sessions/device%2Fwith%20%3F%23", server.request().url.encodedPath)
        server.body = """{"revoked":3}"""
        assertEquals(3, server.api.revokeOthers())
        assertEquals("POST", server.request().method)
        assertEquals("/api/sessions/revoke-others", server.request().url.encodedPath)
        server.body = JSONObject(invite("fresh", "me", false)).put("code", "test-code")
            .put("url", "https://example.test/signup?code=test-code").toString()
        val fresh = server.api.createInvite("Uma nota \"privada\"")
        assertEquals("POST", server.request().method)
        assertEquals("/api/invites", server.request().url.encodedPath)
        assertEquals("Uma nota \"privada\"", server.json().getString("note"))
        assertEquals(1, server.json().length())
        assertEquals("test-code", fresh.code)
        assertEquals("https://example.test/signup?code=test-code", fresh.url)
        server.api.revokeInvite("invite/a")
        assertEquals("DELETE", server.request().method)
        assertEquals("/api/invites/invite%2Fa", server.request().url.encodedPath)
        for (name in listOf("Apelido \"💛\"", "")) {
            server.body = JSONObject().put("name", name).toString()
            assertEquals(name, server.api.nickname("person/a ?#", name))
            assertEquals("PUT", server.request().method)
            assertEquals("/api/people/person%2Fa%20%3F%23/nickname", server.request().url.encodedPath)
            assertEquals(name, server.json().getString("name"))
            assertEquals(1, server.json().length())
        }
        server.requests.forEach {
            assertEquals("harmony_session=test-session", it.header("Cookie"))
            assertEquals("test-agent", it.header("User-Agent"))
            assertNull(it.url.query)
        }
    }

    @Test fun mutationFailuresAreNotReportedAsSuccess() {
        val server = Server()
        server.code = 403
        server.body = """{"error":"not_allowed"}"""
        assertThrows(ApiError::class.java) { server.api.revokeSession("other") }
        assertThrows(ApiError::class.java) { server.api.revokeOthers() }
        assertThrows(ApiError::class.java) { server.api.createInvite("") }
        assertThrows(ApiError::class.java) { server.api.revokeInvite("other") }
        assertThrows(ApiError::class.java) { server.api.nickname("person", "nickname") }
    }

    @Test fun invitationStateMatchesAllThreeCreatorFormsAndDayRounding() {
        val now = Instant.parse(created).toEpochMilli()
        val zone = ZoneId.of("UTC")
        val own = Json.invite(JSONObject(invite("one", "me", false)))
        val used = Json.invite(JSONObject(invite("two", "owner", true, "friend")))
        val cli = Json.invite(JSONObject(invite("three", "", false)))
        assertEquals("Criado por você · 26/09, 10:00 · vence em 7 dias", inviteState(own, "me", now, zone))
        assertEquals("Criado por @owner · 26/09, 10:00 · usado por @friend", inviteState(used, "me", now, zone))
        assertEquals("Criado pela linha de comando · 26/09, 10:00 · vence em 7 dias", inviteState(cli, "me", now, zone))
        assertTrue(inviteState(own.copy(expiresAt = now + 1), "me", now, zone).endsWith("vence em 1 dia"))
        assertTrue(inviteState(own.copy(expiresAt = now - 1), "me", now, zone).endsWith("vence em 1 dia"))
        assertTrue(inviteState(own.copy(expiresAt = now + 86_400_001), "me", now, zone).endsWith("vence em 2 dias"))
    }

    @Test fun presenceUsesExistingWireStates() {
        assertEquals("Online", personStateLabel("online"))
        assertEquals("Ausente", personStateLabel("away"))
        assertEquals("Em chamada", personStateLabel("in_call"))
        assertEquals("Offline", personStateLabel("offline"))
    }
}
