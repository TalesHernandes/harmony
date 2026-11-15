package com.taleshernandes.harmony.media

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.os.Build
import android.os.Process
import com.taleshernandes.harmony.diag.Diag
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/** Shares the existing projection, never consumes its consent Intent again. */
internal class ScreenAudio(private val failed: (ScreenAudio) -> Unit) {
    val mix = PlaybackMix()
    private val control = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "screen-audio-control") }
    @Volatile private var running = false
    private var recorder: AudioRecord? = null
    private var reader: Thread? = null
    private var closed = false

    fun start(projection: MediaProjection): Boolean {
        if (Build.VERSION.SDK_INT < 29) {
            Diag.log("screen audio: playback capture requires Android 10; screen and microphone continue")
            control.shutdown()
            return false
        }
        try {
            val config = AudioPlaybackCaptureConfiguration.Builder(projection)
                .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                .addMatchingUsage(AudioAttributes.USAGE_GAME)
                .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                .excludeUid(Process.myUid())
                .build()
            var chosen: AudioRecord? = null
            // Prefer precisely the microphone's format. Fall back to formats
            // accepted by the device, then convert the returned PCM in software.
            for (rate in intArrayOf(48000, 44100)) {
                for (mask in intArrayOf(AudioFormat.CHANNEL_IN_MONO, AudioFormat.CHANNEL_IN_STEREO)) {
                    if (chosen != null) break
                    val min = AudioRecord.getMinBufferSize(rate, mask, AudioFormat.ENCODING_PCM_16BIT)
                    if (min <= 0) continue
                    val candidate = try {
                        AudioRecord.Builder().setAudioFormat(AudioFormat.Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT).setSampleRate(rate).setChannelMask(mask).build())
                            .setBufferSizeInBytes(maxOf(min, rate / 100 * 2 * 2 * 4))
                            .setAudioPlaybackCaptureConfig(config).build()
                    } catch (_: IllegalArgumentException) { null } catch (_: UnsupportedOperationException) { null }
                    if (candidate != null) {
                        if (candidate.state == AudioRecord.STATE_INITIALIZED) chosen = candidate else candidate.release()
                    }
                }
            }
            val record = chosen ?: error("No playback capture format")
            recorder = record
            val rate = record.sampleRate
            val channels = record.channelCount
            check(rate in 8000..192000 && channels in 1..2 && record.audioFormat == AudioFormat.ENCODING_PCM_16BIT)
            val input = ShortArray(maxOf(1, rate / 100) * channels)
            val output = ShortArray((input.size / channels * 48000L / rate).toInt() + 4)
            val converter = PlaybackResampler(rate, channels)
            record.startRecording()
            check(record.recordingState == AudioRecord.RECORDSTATE_RECORDING)
            running = true
            reader = Thread({ capture(record, input, output, converter) }, "screen-audio").also { it.start() }
            control.scheduleWithFixedDelay({
                if (running) {
                    report()
                    if (mix.formatFailed) fail("microphone format changed")
                }
            }, 30, 30, TimeUnit.SECONDS)
            Diag.log("screen audio: capturing rate=$rate channels=$channels -> 48000 mono; own UID excluded; mixed into mic")
            return true
        } catch (e: RuntimeException) {
            Diag.log("screen audio: capture failed (${e.javaClass.simpleName}); screen and microphone continue")
            close()
            return false
        }
    }

    private fun capture(record: AudioRecord, input: ShortArray, output: ShortArray, converter: PlaybackResampler) {
        try {
            Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO)
            while (running) {
                var filled = 0
                while (running && filled < input.size) {
                    val count = record.read(input, filled, input.size - filled, AudioRecord.READ_BLOCKING)
                    if (!running) return
                    check(count > 0) { "Playback read error $count" }
                    filled += count
                }
                if (running) mix.ring.offer(output, converter.convert(input, filled, output))
            }
        } catch (e: RuntimeException) {
            if (running) fail("read ${e.javaClass.simpleName}")
        }
    }

    private fun fail(reason: String) {
        if (!running) return
        running = false
        mix.enabled = false
        Diag.log("screen audio: stopped ($reason); screen and microphone continue")
        failed(this)
    }

    private fun report() {
        val s = mix.ring.stats()
        Diag.log("screen audio: mean_abs_pcm16=${s.meanAbsolute} missing_samples=${s.missing} dropped_samples=${s.dropped} queued_samples=${s.queued}")
    }

    /** Stop unblocks read; release occurs only after the reader has exited. */
    @Synchronized fun close() {
        if (closed) return
        closed = true
        running = false
        mix.enabled = false
        control.shutdownNow()
        val record = recorder
        recorder = null
        runCatching { record?.stop() }
        val thread = reader
        reader = null
        // Cleanup can wait for AudioRecord, but never on the microphone callback.
        Thread({
            thread?.join()
            runCatching { record?.release() }
            report()
        }, "screen-audio-release").start()
    }
}
