package com.taleshernandes.harmony

import com.taleshernandes.harmony.net.Api
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.Assert.*
import org.junit.Test
import java.io.IOException

class SearchApiTest {
    @Test fun searchEscapesTheQueryAndUsesThisServersSession() {
        var requests = 0
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            requests++
            assertEquals("example.test", chain.request().url.host)
            assertEquals(8443, chain.request().url.port)
            assertEquals("/api/search", chain.request().url.encodedPath)
            assertEquals("two words & #❤️", chain.request().url.queryParameter("q"))
            assertEquals("harmony_session=test-session", chain.request().header("Cookie"))
            Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1)
                .code(200).message("OK").body("""{"messages":[]}""".toResponseBody()).build()
        }.build()
        assertTrue(Api(client, "https://example.test:8443", { "test-session" }, "test")
            .search("two words & #❤️").isEmpty())
        assertEquals(1, requests)
    }

    @Test fun svgDownloadUsesOnlyTheVersionedAssetPath() {
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            assertEquals("/static/assets/twemoji-15/2764-fe0f-200d-1f525.svg", chain.request().url.encodedPath)
            Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1)
                .code(200).message("OK").body("<svg/>".toResponseBody()).build()
        }.build()
        val api = Api(client, "https://example.test", { null }, "test")
        assertEquals("<svg/>", api.twemoji("2764-fe0f-200d-1f525").toString(Charsets.UTF_8))
        for (name in listOf("../../private", "2764.svg", "https://other.test/2764", "", "ABC")) {
            try { api.twemoji(name); fail("Accepted $name") } catch (_: IllegalArgumentException) { }
        }
    }

    @Test fun oversizedSvgIsRejected() {
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1)
                .code(200).message("OK").body(ByteArray(256 * 1024 + 1).toResponseBody()).build()
        }.build()
        try {
            Api(client, "https://example.test", { null }, "test").twemoji("2764")
            fail("Accepted an oversized SVG")
        } catch (_: IOException) { }
    }
}
