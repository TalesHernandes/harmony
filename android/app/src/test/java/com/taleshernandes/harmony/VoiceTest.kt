package com.taleshernandes.harmony

import com.taleshernandes.harmony.media.Speaking
import com.taleshernandes.harmony.model.GatewayEvent
import com.taleshernandes.harmony.net.Json
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.ApiError
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class VoiceTest {
    @Test fun onlyRestartAndAnswerHaveShortHttpDeadlines() {
        val client = OkHttpClient.Builder().connectTimeout(20, java.util.concurrent.TimeUnit.SECONDS)
            .readTimeout(20, java.util.concurrent.TimeUnit.SECONDS).addInterceptor { chain ->
                val fast = chain.request().url.encodedPath in setOf("/api/voice/restart", "/api/voice/answer")
                assertEquals(if (fast) 3000 else 20000, chain.connectTimeoutMillis())
                assertEquals(if (fast) 3000 else 20000, chain.readTimeoutMillis())
                Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1)
                    .code(200).message("OK")
                    .body("""{"session_id":"S1","sdp":"offer","ice_restart":true}""".toResponseBody()).build()
            }.build()
        val api = Api(client, "https://example.test", { "test-session" }, "test")
        api.voiceRestart("S1")
        api.voiceAnswer("S1", "answer")
        api.voiceState("S1", false, false)
        api.networkChanged()
    }

    @Test
    fun missingRestartSessionExposesTheStatusAndReasonForImmediateRejoin() {
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            assertEquals("POST", chain.request().method)
            assertEquals("/api/voice/restart", chain.request().url.encodedPath)
            assertEquals("harmony_session=test-session", chain.request().header("Cookie"))
            Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1)
                .code(404).message("Not Found")
                .body("""{"error":"not_found"}""".toResponseBody()).build()
        }.build()
        try {
            Api(client, "https://example.test", { "test-session" }, "test").voiceRestart("S1")
            fail("Accepted a missing voice session")
        } catch (e: ApiError) {
            assertEquals(404, e.status)
            assertEquals("not_found", e.reason)
        }
    }

    @Test
    fun restartOfferPreservesTheSessionAndDistinguishesReplay() {
        val replay = Json.voiceOffer(JSONObject("""{"session_id":"S1","sdp":"offer"}"""))
        val restart = Json.voiceOffer(JSONObject("""{"session_id":"S1","sdp":"new-offer","ice_restart":true}"""))
        assertEquals(replay.sessionId, restart.sessionId)
        assertEquals(false, replay.iceRestart)
        assertEquals(true, restart.iceRestart)
    }
    // Shaped like voice.go handleVoiceJoin writes it: sfu.Offer plus ice_servers.
    private val join = """{"session_id":"S1","type":"offer","sdp":"v=0\r\n",
        "send_mids":{"mic":"0"},
        "tracks":[{"mid":"1","username":"sol","display_name":"Sol","kind":"audio","source":"mic"},
                  {"mid":"2","username":"sol","display_name":"Sol","kind":"video","source":"screen"}],
        "ice_servers":[{"urls":["stun:harmony.taleshernandes.com:3478"]},
                       {"urls":["turn:harmony.taleshernandes.com:3478?transport=udp","turn:harmony.taleshernandes.com:3478?transport=tcp"],
                        "username":"1790000000:tales","credential":"abc="}]}"""

    @Test
    fun joinAnswer() {
        val o = Json.voiceOffer(JSONObject(join))
        assertEquals("S1", o.sessionId)
        assertEquals(mapOf("mic" to "0"), o.sendMids)
        assertEquals(listOf("1", "2"), o.tracks.map { it.mid })
        assertEquals("screen", o.tracks[1].source)
        assertEquals(2, o.iceServers.size)
        assertEquals(null, o.iceServers[0].username)
        assertEquals(2, o.iceServers[1].urls.size)
        assertEquals("abc=", o.iceServers[1].credential)
    }

    @Test
    fun renegotiationComesOnTheSocketWithoutIceServers() {
        val ev = Json.event("""{"type":"voice_offer","data":{"session_id":"S1","type":"offer","sdp":"x","send_mids":{"mic":"0"},"tracks":[]}}""")
        ev as GatewayEvent.VoiceOfferEvent
        assertEquals("S1", ev.offer.sessionId)
        assertEquals(emptyList<Any>(), ev.offer.iceServers)
        assertEquals("voice_offer", ev.type)
    }

    @Test
    fun aSingleUrlStringIsStillAServer() {
        val o = Json.voiceOffer(JSONObject("""{"session_id":"S","ice_servers":[{"urls":"stun:x:3478"}]}"""))
        assertEquals(listOf("stun:x:3478"), o.iceServers.single().urls)
    }

    @Test
    fun speakingLightsFastAndHoldsThroughGaps() {
        val s = Speaking(floor = 0.02, holdMs = 320)
        s.feed("sol", 0.30, now = 1000)
        assertEquals(setOf("sol"), s.talking(1000))
        // A quiet reading between syllables does not put the light out.
        s.feed("sol", 0.0, now = 1100)
        assertEquals(setOf("sol"), s.talking(1300))
        // Quiet for longer than the hold: out.
        assertEquals(emptySet<String>(), s.talking(1321))
    }

    @Test
    fun belowTheFloorIsSilence() {
        val s = Speaking(floor = 0.02, holdMs = 320)
        s.feed("sol", 0.019, now = 1000)
        s.feed("", 0.9, now = 1000) // a slot nobody could be named for
        assertTrue(s.talking(1000).isEmpty())
        s.feed("sol", 0.020001, now = 1000)
        assertEquals(setOf("sol"), s.talking(1000))
    }

    @Test
    fun everyoneIsTheirOwn() {
        val s = Speaking()
        s.feed("sol", 0.5, now = 0)
        s.feed("tales", 0.5, now = 200)
        assertEquals(setOf("sol", "tales"), s.talking(299))
        assertEquals(setOf("tales"), s.talking(400))
        s.clear()
        assertTrue(s.talking(400).isEmpty())
    }
}
