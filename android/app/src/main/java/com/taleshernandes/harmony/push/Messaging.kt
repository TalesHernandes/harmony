package com.taleshernandes.harmony.push

import android.content.Context
import com.google.firebase.FirebaseApp
import com.google.firebase.messaging.FirebaseMessagingService
import com.google.firebase.messaging.RemoteMessage
import com.taleshernandes.harmony.HarmonyApp

/**
 * Where Firebase lands. It may start the whole app from nothing — a phone in a
 * pocket with the app closed — which is why the app's start opens its socket
 * as "behind the screen" (Core, Gateway).
 *
 * onMessageReceived runs on Firebase's own background thread and may block
 * there for a few seconds: that is where the text is fetched from our server.
 */
class Messaging : FirebaseMessagingService() {
    override fun onNewToken(token: String) {
        (application as HarmonyApp).core.pushTokenChanged(token)
    }

    override fun onMessageReceived(message: RemoteMessage) {
        // Firebase lowers a high-priority message to normal when it judges
        // the app does not deserve it; a lowered one waits for the phone's
        // next wake-up, which is seconds or minutes. Said in the log.
        val lowered = message.originalPriority == RemoteMessage.PRIORITY_HIGH &&
            message.priority != RemoteMessage.PRIORITY_HIGH
        (application as HarmonyApp).core.onWake(message.data, if (lowered) " (prioridade rebaixada pelo Firebase)" else "")
    }

    companion object {
        /** Whether this build has the Firebase config (build.gradle.kts): without it there are no notifications. */
        fun available(context: Context): Boolean = FirebaseApp.getApps(context).isNotEmpty()
    }
}
