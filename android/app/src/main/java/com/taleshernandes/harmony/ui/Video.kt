package com.taleshernandes.harmony.ui

import android.content.Intent
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.key
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.height
import androidx.compose.runtime.*
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import kotlinx.coroutines.delay
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.foundation.layout.wrapContentSize
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.requiredSize
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.unit.IntOffset
import kotlin.math.roundToInt
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.taleshernandes.harmony.media.Engine
import com.taleshernandes.harmony.media.Pictures
import com.taleshernandes.harmony.media.Stage
import com.taleshernandes.harmony.media.VoiceCall
import com.taleshernandes.harmony.model.Member
import org.webrtc.RendererCommon
import org.webrtc.SurfaceViewRenderer

/**
 * One picture from the call, drawn by WebRTC's own renderer.
 *
 * The track never enters the screen's state: the renderer is handed to the
 * call ([Pictures.attach]), which feeds it for as long as the picture lasts
 * and takes it back if the connection goes first. [onWidth] hears the drawn
 * width in real pixels — the number the picture to ask for is chosen by.
 */
@Composable
fun VideoView(
    voice: Pictures,
    video: VoiceCall.Video,
    modifier: Modifier = Modifier,
    mirror: Boolean = false,
    onWidth: ((Int) -> Unit)? = null,
    fill: Boolean = false,
) {
    // Everything below belongs to this picture: without the key, the view
    // made for one picture would stay on screen showing a released renderer
    // when the list changes order (the own camera arriving first).
    key(video.mid) { VideoViewOf(voice, video, modifier, mirror, onWidth, fill) }
}

@Composable
private fun VideoViewOf(
    voice: Pictures,
    video: VoiceCall.Video,
    modifier: Modifier,
    mirror: Boolean,
    onWidth: ((Int) -> Unit)?,
    fill: Boolean,
) {
    val context = LocalContext.current
    val renderer = remember {
        SurfaceViewRenderer(context).apply {
            init(Engine.egl.eglBaseContext, object : RendererCommon.RendererEvents {
                override fun onFirstFrameRendered() {}
                override fun onFrameResolutionChanged(width: Int, height: Int, rotation: Int) {
                    if (video.isScreen) com.taleshernandes.harmony.diag.Diag.log(
                        "screen renderer: frame=${width}x$height rotation=$rotation hardwareScaler=true")
                }
            })
            // The whole picture, with bars if the shapes differ: a game's HUD
            // at the edges is part of what is being shown.
            setScalingType(RendererCommon.ScalingType.SCALE_ASPECT_FIT)
            setEnableHardwareScaler(true)
            setMirror(mirror)
        }
    }
    // The back camera is not a mirror: only the front one is drawn flipped.
    val front by voice.frontCamera.collectAsState()
    LaunchedEffect(mirror, front) { renderer.setMirror(mirror && front) }
    DisposableEffect(Unit) {
        voice.attach(video.mid, renderer)
        onDispose {
            voice.detach(video.mid, renderer)
            renderer.release()
        }
    }
    // The renderer has to be free to choose its own size. SCALE_ASPECT_FIT
    // only letterboxes by measuring itself to the picture's shape; forced to
    // fill the box, it crops the frame to the box instead — a 9:16 live in a
    // 16:9 stage lost its top and bottom (owner, 01/10/2026). So it sits
    // centred in a black box, as the web's object-fit: contain does.
    Box(modifier.background(Color.Black), contentAlignment = Alignment.Center) {
        AndroidView(
            factory = { renderer },
            // fill: forced to the box, the renderer crops the frame to it —
            // YouTube's "zoom to fill", chosen by a pinch in full screen.
            modifier = (if (fill) Modifier.fillMaxSize() else Modifier.wrapContentSize())
                .onSizeChanged {
                    if (video.isScreen) com.taleshernandes.harmony.diag.Diag.log(
                        "screen renderer: view=${it.width}x${it.height} fill=$fill")
                    onWidth?.invoke(it.width)
                }
                .semantics { contentDescription = "${if (video.isScreen) "tela" else "câmera"} de ${video.username}" },
        )
    }
}

/** The room's full screen: the watched live, or the camera tapped. */
@Composable
fun FullVideo(voice: VoiceCall, state: VoiceCall.State, floating: Boolean, onExit: () -> Unit,
    onMute: () -> Unit, members: Map<String, Member> = emptyMap(), lastName: String = "alguém", cameraMid: String? = null) {
    // A camera opens here too, the same way as a live (owner, 01/10/2026).
    val camera = cameraMid?.let { mid -> state.videos.firstOrNull { it.mid == mid } }
    FullPicture(voice, camera ?: state.videos.firstOrNull { it.isScreen && it.username == state.watching },
        reconnecting = state.phase == VoiceCall.Phase.RECONNECTING || state.phase == VoiceCall.Phase.CONNECTING,
        floating = floating, onExit = onExit, muted = state.muted, deafened = state.deafened, onMute = onMute,
        onLeave = voice::leave, leaveLabel = "sair da sala", nameOf = { nameOf(it.username, it.displayName, members) },
        lastName = lastName)
}

/**
 * One picture of a call on the whole screen, a room's or a direct call's:
 * pinch to fill and zoom, a tap for the controls (the live's volume, the
 * microphone, leaving). Full-screen controls are opaque islands; only their
 * alpha is animated. [current] is the picture, or null when it went; while
 * [reconnecting] the last one stays.
 */
@Composable
fun FullPicture(stage: Stage, current: VoiceCall.Video?, reconnecting: Boolean, floating: Boolean, onExit: () -> Unit,
    muted: Boolean, deafened: Boolean, onMute: () -> Unit, onLeave: () -> Unit, leaveLabel: String,
    nameOf: (VoiceCall.Video) -> String, lastName: String = "alguém",
    ended: (String) -> String = { "A live de $it acabou" }, stillHere: String = "Você continua na sala") {
    var lastVideo by remember { mutableStateOf<VoiceCall.Video?>(null) }
    LaunchedEffect(current) { if (current != null) lastVideo = current }
    val screen = current ?: lastVideo.takeIf { reconnecting }
    val camera = screen?.takeIf { !it.isScreen }
    val progress by (stage.screenProgress ?: remember { kotlinx.coroutines.flow.MutableStateFlow<Map<String, com.taleshernandes.harmony.media.ScreenProgress>>(emptyMap()) }).collectAsState()
    val revision by stage.volumeRevision.collectAsState()
    val name = screen?.let(nameOf) ?: lastName
    var touchedAt by remember { mutableLongStateOf(0L) }
    var visible by remember { mutableStateOf(false) }
    // Pinch out fills the screen (cropping the edges), as YouTube does; past
    // fill it keeps zooming, up to 5x, and one finger moves the picture.
    // Pinching in stops at fill, and only a new pinch from fill shows it all
    // again (owner, 01/10/2026: fill must not be a wall).
    var zoom by remember(current?.mid) { mutableStateOf(FullZoom()) }
    fun touched() { touchedAt = android.os.SystemClock.elapsedRealtime(); visible = true }
    LaunchedEffect(touchedAt) { if (visible) { delay(3000); visible = false } }
    val alpha by animateFloatAsState(if (visible) 1f else 0f, tween(180, easing = FastOutSlowInEasing), label = "video controls")
    BackHandler(enabled = !floating, onBack = onExit)
    BoxWithConstraints(Modifier.fillMaxSize().background(Background)
        .then(if (floating) Modifier else Modifier.safeDrawingPadding())
        .pointerInput(floating) {
            if (!floating) awaitPointerEventScope {
                while (true) {
                    val event = awaitPointerEvent(androidx.compose.ui.input.pointer.PointerEventPass.Initial)
                    // Renews the timer while the controls are in use (the
                    // volume dragged); showing and hiding is the tap's.
                    if (visible && event.changes.any { it.pressed }) touchedAt = android.os.SystemClock.elapsedRealtime()
                }
            }
        }, contentAlignment = Alignment.Center) {
        val safeWidth = maxWidth
        if (screen != null) {
            // Keep the renderer, AndroidView and mid key exactly as before.
            // The whole screen is the box: the renderer letterboxes inside it
            // (any shape, 9:16 included), or crops to it when filling.
            val z = if (floating) FullZoom() else zoom
            // Past fill the view is laid out larger than the screen, in the
            // screen's shape, so the renderer crops the same way and the
            // window cuts the rest: no frame is redrawn, no transform over
            // the video. The hardware scaler keeps the buffer at frame size.
            VideoView(stage, screen,
                if (z.scale > 1f) Modifier.offset { IntOffset(z.panX.roundToInt(), z.panY.roundToInt()) }
                    .requiredSize(maxWidth * z.scale, maxHeight * z.scale)
                else Modifier.fillMaxSize(),
                mirror = screen.mid == stage.ownCamera,
                onWidth = { if (screen.isScreen) stage.drawnAt(screen.username, it) },
                fill = z.fill)
        } else if (floating) {
            Column(Modifier.fillMaxSize().background(Surface1).padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterVertically), horizontalAlignment = Alignment.CenterHorizontally) {
                VideoAvatar(name, 40, screen?.username)
                Text(ended(name), fontSize = 14.sp, lineHeight = 20.sp, letterSpacing = (-0.28).sp, fontWeight = FontWeight.SemiBold,
                    textAlign = androidx.compose.ui.text.style.TextAlign.Center)
                Text(stillHere, color = Muted, fontSize = 12.sp, lineHeight = 16.sp, letterSpacing = (-0.24).sp)
            }
        }
        if (floating && screen != null && stage.screenProgress != null) {
            FrozenPicture(progress[screen.mid], screen.mid, Modifier.align(Alignment.BottomStart))
        }
        if (!floating) {
            Box(Modifier.matchParentSize()
                .pointerInput(Unit) {
                    awaitEachGesture {
                        awaitFirstDown(requireUnconsumed = false)
                        val start = zoom
                        var pinch = 1f
                        do {
                            val event = awaitPointerEvent()
                            val factor = event.calculateZoom()
                            pinch *= factor
                            val next = zoom.step(start, pinch, factor, event.calculatePan(), size.width.toFloat(), size.height.toFloat())
                            // A pinch, or a drag of a zoomed picture, is not
                            // the tap that shows the controls.
                            if (next != zoom || event.changes.size > 1) event.changes.forEach { it.consume() }
                            zoom = next
                        } while (event.changes.any { it.pressed })
                    }
                }
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {
                // A tap shows the controls, and a tap with them showing hides
                // them (owner, 01/10/2026) — not only the 3 s timer.
                if (visible) visible = false else touched()
            }.semantics { contentDescription = if (visible) "esconder controles da tela cheia" else "mostrar controles da tela cheia" })
            if (visible || alpha > 0f) Box(Modifier.matchParentSize().graphicsLayer { this.alpha = alpha }
                .padding(LiveOverlay.INSET.dp)) {
                Row(Modifier.align(Alignment.TopStart).height(28.dp)
                    .widthIn(max = (safeWidth - (2 * LiveOverlay.INSET + LiveOverlay.TARGET + LiveOverlay.GAP).dp).coerceAtLeast(0.dp))
                    .background(Background, RoundedCornerShape(8.dp)).padding(horizontal = 10.dp),
                    verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Glyph(if (camera != null) Glyphs.Camera else Glyphs.DisplayPulse, null, size = 12)
                    Text(name, style = MetaText, fontWeight = FontWeight.Medium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                VideoControl(Glyphs.ChevronsCollapseUpRight, "sair da tela cheia", Modifier.align(Alignment.TopEnd), enabled = visible) { touched(); onExit() }
                Row(Modifier.align(Alignment.BottomCenter).fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    screen?.takeIf { it.isScreen }?.let { video ->
                        val level = remember(revision, video.username) { stage.volumes.get(video.username, true) }
                        Row(Modifier.width(LiveOverlay.volumeWidth(safeWidth.value).dp).height(LiveOverlay.TARGET.dp)
                            .background(Background, RoundedCornerShape(8.dp)).padding(end = 8.dp),
                            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                            VideoControl(if (level == 0f) Glyphs.VolumeXmark else Glyphs.Volume,
                                if (level == 0f) "ligar som da live" else "silenciar som da live", tint = if (level == 0f) Muted else Foreground,
                                enabled = visible) { touched(); stage.toggleScreenSound(video.username) }
                            LiveVolume(level, visible) { touched(); stage.setVolume(video.username, true, it) }
                        }
                    }
                    Spacer(Modifier.weight(1f))
                    Spacer(Modifier.width(LiveOverlay.GAP.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(LiveOverlay.GAP.dp)) {
                        val micOff = muted || deafened
                        VideoControl(if (micOff) Glyphs.MicrophoneSlash else Glyphs.Microphone,
                            if (micOff) "ligar microfone" else "desligar microfone", background = if (micOff) Foreground else Surface1,
                            tint = if (micOff) Background else Foreground, enabled = visible && !deafened) { touched(); onMute() }
                        VideoControl(Glyphs.HandsetArrowOut, leaveLabel, background = Danger, enabled = visible) { onLeave(); onExit() }
                    }
                }
            }
        }
    }
}

@Composable
private fun VideoAvatar(name: String, size: Int, username: String?) = Avatar(name, size, username = username)

/**
 * A picture on the whole screen, or back: the way the phone is held, standing
 * or lying either way round (owner, 01/10/2026: a live must be watchable
 * standing up too), over the camera's notch, and the bars out of the way until
 * a swipe brings them back for a moment. The room's and the direct call's.
 */
internal fun android.app.Activity.fullScreenWindow(on: Boolean) {
    if (android.os.Build.VERSION.SDK_INT >= 28) window.attributes = window.attributes.apply {
        layoutInDisplayCutoutMode = if (on) android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            else android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT
    }
    requestedOrientation = if (on) android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR
        else android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
    val bars = androidx.core.view.WindowCompat.getInsetsController(window, window.decorView)
    if (on) {
        bars.systemBarsBehavior = androidx.core.view.WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        bars.hide(androidx.core.view.WindowInsetsCompat.Type.systemBars())
    } else {
        bars.show(androidx.core.view.WindowInsetsCompat.Type.systemBars())
    }
}

/**
 * A button of the floating window: a broadcast to this app, [extras] saying
 * which call it was for, so a press that arrives late does nothing to another.
 */
internal fun android.app.Activity.pipButton(id: String, icon: Int, label: String, extras: Intent.() -> Unit): android.app.RemoteAction {
    val intent = Intent(id).setPackage(packageName).apply(extras)
    val pending = android.app.PendingIntent.getBroadcast(this, id.hashCode(), intent,
        android.app.PendingIntent.FLAG_IMMUTABLE or android.app.PendingIntent.FLAG_UPDATE_CURRENT)
    return android.app.RemoteAction(android.graphics.drawable.Icon.createWithResource(this, icon), label, label, pending)
}

/** The web's 24dp tile chip; fullscreen keeps its separate 44dp controls. */
@Composable
internal fun TileVideoControl(icon: androidx.compose.ui.graphics.vector.ImageVector, description: String, action: () -> Unit) {
    Box(Modifier.size(24.dp).clip(RoundedCornerShape(8.dp)).background(Background)
        .clickable(role = Role.Button, onClick = action).semantics { contentDescription = description },
        contentAlignment = Alignment.Center) { Glyph(icon, null, size = 12) }
}

@Composable
private fun VideoControl(icon: androidx.compose.ui.graphics.vector.ImageVector, description: String, modifier: Modifier = Modifier,
    background: Color = Surface1, tint: Color = Foreground, enabled: Boolean = true, action: () -> Unit) {
    Box(modifier.size(LiveOverlay.TARGET.dp).clip(RoundedCornerShape(8.dp)).background(background)
        .clickable(enabled = enabled, role = Role.Button, interactionSource = remember { MutableInteractionSource() }, indication = null, onClick = action)
        .semantics { contentDescription = description }, contentAlignment = Alignment.Center) {
        Glyph(icon, null, tint = tint, size = 20)
    }
}

@OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
@Composable
private fun LiveVolume(level: Float, enabled: Boolean, onChange: (Float) -> Unit) {
    androidx.compose.material3.Slider(value = level, onValueChange = onChange, enabled = enabled,
        valueRange = 0f..com.taleshernandes.harmony.media.Volumes.MAX,
        modifier = Modifier.height(LiveOverlay.TARGET.dp).semantics { contentDescription = "volume Som da live" },
        thumb = { Box(Modifier.size(16.dp).background(Foreground, androidx.compose.foundation.shape.CircleShape)) },
        track = { slider ->
            Box(Modifier.fillMaxWidth().height(4.dp).background(Surface3, androidx.compose.foundation.shape.CircleShape)) {
                Box(Modifier.fillMaxWidth(slider.value / com.taleshernandes.harmony.media.Volumes.MAX).height(4.dp)
                    .background(Accent, androidx.compose.foundation.shape.CircleShape))
            }
        })
}

@Composable
private fun FrozenPicture(progress: com.taleshernandes.harmony.media.ScreenProgress?, mid: String, modifier: Modifier) {
    val started = remember(mid) { android.os.SystemClock.elapsedRealtime() }
    var now by remember { mutableLongStateOf(started) }
    LaunchedEffect(mid) { while (true) { delay(250); now = android.os.SystemClock.elapsedRealtime() } }
    val stalled = (progress ?: com.taleshernandes.harmony.media.ScreenProgress()).stalled(now, started)
    LaunchedEffect(stalled) { com.taleshernandes.harmony.diag.Diag.log(if (stalled) "pip: picture stalled" else "pip: picture moving") }
    if (stalled) Row(modifier.padding(8.dp).height(28.dp).background(Surface1, RoundedCornerShape(8.dp)).padding(horizontal = 10.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Glyph(Glyphs.AntennaSignal, null, tint = Warning, size = 12)
        Text("Imagem parada · reconectando", color = Warning, fontSize = 12.sp, lineHeight = 16.sp, letterSpacing = (-0.24).sp, fontWeight = FontWeight.Medium)
    }
}

/**
 * The full-screen picture's zoom: shown whole ([fill] false), or filling the
 * screen times [scale] (1 to 5), moved by [panX]/[panY] in pixels.
 */
internal data class FullZoom(val fill: Boolean = false, val scale: Float = 1f, val panX: Float = 0f, val panY: Float = 0f) {
    /**
     * One move of a gesture that began at [start], with [pinch] its zoom so
     * far and [factor] this move's. Fill is a stop: a pinch in that began
     * above it ends there, and only one that began at it shows it all again.
     */
    fun step(start: FullZoom, pinch: Float, factor: Float, pan: Offset, width: Float, height: Float): FullZoom {
        if (!fill) return if (pinch > 1.15f) FullZoom(fill = true) else this
        val wanted = scale * factor
        val s = when {
            wanted >= 1f -> wanted.coerceAtMost(MAX)
            start.fill && start.scale <= 1f && pinch < 0.87f -> return FullZoom()
            else -> 1f
        }
        // Never past the picture's edge.
        val ratio = s / scale
        val maxX = width * (s - 1f) / 2f
        val maxY = height * (s - 1f) / 2f
        return FullZoom(true, s, (panX * ratio + pan.x).coerceIn(-maxX, maxX), (panY * ratio + pan.y).coerceIn(-maxY, maxY))
    }

    companion object { const val MAX = 5f }
}
