package com.taleshernandes.harmony.ui

import androidx.compose.foundation.IndicationNodeFactory
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.interaction.InteractionSource
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.PressInteraction
import androidx.compose.ui.node.DrawModifierNode
import kotlinx.coroutines.launch
import androidx.compose.ui.composed
import androidx.compose.ui.graphics.drawscope.ContentDrawScope
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.rememberTransition
import androidx.compose.animation.core.MutableTransitionState
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.DialogWindowProvider
import androidx.compose.ui.platform.LocalView
import androidx.compose.runtime.SideEffect
import kotlinx.coroutines.flow.first
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.getValue
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.autofill.ContentType
import androidx.compose.ui.text.ExperimentalTextApi
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontVariation
import com.taleshernandes.harmony.R
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.contentType
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.LineHeightStyle
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import java.util.Locale

// ------------------------------------------------------------ tokens
// The design's ten colours with a meaning (design/android/README.md, Tokens).
// Each one says one thing; none is used for anything else.

internal val Background = Color(0xFF060607)
internal val Surface1 = Color(0xFF18181B)
internal val Surface2 = Color(0xFF232325)
internal val Surface3 = Color(0xFF323235)
internal val Selected = Color(0xFF27272A)
internal val Foreground = Color(0xFFFCFCFC)
internal val Muted = Color(0xFFA1A1AA)
internal val Accent = Color(0xFF0485F7)
internal val AccentSoft = Color(0x1F0485F7)
internal val AccentSoftFg = Color(0xFF61A8FC)
internal val MentionBox = Color(0x1A0485F7)
internal val Success = Color(0xFF17C964)
internal val Warning = Color(0xFFF7B750)
internal val Danger = Color(0xFFDB3B3E)
internal val DangerSoft = Color(0x26DB3B3E)
internal val DangerSoftFg = Color(0xFFF5645B)
internal val Offline = Color(0x66A1A1AA)
internal val Scrim = Color(0x99000000)

internal val Shape8 = RoundedCornerShape(8.dp)

// Paint the web hover surface immediately, inside the item's existing clip.
private data class WebPressIndication(val color: Color) : IndicationNodeFactory {
    override fun create(interactionSource: InteractionSource): androidx.compose.ui.node.DelegatableNode =
        WebPressNode(interactionSource, color)
}

private class WebPressNode(private val source: InteractionSource, private val color: Color) : Modifier.Node(), DrawModifierNode {
    private var pressed by mutableStateOf(false)

    override fun onAttach() {
        coroutineScope.launch {
            val presses = mutableSetOf<PressInteraction.Press>()
            source.interactions.collect { interaction ->
                when (interaction) {
                    is PressInteraction.Press -> presses.add(interaction)
                    is PressInteraction.Release -> presses.remove(interaction.press)
                    is PressInteraction.Cancel -> presses.remove(interaction.press)
                }
                pressed = presses.isNotEmpty()
            }
        }
    }

    override fun onDetach() { pressed = false }

    override fun ContentDrawScope.draw() {
        if (pressed) drawRoundRect(color, cornerRadius = CornerRadius(8.dp.toPx()))
        drawContent()
    }
}

internal fun Modifier.webClickable(
    enabled: Boolean = true,
    role: Role? = null,
    hover: Color = Surface2,
    onClick: () -> Unit,
): Modifier = composed {
    clickable(interactionSource = remember { MutableInteractionSource() },
        indication = remember(hover) { WebPressIndication(hover) }, enabled = enabled, role = role, onClick = onClick)
}

@OptIn(ExperimentalFoundationApi::class)
internal fun Modifier.webCombinedClickable(
    enabled: Boolean = true,
    onLongClick: () -> Unit,
    onClick: () -> Unit,
): Modifier = composed {
    combinedClickable(interactionSource = remember { MutableInteractionSource() },
        indication = remember { WebPressIndication(Surface2) }, enabled = enabled,
        onLongClick = onLongClick, onClick = onClick)
}

internal val ptBr: Locale = Locale.forLanguageTag("pt-BR")

// ------------------------------------------------------------ type
// Match CSS line boxes: Material's inherited trimming otherwise turns a
// 20/28 title into a 24.4dp box and shifts every field below it upwards.
private val WebLineHeight = LineHeightStyle(LineHeightStyle.Alignment.Center, LineHeightStyle.Trim.None)
private val WebPlatformText = PlatformTextStyle(includeFontPadding = false)

private val Tight = (-0.02).em

// The web's Inter, as one variable file: each weight is an instance of it.
@OptIn(ExperimentalTextApi::class)
internal val Inter = FontFamily(
    listOf(FontWeight.Normal, FontWeight.Medium, FontWeight.SemiBold, FontWeight.Bold).map {
        Font(R.font.inter_variable, it, variationSettings = FontVariation.Settings(FontVariation.weight(it.weight)))
    },
)
internal val TitleText = TextStyle(platformStyle = WebPlatformText, lineHeightStyle = WebLineHeight, fontFamily = Inter, fontSize = 20.sp, lineHeight = 28.sp, fontWeight = FontWeight.SemiBold, letterSpacing = (-0.015).em)
internal val BodyText = TextStyle(platformStyle = WebPlatformText, lineHeightStyle = WebLineHeight, fontFamily = Inter, fontSize = 16.sp, lineHeight = 24.sp, letterSpacing = 0.sp)
internal val ItemText = TextStyle(platformStyle = WebPlatformText, lineHeightStyle = WebLineHeight, fontFamily = Inter, fontSize = 16.sp, lineHeight = 24.sp, fontWeight = FontWeight.Medium, letterSpacing = (-0.015).em)
internal val AuthorText = TextStyle(platformStyle = WebPlatformText, lineHeightStyle = WebLineHeight, fontFamily = Inter, fontSize = 16.sp, lineHeight = 24.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 0.sp)
internal val PersonText = TextStyle(platformStyle = WebPlatformText, lineHeightStyle = WebLineHeight, fontFamily = Inter, fontSize = 16.sp, lineHeight = 24.sp, fontWeight = FontWeight.Medium, letterSpacing = (-0.015).em)
internal val MetaText = TextStyle(platformStyle = WebPlatformText, lineHeightStyle = WebLineHeight, fontFamily = Inter, fontSize = 13.sp, lineHeight = 18.sp, letterSpacing = Tight)
internal val LabelText = TextStyle(platformStyle = WebPlatformText, lineHeightStyle = WebLineHeight, fontFamily = Inter, fontSize = 12.sp, lineHeight = 16.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 0.06.em)
internal val ButtonText = TextStyle(platformStyle = WebPlatformText, lineHeightStyle = WebLineHeight, fontFamily = Inter, fontSize = 16.sp, lineHeight = 24.sp, fontWeight = FontWeight.SemiBold, letterSpacing = Tight)
internal val SmallButtonText = TextStyle(platformStyle = WebPlatformText, lineHeightStyle = WebLineHeight, fontFamily = Inter, fontSize = 15.sp, lineHeight = 20.sp, fontWeight = FontWeight.SemiBold, letterSpacing = Tight)

// ------------------------------------------------------------ pieces

/** A kit glyph at one of the kit's sizes (12, 16, 20, 24), in the colour of what holds it. */
@Composable
internal fun Glyph(icon: ImageVector, description: String?, tint: Color = Foreground, size: Int = 20) {
    Icon(icon, contentDescription = description, tint = tint, modifier = Modifier.size(size.dp))
}

/**
 * "Not hearing anybody": the headphones struck through, as the web's
 * `Deafened` draws it (a 2-wide bar at -45deg, 2 past the box on each end).
 */
@Composable
internal fun Deafened(size: Int, tint: Color, description: String? = null) {
    // The speaker with the X, as the web's volume-xmark: the owner found the slashed headphones ugly (2026-10-01).
    Glyph(Glyphs.VolumeXmark, description, tint, size)
}

/** A person's state beside their name: hearing off, else microphone off, both red as on the web. */
@Composable
internal fun MicMark(deafened: Boolean, muted: Boolean, size: Int) {
    if (deafened) Deafened(size, DangerSoftFg, "sem som")
    else if (muted) Glyph(Glyphs.MicrophoneSlash, "mudo", tint = DangerSoftFg, size = size)
}

// Native emoji ems are larger than their CSS image boxes: the 28sp thumb
// measured 31dp in the capture. Scale the font, then center a fixed-size box.
internal const val SystemEmojiFontScale = 6f / 7f

@Composable
internal fun SystemEmoji(emoji: String, size: Int) {
    Box(Modifier.size(size.dp), contentAlignment = Alignment.Center) {
        Text(emoji, style = BodyText.copy(fontSize = (size * SystemEmojiFontScale).sp,
            lineHeight = size.sp, lineHeightStyle = WebLineHeight.copy(mode = LineHeightStyle.Mode.Fixed)),
            maxLines = 1, softWrap = false, overflow = TextOverflow.Visible,
            textAlign = TextAlign.Center)
    }
}

/** An island: a surface block with radius 8, loose on the background. */
internal fun Modifier.island(color: Color = Surface1): Modifier = clip(Shape8).background(color)

/** A 48×48 target with a 20 glyph: every icon button in the app. */
@Composable
internal fun IconButton48(
    icon: ImageVector,
    description: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    tint: Color = Foreground,
    background: Color = Color.Transparent,
    enabled: Boolean = true,
    size: Int = 20,
) {
    Box(
        modifier.size(48.dp).clip(Shape8).background(background)
            .webClickable(enabled = enabled, role = Role.Button, onClick = onClick)
            .semantics { contentDescription = description },
        contentAlignment = Alignment.Center,
    ) { Glyph(icon, null, tint = tint, size = size) }
}

/** The screen header: an island of 56, horizontal padding 4. */
@Composable
internal fun Header(modifier: Modifier = Modifier, content: @Composable RowScope.() -> Unit) {
    Row(
        modifier.fillMaxWidth().height(56.dp).island().padding(horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp),
        content = content,
    )
}

/** A section label: 12/16 600, capitals, muted. */
@Composable
internal fun SectionLabel(text: String, modifier: Modifier = Modifier) {
    Text(text.uppercase(ptBr), style = LabelText, color = Muted, modifier = modifier, maxLines = 1, overflow = TextOverflow.Ellipsis)
}

/**
 * The focus ring: 2 of the colour behind the field, then 2 of accent, drawn
 * outside the field so it takes no room.
 */
internal fun Modifier.focusRing(on: Boolean, gap: Color): Modifier = drawBehind {
    if (!on) return@drawBehind
    val g = 2.dp.toPx()
    val r = 8.dp.toPx()
    drawRoundRect(Accent, Offset(-2 * g, -2 * g), Size(size.width + 4 * g, size.height + 4 * g), CornerRadius(r + 2 * g))
    drawRoundRect(gap, Offset(-g, -g), Size(size.width + 2 * g, size.height + 2 * g), CornerRadius(r + g))
}

/** A field: 48 high, padding 0 12, surface secondary, 16sp; the accent ring while it has focus. */
@Composable
internal fun Field(
    value: String,
    onValue: (String) -> Unit,
    description: String,
    modifier: Modifier = Modifier,
    placeholder: String? = null,
    enabled: Boolean = true,
    options: KeyboardOptions = KeyboardOptions.Default,
    actions: KeyboardActions = KeyboardActions.Default,
    secret: Boolean = false,
    autofill: ContentType? = null,
    focus: FocusRequester? = null,
    ringGap: Color = Surface1,
    trailing: @Composable (() -> Unit)? = null,
    height: Int = 48,
    background: Color = Surface2,
    style: TextStyle = BodyText,
) {
    var focused by remember { mutableStateOf(false) }
    BasicTextField(
        value = value,
        onValueChange = onValue,
        singleLine = true,
        enabled = enabled,
        // The password's dots are spaced 0.2em apart, as drawn.
        textStyle = style.copy(color = if (enabled) Foreground else Muted, letterSpacing = if (secret) 0.2.em else style.letterSpacing),
        cursorBrush = SolidColor(Foreground),
        keyboardOptions = options,
        keyboardActions = actions,
        visualTransformation = if (secret) PasswordVisualTransformation() else VisualTransformation.None,
        modifier = modifier.fillMaxWidth().height(height.dp)
            .then(if (focus != null) Modifier.focusRequester(focus) else Modifier)
            .onFocusChanged { focused = it.isFocused }
            .focusRing(focused, ringGap)
            .clip(Shape8).background(background)
            .semantics {
                contentDescription = description
                if (autofill != null) contentType = autofill
            },
        decorationBox = { input ->
            Row(Modifier.fillMaxHeight().padding(start = 12.dp, end = if (trailing != null) 0.dp else 12.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
                    if (value.isEmpty() && placeholder != null) {
                        Text(placeholder, style = style, color = Muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                    input()
                }
                trailing?.invoke()
            }
        },
    )
}

/** A label over its field, gap 6. */
@Composable
internal fun Labelled(label: String, modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        SectionLabel(label)
        content()
    }
}

/**
 * A button of 48: accent is "what you chose", surface secondary the other
 * one. Disabled, it goes to surface secondary with a muted label.
 */
@Composable
internal fun Button48(
    label: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    color: Color = Accent,
    enabled: Boolean = true,
    icon: ImageVector? = null,
    style: TextStyle = ButtonText,
    textColor: Color = Foreground,
    maxLines: Int = 1,
) {
    val on = enabled
    Row(
        modifier.heightIn(min = 48.dp).clip(Shape8).background(if (on) color else Surface2)
            .clickable(enabled = on, role = Role.Button, onClick = onClick)
            .padding(horizontal = 16.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        icon?.let { Glyph(it, null, tint = if (on) textColor else Muted) }
        Text(label, style = style, color = if (on) textColor else Muted, maxLines = maxLines, overflow = TextOverflow.Ellipsis)
    }
}

/** First and last word's first letter, the web's rule (Call.tsx initials). */
internal fun initials(name: String): String {
    val parts = name.trim().split(Regex("\\s+")).filter { it.isNotEmpty() && it[0].isLetterOrDigit() }
    val first = parts.firstOrNull()?.take(1) ?: "?"
    val second = if (parts.size > 1) parts.last().take(1) else ""
    return (first + second).uppercase()
}

/** Where a presence puts the dot: success online, warning away, offline otherwise. */
internal fun presenceColor(state: String?): Color = when (state) {
    "online", "in_call" -> Success
    "away" -> Warning
    else -> Offline
}

/**
 * A face: surface tertiary circle with the initials at 600. The status dot
 * sits at the lower right, cut out of the face (a ring of whatever is behind,
 * not a painted one). Talking puts the success ring inset 2.
 */
@Composable
internal fun Avatar(
    name: String,
    size: Int,
    modifier: Modifier = Modifier,
    dot: Color? = null,
    speaking: Boolean = false,
    face: Color = Surface3,
    ink: Color = Foreground,
    // Whose face this is, to look up a photo (Avatars); without it, always initials.
    username: String? = null,
) {
    val versions by com.taleshernandes.harmony.model.Avatars.versions.collectAsState()
    val version = username?.let { versions[it] }.orEmpty()
    val app = androidx.compose.ui.platform.LocalContext.current.applicationContext as? com.taleshernandes.harmony.HarmonyApp
    val photo = if (username != null && version.isNotEmpty())
        avatarPhoto({ app?.core?.currentApi() }, com.taleshernandes.harmony.model.Avatars.url(username, version), size * 3) else null
    val font = when {
        size <= 24 -> 11
        size <= 28 -> 13
        size <= 32 -> 14
        size <= 36 -> 13
        size <= 40 -> 15
        size <= 56 -> 20
        else -> 24
    }
    val dotSize = if (size >= 56) 16 else 10
    val cut = if (size >= 56) 11 else 7
    Box(modifier.size(size.dp)) {
        Box(
            Modifier.matchParentSize()
                .then(
                    if (dot == null) Modifier else Modifier
                        .graphicsLayer { compositingStrategy = CompositingStrategy.Offscreen }
                        .drawWithContent {
                            drawContent()
                            drawCircle(Color.Black, radius = cut.dp.toPx(),
                                center = Offset(this.size.width * 0.854f, this.size.height * 0.854f), blendMode = BlendMode.Clear)
                        },
                )
                .clip(CircleShape).background(face)
                .then(if (speaking) Modifier.border(2.dp, Success, CircleShape) else Modifier),
            contentAlignment = Alignment.Center,
        ) {
            if (photo != null) androidx.compose.foundation.Image(photo, null, Modifier.matchParentSize(), contentScale = androidx.compose.ui.layout.ContentScale.Crop)
            else Text(initials(name), style = MetaText.copy(fontSize = font.sp, lineHeight = (if (size >= 56) 28 else if (size <= 32) 20 else 18).sp,
                fontWeight = FontWeight.SemiBold, letterSpacing = (if (size >= 56) -0.015 else -0.02).em), color = ink, maxLines = 1)
        }
        if (dot != null) {
            val at = size * 0.854f - dotSize / 2f
            Box(Modifier.offset(at.dp, at.dp).size(dotSize.dp).clip(CircleShape).background(dot))
        }
    }
}

/** The number on a closed tab or a conversation: a pill of 24, foreground with surface text. */
@Composable
internal fun CountPill(n: Int) {
    Box(
        Modifier.height(24.dp).widthIn(min = 24.dp).clip(CircleShape).background(Foreground).padding(horizontal = 8.dp),
        contentAlignment = Alignment.Center,
    ) { Text(if (n > 99) "99+" else "$n", style = MetaText.copy(fontSize = 14.sp, lineHeight = 24.sp, fontWeight = FontWeight.SemiBold), color = Surface1) }
}

/** Mentions waiting: the @ chip of 20 in accent soft. */
@Composable
internal fun MentionChip(n: Int) {
    Row(
        Modifier.height(20.dp).clip(CircleShape).background(AccentSoft).padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        Glyph(Glyphs.At, null, tint = AccentSoftFg, size = 12)
        Text(if (n > 9) "9+" else "$n", fontSize = 12.sp, lineHeight = 20.sp, fontWeight = FontWeight.SemiBold, color = AccentSoftFg)
    }
}

/** A rail of 4: tertiary, the full part in accent. Progress and volume both. */
@Composable
internal fun Rail(fraction: Float, modifier: Modifier = Modifier) {
    Box(modifier.fillMaxWidth().height(4.dp).clip(CircleShape).background(Surface3)) {
        Box(Modifier.fillMaxWidth(fraction.coerceIn(0f, 1f)).fillMaxHeight().clip(CircleShape).background(Accent))
    }
}

/**
 * A strip: the update, a notice, the call going on. An island in the layout,
 * padding 8 8 8 12, gap 12: an icon box of 32, a title and a line, and one
 * button of 48. A progress rail under it makes it 76 instead of 64.
 */
@Composable
internal fun Strip(
    icon: ImageVector,
    title: String,
    line: String?,
    modifier: Modifier = Modifier,
    danger: Boolean = false,
    action: String? = null,
    actionMain: Boolean = true,
    onAction: () -> Unit = {},
    progress: Float? = null,
    onClick: (() -> Unit)? = null,
) {
    Column(
        modifier.fillMaxWidth().island()
            .then(if (onClick != null) Modifier.webClickable(onClick = onClick) else Modifier)
            .padding(start = 12.dp, end = 8.dp, top = 8.dp, bottom = 8.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Row(Modifier.heightIn(min = 48.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Box(Modifier.size(32.dp).clip(Shape8).background(if (danger) DangerSoft else AccentSoft), contentAlignment = Alignment.Center) {
                Glyph(icon, null, tint = if (danger) DangerSoftFg else AccentSoftFg, size = 16)
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(title, style = PersonText.copy(fontWeight = FontWeight.Medium), maxLines = 2, overflow = TextOverflow.Ellipsis)
                if (!line.isNullOrEmpty()) Text(line, style = MetaText, color = Muted, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
            if (action != null) {
                Button48(action, onAction, color = if (actionMain) Accent else Surface2, style = SmallButtonText)
            }
        }
        if (progress != null) Rail(progress, Modifier.padding(end = 4.dp))
    }
}

internal enum class PanelEdge { Bottom, Start, End }
internal val LocalPanelHosted = staticCompositionLocalOf { false }
internal val LocalPanelDismiss = staticCompositionLocalOf<((() -> Unit) -> Unit)> { { it() } }

/**
 * One heading, close target and content gutter for every app panel. The
 * handle only on the ones pulled down: on a side panel it promised a gesture
 * that is not there and took height the lying-down menu needs (owner, 18/10/2026).
 */
@Composable
private fun PanelCard(title: String, onClose: () -> Unit, modifier: Modifier, handle: Boolean,
    shape: Shape, expanded: Boolean, scroll: Boolean, handleModifier: Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(modifier.clip(shape).background(Surface1)) {
        Column(handleModifier) {
            if (handle) Box(Modifier.fillMaxWidth().height(24.dp), contentAlignment = Alignment.Center) {
                Box(Modifier.size(36.dp, 4.dp).clip(CircleShape).background(Surface3))
            } else Spacer(Modifier.height(8.dp))
            Row(Modifier.fillMaxWidth().height(48.dp).padding(horizontal = 12.dp),
                verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(title, style = TitleText, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                WebIcon(Glyphs.Xmark, "fechar", onClose, target = 32, size = 16)
            }
        }
        Column(Modifier.weight(1f, fill = expanded).fillMaxWidth().clipToBounds()
            .then(if (scroll) Modifier.verticalScroll(rememberScrollState()) else Modifier)
            .padding(12.dp), content = content)
    }
}

/** Keep the window alive through its exit, including callers that remove it on dismissal. */
@Composable
internal fun Sheet(onDismiss: () -> Unit, title: String, expanded: Boolean = false,
    scroll: Boolean = true, width: Int = 640, edge: PanelEdge = PanelEdge.Bottom,
    open: Boolean = true, content: @Composable ColumnScope.() -> Unit) {
    val transition = remember { MutableTransitionState(false) }
    var pending by remember { mutableStateOf<(() -> Unit)?>(null) }
    val dismiss by rememberUpdatedState(onDismiss)
    fun close(action: () -> Unit = { dismiss() }) {
        if (pending == null) pending = action
    }
    LaunchedEffect(pending) {
        val action = pending ?: return@LaunchedEffect
        transition.targetState = false
        snapshotFlow { transition.isIdle && !transition.currentState }.first { it }
        action()
        pending = null
    }
    if (!open && !transition.currentState && !transition.targetState && transition.isIdle) return
    Dialog(onDismissRequest = { close() }, properties = DialogProperties(
        usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        // Register both animations before starting entry in the new window.
        val animation = rememberTransition(transition, label = "panel")
        LaunchedEffect(open) { transition.targetState = open }
        val window = (LocalView.current.parent as? DialogWindowProvider)?.window
        SideEffect { window?.setDimAmount(0f); window?.setWindowAnimations(0) }
        Box(Modifier.fillMaxSize()) {
            val fade = tween<Float>(180, easing = FastOutSlowInEasing)
            animation.AnimatedVisibility(visible = { it }, enter = fadeIn(fade), exit = fadeOut(fade)) {
                Box(Modifier.fillMaxSize().background(Scrim).clickable(
                    interactionSource = remember { MutableInteractionSource() }, indication = null, onClick = { close() })
                    .semantics { contentDescription = "fechar" })
            }
            Box(Modifier.fillMaxSize().safeDrawingPadding().imePadding()
                .padding(horizontal = if (edge == PanelEdge.Bottom) 8.dp else 0.dp, vertical = 8.dp)) {
                val enter = when (edge) {
                    PanelEdge.Bottom -> slideInVertically(tween(180, easing = FastOutSlowInEasing)) { it }
                    PanelEdge.Start -> slideInHorizontally(tween(180, easing = FastOutSlowInEasing)) { -it }
                    PanelEdge.End -> slideInHorizontally(tween(180, easing = FastOutSlowInEasing)) { it }
                }
                val exit = when (edge) {
                    PanelEdge.Bottom -> slideOutVertically(tween(180, easing = FastOutSlowInEasing)) { it }
                    PanelEdge.Start -> slideOutHorizontally(tween(180, easing = FastOutSlowInEasing)) { -it }
                    PanelEdge.End -> slideOutHorizontally(tween(180, easing = FastOutSlowInEasing)) { it }
                }
                animation.AnimatedVisibility(visible = { it }, modifier = Modifier.align(when (edge) {
                    PanelEdge.Bottom -> Alignment.BottomCenter
                    PanelEdge.Start -> Alignment.CenterStart
                    PanelEdge.End -> Alignment.CenterEnd
                }), enter = enter + fadeIn(fade), exit = exit + fadeOut(fade)) {
                    val shape = when (edge) {
                        PanelEdge.Bottom -> Shape8
                        PanelEdge.Start -> RoundedCornerShape(topEnd = 8.dp, bottomEnd = 8.dp)
                        PanelEdge.End -> RoundedCornerShape(topStart = 8.dp, bottomStart = 8.dp)
                    }
                    val card = Modifier.widthIn(max = width.dp).fillMaxWidth()
                        .then(if (expanded) Modifier.fillMaxHeight() else Modifier)
                    val gesture = Modifier.pointerInput(edge) {
                        var moved = 0f
                        if (edge == PanelEdge.Bottom) detectVerticalDragGestures(
                            onDragStart = { moved = 0f }, onVerticalDrag = { change, dy ->
                                moved += dy
                                if (moved > 48.dp.toPx()) { change.consume(); close() }
                            })
                        else detectHorizontalDragGestures(onDragStart = { moved = 0f }, onHorizontalDrag = { change, dx ->
                            moved += if (edge == PanelEdge.End) dx else -dx
                            if (moved > 48.dp.toPx()) { change.consume(); close() }
                        })
                    }
                    CompositionLocalProvider(LocalPanelHosted provides true, LocalPanelDismiss provides { action -> close(action) }) {
                        PanelCard(title, { close() }, card.then(if (edge != PanelEdge.Bottom) gesture else Modifier), edge == PanelEdge.Bottom,
                            shape, expanded, scroll, if (edge == PanelEdge.Bottom) gesture else Modifier, content)
                    }
                }
            }
        }
    }
}

/** A line of a sheet: 56, padding 0 12, an optional 20 glyph, 16/20 500. */
@Composable
internal fun SheetRow(
    label: String,
    onClick: () -> Unit,
    icon: ImageVector? = null,
    tint: Color = Foreground,
    iconTint: Color = tint,
    selected: Boolean = false,
    trailing: @Composable (() -> Unit)? = null,
) {
    Row(
        Modifier.fillMaxWidth().height(56.dp).clip(Shape8).background(if (selected) Selected else Color.Transparent)
            .webClickable(role = Role.Button, onClick = onClick).padding(horizontal = 12.dp)
            .semantics { contentDescription = label },
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        icon?.let { Glyph(it, null, tint = iconTint) }
        Text(label, style = ItemText, color = tint, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
        trailing?.invoke()
    }
}

/** The photo for this address, null while it loads or when it cannot (the face then shows initials). */
@Composable
private fun avatarPhoto(api: () -> com.taleshernandes.harmony.net.Api?, url: String, maxPx: Int): androidx.compose.ui.graphics.ImageBitmap? {
    var bmp by remember(url) { mutableStateOf(Pictures.cached(url, maxPx)) }
    androidx.compose.runtime.LaunchedEffect(url) {
        if (bmp == null) api()?.let { bmp = Pictures.load(it, url, maxPx) }
    }
    return bmp
}
