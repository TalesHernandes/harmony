package com.taleshernandes.harmony.model

// The shapes the server sends, as docs/PROTOCOL-HARMONY.md §1–§3 describes them.

data class User(val username: String, val displayName: String, val isAdmin: Boolean,
    val email: String = "", val emailVerified: Boolean = true, val twofa: String = "")

data class Channel(val id: String, val kind: String, val name: String, val topic: String,
    val muted: Boolean = false, val unread: Int = 0, val mentions: Int = 0,
    val lastMessageId: Long = 0, val lastReadId: Long = 0, val peer: Author? = null,
    val categoryId: String = "", val position: Int = 0) {
    val isVoice get() = kind == KIND_VOICE
    val isText get() = kind == "text"
    val isDm get() = kind == "dm"

    companion object {
        const val KIND_VOICE = "voice"
    }
}

/** One device inside a voice room. The same person on two devices is two peers. */
data class Peer(
    val sessionId: String,
    val username: String,
    val displayName: String,
    val muted: Boolean,
    val deafened: Boolean,
    val joinedAt: Long,
    val publishing: List<String>,
) {
    val sharingScreen get() = "screen" in publishing
    val camera get() = "cam" in publishing
}

/** What arrives on /ws that the app acts on today. Everything else is [Other]. */
sealed interface GatewayEvent {
    /** The wire name, for the log: class names do not survive the shrinker. */
    val type: String

    data class MessageNew(val message: Message) : GatewayEvent {
        override val type get() = "message"
    }
    data class MessageChanged(val message: Message) : GatewayEvent {
        override val type get() = "message_changed"
    }
    data class Read(val channelId: String, val lastReadId: Long) : GatewayEvent {
        override val type get() = "read"
    }
    data class TypingEvent(val channelId: String, val username: String, val displayName: String) : GatewayEvent {
        override val type get() = "typing"
    }

    data class VoiceRoom(val channelId: String, val peers: List<Peer>) : GatewayEvent {
        override val type get() = "voice_room"
    }
    data class Roles(val roles: List<Role>) : GatewayEvent {
        override val type get() = "roles"
    }
    data class Layout(val categories: List<Category>, val channels: List<ChannelPlacement>) : GatewayEvent {
        override val type get() = "layout"
    }
    data class ChannelUpsert(val channel: Channel) : GatewayEvent {
        override val type get() = "channel"
    }
    data class ChannelRemoved(val id: String) : GatewayEvent {
        override val type get() = "channel_removed"
    }
    data class Presence(val username: String, val state: String) : GatewayEvent {
        override val type get() = "presence"
    }
    /** A renegotiation for one voice session (PROTOCOL-HARMONY §4.4). */
    data class VoiceOfferEvent(val offer: VoiceOffer) : GatewayEvent {
        override val type get() = "voice_offer"
    }
    data class Other(override val type: String) : GatewayEvent
}

/** Whose sound arrives on which slot of the voice connection. */
data class TrackInfo(val mid: String, val username: String, val displayName: String, val kind: String, val source: String)

data class IceServer(val urls: List<String>, val username: String?, val credential: String?)

/**
 * What the media server asks this device to answer, on join and on every
 * renegotiation (PROTOCOL-HARMONY §4.1, §4.4). [iceServers] only comes with
 * the join.
 */
data class VoiceOffer(
    val sessionId: String,
    val sdp: String,
    val sendMids: Map<String, String>,
    val tracks: List<TrackInfo>,
    val iceServers: List<IceServer>,
    val iceRestart: Boolean = false,
)

/** A voice room as the screen draws it: the channel and who is inside. */
data class VoiceRoom(val channel: Channel, val peers: List<Peer>)

/**
 * The rooms list, kept in step with the server.
 *
 * The rule the web learned (gateway.ts): the server sends no snapshot, so every
 * time the socket opens the app fetches both lists again and replaces what it
 * had ([resync]); between opens, each event is applied on top ([apply]). A
 * `voice_room` event carries the whole room, never a difference, so applying it
 * is a replacement and applying it twice is harmless.
 */
data class RoomsState(
    val channels: List<Channel> = emptyList(),
    val peers: Map<String, List<Peer>> = emptyMap(),
    val categories: List<Category> = emptyList(),
    val canManage: Boolean = false,
    val roles: List<Role> = emptyList(),
    val roleColors: List<String> = emptyList(),
    val roleOwner: Boolean = false,
) {
    val voiceRooms: List<VoiceRoom>
        get() = channels.filter { it.isVoice }.map { VoiceRoom(it, peers[it.id].orEmpty()) }

    fun resync(channels: List<Channel>, rooms: Map<String, List<Peer>>) =
        RoomsState(channels, rooms)

    val textChannels: List<Channel> get() = channels.filter { it.isText }
    val dms: List<Channel> get() = channels.filter { it.isDm }
        .sortedByDescending { if (it.lastMessageId == 0L) Long.MAX_VALUE else it.lastMessageId }

    fun markRead(channelId: String, lastReadId: Long): RoomsState = copy(channels = channels.map {
        if (it.id == channelId) it.copy(lastReadId = lastReadId, unread = 0, mentions = 0) else it
    })

    fun apply(ev: GatewayEvent): RoomsState = apply(ev, null, null)

    fun apply(ev: GatewayEvent, me: String?, open: String?): RoomsState = when (ev) {
        is GatewayEvent.MessageNew -> copy(channels = channels.map { c ->
            val msg = ev.message
            if (c.id != msg.channelId) c else {
                val blocked = PersonalPreferences.blocked(msg.author.username)
                val quiet = blocked || c.muted || c.id == open || msg.author.username == me
                c.copy(
                    lastMessageId = maxOf(c.lastMessageId, msg.id),
                    unread = c.unread + if (quiet) 0 else 1,
                    mentions = c.mentions + if (!blocked && !c.muted && c.id != open && me in msg.mentions) 1 else 0,
                )
            }
        })
        is GatewayEvent.Roles -> copy(roles = ev.roles, canManage = roleOwner || permissionsOf(me, false, ev.roles).contains("manage_channels"))
        is GatewayEvent.Layout -> copy(categories = ev.categories, channels = channels.map { c ->
            ev.channels.firstOrNull { it.id == c.id }?.let { c.copy(categoryId = it.categoryId, position = it.position) } ?: c
        })
        is GatewayEvent.Read -> markRead(ev.channelId, ev.lastReadId)
        is GatewayEvent.VoiceRoom -> copy(peers = peers + (ev.channelId to ev.peers))
        is GatewayEvent.ChannelUpsert -> {
            val i = channels.indexOfFirst { it.id == ev.channel.id }
            if (i < 0) copy(channels = channels + ev.channel)
            else copy(channels = channels.toMutableList().also {
                it[i] = ev.channel.copy(unread = channels[i].unread, mentions = channels[i].mentions)
            })
        }
        is GatewayEvent.ChannelRemoved ->
            copy(channels = channels.filterNot { it.id == ev.id }, peers = peers - ev.id)
        is GatewayEvent.Presence, is GatewayEvent.VoiceOfferEvent, is GatewayEvent.Other,
        is GatewayEvent.MessageChanged, is GatewayEvent.TypingEvent,
        is CallIncoming, is CallAccepted, is CallTaken, is CallEnded,
        is CallSignalEvent, is CallStateEvent -> this
    }
}
