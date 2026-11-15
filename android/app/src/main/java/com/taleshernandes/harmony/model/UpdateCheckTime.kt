package com.taleshernandes.harmony.model

import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

/** Calendar days in the phone's zone, including daylight-saving transitions. */
fun updateCheckTime(checked: Long?, now: Long = System.currentTimeMillis(), zone: ZoneId = ZoneId.systemDefault()): String {
    if (checked == null) return "Ainda não procurado"
    val whenChecked = Instant.ofEpochMilli(checked).atZone(zone)
    val today = Instant.ofEpochMilli(now).atZone(zone).toLocalDate()
    val day = when (whenChecked.toLocalDate()) {
        today -> "hoje"
        today.minusDays(1) -> "ontem"
        else -> whenChecked.format(DateTimeFormatter.ofPattern("dd/MM/yyyy", Locale.forLanguageTag("pt-BR")))
    }
    return "procurado $day às ${whenChecked.format(DateTimeFormatter.ofPattern("HH:mm"))}"
}
