package com.taleshernandes.harmony

import com.taleshernandes.harmony.model.*
import com.taleshernandes.harmony.net.Json
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.ApiError
import com.taleshernandes.harmony.net.Unauthorized
import com.taleshernandes.harmony.push.Wake
import okhttp3.*
import okhttp3.ResponseBody.Companion.toResponseBody
import okio.Buffer
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class DirectJsonTest {
    // call.go: userCard.json; names and IDs are test values, not production data.
    private val from = """{"username":"alice","display_name":"Alice"}"""
    private val to = """{"username":"bob","display_name":"Bob"}"""

    private fun event(type: String, data: String): GatewayEvent =
        requireNotNull(Json.event("""{"type":"$type","data":$data}"""))

    private fun signal(kind: String, payload: String): GatewayEvent =
        event("call_signal", """{"id":"call-1","kind":"$kind","payload":$payload}""")

    @Test
    fun lifecycleEventsKeepRoomsUnchanged() {
        // call.go: handleCallStart, handleCallAccept, handleCallHangup, handleCallState.
        val samples = listOf(
            Triple("call", """{"id":"call-1","from":$from}""", CallIncoming("call-1", CallPerson("alice", "Alice"))),
            Triple("call_accepted", """{"id":"call-1","peer":$to}""", CallAccepted("call-1", CallPerson("bob", "Bob"))),
            Triple("call_taken", """{"id":"call-1","by":"session-2"}""", CallTaken("call-1", "session-2")),
            Triple("call_ended", """{"id":"call-1","reason":"rejected"}""", CallEnded("call-1", "rejected")),
            Triple("call_state", """{"id":"call-1","mic":true,"cam":false,"screen":true}""", CallStateEvent("call-1", true, false, true)),
        )
        val rooms = RoomsState(listOf(Channel("v1", "voice", "Room", "")))
        for ((type, data, expected) in samples) {
            val actual = event(type, data)
            assertEquals(expected, actual)
            assertEquals(type, actual.type)
            assertSame(rooms, rooms.apply(actual))
        }
        val description = signal("offer", """{"type":"offer","sdp":"v=0\r\n"}""")
        assertEquals("call_signal", description.type)
        assertSame(rooms, rooms.apply(description))
    }

    @Test
    fun browserDescriptionsAndMissingType() {
        // direct.ts sends pc.localDescription; api.ts JSON.stringify calls its toJSON.
        // call.go handleCallSignal wraps the untouched {type,sdp} in data.payload.
        // SDP is abbreviated: this tests transport, not WebRTC SDP validation.
        for (kind in listOf("offer", "answer")) {
            assertEquals(CallSignalEvent(CallSignal.Description("call-1", Sdp(kind, "v=0\r\n"))),
                signal(kind, """{"type":"$kind","sdp":"v=0\r\n"}"""))
            assertEquals(CallSignalEvent(CallSignal.Description("call-1", Sdp(kind, "v=0\r\n"))),
                signal(kind, """{"sdp":"v=0\r\n"}"""))
        }
        // The payload type is authoritative when present, rather than the outer kind.
        assertEquals(CallSignalEvent(CallSignal.Description("call-1", Sdp("answer", "v=0\r\n"))),
            signal("offer", """{"type":"answer","sdp":"v=0\r\n"}"""))
    }

    @Test
    fun chromeAndFirefoxCandidateShapes() {
        // direct.ts onicecandidate -> api.ts JSON.stringify -> RTCIceCandidate.toJSON.
        // Representative Chrome/Firefox candidate strings with documentation IPs;
        // these are format fixtures, not captures from a live browser session.
        val chrome = "candidate:842163049 1 udp 1677729535 192.0.2.1 54400 typ srflx raddr 0.0.0.0 rport 0 generation 0 ufrag test network-cost 999"
        val chromePayload = """{"candidate":"$chrome","sdpMid":"0","sdpMLineIndex":0,"usernameFragment":"test"}"""
        assertEquals(CallSignalEvent(CallSignal.Ice("call-1", Candidate(chrome, "0", 0))), signal("ice", chromePayload))
        val firefox = "candidate:0 1 UDP 2122252543 192.0.2.2 54401 typ host"
        val firefoxPayload = """{"candidate":"$firefox","sdpMid":"1","sdpMLineIndex":1,"usernameFragment":null}"""
        assertEquals(CallSignalEvent(CallSignal.Ice("call-1", Candidate(firefox, "1", 1))), signal("ice", firefoxPayload))
    }

    @Test
    fun nullableAndMissingCandidateFields() {
        // RTCIceCandidateInit permits null mid/index; Android's index fallback is 0.
        for (fields in listOf("", ""","sdpMid":null,"sdpMLineIndex":null""")) {
            assertEquals(CallSignalEvent(CallSignal.Ice("call-1", Candidate("candidate:test", null, 0))),
                signal("ice", """{"candidate":"candidate:test"$fields}"""))
        }
        assertEquals(CallSignalEvent(CallSignal.Ice("call-1", Candidate("candidate:test", "2", 0))),
            signal("ice", """{"candidate":"candidate:test","sdpMid":"2","sdpMLineIndex":null}"""))
        assertEquals(CallSignalEvent(CallSignal.Ice("call-1", Candidate("candidate:test", null, 2))),
            signal("ice", """{"candidate":"candidate:test","sdpMid":null,"sdpMLineIndex":2}"""))
        // Empty candidate denotes end-of-candidates and must survive serialization.
        assertEquals(CallSignalEvent(CallSignal.Ice("call-1", Candidate("", "0", 0))),
            signal("ice", """{"candidate":"","sdpMid":"0","sdpMLineIndex":0}"""))
    }

    @Test
    fun malformedSignalsAreOther() {
        // Deliberately malformed variants of handleCallSignal's envelope.
        for (kind in listOf("offer", "answer", "ice", "unknown")) {
            for (payload in listOf("null", "[]", "42", "\"text\"")) {
                assertEquals(GatewayEvent.Other("call_signal"), signal(kind, payload))
            }
        }
        for (kind in listOf("offer", "answer")) {
            for (payload in listOf("{}", """{"sdp":""}""", """{"sdp":null}""", """{"sdp":42}""")) {
                assertEquals(GatewayEvent.Other("call_signal"), signal(kind, payload))
            }
        }
        assertEquals(GatewayEvent.Other("call_signal"), signal("unknown", "{}"))
        assertEquals(GatewayEvent.Other("call_signal"), event("call_signal", """{"id":"call-1","kind":"ice"}"""))
        for (type in listOf("call", "call_accepted", "call_taken", "call_ended", "call_signal", "call_state")) {
            assertEquals(GatewayEvent.Other(type), event(type, "null"))
        }
    }

    @Test
    fun startResponseWithTurn() {
        // call.go: callJSON + iceServers in handleCallStart (201, polite=false).
        val call = Json.call(JSONObject("""{"id":"call-1","from":$from,"to":$to,"polite":false,
            "ice_servers":[{"urls":["stun:stun.example.test:3478"]},
            {"urls":["turn:turn.example.test:3478?transport=udp","turn:turn.example.test:3478?transport=tcp"],
            "username":"2000000000:alice","credential":"test-only-credential"}]}"""))
        assertEquals("call-1", call.id)
        assertEquals(CallPerson("alice", "Alice"), call.from)
        assertEquals(CallPerson("bob", "Bob"), call.to)
        assertFalse(call.polite)
        assertFalse(call.answered)
        assertEquals(IceServer(listOf("stun:stun.example.test:3478"), null, null), call.iceServers[0])
        assertEquals(IceServer(listOf("turn:turn.example.test:3478?transport=udp", "turn:turn.example.test:3478?transport=tcp"),
            "2000000000:alice", "test-only-credential"), call.iceServers[1])
    }

    @Test
    fun getResponseAndAbsentIceServers() {
        // call.go handleCallGet calls callJSON with nil ICE: JSON null, not an array.
        for (answered in listOf(false, true)) {
            val call = Json.call(JSONObject("""{"id":"call-1","from":$from,"to":$to,
                "ice_servers":null,"polite":true,"answered":$answered}"""))
            assertEquals("call-1", call.id)
            assertEquals(CallPerson("alice", "Alice"), call.from)
            assertEquals(CallPerson("bob", "Bob"), call.to)
            assertTrue(call.polite)
            assertEquals(answered, call.answered)
            assertTrue(call.iceServers.isEmpty())
        }
        assertTrue(Json.call(JSONObject("""{"id":"call-1","from":$from,"to":$to,"polite":false}""")).iceServers.isEmpty())
    }

    @Test
    fun acceptResponseAndSingleIceUrlCompatibility() {
        // call.go handleCallAccept: same callJSON shape, polite=true, no answered.
        val body = JSONObject("""{"id":"call-1","from":$from,"to":$to,
            "ice_servers":[{"urls":["stun:stun.example.test:3478"]}],"polite":true}""")
        val call = Json.call(body)
        assertEquals(CallInfo("call-1", CallPerson("alice", "Alice"), CallPerson("bob", "Bob"),
            listOf(IceServer(listOf("stun:stun.example.test:3478"), null, null)), true), call)
        // WebRTC also accepts a string URL; preserve the existing voice parser support.
        body.getJSONArray("ice_servers").getJSONObject(0).put("urls", "stun:stun.example.test:3478")
        assertEquals(call, Json.call(body))
        assertEquals(call.iceServers, Json.voiceOffer(body).iceServers)
    }

    @Test
    fun descriptionsRoundTrip() {
        for (kind in listOf("offer", "answer")) {
            val sdp = Sdp(kind, "v=0\r\na=mid:0\r\n")
            val json = Json.sdpJson(sdp)
            assertEquals(setOf("type", "sdp"), json.keys().asSequence().toSet())
            assertEquals(kind, json.getString("type"))
            assertEquals(sdp.sdp, json.getString("sdp"))
            assertEquals(CallSignalEvent(CallSignal.Description("call-1", sdp)), signal(kind, json.toString()))
        }
    }

    @Test
    fun candidatesRoundTripWithJsonNull() {
        for (mid in listOf("2", null)) {
            val candidate = Candidate("candidate:0 1 UDP 2122252543 192.0.2.2 54401 typ host", mid, 2)
            val json = Json.candidateJson(candidate)
            assertEquals(setOf("candidate", "sdpMid", "sdpMLineIndex"), json.keys().asSequence().toSet())
            assertEquals(candidate.candidate, json.getString("candidate"))
            assertEquals(2, json.getInt("sdpMLineIndex"))
            if (mid == null) assertSame(JSONObject.NULL, json.get("sdpMid")) else assertEquals(mid, json.getString("sdpMid"))
            assertEquals(CallSignalEvent(CallSignal.Ice("call-1", candidate)), signal("ice", json.toString()))
        }
    }

    @Test
    fun callPushWakesWithoutAChannel() {
        // notify.go sendFCM: only k, call and at for notifyCall (all strings).
        assertEquals(Wake(Wake.CALL, "", 0L, "call-1", 1727200000000L),
            Wake.parse(mapOf("k" to "call", "call" to "call-1", "at" to "1727200000000")))
    }

    @Test
    fun personNameFallsBackForBlankDisplayName() {
        assertEquals("Alice", CallPerson("alice", "Alice").name)
        assertEquals("alice", CallPerson("alice", "").name)
        assertEquals("alice", CallPerson("alice", "  ").name)
    }

    @Test
    fun apiRoutesBodiesAndEscapedIds() {
        // Reuse ChatJsonTest's OkHttp interceptor approach, without network or new dependencies.
        val requests = mutableListOf<Request>()
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val req = chain.request().also { requests.add(it) }
            val path = req.url.encodedPath
            val code = when {
                path.endsWith("/signal") || path.endsWith("/state") -> 204
                path == "/api/calls" -> 201
                else -> 200
            }
            // call.go: real response shapes for all six routes.
            val body = when {
                code == 204 -> ""
                path.endsWith("/hangup") -> """{"ended":true}"""
                else -> """{"id":"call-1","from":$from,"to":$to,"ice_servers":null,"polite":true}"""
            }
            Response.Builder().request(req).protocol(Protocol.HTTP_1_1).code(code).message("test")
                .body(body.toResponseBody()).build()
        }.build()
        val api = Api(client, "https://example.test", { "test-session" }, "test-agent")
        fun body() = JSONObject(Buffer().also { requests.last().body!!.writeTo(it) }.readUtf8())
        assertEquals("call-1", api.callStart("bob").id)
        assertEquals("/api/calls", requests.last().url.encodedPath)
        assertEquals("bob", body().getString("username"))
        val id = "call/with?query#fragment%"
        val path = "/api/calls/call%2Fwith%3Fquery%23fragment%25"
        assertEquals("call-1", api.callGet(id).id)
        assertEquals("GET", requests.last().method)
        assertEquals(path, requests.last().url.encodedPath)
        assertTrue(api.callAccept(id).polite)
        assertEquals("$path/accept", requests.last().url.encodedPath)
        for (reason in listOf("hangup", "rejected", "missed")) {
            api.callHangup(id, reason)
            assertEquals("$path/hangup", requests.last().url.encodedPath)
            assertEquals(reason, body().getString("reason"))
        }
        for (kind in listOf("offer", "answer")) {
            api.callDescription(id, Sdp(kind, "v=0\r\n"))
            assertEquals("$path/signal", requests.last().url.encodedPath)
            assertEquals(kind, body().getString("kind"))
            assertEquals(kind, body().getJSONObject("payload").getString("type"))
            assertEquals("v=0\r\n", body().getJSONObject("payload").getString("sdp"))
        }
        api.callCandidate(id, Candidate("candidate:test", null, 2))
        assertEquals("$path/signal", requests.last().url.encodedPath)
        assertEquals("ice", body().getString("kind"))
        assertEquals("candidate:test", body().getJSONObject("payload").getString("candidate"))
        assertSame(JSONObject.NULL, body().getJSONObject("payload").get("sdpMid"))
        assertEquals(2, body().getJSONObject("payload").getInt("sdpMLineIndex"))
        api.callState(id, true, false, true)
        assertEquals("$path/state", requests.last().url.encodedPath)
        assertTrue(body().getBoolean("mic"))
        assertFalse(body().getBoolean("cam"))
        assertTrue(body().getBoolean("screen"))
        for ((index, req) in requests.withIndex()) {
            assertEquals(if (index == 1) "GET" else "POST", req.method)
            assertNull(req.url.query)
            assertNull(req.url.fragment)
            assertEquals("harmony_session=test-session", req.header("Cookie"))
            assertEquals("test-agent", req.header("User-Agent"))
        }
    }

    @Test
    fun apiPreservesCallErrors() {
        var code = 404
        var reason = "not_found"
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1).code(code).message("test")
                .body("""{"error":"$reason"}""".toResponseBody()).build()
        }.build()
        val api = Api(client, "https://example.test", { null }, "test")
        // call.go: callError, handleCallStart and handleCallAccept.
        val attempts: List<Triple<Int, String, () -> Unit>> = listOf(
            Triple(404, "not_found") { api.callGet("missing") },
            Triple(409, "busy") { api.callStart("bob") },
            Triple(409, "taken") { api.callAccept("call-1") },
            Triple(409, "not_answered") { api.callDescription("call-1", Sdp("offer", "v=0\r\n")) },
            Triple(429, "ring_limit") { api.callStart("bob") },
        )
        for ((status, error, attempt) in attempts) {
            code = status
            reason = error
            val actual = assertThrows(ApiError::class.java) { attempt() }
            assertEquals(status, actual.status)
            assertEquals(error, actual.reason)
        }
        code = 401
        assertThrows(Unauthorized::class.java) { api.callGet("call-1") }
    }
}
