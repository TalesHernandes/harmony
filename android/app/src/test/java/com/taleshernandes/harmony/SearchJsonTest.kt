package com.taleshernandes.harmony

import com.taleshernandes.harmony.net.searchHits
import org.junit.Assert.*
import org.junit.Test

class SearchJsonTest {
    @Test fun resultsCarryUnloadedChannelsAndKeepTheServerOrder() {
        val hits = searchHits("""{"messages":[
            {"id":12,"channel_id":"old-dm","body":"old word","author":{"username":"ana","display_name":"Ana"},
             "created_at":"2026-10-01T12:00:00Z","channel":{"id":"old-dm","kind":"dm","name":"","peer":{"username":"ana","display_name":"Ana"}}},
            {"id":3,"channel_id":"text","body":"word","author":{"username":"bruno"},
             "created_at":"2026-09-01T12:00:00Z","channel":{"id":"text","kind":"text","name":"geral"}}
        ]}""")
        assertEquals(listOf(12L, 3L), hits.map { it.message.id })
        assertEquals("old-dm", hits[0].channel?.id)
        assertTrue(requireNotNull(hits[0].channel).isDm)
        assertEquals("Ana", hits[0].channel?.peer?.displayName)
        assertEquals("geral", hits[1].channel?.name)
    }

    @Test fun emptyResultsAndMissingChannelAreReadable() {
        assertTrue(searchHits("""{"messages":[]}""").isEmpty())
        assertNull(searchHits("""{"messages":[{"id":1,"channel_id":"gone","author":{},"body":"word"}]}""").single().channel)
    }
}
