package com.taleshernandes.harmony

import com.taleshernandes.harmony.model.*
import com.taleshernandes.harmony.net.*
import okhttp3.*
import okhttp3.ResponseBody.Companion.toResponseBody
import okio.Buffer
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ChannelApiTest {
    @Test fun saveLayoutUsesOneCompleteAuthenticatedPutAndKeepsConflictReason() {
        var calls = 0
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            calls++
            val req = chain.request()
            assertEquals("PUT", req.method)
            assertEquals("/api/layout", req.url.encodedPath)
            assertEquals("harmony_session=test", req.header("Cookie"))
            val body = JSONObject(Buffer().also { req.body!!.writeTo(it) }.readUtf8())
            assertEquals(2, body.getJSONArray("channels").length())
            assertEquals("b", body.getJSONArray("channels").getJSONObject(0).getString("id"))
            Response.Builder().request(req).protocol(Protocol.HTTP_1_1).code(409).message("Conflict")
                .body("""{"error":"stale_layout"}""".toResponseBody()).build()
        }.build()
        val layout = ChannelLayout(emptyList(), listOf(ChannelPlacement("a", ""), ChannelPlacement("b", ""))).reorder(0, 1, false)
        try {
            Api(client, "https://example.test", { "test" }, "test").organize("PUT", "/api/layout", layout.body())
            fail("Conflict was swallowed")
        } catch (e: ApiError) { assertEquals("stale_layout", e.reason) }
        assertEquals(1, calls)
    }

    @Test fun pinAndUnpinParseAuthoritativeMessageAndPinsStayNewestFirst() {
        val methods = mutableListOf<String>()
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val req = chain.request(); methods.add(req.method)
            val body = if (req.method == "GET") """{"messages":[{"id":1},{"id":3}],"limit":250}"""
                else """{"id":3,"pinned":${req.method == "POST"}}"""
            Response.Builder().request(req).protocol(Protocol.HTTP_1_1).code(200).message("OK").body(body.toResponseBody()).build()
        }.build()
        val api = Api(client, "https://example.test", { "test" }, "test")
        assertTrue(api.pin(3, true).pinned)
        assertFalse(api.pin(3, false).pinned)
        assertEquals(listOf(3L, 1L), api.pins("channel").map { it.id })
        assertEquals(listOf("POST", "DELETE", "GET"), methods)
    }
}
