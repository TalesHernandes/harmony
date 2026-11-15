package com.taleshernandes.harmony

import com.taleshernandes.harmony.model.Release
import com.taleshernandes.harmony.model.Releases
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.Json
import com.taleshernandes.harmony.net.Unauthorized
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Request
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.util.concurrent.CancellationException

class ReleaseTest {
    private val sha = "a".repeat(64)
    private fun json(code: Any = 11, name: Any = "\"0.9\"", size: Any = 100, s: Any = "\"$sha\"", url: Any = "\"/api/app/android/harmony-11-0.9.apk\"") =
        """{"version_code":$code,"version_name":$name,"size":$size,"sha256":$s,"url":$url}"""

    @Test fun readsTheServersAnswer() {
        assertEquals(Release(11, "0.9", 100, sha, "/api/app/android/harmony-11-0.9.apk"), Json.release(json()))
    }

    @Test fun anythingUnsoundIsNoRelease() {
        for (bad in listOf(
            "not json", "{}", json(code = 0), json(code = -1), json(size = 0), json(name = "\"\""),
            json(s = "\"${"A".repeat(64)}\""), json(s = "\"abc\""),
            json(url = "\"https://evil.test/api/app/android/x.apk\""),
            json(url = "\"//evil.test/api/app/android/x.apk\""),
            json(url = "\"/api/app/android/../../api/me\""),
            json(url = "\"/api/app/android/a/b.apk\""),
            json(url = "\"/api/app/android/%2e%2e\""),
            json(url = "\"/api/app/android/\""),
            json(url = "\"/api/files/x\""),
        )) assertNull(bad, Json.release(bad))
    }

    @Test fun newerIsStrictlyNewer() {
        val r = Release(11, "0.9", 1, sha, "/api/app/android/x.apk")
        assertTrue(Releases.isNewer(10, r))
        assertFalse(Releases.isNewer(11, r))
        assertFalse(Releases.isNewer(12, r))
        assertFalse(Releases.isNewer(10, null))
    }

    private val apk = ByteArray(300_000) { (it * 31).toByte() }
    private fun hex(b: ByteArray) = MessageDigest.getInstance("SHA-256").digest(b).joinToString("") { "%02x".format(it) }
    private fun release(bytes: ByteArray = apk, size: Long = apk.size.toLong(), sum: String = hex(apk)) =
        Release(11, "0.9", size, sum, "/api/app/android/harmony-11-0.9.apk").also { assertTrue(bytes.isNotEmpty()) }

    private fun api(code: Int = 200, body: ByteArray = apk, seen: MutableList<Request> = mutableListOf()): Api {
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val req = chain.request()
            seen.add(req)
            Response.Builder().request(req).protocol(Protocol.HTTP_1_1).code(code).message("x")
                .body(body.toResponseBody("application/vnd.android.package-archive".toMediaType())).build()
        }.build()
        return Api(client, "https://example.test", { "test-session" }, "test-agent")
    }

    private fun temp(): File = File.createTempFile("harmony", ".apk").also { it.delete() }

    @Test fun downloadsWithTheSessionAndChecks() {
        val seen = mutableListOf<Request>()
        val dest = temp()
        var last = 0L
        api(seen = seen).downloadRelease(release(), dest, { false }) { done, total ->
            assertEquals(apk.size.toLong(), total)
            last = done
        }
        assertArrayEquals(apk, dest.readBytes())
        assertEquals(apk.size.toLong(), last)
        assertEquals("/api/app/android/harmony-11-0.9.apk", seen.single().url.encodedPath)
        assertEquals("harmony_session=test-session", seen.single().header("Cookie"))
        dest.delete()
    }

    private fun refused(api: Api, r: Release, cancelled: () -> Boolean = { false }): Exception {
        val dest = temp()
        try {
            api.downloadRelease(r, dest, cancelled) { _, _ -> }
            fail("should have refused")
            throw AssertionError()
        } catch (e: Exception) {
            assertFalse("a refused download leaves no file", dest.exists())
            return e
        }
    }

    @Test fun aWrongHashLeavesNothing() {
        assertTrue(refused(api(), release(sum = "b".repeat(64))) is IOException)
    }

    @Test fun shortOrLongLeavesNothing() {
        assertTrue(refused(api(body = apk.copyOf(1000)), release()) is IOException)
        assertTrue(refused(api(), release(size = 1000)) is IOException)
    }

    @Test fun cancelledLeavesNothing() {
        var calls = 0
        assertTrue(refused(api(), release(), cancelled = { ++calls > 2 }) is CancellationException)
    }

    @Test fun signedOutIsUnauthorized() {
        assertTrue(refused(api(code = 401, body = ByteArray(1)), release()) is Unauthorized)
    }

    @Test fun noAppIsNull() {
        assertNull(api(code = 404, body = """{"error":"no_app"}""".toByteArray()).appRelease())
    }
}
