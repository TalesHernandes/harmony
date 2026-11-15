package com.taleshernandes.harmony.ui

import android.graphics.BitmapFactory
import android.util.LruCache
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.net.Api
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Pictures from the server, without an image library (plan §2: try without
 * one first). A picture is behind the session cookie, so it is fetched by the
 * app's own [Api] and decoded here, shrunk on the way in to the size it is
 * drawn at — a 4000-pixel photo decoded whole is 64 MB of memory for a
 * thumbnail.
 */
internal object Pictures {
    // Sized in bytes of decoded pixels: an eighth of what the app may use.
    private val cache = object : LruCache<String, ImageBitmap>((Runtime.getRuntime().maxMemory() / 8).toInt()) {
        override fun sizeOf(key: String, value: ImageBitmap) = value.width * value.height * 4
    }

    // Bumped on sign-out: the same address on the next account (or server)
    // may be another picture, and a load started before must not refill it.
    @Volatile private var epoch = 0

    fun cached(url: String, maxPx: Int): ImageBitmap? = cache.get("$url@$maxPx")

    fun clear() {
        epoch++
        cache.evictAll()
    }

    suspend fun load(api: Api, url: String, maxPx: Int): ImageBitmap? {
        cached(url, maxPx)?.let { return it }
        val mine = epoch
        return withContext(Dispatchers.IO) {
            try {
                val bytes = api.file(url)
                val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
                var sample = 1
                while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= maxPx) sample *= 2
                val bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })
                    ?: return@withContext null
                bmp.asImageBitmap().also { if (mine == epoch) cache.put("$url@$maxPx", it) }
            } catch (e: Exception) {
                Diag.log("imagem falhou: ${e.javaClass.simpleName}")
                null
            }
        }
    }
}

/** A server picture, with the space it will take held while it loads. */
@Composable
internal fun RemotePicture(
    api: () -> Api?,
    url: String,
    maxPx: Int,
    description: String,
    modifier: Modifier,
    scale: ContentScale = ContentScale.Crop,
) {
    var bmp by remember(url, maxPx) { mutableStateOf(Pictures.cached(url, maxPx)) }
    LaunchedEffect(url, maxPx) {
        if (bmp == null) api()?.let { bmp = Pictures.load(it, url, maxPx) }
    }
    val b = bmp
    if (b == null) {
        // The grey square with the picture glyph: a place for a picture, not a picture.
        Box(modifier.background(Surface2), contentAlignment = Alignment.Center) { Glyph(Glyphs.Picture, null, tint = Muted) }
    } else {
        Image(b, contentDescription = description, modifier = modifier, contentScale = scale)
    }
}
