package com.taleshernandes.harmony.push

import com.taleshernandes.harmony.model.Channel
import com.taleshernandes.harmony.model.Message
import org.json.JSONObject

/**
 * What a Firebase message carries: numbers, and nothing that anybody wrote
 * (docs/PLANO-NATIVO.md §5, PROTOCOL-HARMONY §6). Google sees "message 812 in
 * channel X"; the text is fetched from our server, with this phone's session,
 * when the message arrives.
 */
data class Wake(
    val kind: String, val channelId: String, val messageId: Long, val callId: String, val at: Long,
    /** [WATCH] only: which way the server went, [DOWN] or [UP]. */
    val state: String = "",
) {
    companion object {
        const val DM = "dm"
        const val MENTION = "mention"
        const val RING = "ring"
        const val CALL = "call"
        /** The watchman on the other machine (cmd/harmony/watch.go), not our server. */
        const val WATCH = "watch"
        const val DOWN = "down"
        const val UP = "up"

        fun parse(data: Map<String, String>): Wake? {
            val kind = data["k"] ?: return null
            val channel = data["c"].orEmpty()
            val message = data["m"]?.toLongOrNull() ?: 0L
            when (kind) {
                DM, MENTION -> if (channel.isEmpty() || message <= 0) return null
                RING -> if (channel.isEmpty()) return null
                CALL -> if (data["call"].isNullOrEmpty()) return null
                WATCH -> if (data["s"] != DOWN && data["s"] != UP) return null
                else -> return null
            }
            return Wake(kind, channel, message, data["call"].orEmpty(), data["at"]?.toLongOrNull() ?: 0L, data["s"].orEmpty())
        }
    }
}

/**
 * One notification to draw. [sender] empty means the text could not be
 * fetched, and [text] is the generic line instead.
 */
data class Notice(
    val kind: String,
    val channelId: String,
    /** A mention's channel ("#geral"); a DM has none, the sender is the conversation. */
    val conversation: String?,
    val sender: String,
    val text: String,
    val at: Long,
    /** The message it is about; 0 for a room calling. A read up to it takes it away. */
    val messageId: Long = 0,
)

/** What came back from our server when the phone woke up. */
sealed interface Fetched {
    /** The network, or the server, did not answer: say something generic. */
    data object Failed : Fetched
    data class Got(val message: Message?, val channel: Channel) : Fetched
}

/**
 * The decisions, apart from Android so that they can be tested: what to say,
 * and when to say nothing at all.
 */
object NoticeRules {
    /** A room's call is over in a minute (notify.go, pendingRingTTL). */
    const val RING_TTL_MS = 60_000L
    private const val MAX_TEXT = 500

    fun message(w: Wake, fetched: Fetched, me: String?): Notice? = when (fetched) {
        Fetched.Failed -> Notice(w.kind, w.channelId, null, "", generic(w.kind), w.at, w.messageId)
        is Fetched.Got -> {
            val m = fetched.message
            val c = fetched.channel
            when {
                // Gone, or removed before the phone woke: nothing to say.
                m == null || m.deleted -> null
                // Read on the computer already: the phone in the pocket has
                // nothing to add (the web does the same, push.go).
                c.lastReadId >= m.id -> null
                me != null && m.author.username == me -> null
                else -> Notice(
                    w.kind, w.channelId,
                    conversation = if (w.kind == Wake.MENTION) "#${c.name}" else null,
                    sender = m.author.displayName.ifBlank { m.author.username },
                    text = preview(m),
                    at = m.createdAt,
                    messageId = m.id,
                )
            }
        }
    }

    /**
     * A room calling. The caller's name is only in what the server kept for
     * this person (/api/notifications/pending), never in the push. [pending]
     * null is the fetch failing.
     */
    fun ring(w: Wake, pending: String?, now: Long): Notice? {
        if (w.at > 0 && now - w.at > RING_TTL_MS) return null
        if (pending == null) return Notice(w.kind, w.channelId, null, "", generic(w.kind), w.at)
        val items = runCatching { JSONObject(pending).optJSONArray("items") }.getOrNull() ?: return null
        for (i in 0 until items.length()) {
            val o = items.optJSONObject(i) ?: continue
            if (o.optString("tag") != "${Wake.RING}:${w.channelId}") continue
            // "tales está te chamando", "em 🔊 geral"
            return Notice(w.kind, w.channelId, o.optString("body"), o.optString("title"), o.optString("body"),
                o.optLong("at", w.at))
        }
        // Not there any more: answered, or over.
        return null
    }

    fun preview(m: Message): String {
        val body = m.body.trim()
        if (body.isNotEmpty()) return if (body.length > MAX_TEXT) body.take(MAX_TEXT) + "…" else body
        val images = m.attachments.count { it.isImage }
        return when {
            images == 1 && m.attachments.size == 1 -> "📷 Imagem"
            images == m.attachments.size && images > 1 -> "📷 $images imagens"
            m.attachments.size == 1 -> "📎 ${m.attachments[0].name}"
            m.attachments.isNotEmpty() -> "📎 ${m.attachments.size} arquivos"
            else -> "Mensagem"
        }
    }

    fun generic(kind: String): String = when (kind) {
        Wake.DM -> "Nova mensagem"
        Wake.MENTION -> "Você foi mencionado"
        Wake.RING -> "Alguém está te chamando numa sala"
        else -> "Algo novo no harmony"
    }

    /** What the lock screen shows when the phone hides sensitive content. */
    fun publicLine(kind: String): String = when (kind) {
        Wake.DM -> "Nova mensagem"
        Wake.MENTION -> "Nova menção"
        Wake.RING -> "Chamado para uma sala"
        else -> "Algo novo"
    }
}
