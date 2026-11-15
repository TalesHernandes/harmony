package com.taleshernandes.harmony.ui

import androidx.compose.ui.unit.sp
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.offset
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.ui.draw.clip
import androidx.compose.runtime.collectAsState
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.net.Uri
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ClipOp
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.clipPath
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import com.taleshernandes.harmony.net.ApiError
import java.io.ByteArrayOutputStream
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * The picture the person picked, decoded at most [PICK_PX] on the long side
 * (a camera roll photo is 3 to 12 MB, in formats the server does not read,
 * like HEIC and WebP) and turned upright when the file says it was taken
 * sideways. The editor works on this; only the framed square leaves the phone.
 */
internal fun loadPickedPhoto(context: Context, uri: Uri): Bitmap {
    val resolver = context.contentResolver
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    resolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, bounds) }
    require(bounds.outWidth > 0 && bounds.outHeight > 0) { "not an image" }
    var sample = 1
    while (max(bounds.outWidth, bounds.outHeight) / (sample * 2) >= PICK_PX) sample *= 2
    val bmp = resolver.openInputStream(uri)?.use {
        BitmapFactory.decodeStream(it, null, BitmapFactory.Options().apply { inSampleSize = sample })
    } ?: error("not an image")
    val degrees = runCatching {
        resolver.openInputStream(uri)?.use {
            when (android.media.ExifInterface(it).getAttributeInt(android.media.ExifInterface.TAG_ORIENTATION, 1)) {
                android.media.ExifInterface.ORIENTATION_ROTATE_90 -> 90
                android.media.ExifInterface.ORIENTATION_ROTATE_180 -> 180
                android.media.ExifInterface.ORIENTATION_ROTATE_270 -> 270
                else -> 0
            }
        } ?: 0
    }.getOrDefault(0)
    return if (degrees == 0) bmp
    else Bitmap.createBitmap(bmp, 0, 0, bmp.width, bmp.height, Matrix().apply { postRotate(degrees.toFloat()) }, true)
}

/** The framed square, [left]/[top]/[side] in the bitmap's pixels, as a JPEG of [OUT_PX]. */
internal fun framedJpeg(src: Bitmap, left: Int, top: Int, side: Int): ByteArray {
    val s = side.coerceIn(1, min(src.width, src.height))
    val x = left.coerceIn(0, src.width - s)
    val y = top.coerceIn(0, src.height - s)
    val square = Bitmap.createBitmap(src, x, y, s, s)
    val out = if (s > OUT_PX) Bitmap.createScaledBitmap(square, OUT_PX, OUT_PX, true) else square
    return ByteArrayOutputStream().use { o ->
        out.compress(Bitmap.CompressFormat.JPEG, 90, o)
        o.toByteArray()
    }
}

/**
 * Framing the photo before it goes up (owner, 02/10/2026): the picture under a
 * round window the size of the screen's width, two fingers to zoom (up to five
 * times), one to move it. It always covers the circle, so there is never an
 * empty corner in the face. Saving cuts exactly the square under the window.
 */
@Composable
internal fun PhotoEditor(photo: Bitmap, onCancel: () -> Unit, onSave: (ByteArray) -> Unit) {
    BackHandler(onBack = onCancel)
    val w = photo.width.toFloat()
    val h = photo.height.toFloat()
    var windowPx by remember(photo) { mutableFloatStateOf(0f) }
    var zoom by remember(photo) { mutableFloatStateOf(1f) }
    var dx by remember(photo) { mutableFloatStateOf(0f) }
    var dy by remember(photo) { mutableFloatStateOf(0f) }
    val image = remember(photo) { photo.asImageBitmap() }
    // The smallest scale that still covers the window.
    fun base() = if (windowPx > 0f) max(windowPx / w, windowPx / h) else 1f
    fun clamp() {
        val s = base() * zoom
        val maxX = max(0f, (w * s - windowPx) / 2f)
        val maxY = max(0f, (h * s - windowPx) / 2f)
        dx = dx.coerceIn(-maxX, maxX)
        dy = dy.coerceIn(-maxY, maxY)
    }
    Sheet(onCancel, title = "Ajustar a foto", expanded = true, scroll = false) {
        Column(Modifier.fillMaxSize(),
            verticalArrangement = Arrangement.spacedBy(16.dp)) {
            Text("Use dois dedos para aproximar e um para mover.", style = MetaText, color = Muted)
            BoxWithConstraints(Modifier.fillMaxWidth().weight(1f), contentAlignment = Alignment.Center) {
                windowPx = with(LocalDensity.current) { minOf(maxWidth, maxHeight).toPx() }
                Canvas(Modifier.fillMaxSize()
                    .pointerInput(photo) {
                        detectTransformGestures { _, pan, gestureZoom, _ ->
                            zoom = (zoom * gestureZoom).coerceIn(1f, 5f)
                            dx += pan.x
                            dy += pan.y
                            clamp()
                        }
                    }
                    .semantics { contentDescription = "foto para enquadrar" }) {
                    val s = base() * zoom
                    val cx = size.width / 2f + dx
                    val cy = size.height / 2f + dy
                    drawImage(image,
                        dstOffset = IntOffset((cx - w * s / 2f).roundToInt(), (cy - h * s / 2f).roundToInt()),
                        dstSize = IntSize((w * s).roundToInt(), (h * s).roundToInt()))
                    // Everything outside the round window is dimmed.
                    val hole = Path().apply {
                        addOval(androidx.compose.ui.geometry.Rect(Offset(size.width / 2f, size.height / 2f), windowPx / 2f))
                    }
                    clipPath(hole, clipOp = ClipOp.Difference) { drawRect(Color.Black.copy(alpha = 0.6f)) }
                    drawCircle(Foreground.copy(alpha = 0.8f), radius = windowPx / 2f, style = Stroke(width = 2.dp.toPx()))
                }
            }
            // The same zoom as the web's editor (owner, 03/10/2026): a rail under
            // the picture, beside the pinch.
            Row(Modifier.fillMaxWidth().height(48.dp).clip(Shape8).background(Surface2).padding(horizontal = 12.dp),
                verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Text("Zoom", style = ItemText.copy(fontSize = 15.sp, lineHeight = 22.sp))
                ZoomRail((zoom - 1f) / 4f, Modifier.weight(1f)) { zoom = 1f + it * 4f; clamp() }
                Text("${(zoom * 100).roundToInt()}%", style = MetaText, color = Muted)
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                WebButton("Cancelar", onCancel, Modifier.weight(1f), height = 48, background = Surface2)
                WebButton("Salvar", {
                    // Saving reads the same numbers the drawing used.
                    val s = base() * zoom
                    val left = ((w * s - windowPx) / 2f - dx) / s
                    val top = ((h * s - windowPx) / 2f - dy) / s
                    onSave(framedJpeg(photo, left.roundToInt(), top.roundToInt(), (windowPx / s).roundToInt()))
                }, Modifier.weight(1f), height = 48, background = Accent, enabled = windowPx > 0f)
            }
        }
    }
}

/** A 6 dp rail with the foreground square, as a person's volume (Person.kt), for 0..1. */
@Composable
private fun ZoomRail(at: Float, modifier: Modifier, onChange: (Float) -> Unit) {
    var width by remember { androidx.compose.runtime.mutableIntStateOf(0) }
    val change by androidx.compose.runtime.rememberUpdatedState(onChange)
    Box(modifier.height(44.dp)
        .onSizeChanged { width = it.width }
        .pointerInput(Unit) {
            awaitEachGesture {
                val down = awaitFirstDown()
                if (size.width > 0) change((down.position.x / size.width).coerceIn(0f, 1f))
                drag(down.id) { c ->
                    if (size.width > 0) change((c.position.x / size.width).coerceIn(0f, 1f))
                    c.consume()
                }
            }
        }
        .semantics {
            contentDescription = "zoom da foto"
            progressBarRangeInfo = ProgressBarRangeInfo(at, 0f..1f)
            setProgress { change(it.coerceIn(0f, 1f)); true }
        }, contentAlignment = Alignment.CenterStart) {
        Box(Modifier.fillMaxWidth().height(6.dp).clip(CircleShape).background(Surface3)) {
            Box(Modifier.fillMaxWidth(at.coerceIn(0f, 1f)).height(6.dp).clip(CircleShape).background(Accent))
        }
        Box(Modifier.offset { IntOffset(((width - 16.dp.toPx()) * at.coerceIn(0f, 1f)).roundToInt(), 0) }
            .size(16.dp).clip(RoundedCornerShape(6.dp)).background(Foreground))
    }
}

/** What the server's reason for refusing a photo means, in words. */
internal fun photoErrorText(e: Exception): String = when {
    e is ApiError && (e.reason == "avatar_limit" || e.status == 429) -> "Você já mudou a foto muitas vezes. Espere um pouco."
    e is ApiError && e.status == 413 -> "Essa foto é grande demais."
    e is ApiError && (e.status == 415 || e.reason == "bad_image") -> "Não deu para ler essa foto. Escolha outra."
    e is IllegalArgumentException || e is IllegalStateException -> "Não deu para ler essa foto. Escolha outra."
    else -> accountError(e)
}

private const val PICK_PX = 2048
private const val OUT_PX = 512

/** The person's photo, big, over a dark screen; a tap anywhere or Back closes it. */
@Composable
internal fun BigPhoto(api: () -> com.taleshernandes.harmony.net.Api?, username: String, name: String, onClose: () -> Unit) {
    val versions by com.taleshernandes.harmony.model.Avatars.versions.collectAsState()
    val version = versions[username] ?: return
    Sheet(onClose, title = "Foto de $name", expanded = true, scroll = false) {
        BoxWithConstraints(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            val side = minOf(maxWidth, maxHeight)
            RemotePicture(api, com.taleshernandes.harmony.model.Avatars.url(username, version), 1024, "foto de $name",
                Modifier.size(side).clip(CircleShape))
        }
    }
}
