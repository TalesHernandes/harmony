package com.taleshernandes.harmony.ui

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathFillType
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.unit.dp
import org.w3c.dom.Element
import org.w3c.dom.Node
import org.xml.sax.InputSource
import org.xml.sax.helpers.DefaultHandler
import java.io.StringReader
import javax.xml.parsers.DocumentBuilderFactory

/** Only the SVG vocabulary actually used by Twemoji 15. Reject the whole
 * picture when anything is unsupported: a missing layer is a wrong emoji. */
internal object TwemojiSvg {
    const val MAX_BYTES = 256 * 1024
    private val numbers = Regex("[\\s,]+")
    private data class Paint(val color: Color = Color.Black, val alpha: Float = 1f,
                             val rule: PathFillType = PathFillType.NonZero)
    private data class Transform(val angle: Float = 0f, val x: Float = 0f, val y: Float = 0f,
                                 val sx: Float = 1f, val sy: Float = 1f,
                                 val dx: Float = 0f, val dy: Float = 0f)

    fun read(svg: String): ImageVector? = try {
        require(svg.toByteArray(Charsets.UTF_8).size <= MAX_BYTES)
        // Android and desktop JAXP differ in which security features they
        // expose. Refuse declarations before parsing, and never resolve an entity.
        require(!svg.contains("<!DOCTYPE", true) && !svg.contains("<!ENTITY", true))
        val factory = DocumentBuilderFactory.newInstance().apply {
            isNamespaceAware = true
            isExpandEntityReferences = false
        }
        val parser = factory.newDocumentBuilder().apply {
            setEntityResolver { _, _ -> throw IllegalArgumentException("External entity") }
            setErrorHandler(DefaultHandler())
        }
        val root = parser.parse(InputSource(StringReader(svg))).documentElement
        require(root.localName == "svg")
        attributes(root, setOf("viewBox", "space"))
        val box = floats(root.getAttribute("viewBox"))
        require(box.size == 4 && box[0] == 0f && box[1] == 0f && box[2] > 0f && box[3] > 0f)
        val builder = ImageVector.Builder("twemoji", box[2].dp, box[3].dp, box[2], box[3])
        children(root) { draw(it, Paint(), builder, 0) }
        builder.build()
    } catch (_: Exception) {
        null
    }

    private fun attributes(element: Element, allowed: Set<String>) {
        for (i in 0 until element.attributes.length) {
            val attr = element.attributes.item(i)
            if (attr.namespaceURI == "http://www.w3.org/2000/xmlns/") continue
            require(attr.namespaceURI == null ||
                (attr.namespaceURI == "http://www.w3.org/XML/1998/namespace" && attr.localName == "space"))
            require((attr.localName ?: attr.nodeName) in allowed)
        }
        require(element.namespaceURI == null || element.namespaceURI == "http://www.w3.org/2000/svg")
    }

    private fun children(element: Element, visit: (Element) -> Unit) {
        for (i in 0 until element.childNodes.length) {
            val node = element.childNodes.item(i)
            when (node.nodeType) {
                Node.ELEMENT_NODE -> visit(node as Element)
                Node.TEXT_NODE, Node.CDATA_SECTION_NODE -> require(node.textContent.isBlank())
                Node.COMMENT_NODE -> Unit
                else -> error("Unsupported XML node")
            }
        }
    }

    private fun draw(element: Element, inherited: Paint, builder: ImageVector.Builder, depth: Int) {
        require(depth < 32)
        val geometry = when (element.localName) {
            "g" -> emptySet()
            "path" -> setOf("d")
            "circle" -> setOf("cx", "cy", "r")
            "ellipse" -> setOf("cx", "cy", "rx", "ry")
            else -> error("Unsupported SVG element")
        }
        attributes(element, geometry + setOf("fill", "fill-opacity", "fill-rule", "clip-rule", "transform", "opacity"))
        // Group opacity is compositing, not inherited fill alpha; do not
        // approximate it by darkening each overlapping child independently.
        require(element.localName != "g" || !element.hasAttribute("opacity"))
        val paint = Paint(
            if (element.hasAttribute("fill")) color(element.getAttribute("fill")) else inherited.color,
            if (element.hasAttribute("fill-opacity")) alpha(element.getAttribute("fill-opacity")) else inherited.alpha,
            if (element.hasAttribute("fill-rule")) rule(element.getAttribute("fill-rule")) else inherited.rule,
        )
        if (element.hasAttribute("clip-rule")) rule(element.getAttribute("clip-rule"))
        val t = transform(element.getAttribute("transform"))
        builder.addGroup(rotate = t.angle, pivotX = t.x, pivotY = t.y, scaleX = t.sx, scaleY = t.sy,
            translationX = t.dx, translationY = t.dy)
        if (element.localName == "g") {
            children(element) { draw(it, paint, builder, depth + 1) }
        } else {
            val path = when (element.localName) {
                "path" -> element.getAttribute("d").also { require(it.isNotBlank()) }
                "circle" -> ellipse(element, number(element, "r"), number(element, "r"))
                else -> ellipse(element, number(element, "rx"), number(element, "ry"))
            }
            children(element) { error("Shape with children") }
            builder.addPath(PathParser().parsePathString(path).toNodes(), pathFillType = paint.rule,
                fill = SolidColor(paint.color), fillAlpha = paint.alpha *
                    if (element.hasAttribute("opacity")) alpha(element.getAttribute("opacity")) else 1f)
        }
        builder.clearGroup()
    }

    private fun ellipse(e: Element, rx: Float, ry: Float): String {
        require(rx > 0f && ry > 0f)
        val cx = number(e, "cx")
        val cy = number(e, "cy")
        return "M ${cx - rx} $cy A $rx $ry 0 1 0 ${cx + rx} $cy A $rx $ry 0 1 0 ${cx - rx} $cy Z"
    }

    private fun color(text: String): Color {
        if (text == "none") return Color.Transparent
        // Four CSS names occur in the scanned originals, including the
        // Indian flag's navy. CSS green is #008000, not bright #00ff00.
        when (text) {
            "green" -> return Color(0xFF008000)
            "navy" -> return Color(0xFF000080)
            "red" -> return Color(0xFFFF0000)
            "azure" -> return Color(0xFFF0FFFF)
        }
        require(text.matches(Regex("#[0-9a-fA-F]{3}([0-9a-fA-F]{3})?")))
        val hex = text.drop(1).let { if (it.length == 3) it.flatMap { c -> listOf(c, c) }.joinToString("") else it }
        return Color((0xff000000L or hex.toLong(16)).toInt())
    }
    private fun number(e: Element, name: String): Float = finite(e.getAttribute(name))
    private fun finite(s: String): Float = s.toFloat().also { require(it.isFinite()) }
    private fun floats(s: String): List<Float> = s.trim().split(numbers).map(::finite)
    private fun alpha(s: String): Float = finite(s).also { require(it in 0f..1f) }
    private fun rule(s: String): PathFillType = when (s) {
        "evenodd" -> PathFillType.EvenOdd
        "nonzero" -> PathFillType.NonZero
        else -> error("Unsupported fill rule")
    }
    private fun transform(s: String): Transform {
        if (s.isEmpty()) return Transform()
        val match = Regex("(rotate|matrix)\\(([^()]*)\\)").matchEntire(s) ?: error("Unsupported transform")
        val n = floats(match.groupValues[2])
        return when (match.groupValues[1]) {
            "rotate" -> {
                require(n.size == 1 || n.size == 3)
                Transform(angle = n[0], x = n.getOrElse(1) { 0f }, y = n.getOrElse(2) { 0f })
            }
            else -> {
                // Compose groups support scale/translation. A skew matrix
                // cannot be represented faithfully, so fall back for that file.
                require(n.size == 6 && n[1] == 0f && n[2] == 0f)
                Transform(sx = n[0], sy = n[3], dx = n[4], dy = n[5])
            }
        }
    }
}
