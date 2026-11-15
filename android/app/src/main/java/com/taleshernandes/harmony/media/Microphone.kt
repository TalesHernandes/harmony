package com.taleshernandes.harmony.media

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager

internal fun microphoneAllowed(context: Context): Boolean =
    context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
