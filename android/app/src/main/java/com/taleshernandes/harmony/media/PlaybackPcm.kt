package com.taleshernandes.harmony.media

import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.locks.ReentrantLock
import kotlin.math.abs

/** Bounded PCM16 queue. The audio callback never waits for the producer. */
internal class PlaybackRing(capacity: Int = 4800) {
    init { require(capacity > 0) }
    private val samples = ShortArray(capacity)
    internal val lock = ReentrantLock()
    private var head = 0
    private var size = 0
    private val missing = AtomicLong()
    private var dropped = 0L
    private var total = 0L
    private var magnitude = 0L

    fun offer(input: ShortArray, count: Int) {
        require(count in 0..input.size)
        lock.lock()
        try {
            for (i in 0 until count) {
                if (size == samples.size) {
                    head = (head + 1) % samples.size
                    size--
                    dropped++
                }
                val value = input[i]
                samples[(head + size) % samples.size] = value
                size++
                magnitude += abs(value.toInt())
                total++
            }
        } finally { lock.unlock() }
    }

    /** Absolute little-endian access preserves the ADM's position/limit/order. */
    fun mixInto(pcm: ByteBuffer, count: Int) {
        if (!lock.tryLock()) { missing.addAndGet(count.toLong()); return }
        try {
            val available = minOf(size, count)
            for (i in 0 until available) {
                val offset = i * 2
                val mic = ((pcm.get(offset).toInt() and 255) or (pcm.get(offset + 1).toInt() shl 8)).toShort().toInt()
                val sum = saturate(mic + samples[head].toInt())
                pcm.put(offset, sum.toByte())
                pcm.put(offset + 1, (sum shr 8).toByte())
                head = (head + 1) % samples.size
            }
            size -= available
            missing.addAndGet((count - available).toLong())
        } finally { lock.unlock() }
    }

    data class Stats(val meanAbsolute: Long, val missing: Long, val dropped: Long, val queued: Int)
    fun stats(): Stats {
        lock.lock()
        try {
            val result = Stats(if (total == 0L) 0 else magnitude / total, missing.getAndSet(0), dropped, size)
            total = 0; magnitude = 0; dropped = 0
            return result
        } finally { lock.unlock() }
    }

    companion object { fun saturate(value: Int) = value.coerceIn(-32768, 32767) }
}

/** Streaming linear interpolation, including boundaries between reads. */
internal class PlaybackResampler(private val inputRate: Int, private val channels: Int) {
    init { require(inputRate in 8000..192000 && channels in 1..2) }
    private var frame = 0L
    private var next = 0L
    private var previous = 0

    fun convert(input: ShortArray, count: Int, output: ShortArray): Int {
        require(count in 0..input.size && count % channels == 0)
        var written = 0
        var index = 0
        while (index < count) {
            val current = if (channels == 1) input[index].toInt() else
                (input[index].toInt() + input[index + 1].toInt()) / 2
            if (frame == 0L) {
                check(written < output.size)
                output[written++] = current.toShort()
                next += inputRate
            } else {
                val end = frame * RATE
                val begin = end - RATE
                while (next <= end) {
                    check(written < output.size)
                    output[written++] = (previous + (current - previous).toLong() * (next - begin) / RATE).toShort()
                    next += inputRate
                }
            }
            previous = current
            frame++
            index += channels
        }
        return written
    }

    companion object { const val RATE = 48000 }
}

/** Installed only for a screen with sound; silence voice before adding playback. */
internal class PlaybackMix(val ring: PlaybackRing = PlaybackRing()) {
    @Volatile var microphoneMuted = false
    @Volatile var enabled = true
    @Volatile var formatFailed = false
        private set

    fun process(pcm: ByteBuffer, bytes: Int, rate: Int, channels: Int) {
        if (microphoneMuted) for (i in 0 until minOf(bytes, pcm.limit())) pcm.put(i, 0)
        if (!enabled) return
        if (rate != PlaybackResampler.RATE || channels != 1 || bytes != 960 || pcm.limit() < bytes) {
            formatFailed = true
            enabled = false
            return
        }
        ring.mixInto(pcm, 480)
    }
}

internal object ScreenAudioPolicy {
    fun sendTrack(muted: Boolean, deafened: Boolean, playback: Boolean) = playback || (!muted && !deafened)
}
