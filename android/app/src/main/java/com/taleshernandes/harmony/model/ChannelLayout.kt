package com.taleshernandes.harmony.model

import org.json.JSONArray
import org.json.JSONObject

data class Category(val id: String, val name: String, val position: Int = 0)
data class ChannelPlacement(val id: String, val categoryId: String, val position: Int = 0)
data class ChannelSnapshot(val channels: List<Channel>, val categories: List<Category>, val canManage: Boolean)
data class ChannelSection(val category: Category?, val channels: List<Channel>)

fun canPin(manageMessages: Boolean, channel: Channel) = channel.isDm || (channel.isText && manageMessages)

fun RoomsState.sections(closed: Set<String>, openId: String?, voiceId: String?): List<ChannelSection> =
    (listOf<Category?>(null) + categories.sortedBy { it.position }).map { category ->
        val id = category?.id.orEmpty()
        ChannelSection(category, channels.filter { !it.isDm && it.categoryId == id }
            .filter { id !in closed || it.id == openId || it.id == voiceId || peers[it.id].orEmpty().isNotEmpty() }
            .sortedWith(compareBy<Channel> { it.isVoice }.thenBy { it.position }))
    }

/** A complete draft, including channels hidden by a collapsed folder; DMs never enter layout. */
data class ChannelLayout(val categories: List<Category>, val channels: List<ChannelPlacement>) {
    fun moveChannel(id: String, categoryId: String) = copy(channels = channels.filterNot { it.id == id } +
        channels.filter { it.id == id }.map { it.copy(categoryId = categoryId) })
    fun reorder(from: Int, to: Int, category: Boolean): ChannelLayout {
        fun <T> moved(list: List<T>): List<T> = list.toMutableList().apply { add(to, removeAt(from)) }
        return if (category) copy(categories = moved(categories)) else copy(channels = moved(channels))
    }
    fun body() = JSONObject().put("categories", JSONArray(categories.map { it.id }))
        .put("channels", JSONArray(channels.map {
            JSONObject().put("id", it.id).put("category_id", it.categoryId.ifEmpty { null } ?: JSONObject.NULL)
        }))
    companion object {
        fun from(state: RoomsState) = ChannelLayout(state.categories.sortedBy { it.position },
            state.channels.filterNot { it.isDm }.sortedBy { it.position }.map { ChannelPlacement(it.id, it.categoryId, it.position) })
    }
}
