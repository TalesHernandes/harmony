package com.taleshernandes.harmony

import com.taleshernandes.harmony.model.*
import com.taleshernandes.harmony.net.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ChannelLayoutTest {
    private fun channel(id: String, kind: String = "text", folder: String = "", pos: Int = 0) =
        Channel(id, kind, id, "", categoryId = folder, position = pos)

    @Test fun snapshotAndLayoutBuildSectionsAndPreserveLocalUnread() {
        val snapshot = Json.channelSnapshot("""{"can_manage":true,"categories":[{"id":"f","name":"Jogos","position":2}],"channels":[{"id":"t","kind":"text","position":0,"unread":3},{"id":"v","kind":"voice","category_id":"f","position":1}]}""")
        val initial = RoomsState(snapshot.channels, categories = snapshot.categories, canManage = snapshot.canManage)
        val layout = Json.event("""{"type":"layout","data":{"categories":[{"id":"f","name":"Jogos","position":0}],"channels":[{"id":"v","category_id":"f","position":0},{"id":"t","category_id":"f","position":1}]}}""")!!
        val state = initial.apply(layout)
        assertTrue(state.canManage)
        assertEquals(listOf("t", "v"), state.sections(emptySet(), null, null)[1].channels.map { it.id })
        assertEquals(3, state.channels.first { it.id == "t" }.unread)
        assertTrue(state.sections(setOf("f"), null, null)[1].channels.isEmpty())
        assertEquals(listOf("t"), state.sections(setOf("f"), "t", null)[1].channels.map { it.id })
        val busy = state.copy(peers = mapOf("v" to listOf(Peer("s", "u", "User", true, true, 0, emptyList()))))
        assertEquals(listOf("t", "v"), busy.sections(setOf("f"), "t", null)[1].channels.map { it.id })
        assertEquals(listOf("v"), state.sections(setOf("f"), null, "v")[1].channels.map { it.id })
    }

    @Test fun dragAndMoveSendEveryChannelOnceAndExcludePrivateConversations() {
        val state = RoomsState(listOf(channel("a"), channel("b"), channel("v", "voice", "f"), channel("dm", "dm")),
            categories = listOf(Category("f", "First"), Category("g", "Second", 1)))
        val draft = ChannelLayout.from(state).reorder(0, 1, false).reorder(0, 1, true).moveChannel("b", "g")
        val body = draft.body()
        assertEquals(listOf("g", "f"), (0..1).map { body.getJSONArray("categories").getString(it) })
        val channels = body.getJSONArray("channels")
        assertEquals(listOf("a", "v", "b"), (0..2).map { channels.getJSONObject(it).getString("id") })
        assertTrue(channels.getJSONObject(0).isNull("category_id"))
        assertEquals("g", channels.getJSONObject(2).getString("category_id"))
        assertEquals(3, channels.length())
    }

    @Test fun permissionComesFromSnapshotAndPrivateParticipantsCanPin() {
        assertFalse(Json.channelSnapshot("""{"channels":[],"categories":[]}""").canManage)
        assertTrue(Json.channelSnapshot("""{"can_manage":true}""").canManage)
        assertFalse(canPin(false, channel("t")))
        assertTrue(canPin(true, channel("t")))
        assertTrue(canPin(false, channel("dm", "dm")))
        assertTrue(Json.message(JSONObject("""{"id":1,"pinned":true}""")).pinned)
        assertFalse(Json.message(JSONObject("""{"id":1}""")).pinned)
    }
}
