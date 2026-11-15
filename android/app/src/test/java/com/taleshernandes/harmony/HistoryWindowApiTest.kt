package com.taleshernandes.harmony

import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.ApiError
import com.taleshernandes.harmony.net.Json
import com.taleshernandes.harmony.net.Unauthorized
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.Assert.*
import org.junit.Test

class HistoryWindowApiTest {
    private val body = """{"messages":[
        {"id":43,"channel_id":"dm/a & b","body":"after"},
        {"id":41,"channel_id":"dm/a & b","body":"before"},
        {"id":42,"channel_id":"dm/a & b","body":"hit"}],
        "has_more":true,"has_newer":true}"""

    @Test fun aroundUsesTheChannelSessionAndParsesBothBoundaries() {
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val request = chain.request()
            assertEquals("GET", request.method)
            assertEquals("example.test", request.url.host)
            assertEquals(8443, request.url.port)
            assertEquals(listOf("api", "channels", "dm/a & b", "messages"), request.url.pathSegments)
            assertEquals(setOf("around", "limit"), request.url.queryParameterNames)
            assertEquals("42", request.url.queryParameter("around"))
            assertEquals("50", request.url.queryParameter("limit"))
            assertEquals("harmony_session=test-session", request.header("Cookie"))
            Response.Builder().request(request).protocol(Protocol.HTTP_1_1)
                .code(200).message("OK").body(body.toResponseBody()).build()
        }.build()
        val page = Api(client, "https://example.test:8443", { "test-session" }, "test")
            .history("dm/a & b", around = 42, limit = 50)
        assertEquals(listOf(41L, 42L, 43L), page.messages.map { it.id })
        assertEquals("hit", page.messages[1].body)
        assertTrue(page.hasMore && page.hasNewer)
    }

    @Test fun afterUsesOnlyItsCursorAndParsesTheLiveBoundary() {
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            assertEquals(setOf("after"), chain.request().url.queryParameterNames)
            assertEquals("43", chain.request().url.queryParameter("after"))
            Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1)
                .code(200).message("OK")
                .body("""{"messages":[{"id":44}],"has_newer":false}""".toResponseBody()).build()
        }.build()
        val page = Api(client, "https://example.test", { "session-a" }, "test").history("c", after = 43)
        assertEquals(44L, page.messages.single().id)
        assertFalse(page.hasNewer)
    }

    @Test fun oldAndEmptyResponsesDoNotInventANewerBoundary() {
        assertFalse(Json.history("""{"messages":[],"has_more":false}""").hasNewer)
        assertTrue(Json.history("""{"messages":[],"has_newer":true}""").hasNewer)
    }

    @Test fun inaccessibleOrMissingHitsPropagateTheServerError() {
        for (status in listOf(401, 403, 404)) {
            val client = OkHttpClient.Builder().addInterceptor { chain ->
                Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1)
                    .code(status).message("Denied").body("""{"error":"not_found"}""".toResponseBody()).build()
            }.build()
            try {
                Api(client, "https://example.test", { "session-a" }, "test").history("private-b", around = 42)
                fail("Accepted HTTP $status")
            } catch (_: Unauthorized) {
                assertEquals(401, status)
            } catch (e: ApiError) {
                assertEquals(status, e.status)
            }
        }
    }

    @Test fun incompatibleCursorsAreRejectedBeforeAnyRequest() {
        val client = OkHttpClient.Builder().addInterceptor { error("Unexpected HTTP request") }.build()
        val api = Api(client, "https://example.test", { null }, "test")
        for ((before, around, after) in listOf(Triple(1L, 2L, null), Triple(null, 2L, 3L), Triple(1L, null, 3L))) {
            try {
                api.history("c", before = before, around = around, after = after)
                fail("Accepted incompatible cursors")
            } catch (_: IllegalArgumentException) { }
        }
    }
}
