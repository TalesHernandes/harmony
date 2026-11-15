package com.taleshernandes.harmony.net

import com.taleshernandes.harmony.model.Channel
import com.taleshernandes.harmony.model.Message
import org.json.JSONObject

/** The API carries the channel even when its history has never been loaded. */
data class SearchHit(val message: Message, val channel: Channel?)

internal fun searchHits(body: String): List<SearchHit> {
    val messages = JSONObject(body).getJSONArray("messages")
    return (0 until messages.length()).map { index ->
        val hit = messages.getJSONObject(index)
        SearchHit(Json.message(hit), hit.optJSONObject("channel")?.let(Json::channel))
    }
}
