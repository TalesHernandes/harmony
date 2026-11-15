package com.taleshernandes.harmony.push

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.net.Uri
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.Person
import com.taleshernandes.harmony.HarmonyApp
import com.taleshernandes.harmony.R
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.media.DirectCall
import com.taleshernandes.harmony.model.CallPerson
import com.taleshernandes.harmony.ui.CallActivity

/**
 * A direct call ringing this phone, the way a telephone rings: the whole
 * screen with the phone locked, the ringtone over and over, Atender and
 * Recusar (plan A7).
 *
 * All of it is one notification, drawn by Android: a full-screen intent that
 * opens [CallActivity] over the lock screen, the call style that puts the two
 * buttons in front, and the ringtone looping (FLAG_INSISTENT) until the
 * notification goes. Nothing of ours has to stay alive for it to ring — a
 * phone woken by Firebase may be put back to sleep the moment the message is
 * handled, and the ringing must not depend on that.
 *
 * The name of who is calling is on it, on the lock screen too, the way a
 * telephone shows it: it came from our server with this phone's session,
 * never through Google (push/Wake.kt).
 */
class Ringing(private val context: Context) {
    private val nm = NotificationManagerCompat.from(context)

    /** Made at start, so it is in the settings before the first call. */
    fun createChannel() {
        val system = context.getSystemService(NotificationManager::class.java) ?: return
        // The first channel rang with the phone's own ringtone, and a channel
        // never changes its sound once made: the harmony's ring (29/10/2026)
        // needed a new one, and the old one would linger in the settings.
        if (system.getNotificationChannel(OLD_CHANNEL) != null) system.deleteNotificationChannel(OLD_CHANNEL)
        if (system.getNotificationChannel(CHANNEL) != null) return
        system.createNotificationChannel(
            NotificationChannel(CHANNEL, "Chamadas diretas", NotificationManager.IMPORTANCE_HIGH).apply {
                description = "Quando alguém te liga pelo harmony."
                setSound(
                    Uri.parse("android.resource://${context.packageName}/${R.raw.som_toque}"),
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                        .build(),
                )
                enableVibration(true)
                vibrationPattern = longArrayOf(0, 1000, 1000)
                lockscreenVisibility = Notification.VISIBILITY_PUBLIC
            },
        )
    }

    /** Whether Android lets the call take the whole screen (Android 14 may not). Said in the log at start. */
    fun fullScreenAllowed(): Boolean {
        if (Build.VERSION.SDK_INT < 34) return true
        return context.getSystemService(NotificationManager::class.java)?.canUseFullScreenIntent() ?: false
    }

    fun show(callId: String, from: CallPerson, at: Long) {
        if (!nm.areNotificationsEnabled()) {
            Diag.log("chamada não tocou: notificações desligadas no Android")
            return
        }
        val name = from.name.ifEmpty { "Alguém" }
        val full = activity(callId, null)
        val answer = activity(callId, CallActivity.ACTION_ANSWER)
        val decline = PendingIntent.getBroadcast(
            context, 0,
            Intent(context, CallActions::class.java).setAction(ACTION_DECLINE).setData(uri(callId)),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val remaining = (at + DirectCall.RING_MS - System.currentTimeMillis()).coerceIn(5_000L, DirectCall.RING_MS)
        val b = NotificationCompat.Builder(context, CHANNEL)
            .setSmallIcon(R.drawable.notif_chamada_direta)
            .setContentTitle(name)
            .setContentText("Chamada do harmony")
            .setCategory(NotificationCompat.CATEGORY_CALL)
            .setPriority(NotificationCompat.PRIORITY_MAX)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
            .setOngoing(true)
            .setAutoCancel(false)
            .setContentIntent(full)
            .setFullScreenIntent(full, true)
            .setTimeoutAfter(remaining)
            .setWhen(at)
        val person = Person.Builder().setName(name).setImportant(true).build()
        val styled = b.setStyle(NotificationCompat.CallStyle.forIncomingCall(person, decline, answer))
        val n = try {
            styled.build()
        } catch (e: IllegalArgumentException) {
            null
        } ?: plain(b, decline, answer)
        n.flags = n.flags or Notification.FLAG_INSISTENT
        try {
            nm.notify(TAG, ID, n)
            Diag.log("chamada tocando${if (fullScreenAllowed()) "" else " (sem tela cheia: o Android não liberou)"}")
        } catch (e: SecurityException) {
            Diag.log("chamada não tocou: ${e.javaClass.simpleName}")
        } catch (e: IllegalArgumentException) {
            // The call style refused (Android wants it on a foreground service
            // or with the full screen): the same ring, with plain buttons.
            Diag.log("chamada: estilo de chamada recusado, tocando sem ele")
            val fallback = plain(b.setStyle(null), decline, answer)
            fallback.flags = fallback.flags or Notification.FLAG_INSISTENT
            runCatching { nm.notify(TAG, ID, fallback) }
        }
    }

    private fun plain(b: NotificationCompat.Builder, decline: PendingIntent, answer: PendingIntent): Notification =
        b.clearActions()
            .addAction(0, "Recusar", decline)
            .addAction(0, "Atender", answer)
            .build()

    fun cancel() = nm.cancel(TAG, ID)

    private fun activity(callId: String, action: String?): PendingIntent {
        val i = Intent(context, CallActivity::class.java)
            .setData(uri(callId).buildUpon().appendPath(action ?: "abrir").build())
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
            .putExtra(CallActivity.EXTRA_CALL, callId)
        if (action != null) i.action = action
        return PendingIntent.getActivity(context, 0, i, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
    }

    private fun uri(callId: String): Uri = Uri.Builder().scheme("harmony").authority("chamada").appendPath(callId).build()

    companion object {
        const val CHANNEL = "direct_call_toque"
        private const val OLD_CHANNEL = "direct_call"
        const val ACTION_DECLINE = "com.taleshernandes.harmony.DECLINE"
        private const val TAG = "direct"
        // The call's own service notification is 1; the conversations' are 100–102.
        private const val ID = 200
    }
}

/** Recusar, from the notification, without opening the app. */
class CallActions : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Ringing.ACTION_DECLINE) return
        val id = intent.data?.lastPathSegment ?: return
        Diag.log("chamada recusada pelo aviso")
        val core = (context.applicationContext as HarmonyApp).core
        // Held until the server has heard it: the process may otherwise be
        // put away with the refusal still unsent, and the other side would
        // go on ringing to nobody.
        val pending = goAsync()
        core.ringing.cancel()
        core.direct.decline(id) { pending.finish() }
    }
}
