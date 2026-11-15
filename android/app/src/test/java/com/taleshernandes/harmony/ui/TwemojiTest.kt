package com.taleshernandes.harmony.ui

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathFillType
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.VectorGroup
import androidx.compose.ui.graphics.vector.VectorPath
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import org.junit.Assert.*
import org.junit.Test

class TwemojiTest {
    private fun svg(body: String) = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 36 36">$body</svg>"""
    private fun path(root: VectorGroup): VectorPath {
        val child = root[0]
        return if (child is VectorPath) child else path(child as VectorGroup)
    }

    @Test fun pathUsesItsOriginalColorAndGeometry() {
        val picture = requireNotNull(TwemojiSvg.read(svg("""<path fill="#ffcc4d" d="M0 0L36 0L36 36Z"/>""")))
        assertEquals(36f, picture.viewportWidth, 0f)
        assertEquals(SolidColor(Color(0xFFFFCC4D)), path(picture.root).fill)
        assertEquals(4, path(picture.root).pathData.size)
    }

    @Test fun circleBecomesTwoArcsAndRemainsFilled() {
        val picture = requireNotNull(TwemojiSvg.read(svg("""<circle cx="18" cy="18" r="12" fill="#fff"/>""")))
        val circle = path(picture.root)
        assertEquals(SolidColor(Color.White), circle.fill)
        assertEquals(4, circle.pathData.size)
        assertEquals(PathFillType.NonZero, circle.pathFillType)
    }

    @Test fun groupsInheritPaintAndShapesKeepOpacityAndRotation() {
        val picture = requireNotNull(TwemojiSvg.read(svg("""<g fill="#abc" fill-rule="evenodd" fill-opacity=".5"><ellipse cx="18" cy="18" rx="10" ry="5" opacity=".4" transform="rotate(-30 18 18)"/></g>""")))
        val group = picture.root[0] as VectorGroup
        val rotated = group[0] as VectorGroup
        assertEquals(-30f, rotated.rotation, 0f)
        assertEquals(18f, rotated.pivotX, 0f)
        assertEquals(.2f, path(picture.root).fillAlpha, .0001f)
        assertEquals(PathFillType.EvenOdd, path(picture.root).pathFillType)
    }

    @Test fun cssColorNamesInTheOriginalFlagsKeepTheirExactColors() {
        for ((name, color) in listOf("green" to Color(0xFF008000), "navy" to Color(0xFF000080),
            "red" to Color(0xFFFF0000), "azure" to Color(0xFFF0FFFF))) {
            val picture = requireNotNull(TwemojiSvg.read(svg("""<path fill="$name" d="M0 0L1 1Z"/>""")))
            assertEquals(SolidColor(color), path(picture.root).fill)
        }
    }

    @Test fun unknownElementRejectsTheEntirePicture() {
        assertNull(TwemojiSvg.read(svg("""<path d="M0 0L1 1Z"/><rect width="36" height="36"/>""")))
    }

    @Test fun unsupportedEffectsNeverProduceAnIncompletePicture() {
        for (body in listOf(
            """<defs><clipPath id="a"><circle cx="18" cy="18" r="12"/></clipPath></defs>""",
            """<path d="M0 0L1 1Z" stroke="#abc"/>""",
            """<circle cx="18" cy="18" r="12" transform="matrix(1 .1 0 1 0 0)"/>""",
            """<g opacity=".5"><path d="M0 0L1 1Z"/></g>""",
            """<path d="M0 0L1 1Z" style="fill:red"/>""",
        )) assertNull(body, TwemojiSvg.read(svg(body)))
    }

    @Test fun malformedOrExternalXmlFallsBack() {
        assertNull(TwemojiSvg.read("<svg>"))
        assertNull(TwemojiSvg.read("""<!DOCTYPE svg [<!ENTITY test SYSTEM "file:///unavailable">]>""" + svg("&test;")))
        assertNull(TwemojiSvg.read(svg("""<circle cx="NaN" cy="18" r="12"/>""")))
    }

    @Test fun fileNamesFollowTheWebVariationSelectorRule() {
        assertEquals("2764", emojiFile("❤️"))
        assertEquals("2764-fe0f-200d-1f525", emojiFile("❤️‍🔥"))
        assertEquals("1f44d-1f3fd", emojiFile("👍🏽"))
        assertEquals("1f1e7-1f1f7", emojiFile("🇧🇷"))
        assertEquals("31-20e3", emojiFile("1️⃣"))
    }

    @Test fun matcherKeepsJoinedSequencesFlagsKeycapsAndTextDefaults() {
        val emoji = listOf("👍🏽", "🇧🇷", "1️⃣", "❤️‍🔥", "👨‍👩‍👧", "©️")
        assertEquals(emoji, EmojiText.matches("© ® ™ ‼ " + emoji.joinToString(" ")).map { it.value })
        assertEquals(emptyList<String>(), EmojiText.matches("ordinary words 123").map { it.value })
    }

    @Test fun inlinePicturesKeepTheOriginalTextAndMentionOffsets() {
        val original = buildAnnotatedString {
            append("👍🏽 @ana ❤️‍🔥")
            addStyle(SpanStyle(color = Color.Blue), 5, 9)
        }
        val (text, pictures) = inlineEmoji(original)
        assertEquals(original.text, text.text)
        assertEquals(original.spanStyles, text.spanStyles)
        assertEquals(listOf("👍🏽", "❤️‍🔥"), pictures.values.toList())
        assertEquals(listOf(0, 10), text.getStringAnnotations(0, text.length).map { it.start })
    }
}
