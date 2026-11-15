package com.taleshernandes.harmony

import com.taleshernandes.harmony.model.*
import org.junit.Assert.*
import org.junit.Test

class ChatStateTest {
    private fun msg(id: Long, channel: String = "c", who: String = "sol") = Message(
        id, channel, Author(who, who), "message $id", 0, null, false, "",
        emptyList(), emptyList(), emptyList(), null,
    )
    private fun page(vararg ids: Long, more: Boolean = true) = HistoryPage(ids.map { msg(it) }, more)
    private val channel = Channel("c", "text", "general", "")
    private fun RoomsState.c() = channels.first { it.id == "c" }

    @Test fun latestMergesAtTheBoundaryAndPageVersionsWin() {
        val base = Timeline().withLatest(page(1, 2, 3, more = false))
        val edit = msg(3).copy(body = "edited", editedAt = 12)
        val next = base.withLatest(HistoryPage(listOf(msg(4), edit), true))
        assertEquals(listOf(1L, 2L, 3L, 4L), next.messages.map { it.id })
        assertEquals(edit, next.messages[2])
        assertFalse(next.hasMore)
        assertTrue(next.loaded)
        assertEquals(1L, next.oldestId)
        assertEquals(4L, next.newestId)
    }

    @Test fun latestWithAGapReplacesEverything() {
        val old = Timeline().withLatest(page(1, 2, more = false))
        val next = old.withLatest(page(5, 6))
        assertEquals(listOf(5L, 6L), next.messages.map { it.id })
        assertTrue(next.hasMore)
        assertEquals(Timeline(emptyList(), false, true), next.withLatest(page(more = false)))
        assertNull(Timeline().oldestId)
        assertNull(Timeline().newestId)
    }

    @Test fun latestUsesPageBoundaryWhenItExtendsBackwards() {
        val next = Timeline().withLatest(page(3, 4)).withLatest(page(1, 2, 3, 4, 5, more = false))
        assertFalse(next.hasMore)
        assertEquals(1L, next.oldestId)
    }

    @Test fun olderDeduplicatesAndKeepsLiveChanges() {
        val live = msg(3).copy(body = "new text")
        val next = Timeline(listOf(live, msg(4)), loaded = true).withOlder(page(3, 2, 1, 2, more = false))
        assertEquals(listOf(1L, 2L, 3L, 4L), next.messages.map { it.id })
        assertEquals(live, next.messages[2])
        assertFalse(next.hasMore)
    }

    @Test fun eventsInsertOnceAndChangesOnlyReplaceLoadedMessages() {
        val base = Timeline().withLatest(page(2, 4))
        val ev = GatewayEvent.MessageNew(msg(3))
        val next = base.apply(ev)
        assertEquals(listOf(2L, 3L, 4L), next.messages.map { it.id })
        assertEquals(next, next.apply(ev))
        val deleted = msg(3).copy(body = "", deleted = true)
        assertEquals(deleted, next.apply(GatewayEvent.MessageChanged(deleted)).messages[1])
        assertEquals(next, next.apply(GatewayEvent.MessageChanged(msg(1))))
        assertEquals(next, next.apply(GatewayEvent.MessageNew(msg(5, "other"))))
        assertEquals(next, next.apply(GatewayEvent.MessageChanged(msg(3, "other"))))
        assertEquals(next, next.apply(GatewayEvent.Read("c", 4)))
    }

    @Test fun postAndSocketCanArriveInEitherOrder() {
        val base = Timeline().withLatest(page(1))
        val sent = msg(2)
        val event = GatewayEvent.MessageNew(sent)
        assertEquals(base.withSent(sent), base.apply(event).withSent(sent))
        assertEquals(base.withSent(sent), base.withSent(sent).apply(event))
        assertEquals("canonical", base.apply(event).withSent(sent.copy(body = "canonical")).messages.last().body)
    }

    @Test fun aroundReplacesTheWindowInsteadOfJoiningDisconnectedHistory() {
        val recent = Timeline().withLatest(page(80, 90))
        val window = recent.withAround(HistoryPage(listOf(msg(20), msg(10), msg(30)), true, true))
        assertEquals(listOf(10L, 20L, 30L), window.messages.map { it.id })
        assertTrue(window.loaded && window.hasMore && window.hasNewer)
        assertEquals(0L, window.newestKnownId)
    }

    @Test fun socketAndPostDoNotAppendAcrossAGapButLoadedChangesStillApply() {
        val window = Timeline().withAround(HistoryPage(listOf(msg(10), msg(20)), true, true))
        val live = window.apply(GatewayEvent.MessageNew(msg(90)))
        assertEquals(window.messages, live.messages)
        assertEquals(90L, live.newestKnownId)
        assertEquals(live, live.apply(GatewayEvent.MessageNew(msg(80))))
        assertEquals(live, live.withSent(msg(90)))
        assertEquals(live, live.apply(GatewayEvent.MessageNew(msg(100, "other"))))
        val edited = msg(20).copy(body = "edited while reading history")
        assertEquals(edited, live.apply(GatewayEvent.MessageChanged(edited)).messages.last())
        assertEquals(live, live.apply(GatewayEvent.MessageChanged(msg(100))))
    }

    @Test fun olderPagesPreserveTheGapAndTheKnownLiveEnd() {
        val window = Timeline().withAround(HistoryPage(listOf(msg(20), msg(30)), true, true))
            .apply(GatewayEvent.MessageNew(msg(90)))
        val older = window.withOlder(page(10, 20, more = false))
        assertEquals(listOf(10L, 20L, 30L), older.messages.map { it.id })
        assertFalse(older.hasMore)
        assertTrue(older.hasNewer)
        assertEquals(90L, older.newestKnownId)
    }

    @Test fun newerPagesCloseTheGapOnlyWhenTheKnownLiveEndIsReached() {
        val window = Timeline().withAround(HistoryPage(listOf(msg(10), msg(20)), false, true))
            .apply(GatewayEvent.MessageNew(msg(90)))
        // The response was captured before the socket's message 90 existed.
        val stillBehind = window.withNewer(HistoryPage(listOf(msg(30), msg(40)), true, false))
        assertEquals(listOf(10L, 20L, 30L, 40L), stillBehind.messages.map { it.id })
        assertTrue(stillBehind.hasNewer)
        assertFalse(stillBehind.hasMore)
        val caughtUp = stillBehind.withNewer(HistoryPage(listOf(msg(90)), true, false))
        assertFalse(caughtUp.hasNewer)
        assertEquals(0L, caughtUp.newestKnownId)
        assertEquals(100L, caughtUp.apply(GatewayEvent.MessageNew(msg(100))).newestId)
    }

    @Test fun newerPageDeduplicatesWithoutUndoingLoadedEditsAndReactions() {
        val changed = msg(20).copy(reactions = listOf(Reaction("👍", listOf("tales"))))
        val window = Timeline().withAround(HistoryPage(listOf(msg(10), changed), true, true))
        val next = window.withNewer(HistoryPage(listOf(msg(20), msg(30), msg(30)), false, true))
        assertEquals(listOf(10L, 20L, 30L), next.messages.map { it.id })
        assertEquals(changed, next.messages[1])
        assertTrue(next.hasMore && next.hasNewer)
    }

    @Test fun emptyNewerResponseAndLateSocketEventKeepTheGap() {
        val window = Timeline().withAround(HistoryPage(listOf(msg(10)), false, true))
            .apply(GatewayEvent.MessageNew(msg(30)))
        val next = window.withNewer(HistoryPage(emptyList(), false, false))
        assertTrue(next.hasNewer)
        assertEquals(listOf(10L), next.messages.map { it.id })
        assertFalse(next.withNewer(HistoryPage(listOf(msg(30)), false)).hasNewer)
    }

    @Test fun returningToLatestDropsTheGapAndMergesOnlyAnOverlappingWindow() {
        val window = Timeline().withAround(HistoryPage(listOf(msg(10), msg(20)), true, true))
            .apply(GatewayEvent.MessageNew(msg(90)))
        val recent = window.withLatest(page(80, 90))
        assertEquals(listOf(80L, 90L), recent.messages.map { it.id })
        assertFalse(recent.hasNewer)
        assertEquals(0L, recent.newestKnownId)
        val overlap = window.withLatest(page(20, 30))
        assertEquals(listOf(10L, 20L, 30L), overlap.messages.map { it.id })
        assertFalse(overlap.hasNewer)
    }

    @Test fun unreadMatchesWebIncludingMutedOpenAndOwnMessages() {
        val base = RoomsState(listOf(channel.copy(lastReadId = 1)))
        val event = GatewayEvent.MessageNew(msg(2).copy(mentions = listOf("tales")))
        val c = base.apply(event, "tales", null).c()
        assertEquals(1, c.unread)
        assertEquals(1, c.mentions)
        assertEquals(2L, c.lastMessageId)
        for (quiet in listOf(base.apply(event, "tales", "c"),
            RoomsState(listOf(channel.copy(muted = true))).apply(event, "tales", null),
            base.apply(GatewayEvent.MessageNew(msg(2, who = "tales")), "tales", null))) {
            assertEquals(0, quiet.c().unread)
            assertEquals(0, quiet.c().mentions)
            assertEquals(2L, quiet.c().lastMessageId)
        }
        assertEquals(1L, base.apply(GatewayEvent.MessageNew(msg(2, who = "tales")), "tales", null).c().lastReadId)
        assertEquals(1, base.apply(event).c().unread)
        assertEquals(0, base.apply(event).c().mentions)
        assertEquals(0, base.apply(GatewayEvent.MessageNew(msg(2)), "tales", null).c().mentions)
    }

    @Test fun explicitSelfMentionMatchesWebAndDmsDoNotInventMentions() {
        val own = GatewayEvent.MessageNew(msg(2, who = "tales").copy(mentions = listOf("tales")))
        val c = RoomsState(listOf(channel)).apply(own, "tales", null).c()
        assertEquals(0, c.unread)
        assertEquals(1, c.mentions)
        val dm = RoomsState(listOf(channel.copy(kind = "dm")))
            .apply(GatewayEvent.MessageNew(msg(2)), "tales", null).c()
        assertEquals(1, dm.unread)
        assertEquals(0, dm.mentions)
    }

    @Test fun readAlwaysClearsAndUpsertPreservesCountersOnly() {
        val base = RoomsState(listOf(channel.copy(unread = 3, mentions = 2, lastMessageId = 10)))
        val read = base.apply(GatewayEvent.Read("c", 5)).c()
        assertEquals(0, read.unread)
        assertEquals(0, read.mentions)
        assertEquals(5L, read.lastReadId)
        assertEquals(base.apply(GatewayEvent.Read("c", 5)), base.markRead("c", 5))
        val updated = base.apply(GatewayEvent.ChannelUpsert(channel.copy(name = "renamed", muted = true))).c()
        assertEquals(3, updated.unread)
        assertEquals(2, updated.mentions)
        assertTrue(updated.muted)
        assertEquals("renamed", updated.name)
        assertEquals(0L, updated.lastMessageId)
    }

    @Test fun channelOrderMatchesWebWithEmptyDmsFirst() {
        val s = RoomsState(listOf(channel, channel.copy(id = "d1", kind = "dm", lastMessageId = 7),
            channel.copy(id = "t2"), channel.copy(id = "d2", kind = "dm", lastMessageId = 12),
            channel.copy(id = "empty", kind = "dm"), channel.copy(id = "voice", kind = "voice")))
        assertEquals(listOf("c", "t2"), s.textChannels.map { it.id })
        assertEquals(listOf("empty", "d2", "d1"), s.dms.map { it.id })
    }

    @Test fun typingExpiresRefreshesAndStopsOnMessage() {
        val t = Typing()
        t.saw("c", "sol", "Sol", 0)
        t.saw("other", "sol", "Other", 0)
        t.saw("c", "tales", "", 1000)
        assertEquals(listOf("Sol", "tales"), t.names("c", 4999))
        assertEquals(listOf("tales"), t.names("c", 5000))
        t.saw("c", "tales", "Tales", 5500)
        assertEquals(listOf("Tales"), t.names("c", 6000))
        val event = GatewayEvent.MessageNew(msg(4, who = "tales"))
        t.stopped(event.message.channelId, event.message.author.username)
        assertTrue(t.names("c", 6000).isEmpty())
        assertTrue(t.names("other", 6000).isEmpty())
    }

    @Test fun typingSenderReservesPerChannelAndResetAllowsNextKey() {
        val t = TypingSender()
        assertTrue(t.shouldSend("c", 0))
        assertFalse(t.shouldSend("c", 3000))
        assertTrue(t.shouldSend("c", 3001))
        assertTrue(t.shouldSend("other", 3001))
        assertFalse(t.shouldSend("c", 3002))
        t.reset("c")
        assertTrue(t.shouldSend("c", 3002))
        assertFalse(t.shouldSend("other", 3002))
    }
}
