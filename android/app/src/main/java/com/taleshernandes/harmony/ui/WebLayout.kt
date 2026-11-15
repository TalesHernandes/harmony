package com.taleshernandes.harmony.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

// CSS pixels in specs-fiel/web are dp. Targets vary by context on the web:
// 44 in the header, 36 in the composer, 32 in panels, 48 in a call.
@Composable
internal fun WebIcon(icon: ImageVector, description: String, onClick: () -> Unit,
    modifier: Modifier = Modifier, target: Int = 32, size: Int = 16,
    tint: Color = Foreground, background: Color = Selected, enabled: Boolean = true) {
    Box(modifier.size(target.dp).clip(Shape8).background(background)
        .webClickable(enabled = enabled, role = Role.Button,
            hover = if (background == Surface2) Surface3 else Surface2, onClick = onClick)
        .semantics { contentDescription = description }, contentAlignment = Alignment.Center) {
        Glyph(icon, null, tint, size)
    }
}

@Composable
internal fun WebButton(label: String, onClick: () -> Unit, modifier: Modifier = Modifier,
    height: Int = 44, enabled: Boolean = true, background: Color = Selected,
    icon: ImageVector? = null, centered: Boolean = true, padding: Int = 16, gap: Int = 8,
    style: TextStyle = MetaText.copy(fontWeight = FontWeight.SemiBold)) {
    Row(modifier.height(height.dp).clip(Shape8).background(background)
        .webClickable(enabled = enabled, role = Role.Button,
            hover = if (background == Surface2) Surface3 else Surface2, onClick = onClick)
        .padding(horizontal = padding.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(gap.dp, if (centered) Alignment.CenterHorizontally else Alignment.Start)) {
        icon?.let { Glyph(it, null, size = 20) }
        Text(label, style = style,
            color = if (enabled) Foreground else Muted, maxLines = 1)
    }
}

/** Full-width bar, with its surface continuing behind the Android status bar. */
@Composable
internal fun ConversationHeader(name: String, kind: String, onList: () -> Unit,
    onSearch: () -> Unit, onPeople: () -> Unit, onCall: (() -> Unit)? = null, peopleOpen: Boolean = false, onPins: (() -> Unit)? = null) {
    val configuration = androidx.compose.ui.platform.LocalConfiguration.current
    // In landscape the 44dp targets need no extra vertical space in a 64dp row.
    val height = if (configuration.screenWidthDp > configuration.screenHeightDp) 44.dp else 64.dp
    Column(Modifier.fillMaxWidth().background(Surface1).windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top))) {
        Row(Modifier.fillMaxWidth().height(height).padding(horizontal = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
            WebIcon(Glyphs.Bars, "canais", onList, target = 44, size = 22, tint = Muted, background = Color.Transparent)
            // The kind glyph and the name as one piece, 6 apart: with the
            // row's 12 and a 28 box around a 22 glyph the # floated away from
            // the name (owner, 02/10/2026).
            Row(Modifier.weight(1f), horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
                if (kind != "dm") Glyph(if (kind == "voice") Glyphs.Headphones else Glyphs.Hashtag, null, Muted, 22)
                Text(name, style = TitleText, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
            }
            onPins?.let { WebIcon(Glyphs.Pin, "Mensagens fixadas", it, target = 44, size = 18, tint = Muted, background = Color.Transparent) }
            WebIcon(Glyphs.Magnifier, "procurar", onSearch, target = 44, size = 18, tint = Muted, background = Color.Transparent)
            onCall?.let { WebIcon(Glyphs.Handset, "ligar", it, target = 44, size = 18, tint = Muted, background = Color.Transparent) }
            WebIcon(Glyphs.Persons, "pessoas", onPeople, target = 44, size = 18, tint = Muted,
                background = if (peopleOpen) Color(0x8027272A) else Color.Transparent)
        }
    }
}

/** In-layout panels keep the conversation visible and give Back to the topmost panel. */
@Composable
internal fun WebModal(onClose: () -> Unit, width: Int = 360, title: String, content: @Composable ColumnScope.() -> Unit) {
    Sheet(onClose, title = title, width = width, content = content)
}

/** Insets are measured by Android; hidden landscape status bars add zero height. */
@Composable
internal fun Modifier.conversationInsets(): Modifier = windowInsetsPadding(
    WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom))

internal val ColumnLabelText = LabelText.copy(fontSize = 13.sp, lineHeight = 18.sp, letterSpacing = 0.78.sp)

@Composable
internal fun WebSearchField(value: String, onValue: (String) -> Unit, description: String, placeholder: String,
    height: Int = 48, size: Int = 20, gap: Int = 12, style: TextStyle = BodyText, clearLabel: String? = null) {
    BasicTextField(value, onValue, singleLine = true, textStyle = style.copy(color = Foreground), cursorBrush = SolidColor(Foreground),
        modifier = Modifier.fillMaxWidth().height(height.dp).clip(Shape8).background(Surface2)
            .semantics { contentDescription = description }, decorationBox = { input ->
            Row(Modifier.fillMaxSize().padding(horizontal = 12.dp), verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(gap.dp)) {
                Glyph(Glyphs.Magnifier, null, size = size)
                Box(Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
                    if (value.isEmpty()) Text(placeholder, style = style, color = Muted, maxLines = 1, overflow = TextOverflow.Clip)
                    input()
                }
                if (value.isNotEmpty() && clearLabel != null) WebIcon(Glyphs.Xmark, clearLabel, { onValue("") }, size = 12)
            }
        })
}
