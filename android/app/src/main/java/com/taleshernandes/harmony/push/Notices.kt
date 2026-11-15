package com.taleshernandes.harmony.push

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.Person
import com.taleshernandes.harmony.R
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.ui.MainActivity

/**
 * Draws what [NoticeRules] decided. Three Android channels, one per kind, so
 * that each can be switched off in the phone's own settings (Ajustes → Apps →
 * harmony → Notificações): direct messages, mentions, and rooms calling.
 *
 * One notification per conversation: a second message in the same one is a
 * new line in it, not a second notification (the messages style, rebuilt from
 * the notification already showing, so it survives the app being closed
 * between the two).
 */
class Notices(private val context: Context) {
    private val nm = NotificationManagerCompat.from(context)

    /** Made at start, so the switches are in the settings before the first message. */
    fun createChannels() {
        val system = context.getSystemService(NotificationManager::class.java) ?: return
        for ((id, name, what) in CHANNELS) {
            if (system.getNotificationChannel(id) != null) continue
            system.createNotificationChannel(
                NotificationChannel(id, name, NotificationManager.IMPORTANCE_HIGH).apply { description = what },
            )
        }
    }

    fun show(n: Notice) {
        if (Build.VERSION.SDK_INT >= 33 &&
            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            Diag.log("aviso não mostrado: notificações não liberadas")
            return
        }
        if (!nm.areNotificationsEnabled()) {
            Diag.log("aviso não mostrado: notificações desligadas no Android")
            return
        }
        val b = NotificationCompat.Builder(context, n.kind)
            .setSmallIcon(icon(n.kind))
            .setWhen(n.at)
            .setShowWhen(true)
            .setAutoCancel(true)
            .setContentIntent(tap(n))
            // The lock screen with "hide sensitive content" on shows the
            // public version: which kind of thing, never who or what.
            .setVisibility(NotificationCompat.VISIBILITY_PRIVATE)
            .setPublicVersion(
                NotificationCompat.Builder(context, n.kind)
                    .setSmallIcon(icon(n.kind))
                    .setContentTitle("harmony")
                    .setContentText(NoticeRules.publicLine(n.kind))
                    .build(),
            )
        if (n.kind == Wake.RING) {
            b.setCategory(NotificationCompat.CATEGORY_CALL)
                .setContentTitle(n.sender.ifEmpty { "harmony" })
                .setContentText(n.text)
                // The call is over a minute after it rang, however late the
                // push arrived; so is being told of it.
                .setTimeoutAfter(
                    (n.at + NoticeRules.RING_TTL_MS - System.currentTimeMillis())
                        .coerceIn(MIN_RING_SHOWN_MS, NoticeRules.RING_TTL_MS),
                )
        } else if (n.sender.isEmpty()) {
            b.setCategory(NotificationCompat.CATEGORY_MESSAGE)
                .setContentTitle("harmony")
                .setContentText(n.text)
        } else {
            b.setCategory(NotificationCompat.CATEGORY_MESSAGE)
                .setStyle(conversation(n))
        }
        try {
            nm.notify(n.channelId, id(n.kind), b.build())
            if (n.messageId > 0) synchronized(shown) { shown[n.channelId] = maxOf(shown[n.channelId] ?: 0L, n.messageId) }
            Diag.log("aviso mostrado: ${n.kind}")
        } catch (e: SecurityException) {
            Diag.log("aviso recusado pelo Android: ${e.javaClass.simpleName}")
        }
    }

    /** From the watchman: one notification, which "voltou" replaces. Only the owner's phone gets these. */
    fun showServer(title: String, text: String, at: Long) {
        if (Build.VERSION.SDK_INT >= 33 &&
            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            Diag.log("aviso do vigia não mostrado: notificações não liberadas")
            return
        }
        val b = NotificationCompat.Builder(context, SERVER)
            .setSmallIcon(R.drawable.notif_app_atualizado)
            .setContentTitle(title)
            .setContentText(text)
            .setStyle(NotificationCompat.BigTextStyle().bigText(text))
            .setWhen(at)
            .setShowWhen(true)
            .setAutoCancel(true)
            .setCategory(NotificationCompat.CATEGORY_STATUS)
        try {
            nm.notify(SERVER, SERVER_ID, b.build())
            Diag.log("aviso do vigia mostrado")
        } catch (e: SecurityException) {
            Diag.log("aviso do vigia recusado pelo Android: ${e.javaClass.simpleName}")
        }
    }

    // The newest message each conversation's notification is showing, so a
    // read that covers only older ones leaves it (a late "read up to 100"
    // from the computer must not take away message 102). Lost with the
    // process: a notification from before is then taken by any read.
    private val shown = HashMap<String, Long>()

    /** Read up to [upTo] (Long.MAX_VALUE: opened in view): the notification goes if that covers it. */
    fun cancel(channelId: String, upTo: Long) {
        synchronized(shown) {
            val newest = shown[channelId]
            if (newest != null && newest > upTo) return
            shown.remove(channelId)
        }
        nm.cancel(channelId, id(Wake.DM))
        nm.cancel(channelId, id(Wake.MENTION))
    }

    /** Signing out: nothing about the old session stays in the shade. */
    fun cancelAll() {
        val system = context.getSystemService(NotificationManager::class.java) ?: return
        synchronized(shown) { shown.clear() }
        for (s in system.activeNotifications) {
            if (s.id in IDS && s.tag != null) nm.cancel(s.tag, s.id)
        }
    }

    private fun conversation(n: Notice): NotificationCompat.MessagingStyle {
        val system = context.getSystemService(NotificationManager::class.java)
        val showing = system?.activeNotifications?.firstOrNull { it.tag == n.channelId && it.id == id(n.kind) }
        val style = showing?.notification
            ?.let { NotificationCompat.MessagingStyle.extractMessagingStyleFromNotification(it) }
            ?: NotificationCompat.MessagingStyle(Person.Builder().setName("Você").build())
        // Firebase may deliver the same push twice.
        if (style.messages.none { it.timestamp == n.at && it.text?.toString() == n.text }) {
            style.addMessage(n.text, n.at, Person.Builder().setName(n.sender).build())
        }
        while (style.messages.size > MAX_LINES) style.messages.removeAt(0)
        if (n.conversation != null) {
            style.conversationTitle = n.conversation
            style.isGroupConversation = true
        }
        return style
    }

    private fun tap(n: Notice): PendingIntent {
        val i = Intent(context, MainActivity::class.java)
            // The notification's identity is in the address: two conversations
            // whose ids hash alike must not share one PendingIntent, which is
            // told apart by the Intent but not by its extras (audit, 26/09/2026).
            .setData(Uri.Builder().scheme("harmony").authority("aviso").appendPath(n.kind).appendPath(n.channelId).build())
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP)
            .putExtra(if (n.kind == Wake.RING) EXTRA_JOIN else EXTRA_OPEN, n.channelId)
            // Tells this tap from an older one the screen may still hold.
            .putExtra(EXTRA_TAP, System.nanoTime())
        return PendingIntent.getActivity(
            context, 0, i,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
    }

    private fun icon(kind: String) = when (kind) {
        Wake.DM -> R.drawable.notif_mensagem_direta
        Wake.MENTION -> R.drawable.notif_mencao
        else -> R.drawable.notif_chamado_sala
    }

    companion object {
        const val EXTRA_OPEN = "harmony.open"
        const val EXTRA_JOIN = "harmony.join"
        const val EXTRA_TAP = "harmony.tap"
        private const val MAX_LINES = 6
        private const val MIN_RING_SHOWN_MS = 5_000L
        // The call's own notification is 1 (CallService).
        private fun id(kind: String) = when (kind) {
            Wake.DM -> 100
            Wake.MENTION -> 101
            else -> 102
        }
        private val IDS = setOf(100, 101, 102)
        private const val SERVER = "server"
        private const val SERVER_ID = 103
        private val CHANNELS = listOf(
            Triple(SERVER, "Servidor fora do ar", "Quando o harmony para de responder, e quando volta."),
            Triple(Wake.DM, "Mensagens diretas", "Quando alguém te manda uma mensagem direta."),
            Triple(Wake.MENTION, "Menções", "Quando alguém te menciona num canal."),
            Triple(Wake.RING, "Chamados para sala", "Quando alguém te chama para uma sala de voz."),
        )
    }
}
