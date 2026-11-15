package com.taleshernandes.harmony.model

data class Author(val username: String, val displayName: String)
data class Attachment(val id: String, val name: String, val mime: String, val size: Long,
    val width: Int, val height: Int, val url: String, val missing: Boolean) {
    val isImage: Boolean get() = mime.startsWith("image/")
}
data class Reaction(val emoji: String, val users: List<String>)
/** The card the server made from a link (api/preview.go); "" when unknown. */
data class Embed(val url: String, val kind: String, val site: String, val title: String,
    val description: String, val image: String, val width: Int, val height: Int)
data class ReplyRef(val id: Long, val author: Author, val body: String, val deleted: Boolean)
data class Message(val id: Long, val channelId: String, val author: Author, val body: String,
    val createdAt: Long, val editedAt: Long?, val deleted: Boolean, val kind: String,
    val attachments: List<Attachment>, val mentions: List<String>, val reactions: List<Reaction>,
    val replyTo: ReplyRef?, val embeds: List<Embed> = emptyList(), val pinned: Boolean = false)
data class HistoryPage(val messages: List<Message>, val hasMore: Boolean, val hasNewer: Boolean = false)
data class Member(val username: String, val displayName: String, val isAdmin: Boolean, val nickname: String?)

/** One channel. While empty, the caller must route only this channel's events here. */
data class Timeline(
    val messages: List<Message> = emptyList(),
    val hasMore: Boolean = true,
    val loaded: Boolean = false,
    /** The retained window stops before the live end; never append across this gap. */
    val hasNewer: Boolean = false,
    /** Only an id is kept for unseen socket messages; their content comes from history. */
    val newestKnownId: Long = 0,
) {
    val oldestId: Long? get() = messages.minOfOrNull { it.id }
    val newestId: Long? get() = messages.maxOfOrNull { it.id }

    fun withLatest(page: HistoryPage): Timeline {
        val first = page.messages.minOfOrNull { it.id }
        val last = newestId
        val overlaps = first != null && last != null && first <= last
        // hasMore describes the oldest retained boundary, not the newest page.
        val more = if (overlaps && loaded && oldestId!! < first) hasMore else page.hasMore
        return Timeline(ordered(if (overlaps) messages + page.messages else page.messages), more, true)
    }

    fun withOlder(page: HistoryPage): Timeline =
        copy(messages = ordered(page.messages + messages), hasMore = page.hasMore, loaded = true)

    fun withAround(page: HistoryPage): Timeline =
        Timeline(ordered(page.messages), page.hasMore, true, page.hasNewer)

    fun withNewer(page: HistoryPage): Timeline {
        val combined = ordered(page.messages + messages)
        val end = combined.lastOrNull()?.id ?: 0
        return copy(messages = combined, hasNewer = page.hasNewer || newestKnownId > end,
            newestKnownId = if (newestKnownId > end) newestKnownId else 0)
    }

    fun withSent(msg: Message): Timeline {
        if (!accepts(msg)) return this
        if (hasNewer && messages.none { it.id == msg.id }) {
            return copy(newestKnownId = maxOf(newestKnownId, msg.id))
        }
        return copy(messages = ordered(messages + msg))
    }

    fun apply(ev: GatewayEvent): Timeline = when (ev) {
        is GatewayEvent.MessageNew ->
            if (messages.any { it.id == ev.message.id }) this else withSent(ev.message)
        is GatewayEvent.MessageChanged ->
            if (accepts(ev.message) && messages.any { it.id == ev.message.id }) withSent(ev.message) else this
        else -> this
    }

    private fun accepts(msg: Message) = messages.firstOrNull()?.channelId?.let { it == msg.channelId } ?: true
    private fun ordered(items: List<Message>) = items.associateBy { it.id }.values.sortedBy { it.id }
}

/** Clock supplied by the caller; entries expire at the web's five-second boundary. */
class Typing(private val ttlMs: Long = 5000) {
    private data class Entry(val name: String, val until: Long)
    private val entries = linkedMapOf<Pair<String, String>, Entry>()

    fun saw(channelId: String, username: String, displayName: String, now: Long) {
        entries[channelId to username] = Entry(displayName.ifEmpty { username }, now + ttlMs)
    }

    fun stopped(channelId: String, username: String) {
        entries.remove(channelId to username)
    }

    fun names(channelId: String, now: Long): List<String> {
        entries.entries.removeAll { it.value.until <= now }
        return entries.filterKeys { it.first == channelId }.values.map { it.name }
    }
}

class TypingSender(private val everyMs: Long = 3000) {
    private val last = mutableMapOf<String, Long>()

    /** A true result reserves this attempt, even if HTTP fails, just like Composer. */
    fun shouldSend(channelId: String, now: Long): Boolean {
        val previous = last[channelId]
        if (previous != null && now - previous <= everyMs) return false
        last[channelId] = now
        return true
    }

    fun reset(channelId: String) {
        last.remove(channelId)
    }
}
