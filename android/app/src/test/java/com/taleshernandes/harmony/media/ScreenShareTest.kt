package com.taleshernandes.harmony.media

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.webrtc.RtpParameters
import org.webrtc.MediaStreamTrack

class ScreenShareTest {
    @Test fun h264WinsRegardlessOfEncoderOrder() {
        assertEquals("H264", ScreenShare.codec(listOf("VP9", "VP8", "H264")))
        assertEquals("H264", ScreenShare.codec(listOf("h264", "AV1")))
    }

    @Test fun vp8IsTheFallbackWithoutH264AndAv1IsNeverSelected() {
        assertEquals("VP8", ScreenShare.codec(listOf("AV1", "VP9", "VP8")))
        assertNull(ScreenShare.codec(listOf("AV1", "VP9")))
        assertNull(ScreenShare.codec(emptyList()))
    }

    @Test fun widescreenAndTallPhonesKeepTheirShapeAndRotate() {
        assertEquals(ScreenShare.Size(1280, 720), ScreenShare.size(1920, 1080))
        assertEquals(ScreenShare.Size(720, 1280), ScreenShare.size(1080, 1920))
        assertEquals(ScreenShare.Size(576, 1280), ScreenShare.size(1080, 2400))
        assertEquals(ScreenShare.Size(1280, 576), ScreenShare.size(2400, 1080))
        assertEquals(ScreenShare.Size(640, 480), ScreenShare.size(640, 480))
    }

    @Test fun dimensionsAreEvenAndDoNotUpscaleSmallContent() {
        val size = ScreenShare.size(1001, 701)
        assertEquals(ScreenShare.Size(1000, 700), size)
        assertEquals(ScreenShare.Size(2, 2), ScreenShare.size(1, 1))
    }

    @Test fun screenParametersMatchWeb720p60AndKeepFrameRate() {
        val p = parameters(1)
        ScreenShare.tune(p)
        assertEquals(12_000_000, p.encodings.single().maxBitrateBps)
        assertEquals(60, p.encodings.single().maxFramerate)
        assertEquals(RtpParameters.DegradationPreference.MAINTAIN_FRAMERATE, p.degradationPreference)
        assertNull(p.encodings.single().rid)
        assertTrue(p.encodings.single().active)
    }

    @Test fun choicesUseWebCeilingsAndAutomaticPhoneDefaults() {
        val expected = mapOf(
            "1080" to listOf(15_000_000, 12_000_000, 9_000_000),
            "720" to listOf(12_000_000, 9_000_000, 6_000_000),
            "480" to listOf(6_000_000, 6_000_000, 6_000_000))
        for ((quality, ceilings) in expected) for ((i, fps) in listOf("60", "30", "15").withIndex()) {
            val options = ScreenOptions(quality, fps, false)
            val p = parameters(1)
            ScreenShare.tune(p, options)
            assertEquals(ceilings[i], p.encodings.single().maxBitrateBps)
            assertEquals(fps.toInt(), p.encodings.single().maxFramerate)
        }
        assertEquals(720, ScreenOptions().height)
        assertEquals(60, ScreenOptions().framesPerSecond)
        assertEquals(12_000_000, ScreenOptions().maxBps)
        assertTrue(ScreenOptions().sound)
        assertEquals(ScreenShare.Size(1920, 1080), ScreenShare.size(2560, 1440, 1080))
        assertEquals(ScreenShare.Size(480, 852), ScreenShare.size(1080, 1920, 480))
    }

    @Test(expected = IllegalArgumentException::class) fun refusesUnknownQuality() { ScreenOptions("bad") }
    @Test(expected = IllegalArgumentException::class) fun refusesUnknownFrameRate() { ScreenOptions(fps = "120") }

    @Test(expected = IllegalStateException::class)
    fun refusesUnexpectedSimulcastInsteadOfLeavingUncappedLayers() {
        ScreenShare.tune(parameters(3))
    }

    @Test(expected = IllegalStateException::class)
    fun requiresAnEncodingFromTheServerSlot() {
        ScreenShare.tune(parameters(0))
    }

    @Test fun reconnectPublishesInTheNewSessionOnce() {
        val choice = ScreenChoice()
        val turn = choice.choose(true)
        assertFalse(choice.shouldPublish(null, "old"))
        assertTrue(choice.accepts(turn)) // capture survives the gap
        assertTrue(choice.shouldPublish("new", "old"))
        assertFalse(choice.shouldPublish("new", "new"))
    }

    @Test fun offDuringReconnectCancelsChoiceAndLateServiceReply() {
        val choice = ScreenChoice()
        val pending = choice.choose(true)
        choice.choose(false) // no session is needed to switch off
        assertFalse(choice.wanted)
        assertFalse(choice.accepts(pending))
        assertFalse(choice.shouldPublish(null, "old"))
        assertFalse(choice.shouldPublish("reconnected", null))
    }

    @Test fun oldStopOrPublishFailureCannotActOnTheNextCapture() {
        val choice = ScreenChoice()
        val old = choice.choose(true)
        val stopped = choice.choose(false)
        val current = choice.choose(true)
        assertFalse(choice.accepts(old))
        assertFalse(choice.accepts(stopped))
        assertTrue(choice.accepts(current))
        assertTrue(choice.shouldPublish("new", null))
    }

    // The SFU's screen order, with every profile/RTX attribute left intact.
    private val screenSection = """m=video 9 UDP/TLS/RTP/SAVPF 102 103 106 107 98 99 100 101 96 97
        |a=mid:screen-slot
        |a=recvonly
        |a=rtpmap:102 H264/90000
        |a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42002a
        |a=rtpmap:103 rtx/90000
        |a=fmtp:103 apt=102
        |a=rtpmap:106 H264/90000
        |a=fmtp:106 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e02a
        |a=rtpmap:107 rtx/90000
        |a=fmtp:107 apt=106
        |a=rtpmap:98 VP9/90000
        |a=fmtp:98 profile-id=0
        |a=rtpmap:99 rtx/90000
        |a=fmtp:99 apt=98
        |a=rtpmap:100 VP9/90000
        |a=fmtp:100 profile-id=2
        |a=rtpmap:101 rtx/90000
        |a=fmtp:101 apt=100
        |a=rtpmap:96 VP8/90000
        |a=rtpmap:97 rtx/90000
        |a=fmtp:97 apt=96
        |a=ice-ufrag:example
        |a=rtcp-fb:* transport-cc
        |a=ssrc:1234 cname:example
        |""".trimMargin()

    private val screenLine = screenSection.lineSequence().first()

    @Test fun emulatorVp8BecomesFirstBeforeRemoteDescriptionWithoutRemovingCodecs() {
        val wanted = "m=video 9 UDP/TLS/RTP/SAVPF 96 97 102 103 106 107 98 99 100 101"
        val result = ScreenShare.preferCodec(screenSection, "screen-slot", "VP8")
        assertEquals(screenSection.replace(screenLine, wanted), result)
        assertEquals(result, ScreenShare.preferCodec(result, "screen-slot", "VP8"))
    }

    @Test fun hardwareH264BecomesFirstEvenWhenTheServerOffersVp9First() {
        val vp9First = "m=video 9 UDP/TLS/RTP/SAVPF 98 99 96 97 106 107 102 103 100 101"
        val input = screenSection.replace(screenLine, vp9First)
        val wanted = "m=video 9 UDP/TLS/RTP/SAVPF 106 107 102 103 98 99 96 97 100 101"
        val chosen = ScreenShare.codec(listOf("VP8", "VP9", "H264"))!!
        assertEquals(input.replace(vp9First, wanted), ScreenShare.preferCodec(input, "screen-slot", chosen))
    }

    @Test fun outgoingMidAloneChangesEvenWithCamAndAnIncomingScreenInTheBundle() {
        val before = "v=0\r\na=group:BUNDLE mic camera screen-slot received\r\n" +
            "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:mic\r\na=rtpmap:111 opus/48000/2\r\n" +
            "m=video 9 UDP/TLS/RTP/SAVPF 98 96\r\na=mid:camera\r\na=rtpmap:98 VP9/90000\r\na=rtpmap:96 VP8/90000\r\n"
        val after = "m=video 9 UDP/TLS/RTP/SAVPF 102 98 96\r\na=mid:received\r\na=sendonly\r\na=rtpmap:102 H264/90000\r\n"
        val screen = screenSection.replace("\n", "\r\n")
        val result = ScreenShare.preferCodec(before + screen + after, "screen-slot", "VP8")
        assertEquals(before + ScreenShare.preferCodec(screen, "screen-slot", "VP8") + after, result)
        assertFalse(result.replace("\r\n", "").contains("\n"))
    }

    @Test fun anAlreadyPreferredH264OfferIsExactlyUnchanged() {
        assertEquals(screenSection, ScreenShare.preferCodec(screenSection, "screen-slot", "H264"))
    }

    @Test fun rtxAssociationIsReadFromAptInsteadOfPayloadAdjacency() {
        val input = "m=video 9 UDP/TLS/RTP/SAVPF 98 120 110\n" +
            "a=mid:new-screen\na=rtpmap:98 VP9/90000\na=rtpmap:120 RTX/90000\n" +
            "a=fmtp:120 rtx-time=200; apt=110\na=rtpmap:110 vp8/90000\n"
        assertEquals(input.replace("98 120 110", "110 120 98"), ScreenShare.preferCodec(input, "new-screen", "VP8"))
    }

    @Test fun anUnknownPayloadIsPreservedWithoutInventingAttributesOrRenumbering() {
        val input = screenSection.replace(screenLine, "$screenLine 122")
        val result = ScreenShare.preferCodec(input, "screen-slot", "VP8")
        assertTrue(result.lineSequence().first().endsWith(" 122"))
        assertEquals(input.substringAfter('\n'), result.substringAfter('\n'))
    }

    @Test(expected = IllegalStateException::class)
    fun missingChosenCodecRefusesTheScreenRatherThanSilentlySendingVp9() {
        val input = "m=video 9 UDP/TLS/RTP/SAVPF 98\na=mid:screen-slot\na=rtpmap:98 VP9/90000\n"
        ScreenShare.preferCodec(input, "screen-slot", "VP8")
    }

    @Test(expected = IllegalStateException::class)
    fun missingOutgoingMidDoesNotRewriteAnotherPersonsVideo() {
        ScreenShare.preferCodec(screenSection, "new-screen", "VP8")
    }

    @Test(expected = IllegalStateException::class)
    fun aScreenMidMustReferToVideo() {
        ScreenShare.preferCodec(screenSection.replace("m=video", "m=audio"), "screen-slot", "VP8")
    }

    @Test(expected = IllegalArgumentException::class)
    fun vp9CannotBeRequestedAsTheScreenCodec() {
        ScreenShare.preferCodec(screenSection, "screen-slot", "VP9")
    }

    @Test fun senderCodecChecksTheEffectiveVp8AndH264Payloads() {
        val vp8 = parameters(1, listOf(rtpCodec("VP8", 96), rtpCodec("VP9", 98)))
        val h264 = parameters(1, listOf(rtpCodec("H264", 106), rtpCodec("VP8", 96)))
        assertEquals(96, ScreenShare.senderCodec(vp8, "VP8").payloadType)
        assertEquals(106, ScreenShare.senderCodec(h264, "H264").payloadType)
    }

    @Test(expected = IllegalStateException::class)
    fun aVp8PreferenceDoesNotProveTheSenderIsNotUsingVp9() {
        val p = parameters(1, listOf(rtpCodec("VP9", 98), rtpCodec("VP8", 96)))
        ScreenShare.senderCodec(p, "VP8")
    }

    @Test(expected = IllegalStateException::class)
    fun hardwareH264MustAlsoBeTheActualSendCodec() {
        val p = parameters(1, listOf(rtpCodec("VP9", 98), rtpCodec("H264", 106)))
        ScreenShare.senderCodec(p, "H264")
    }

    @Test(expected = IllegalStateException::class)
    fun noSenderCodecIsARefusalRatherThanAnUnverifiedPreference() {
        ScreenShare.senderCodec(parameters(1), "H264")
    }

    private fun rtpCodec(name: String, pt: Int): RtpParameters.Codec {
        val constructor = RtpParameters.Codec::class.java.declaredConstructors.single()
        constructor.isAccessible = true
        return constructor.newInstance(pt, name, MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO,
            90000, null, emptyMap<String, String>()) as RtpParameters.Codec
    }

    // RTP data objects have no JNI work, but their outer constructor is package
    // private. Use it directly rather than mocking the values tune() mutates.
    private fun parameters(count: Int, codecs: List<RtpParameters.Codec> = emptyList()): RtpParameters {
        val constructor = RtpParameters::class.java.declaredConstructors.single()
        constructor.isAccessible = true
        return constructor.newInstance("test", null, null, emptyList<Any>(),
            List(count) { RtpParameters.Encoding(null, true, null) }, codecs) as RtpParameters
    }
}
