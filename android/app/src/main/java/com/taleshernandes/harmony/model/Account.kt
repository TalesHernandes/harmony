package com.taleshernandes.harmony.model

import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale
import kotlin.math.ceil

data class AccountSession(
    val id: String,
    val createdAt: Long,
    val lastSeenAt: Long,
    val expiresAt: Long,
    val userAgent: String,
    val current: Boolean,
)

data class Invite(
    val id: String,
    val note: String,
    val createdAt: Long,
    val expiresAt: Long,
    val used: Boolean,
    val createdBy: String,
    val usedBy: String,
    val code: String? = null,
    val url: String? = null,
)

fun accountWhen(time: Long, zone: ZoneId = ZoneId.systemDefault()): String =
    DateTimeFormatter.ofPattern("dd/MM, HH:mm", Locale.forLanguageTag("pt-BR"))
        .withZone(zone).format(Instant.ofEpochMilli(time))

/** The web's three creator labels; the clock and zone are explicit for tests. */
fun inviteState(invite: Invite, me: String, now: Long = System.currentTimeMillis(),
    zone: ZoneId = ZoneId.systemDefault()): String {
    val by = when (invite.createdBy) {
        "" -> "Criado pela linha de comando"
        me -> "Criado por você"
        else -> "Criado por @${invite.createdBy}"
    }
    val end = if (invite.used) "usado por @${invite.usedBy}" else {
        val days = ceil((invite.expiresAt.toDouble() - now) / 86_400_000).toLong().coerceAtLeast(1)
        "vence em $days dia${if (days == 1L) "" else "s"}"
    }
    return "$by · ${accountWhen(invite.createdAt, zone)} · $end"
}
