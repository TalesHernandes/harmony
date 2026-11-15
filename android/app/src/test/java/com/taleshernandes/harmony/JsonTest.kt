package com.taleshernandes.harmony

import com.taleshernandes.harmony.model.GatewayEvent
import com.taleshernandes.harmony.net.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

// The bodies below are copied from what the Go server writes (chat.go
// channelJSON, voice.go handleVoiceRooms and voiceRoomChanged, sfu.Peer), so a
// field renamed there shows up here as a failure rather than as an empty row.
class JsonTest {
    private val peerJson = """{"session_id":"s1","username":"tales","display_name":"Tales",
        "muted":true,"deafened":false,"joined_at":1727200000,"publishing":["mic","screen"]}"""

    @Test
    fun channels() {
        val body = """{"channels":[
            {"id":"c1","kind":"text","name":"geral","topic":"","muted":false,"unread":0,"mentions":0,"last_message_id":5,"last_read_id":5},
            {"id":"v1","kind":"voice","name":"Sala 1","topic":"t","muted":false,"unread":0,"mentions":0,"last_message_id":0,"last_read_id":0},
            {"id":"d1","kind":"dm","name":"","topic":"","peer":{"username":"sol","display_name":"Sol"}}
        ],"can_manage":true}"""
        val list = Json.channels(body)
        assertEquals(listOf("c1", "v1", "d1"), list.map { it.id })
        assertEquals(listOf(false, true, false), list.map { it.isVoice })
        assertEquals("Sala 1", list[1].name)
    }

    @Test
    fun roomsWithAnEmptyRoom() {
        val rooms = Json.rooms("""{"rooms":{"v1":[$peerJson],"v2":[]}}""")
        assertEquals(setOf("v1", "v2"), rooms.keys)
        assertEquals(emptyList<Any>(), rooms["v2"])
        val p = rooms.getValue("v1").single()
        assertEquals("s1", p.sessionId)
        assertEquals("Tales", p.displayName)
        assertTrue(p.muted)
        assertTrue(p.sharingScreen)
        assertEquals(1727200000L, p.joinedAt)
    }

    @Test
    fun voiceRoomEvent() {
        val ev = Json.event("""{"type":"voice_room","data":{"channel_id":"v1","peers":[$peerJson]}}""")
        ev as GatewayEvent.VoiceRoom
        assertEquals("v1", ev.channelId)
        assertEquals("tales", ev.peers.single().username)
    }

    @Test
    fun emptyRoomEventIsAnEmptyList() {
        // Go marshals an empty []Peer as [] and a nil one as null; both mean nobody.
        for (peers in listOf("[]", "null")) {
            val ev = Json.event("""{"type":"voice_room","data":{"channel_id":"v1","peers":$peers}}""")
            assertEquals(GatewayEvent.VoiceRoom("v1", emptyList()), ev)
        }
    }

    @Test
    fun channelEvents() {
        assertEquals(
            GatewayEvent.ChannelRemoved("v9"),
            Json.event("""{"type":"channel_removed","data":{"id":"v9"}}"""),
        )
        val up = Json.event("""{"type":"channel","data":{"id":"v3","kind":"voice","name":"Nova"}}""")
        assertEquals("Nova", (up as GatewayEvent.ChannelUpsert).channel.name)
        assertEquals(
            GatewayEvent.Presence("sol", "away"),
            Json.event("""{"type":"presence","data":{"username":"sol","state":"away"}}"""),
        )
    }

    @Test
    fun unknownAndBrokenMessages() {
        assertEquals(GatewayEvent.Other("message"), Json.event("""{"type":"message","data":{"id":1}}"""))
        // A known type with a payload that is not an object still is not a crash.
        assertEquals(GatewayEvent.Other("voice_room"), Json.event("""{"type":"voice_room","data":3}"""))
        assertNull(Json.event("not json"))
        assertNull(Json.event("""{"data":{}}"""))
    }

    @Test
    fun errors() {
        assertEquals("bad_credentials", Json.error("""{"error":"bad_credentials"}"""))
        assertNull(Json.error("<html>502</html>"))
        assertNull(Json.error("""{"ok":true}"""))
    }

    @Test
    fun user() {
        val u = Json.userOf("""{"user":{"username":"tales","display_name":"Tales","is_admin":true,"created_at":"x"}}""")
        assertEquals("tales", u.username)
        assertTrue(u.isAdmin)
    }
}
