package com.taleshernandes.harmony.net

/** Default-network transitions, independent of callback order and Android APIs. */
class NetworkChanges<T> {
    private var current: T? = null
    private var lost = false
    private var transports: Set<Int>? = null
    private var validated = false
    private var validationGained = false

    fun available(network: T): Boolean {
        val moved = lost || (current != null && current != network)
        current = network
        lost = false
        transports = null
        validated = false
        validationGained = false
        return moved
    }

    fun lost(network: T): Boolean {
        if (network != current) return false
        lost = true
        return true
    }

    fun gainedValidation(network: T): Boolean = network == current && validationGained

    fun capabilities(network: T, next: Set<Int>, ready: Boolean): Boolean {
        if (network != current) return false
        validationGained = !validated && ready
        val changed = transports != null && transports != next
        transports = next
        validated = ready
        return changed
    }
}
