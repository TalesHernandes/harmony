package com.taleshernandes.harmony.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.text.InlineTextContent
import androidx.compose.foundation.text.appendInlineContent
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.Placeholder
import androidx.compose.ui.text.PlaceholderVerticalAlign
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.ApiError
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import java.io.File
import java.security.MessageDigest

private data class EmojiPicture(val vector: ImageVector?)

/** Public, versioned art: cache keys include the server, never a session.
 * At most 128 vectors, 8 MB of disk, and four concurrent downloads. */
private object TwemojiPictures {
    private val memory = object : LinkedHashMap<String, EmojiPicture>(128, .75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, EmojiPicture>?) = size > 128
    }
    private val stripes = Array(32) { Mutex() }
    private val downloads = Semaphore(4)
    private val diskLock = Any()
    private const val DISK_BYTES = 8L * 1024 * 1024

    fun cached(key: String): EmojiPicture? = synchronized(memory) { memory[key] }
    private fun keep(key: String, picture: EmojiPicture): EmojiPicture {
        synchronized(memory) { memory[key] = picture }
        return picture
    }

    suspend fun load(api: Api, name: String, cacheDir: File): EmojiPicture = withContext(Dispatchers.IO) {
        val key = "${api.baseUrl}/$name"
        stripes[(key.hashCode() and Int.MAX_VALUE) % stripes.size].withLock {
            cached(key)?.let { return@withLock it }
            val server = MessageDigest.getInstance("SHA-256").digest(api.baseUrl.toByteArray())
                .joinToString("") { "%02x".format(it) }
            val directory = File(cacheDir, "twemoji-15")
            val file = File(directory, "$server-$name.svg")
            try {
                val saved = synchronized(diskLock) {
                    if (file.isFile && file.length() <= TwemojiSvg.MAX_BYTES) {
                        file.setLastModified(System.currentTimeMillis())
                        file.readText(Charsets.UTF_8)
                    } else null
                }
                if (saved != null) {
                    val vector = TwemojiSvg.read(saved)
                    if (vector != null) return@withLock keep(key, EmojiPicture(vector))
                    synchronized(diskLock) { file.delete() }
                }
                val svg = downloads.withPermit { api.twemoji(name).toString(Charsets.UTF_8) }
                val vector = TwemojiSvg.read(svg)
                // Unsupported drawings are remembered too, so scrolling back
                // does not download the same unusable picture over and over.
                if (vector == null) {
                    Diag.log("twemoji: unsupported SVG")
                    return@withLock keep(key, EmojiPicture(null))
                }
                synchronized(diskLock) {
                    try {
                        if (directory.isDirectory || directory.mkdirs()) {
                            val temporary = File(directory, "${file.name}.tmp")
                            try {
                                temporary.writeText(svg, Charsets.UTF_8)
                                temporary.renameTo(file)
                            } finally { temporary.delete() }
                            val files = directory.listFiles()?.filter { it.extension == "svg" }.orEmpty()
                            var bytes = files.sumOf { it.length() }
                            for (old in files.sortedBy { it.lastModified() }) {
                                if (bytes <= DISK_BYTES) break
                                val size = old.length()
                                if (old.delete()) bytes -= size
                            }
                        }
                    } catch (_: Exception) {
                        // A full cache disk must not discard a decoded picture.
                        Diag.log("twemoji: disk cache unavailable")
                    }
                }
                keep(key, EmojiPicture(vector))
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                Diag.log("twemoji failed: ${e.javaClass.simpleName}")
                val fallback = EmojiPicture(null)
                // Missing version-15 art is permanent; a network failure is
                // retried when this emoji next enters composition.
                if (e is ApiError && e.status == 404) keep(key, fallback) else fallback
            }
        }
    }
}

/** A fixed box throughout loading and fallback; downloading never reflows text. */
@Composable
internal fun Twemoji(emoji: String, size: Dp, api: () -> Api?, modifier: Modifier = Modifier, hold: Boolean = false) {
    val client = api()
    val name = remember(emoji) { emojiFile(emoji) }
    val key = client?.let { "${it.baseUrl}/$name" }
    val directory = LocalContext.current.cacheDir
    var picture by remember(key) { mutableStateOf(key?.let(TwemojiPictures::cached)) }
    LaunchedEffect(key, hold) {
        if (!hold && picture == null && client != null) picture = TwemojiPictures.load(client, name, directory)
    }
    Box(modifier.size(size), contentAlignment = Alignment.Center) {
        val vector = picture?.vector
        if (!hold && vector != null) Image(vector, contentDescription = emoji, modifier = Modifier.fillMaxSize())
        else if (!hold && (picture != null || client == null)) {
            Text(emoji, style = BodyText.copy(fontSize = (size.value * SystemEmojiFontScale).sp,
                lineHeight = size.value.sp), maxLines = 1, softWrap = false,
                overflow = TextOverflow.Visible, textAlign = TextAlign.Center)
        }
    }
}

// Obtain Compose's annotation tag through its public helper. Annotating the
// existing ranges preserves link/style/mention offsets and the original text.
private val inlineTag = buildAnnotatedString { appendInlineContent("emoji", "e") }
    .getStringAnnotations(0, 1).single().tag

internal fun inlineEmoji(text: AnnotatedString): Pair<AnnotatedString, Map<String, String>> {
    val matches = EmojiText.matches(text.text)
    val pictures = linkedMapOf<String, String>()
    val result = buildAnnotatedString {
        append(text)
        for (match in matches) {
            val id = "twemoji-${match.range.first}"
            pictures[id] = match.value
            addStringAnnotation(inlineTag, id, match.range.first, match.range.last + 1)
        }
    }
    return result to pictures
}

@Composable
internal fun emojiContent(pictures: Map<String, String>, size: Float, api: () -> Api?): Map<String, InlineTextContent> =
    pictures.mapValues { (_, emoji) ->
        InlineTextContent(Placeholder(size.sp, size.sp, PlaceholderVerticalAlign.TextBottom)) {
            Twemoji(emoji, size.dp, api)
        }
    }

/** Other web withEmoji runs (quotes and search results) keep their own type size. */
@Composable
internal fun EmojiRun(text: String, api: () -> Api?, style: TextStyle, modifier: Modifier = Modifier,
                      color: androidx.compose.ui.graphics.Color = Foreground, maxLines: Int = Int.MAX_VALUE,
                      overflow: TextOverflow = TextOverflow.Clip) {
    val built = remember(text) { inlineEmoji(AnnotatedString(text)) }
    Text(built.first, style = style, color = color, modifier = modifier, maxLines = maxLines,
        overflow = overflow, inlineContent = emojiContent(built.second, style.fontSize.value * 1.375f, api))
}
