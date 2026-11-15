package com.taleshernandes.harmony

import com.taleshernandes.harmony.model.*
import com.taleshernandes.harmony.push.Fetched
import com.taleshernandes.harmony.push.NoticeRules
import com.taleshernandes.harmony.push.Wake
import org.junit.Assert.*
import org.junit.Test

class WakeTest {
    private fun msg(id: Long, body: String = "bora jogar?", who: String = "joao", name: String = "João",
        deleted: Boolean = false, attachments: List<Attachment> = emptyList()) = Message(
        id, "dm-1", Author(who, name), body, 1_000L * id, null, deleted, "",
        attachments, emptyList(), emptyList(), null,
    )
    private fun image(name: String = "foto.jpg") = Attachment("a", name, "image/jpeg", 10, 1, 1, "/api/files/a", false)
    private fun file(name: String) = Attachment("f", name, "application/pdf", 10, 0, 0, "/api/files/f", false)
    private val dm = Channel("dm-1", "dm", "", "", lastReadId = 800)
    private val geral = Channel("geral", "text", "geral", "", lastReadId = 800)

    // ------------------------------------------------------------ parse

    @Test fun parsesWhatTheServerSends() {
        assertEquals(Wake("dm", "dm-1", 812, "", 1700), Wake.parse(mapOf("k" to "dm", "c" to "dm-1", "m" to "812", "at" to "1700")))
        assertEquals(Wake("ring", "voz", 0, "", 0), Wake.parse(mapOf("k" to "ring", "c" to "voz")))
        assertEquals(Wake("call", "", 0, "x1", 5), Wake.parse(mapOf("k" to "call", "call" to "x1", "at" to "5")))
        // The watchman (cmd/harmony/watch.go wakeApps) sends exactly these keys.
        assertEquals(Wake("watch", "", 0, "", 9, "down"), Wake.parse(mapOf("k" to "watch", "s" to "down", "at" to "9")))
        assertEquals(Wake("watch", "", 0, "", 0, "up"), Wake.parse(mapOf("k" to "watch", "s" to "up")))
        assertNull(Wake.parse(mapOf("k" to "watch")))
        assertNull(Wake.parse(mapOf("k" to "watch", "s" to "sideways")))
    }

    @Test fun refusesWhatCannotBeActedOn() {
        assertNull(Wake.parse(emptyMap()))
        assertNull(Wake.parse(mapOf("k" to "other", "c" to "x", "m" to "1")))
        assertNull(Wake.parse(mapOf("k" to "dm", "c" to "dm-1")))
        assertNull(Wake.parse(mapOf("k" to "dm", "m" to "3")))
        assertNull(Wake.parse(mapOf("k" to "mention", "c" to "g", "m" to "abc")))
        assertNull(Wake.parse(mapOf("k" to "ring")))
        assertNull(Wake.parse(mapOf("k" to "call")))
    }

    // ------------------------------------------------------------ messages

    @Test fun aDirectMessageSaysWhoAndWhat() {
        val n = NoticeRules.message(Wake("dm", "dm-1", 812, "", 0), Fetched.Got(msg(812), dm), "tales")!!
        assertEquals("João", n.sender)
        assertEquals("bora jogar?", n.text)
        assertNull(n.conversation)
        assertEquals(812_000L, n.at)
        assertEquals(812L, n.messageId)
    }

    @Test fun aMentionSaysWhere() {
        val n = NoticeRules.message(Wake("mention", "geral", 812, "", 0), Fetched.Got(msg(812, "@tales olha"), geral), "tales")!!
        assertEquals("#geral", n.conversation)
        assertEquals("@tales olha", n.text)
    }

    @Test fun theUsernameStandsInForAMissingName() {
        val n = NoticeRules.message(Wake("dm", "dm-1", 812, "", 0), Fetched.Got(msg(812, name = ""), dm), "tales")!!
        assertEquals("joao", n.sender)
    }

    @Test fun nothingWhenAlreadyReadGoneOrMine() {
        val w = Wake("dm", "dm-1", 812, "", 0)
        assertNull(NoticeRules.message(w, Fetched.Got(msg(812), dm.copy(lastReadId = 812)), "tales"))
        assertNull(NoticeRules.message(w, Fetched.Got(msg(812, deleted = true), dm), "tales"))
        assertNull(NoticeRules.message(w, Fetched.Got(null, dm), "tales"))
        assertNull(NoticeRules.message(w, Fetched.Got(msg(812, who = "tales"), dm), "tales"))
    }

    @Test fun aFailedFetchStillSaysSomething() {
        val n = NoticeRules.message(Wake("dm", "dm-1", 812, "", 7), Fetched.Failed, "tales")!!
        assertEquals("", n.sender)
        assertEquals("Nova mensagem", n.text)
        assertEquals("dm-1", n.channelId)
        // Still tied to its message, so reading it takes the generic line away too.
        assertEquals(812L, n.messageId)
        assertEquals("Você foi mencionado", NoticeRules.message(Wake("mention", "g", 1, "", 0), Fetched.Failed, null)!!.text)
    }

    @Test fun picturesAndFilesWithoutText() {
        assertEquals("📷 Imagem", NoticeRules.preview(msg(1, "", attachments = listOf(image()))))
        assertEquals("📷 2 imagens", NoticeRules.preview(msg(1, " ", attachments = listOf(image(), image()))))
        assertEquals("📎 conta.pdf", NoticeRules.preview(msg(1, "", attachments = listOf(file("conta.pdf")))))
        assertEquals("📎 2 arquivos", NoticeRules.preview(msg(1, "", attachments = listOf(image(), file("x")))))
        assertEquals("olha", NoticeRules.preview(msg(1, "  olha ", attachments = listOf(image()))))
    }

    @Test fun aVeryLongMessageIsCut() {
        val text = NoticeRules.preview(msg(1, "a".repeat(2000)))
        assertEquals(501, text.length)
        assertTrue(text.endsWith("…"))
    }

    // ------------------------------------------------------------ rooms

    private val pending = """{"items":[
        {"tag":"mention:geral","kind":"mention","title":"João","body":"te mencionou em #geral","url":"/?c=geral","at":10},
        {"tag":"ring:voz","kind":"ring","title":"João está te chamando","body":"em 🔊 geral","url":"/?join=voz","at":1000}
    ]}"""

    @Test fun aRoomCallingNamesTheCallerFromWhatTheServerKept() {
        val n = NoticeRules.ring(Wake("ring", "voz", 0, "", 1000), pending, 5_000)!!
        assertEquals("João está te chamando", n.sender)
        assertEquals("em 🔊 geral", n.text)
        assertEquals("voz", n.channelId)
    }

    @Test fun aRoomCallNoLongerKeptIsOver() {
        assertNull(NoticeRules.ring(Wake("ring", "outra", 0, "", 1000), pending, 5_000))
        assertNull(NoticeRules.ring(Wake("ring", "voz", 0, "", 1000), """{"items":[]}""", 5_000))
        assertNull(NoticeRules.ring(Wake("ring", "voz", 0, "", 1000), "not json", 5_000))
    }

    @Test fun aRoomCallAMinuteOldIsNotShown() {
        assertNull(NoticeRules.ring(Wake("ring", "voz", 0, "", 1000), pending, 1000 + 61_000))
        assertNull(NoticeRules.ring(Wake("ring", "voz", 0, "", 1000), null, 1000 + 61_000))
    }

    @Test fun aRoomCallWithNoNetworkIsStillSaid() {
        val n = NoticeRules.ring(Wake("ring", "voz", 0, "", 1000), null, 2000)!!
        assertEquals("Alguém está te chamando numa sala", n.text)
    }
}
