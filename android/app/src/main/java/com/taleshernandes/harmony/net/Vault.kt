package com.taleshernandes.harmony.net

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import com.taleshernandes.harmony.diag.Diag
import java.security.GeneralSecurityException
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** Which server, and the session cookie for it. */
data class Saved(val baseUrl: String, val cookie: String)

interface Vault {
    fun load(): Saved?
    fun save(s: Saved)
    fun clear()
}

/**
 * The session cookie, sealed with an AES key that the Android keystore makes
 * and never lets out of the phone (on most phones, out of the secure hardware).
 *
 * No library: androidx.security-crypto did exactly this and was deprecated in
 * 2025, and this is forty lines of the platform's own API. The address is kept
 * in the clear; it is not a secret, and it is shown on the sign-in screen.
 */
class KeystoreVault(context: Context) : Vault {
    private val prefs = context.getSharedPreferences("session", Context.MODE_PRIVATE)

    override fun load(): Saved? {
        val base = prefs.getString(KEY_BASE, null) ?: return null
        val sealed = prefs.getString(KEY_COOKIE, null) ?: return null
        return try {
            val raw = Base64.decode(sealed, Base64.NO_WRAP)
            val cipher = Cipher.getInstance(TRANSFORM)
            cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, raw, 0, IV_LEN))
            val plain = cipher.doFinal(raw, IV_LEN, raw.size - IV_LEN)
            Saved(base, String(plain, Charsets.UTF_8))
        } catch (e: GeneralSecurityException) {
            // A key that was wiped (the phone's lock screen was removed, a
            // restore from another phone) leaves a sealed value nobody can open.
            // Signing in again is the honest answer.
            Diag.log("cofre: não abre (${e.javaClass.simpleName}), pedindo login")
            clear()
            null
        } catch (e: IllegalArgumentException) {
            Diag.log("cofre: valor inválido, pedindo login")
            clear()
            null
        }
    }

    override fun save(s: Saved) {
        val cipher = Cipher.getInstance(TRANSFORM)
        cipher.init(Cipher.ENCRYPT_MODE, key())
        val sealed = cipher.iv + cipher.doFinal(s.cookie.toByteArray(Charsets.UTF_8))
        prefs.edit()
            .putString(KEY_BASE, s.baseUrl)
            .putString(KEY_COOKIE, Base64.encodeToString(sealed, Base64.NO_WRAP))
            .apply()
    }

    override fun clear() {
        prefs.edit().remove(KEY_COOKIE).apply()
    }

    /** The last server used, kept after signing out so the field is filled in. */
    fun lastBaseUrl(): String? = prefs.getString(KEY_BASE, null)

    private fun key(): SecretKey {
        val ks = KeyStore.getInstance(KEYSTORE).apply { load(null) }
        (ks.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE)
        gen.init(
            KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return gen.generateKey()
    }

    private companion object {
        const val KEYSTORE = "AndroidKeyStore"
        const val ALIAS = "harmony-session"
        const val TRANSFORM = "AES/GCM/NoPadding"
        const val IV_LEN = 12
        const val KEY_BASE = "base_url"
        const val KEY_COOKIE = "cookie_sealed"
    }
}
