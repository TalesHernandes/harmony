package com.taleshernandes.harmony.model

/** What GET /api/app/android answers (PROTOCOL-HARMONY §8). url is a path on the harmony server. */
data class Release(val versionCode: Int, val versionName: String, val size: Long, val sha256: String, val url: String)

object Releases {
    private const val PREFIX = "/api/app/android/"
    private val HEX = Regex("^[0-9a-f]{64}$")

    /** True only when the server has something strictly newer than what is installed. */
    fun isNewer(installedCode: Long, r: Release?): Boolean = r != null && r.versionCode > installedCode

    /**
     * A path this server serves APKs under, and nothing else: the download
     * carries the session cookie, and must never be pointed anywhere else.
     */
    fun safePath(url: String): Boolean =
        url.startsWith(PREFIX) && url.length > PREFIX.length &&
            ".." !in url && "//" !in url && '\\' !in url && '?' !in url && '#' !in url && '%' !in url &&
            '/' !in url.substring(PREFIX.length)

    internal fun validSha(s: String) = HEX.matches(s)
}
