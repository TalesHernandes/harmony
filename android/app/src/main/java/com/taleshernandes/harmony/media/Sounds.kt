package com.taleshernandes.harmony.media

import android.content.Context
import android.media.AudioAttributes
import android.media.MediaPlayer
import android.media.SoundPool
import com.taleshernandes.harmony.R
import com.taleshernandes.harmony.diag.Diag
import com.taleshernandes.harmony.model.Peer
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/**
 * Plays the harmony's own sounds: a room's comings and goings, mute, a screen
 * starting and ending, and the ring the caller hears (sons/LISTA.md). When
 * each is heard is [SoundRules]; this only plays them.
 *
 * They go out the way the call's own sound goes (Engine.callMode): as a voice
 * call when the call is in call mode, so they follow it to the earpiece or the
 * headset and the echo canceller hears them, and as media otherwise.
 */
class Sounds(context: Context) {
    private val app = context.applicationContext
    private val exec = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "sounds") }
    private val rules = SoundRules(
        play = ::playNow,
        now = System::currentTimeMillis,
        later = { ms, block ->
            val f = exec.schedule(block, ms, TimeUnit.MILLISECONDS)
            ({ f.cancel(false) })
        },
    )
    private val pools = HashMap<Boolean, Pool>()
    private var ring: MediaPlayer? = null

    private class Pool(val pool: SoundPool, val ids: Map<Sound, Int>)

    fun room(channelId: String, peers: List<Peer>, me: String, quiet: Boolean) =
        exec.execute { rules.room(channelId, peers, me, quiet) }

    fun voice(s: VoiceCall.State) = exec.execute { rules.voice(s) }

    fun direct(s: DirectCall.State) = exec.execute {
        rules.direct(s)
        val want = rules.ringback
        if (want == null) stopRing() else if (ring == null) startRing()
    }

    private fun attributes(voice: Boolean): AudioAttributes = AudioAttributes.Builder()
        .setUsage(if (voice) AudioAttributes.USAGE_VOICE_COMMUNICATION else AudioAttributes.USAGE_MEDIA)
        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
        .build()

    // One pool per way of playing, made on first use and kept: the six files
    // are 250 KB, and loading takes longer than a sound should wait.
    private fun pool(voice: Boolean): Pool = pools.getOrPut(voice) {
        val p = SoundPool.Builder().setMaxStreams(3).setAudioAttributes(attributes(voice)).build()
        Pool(p, Sound.entries.associateWith { p.load(app, raw(it), 1) })
    }

    private fun raw(s: Sound): Int = when (s) {
        Sound.ENTRAR -> R.raw.som_entrar
        Sound.SAIR -> R.raw.som_sair
        Sound.MUTAR -> R.raw.som_mutar
        Sound.DESMUTAR -> R.raw.som_desmutar
        Sound.TELA_COMECOU -> R.raw.som_tela_comecou
        Sound.TELA_ACABOU -> R.raw.som_tela_acabou
    }

    private fun playNow(s: Sound) {
        val p = pool(Engine.callMode)
        val id = p.ids.getValue(s)
        // 0 when the file is still loading (the first sound after start): one
        // more try once it has had the time.
        if (p.pool.play(id, 1f, 1f, 1, 0, 1f) == 0) {
            exec.schedule({ p.pool.play(id, 1f, 1f, 1, 0, 1f) }, 150, TimeUnit.MILLISECONDS)
        }
        Diag.log("som: ${s.name.lowercase()}")
    }

    /** Loads the files ahead, so the first join is heard. */
    fun warm() = exec.execute {
        pool(false)
        pool(true)
    }

    private fun startRing() {
        val mp = runCatching {
            MediaPlayer().apply {
                setAudioAttributes(attributes(Engine.callMode))
                val fd = app.resources.openRawResourceFd(R.raw.som_toque)
                setDataSource(fd.fileDescriptor, fd.startOffset, fd.length)
                fd.close()
                isLooping = true
                // From the other end of the line: about 10 dB under the ring
                // the person being called hears (owner, 29/10/2026).
                setVolume(RINGBACK_GAIN, RINGBACK_GAIN)
                prepare()
                start()
            }
        }.onFailure { Diag.log("som: toque não abriu (${it.javaClass.simpleName})") }.getOrNull()
        ring = mp
    }

    private fun stopRing() {
        val mp = ring ?: return
        ring = null
        runCatching { mp.stop() }
        mp.release()
    }

    companion object {
        const val RINGBACK_GAIN = 0.32f
    }
}
