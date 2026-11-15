package com.taleshernandes.harmony.model

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update

/**
 * Whose face is a photo, and which one: the server's `avatar` version per
 * username (PROTOCOL-HARMONY, "Foto de perfil"). A face asks here instead of
 * every list carrying the field; the lists that arrive with it (members,
 * presence, the person's own record) feed it as they are parsed, and a change
 * made by somebody else arrives as a presence event.
 */
object Avatars {
    private val _versions = MutableStateFlow<Map<String, String>>(emptyMap())
    val versions: StateFlow<Map<String, String>> = _versions

    /** An empty version means no photo. */
    fun note(username: String, version: String) {
        if (username.isEmpty()) return
        _versions.update { cur ->
            if ((cur[username] ?: "") == version) cur
            else if (version.isEmpty()) cur - username else cur + (username to version)
        }
    }

    fun clear() = _versions.update { emptyMap() }

    /** The address of the picture, with the version that makes it cacheable forever. */
    fun url(username: String, version: String) = "/api/people/$username/avatar?v=$version"
}
