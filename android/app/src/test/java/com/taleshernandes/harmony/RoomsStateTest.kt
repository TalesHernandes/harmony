package com.taleshernandes.harmony

import com.taleshernandes.harmony.model.Channel
import com.taleshernandes.harmony.model.GatewayEvent
import com.taleshernandes.harmony.model.Peer
import com.taleshernandes.harmony.model.RoomsState
import org.junit.Assert.assertEquals
import org.junit.Test

class RoomsStateTest {
    private val text = Channel("c1", "text", "geral", "")
    private val v1 = Channel("v1", "voice", "Sala 1", "")
    private val v2 = Channel("v2", "voice", "Sala 2", "")
    private fun peer(name: String, session: String = "s-$name") =
        Peer(session, name, name, muted = false, deafened = false, joinedAt = 0, publishing = emptyList())

    private val base = RoomsState().resync(listOf(text, v1, v2), mapOf("v1" to listOf(peer("tales"))))

    @Test
    fun onlyVoiceChannelsAreRoomsInServerOrder() {
        assertEquals(listOf("v1", "v2"), base.voiceRooms.map { it.channel.id })
        assertEquals(listOf("tales"), base.voiceRooms[0].peers.map { it.username })
        assertEquals(emptyList<Peer>(), base.voiceRooms[1].peers)
    }

    @Test
    fun voiceRoomReplacesTheWholeRoom() {
        val s = base.apply(GatewayEvent.VoiceRoom("v1", listOf(peer("sol"))))
        assertEquals(listOf("sol"), s.voiceRooms[0].peers.map { it.username })
        // Somebody leaving the last seat is an empty list, not a missing room.
        val empty = s.apply(GatewayEvent.VoiceRoom("v1", emptyList()))
        assertEquals(emptyList<Peer>(), empty.voiceRooms[0].peers)
        assertEquals(2, empty.voiceRooms.size)
    }

    @Test
    fun theSamePersonOnTwoDevicesIsTwoPeers() {
        val s = base.apply(GatewayEvent.VoiceRoom("v2", listOf(peer("sol", "a"), peer("sol", "b"))))
        assertEquals(listOf("a", "b"), s.voiceRooms[1].peers.map { it.sessionId })
    }

    @Test
    fun applyingTheSameEventTwiceChangesNothing() {
        // The resync replays events that arrived while it was fetching; that
        // replay must be harmless when the fetch already included them.
        val ev = GatewayEvent.VoiceRoom("v2", listOf(peer("sol")))
        assertEquals(base.apply(ev), base.apply(ev).apply(ev))
    }

    @Test
    fun channelCreatedRenamedAndRemoved() {
        val v3 = Channel("v3", "voice", "Sala 3", "")
        val added = base.apply(GatewayEvent.ChannelUpsert(v3))
        assertEquals(listOf("v1", "v2", "v3"), added.voiceRooms.map { it.channel.id })

        val renamed = added.apply(GatewayEvent.ChannelUpsert(v1.copy(name = "Jogos")))
        assertEquals("Jogos", renamed.voiceRooms[0].channel.name)
        assertEquals(listOf("v1", "v2", "v3"), renamed.voiceRooms.map { it.channel.id })

        val removed = renamed.apply(GatewayEvent.ChannelRemoved("v1"))
        assertEquals(listOf("v2", "v3"), removed.voiceRooms.map { it.channel.id })
        assertEquals(null, removed.peers["v1"])
    }

    @Test
    fun resyncReplacesEverything() {
        val s = base.apply(GatewayEvent.VoiceRoom("v2", listOf(peer("sol"))))
            .resync(listOf(v2), emptyMap())
        assertEquals(listOf("v2"), s.voiceRooms.map { it.channel.id })
        assertEquals(emptyList<Peer>(), s.voiceRooms[0].peers)
    }

    @Test
    fun otherEventsLeaveTheListAlone() {
        assertEquals(base, base.apply(GatewayEvent.Other("message")))
        assertEquals(base, base.apply(GatewayEvent.Presence("sol", "online")))
    }
}
