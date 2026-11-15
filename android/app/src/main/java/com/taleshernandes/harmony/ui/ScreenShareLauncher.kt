package com.taleshernandes.harmony.ui

import android.app.Activity
import android.content.Intent
import android.media.projection.MediaProjectionManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.platform.LocalContext

/** Each start asks again: Android 14+ projection grants must never be reused. */
@Composable
fun rememberScreenShare(onResult: (Intent?) -> Unit): () -> Unit {
    val context = LocalContext.current
    val result = rememberUpdatedState(onResult)
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        result.value(if (it.resultCode == Activity.RESULT_OK) it.data else null)
    }
    return remember(context, launcher) {
        {
            val manager = context.getSystemService(MediaProjectionManager::class.java)
            launcher.launch(manager.createScreenCaptureIntent())
        }
    }
}
