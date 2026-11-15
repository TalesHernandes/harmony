package com.taleshernandes.harmony.model

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.util.concurrent.CopyOnWriteArrayList

data class PersonPreference(val username: String, val muted: Boolean, val blocked: Boolean)

/** Session-owned privacy state; never persisted in device preferences. */
object PersonalPreferences {
 private val _state = MutableStateFlow<List<PersonPreference>>(emptyList())
 val state: StateFlow<List<PersonPreference>> = _state
 private val listeners = CopyOnWriteArrayList<() -> Unit>()
 fun blocked(username: String) = _state.value.any { it.username == username && it.blocked }
 fun muted(username: String) = _state.value.any { it.username == username && (it.muted || it.blocked) }
 fun silent(username: String, screen: Boolean) = if (screen) blocked(username) else muted(username)
 fun clear() { _state.value = emptyList(); listeners.forEach { it() } }
 fun replace(items: List<PersonPreference>) { _state.value = items; listeners.forEach { it() } }
 fun listen(f: () -> Unit) { listeners.add(f) }
}
