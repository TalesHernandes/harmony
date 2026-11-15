package com.taleshernandes.harmony

import com.taleshernandes.harmony.net.*
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Request
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import okio.Buffer
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class SecurityTest {
    private val oldUser = """{"username":"me","display_name":"Eu","is_admin":false}"""
    private val ownUser = """{"username":"me","display_name":"Eu","is_admin":false,
        "email":"me@example.test","email_verified":false,"twofa":"totp"}"""

    private class Server {
        var status = 200
        var body = "{}"
        var session: String? = null
        var gated = 0
        val requests = mutableListOf<Request>()
        val api = Api(OkHttpClient.Builder().addInterceptor { chain ->
            requests += chain.request()
            Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1)
                .code(status).message("test").body(body.toResponseBody())
                .apply { session?.let { header("Set-Cookie", "harmony_session=$it; Path=/; HttpOnly; Secure") } }.build()
        }.build(), "https://example.test", { "existing-session" }, "test-agent", { gated++ })
        fun request() = requests.last()
        fun json() = JSONObject(Buffer().also { request().body!!.writeTo(it) }.readUtf8())
    }

    @Test fun ownUserReadsSecurityFieldsAndOldServersRemainVerified() {
        val current = accountStatus("""{"user":$ownUser,"email_required":true}""")
        assertEquals("me@example.test", current.user.email)
        assertFalse(current.user.emailVerified)
        assertEquals("totp", current.user.twofa)
        assertTrue(current.emailRequired)
        val old = accountStatus("""{"user":$oldUser}""")
        assertTrue(old.user.emailVerified)
        assertEquals("", old.user.email)
        assertEquals("", old.user.twofa)
        assertFalse(old.emailRequired)
        assertTrue(Json.userOf("""{"user":${JSONObject(ownUser).put("email_verified", true)}}""").emailVerified)
        val server = Server()
        server.body = """{"user":$ownUser,"email_required":true}"""
        assertEquals(current, server.api.meStatus())
        assertEquals("GET", server.request().method)
        assertEquals("/api/me", server.request().url.encodedPath)
    }

    @Test fun loginWithoutCookieCanAskForEitherSecondFactor() {
        val server = Server()
        for (method in listOf("totp", "email")) {
            server.body = """{"twofa":"$method","challenge":"test-challenge"}"""
            val r = server.api.login("me@example.test", "test-password") as LoginResult.SecondFactor
            assertEquals(method, r.method)
            assertEquals("test-challenge", r.challenge)
            assertEquals("/api/login", server.request().url.encodedPath)
            assertEquals("me@example.test", server.json().getString("username"))
            assertEquals("test-password", server.json().getString("password"))
            assertNull(server.request().header("Cookie"))
        }
        assertTrue(loginResponse("""{"user":$oldUser}""", null) is LoginResult.Failed)
        assertTrue(loginResponse("""{"twofa":"totp","challenge":""}""", null) is LoginResult.Failed)
        assertTrue(loginResponse("""{"twofa":"unknown","challenge":"test"}""", null) is LoginResult.Failed)
    }

    @Test fun bothLoginStepsExtractTheSameSessionCookie() {
        val server = Server()
        server.body = """{"user":$ownUser}"""
        server.session = "new-session"
        val first = server.api.login("me", "test-password") as LoginResult.Ok
        val second = server.api.login2FA("test-challenge", "test-code") as LoginResult.Ok
        assertEquals(first, second)
        assertEquals("new-session", second.cookie)
        assertEquals("/api/login/2fa", server.request().url.encodedPath)
        assertEquals("test-challenge", server.json().getString("challenge"))
        assertEquals("test-code", server.json().getString("code"))
        assertEquals(2, server.json().length())
        assertNull(server.request().header("Cookie"))
    }

    @Test fun invalidAndRateLimitedSecondFactorsStayOnTheLoginFlow() {
        val server = Server()
        server.status = 400; server.body = """{"error":"invalid_code"}"""
        assertEquals(LoginResult.Failed("Código ou link inválido, vencido ou já usado."), server.api.login2FA("test", "wrong"))
        server.status = 429; server.body = """{"error":"rate_limited"}"""
        assertEquals(LoginResult.RateLimited, server.api.login2FA("test", "wrong"))
    }

    @Test fun securityWritesMatchTheServerBodiesAndStatusCodes() {
        val server = Server()
        server.status = 202
        server.api.forgotPassword("me@example.test")
        assertEquals("/api/password/forgot", server.request().url.encodedPath)
        assertEquals("me@example.test", server.json().getString("identifier"))
        assertEquals(1, server.json().length())
        server.api.changeEmail("current-password", "next@example.test")
        assertEquals("/api/me/email", server.request().url.encodedPath)
        assertEquals("current-password", server.json().getString("password"))
        assertEquals("next@example.test", server.json().getString("email"))
        assertEquals(2, server.json().length())
        server.api.resendEmail()
        assertEquals("/api/me/email/resend", server.request().url.encodedPath)
        assertEquals(0, server.json().length())
        server.status = 200; server.body = """{"user":$ownUser}"""
        assertEquals("me@example.test", server.api.verifyEmail("test-code").email)
        assertEquals("/api/me/email/verify", server.request().url.encodedPath)
        assertEquals("test-code", server.json().getString("code"))
        assertEquals(1, server.json().length())
        server.body = "{}"
        server.api.changePassword("old-password", "new-password")
        assertEquals("/api/me/password", server.request().url.encodedPath)
        assertEquals("old-password", server.json().getString("current"))
        assertEquals("new-password", server.json().getString("new"))
        assertEquals(2, server.json().length())
        server.requests.forEach {
            assertEquals("POST", it.method)
            assertEquals("existing-session", it.header("Cookie")?.substringAfter('='))
            assertNull(it.url.query)
        }
    }

    @Test fun factorReadsSetupEmailChallengeAndTenRecoveryCodes() {
        val server = Server()
        server.body = """{"secret":"TESTSECRET","uri":"otpauth://totp/test"}"""
        val setup = server.api.factor("totp/setup", "test-password", "")
        assertEquals("TESTSECRET", setup.secret)
        assertEquals("otpauth://totp/test", setup.uri)
        assertNull(setup.user)
        server.status = 202; server.body = """{"code_required":true}"""
        assertTrue(server.api.factor("disable", "test-password", "").codeRequired)
        assertEquals("", server.json().getString("code"))
        server.status = 200
        val codes = (1..10).map { "test-recovery-$it" }
        server.body = JSONObject().put("user", JSONObject(ownUser))
            .put("recovery_codes", org.json.JSONArray(codes)).toString()
        for (action in listOf("totp/enable", "email/enable", "disable", "recovery")) {
            val r = server.api.factor(action, "test-password", "test-code")
            assertEquals(codes, r.recoveryCodes)
            assertEquals("totp", r.user!!.twofa)
            assertEquals("/api/me/2fa/$action", server.request().url.encodedPath)
            assertEquals("test-password", server.json().getString("password"))
            assertEquals("test-code", server.json().getString("code"))
            assertEquals(2, server.json().length())
        }
    }

    @Test fun emailGateIsReportedEvenForHiddenInvitesAndBinaryRequests() {
        val server = Server()
        server.status = 403; server.body = """{"error":"email_required"}"""
        assertNull(server.api.invites())
        assertEquals(1, server.gated)
        assertEquals("email_required", assertThrows(ApiError::class.java) { server.api.channels() }.reason)
        assertEquals(2, server.gated)
        assertThrows(ApiError::class.java) { server.api.file("/api/files/test") }
        assertEquals(3, server.gated)
        assertThrows(ApiError::class.java) { server.api.twemoji("1f600") }
        assertEquals(4, server.gated)
    }

    @Test fun wrongCurrentPasswordDoesNotDiscardTheSession() {
        val server = Server()
        server.status = 401; server.body = """{"error":"bad_credentials"}"""
        val e = assertThrows(ApiError::class.java) { server.api.changePassword("wrong", "new-password") }
        assertEquals("bad_credentials", e.reason)
        server.body = """{"error":"unauthenticated"}"""
        assertThrows(Unauthorized::class.java) { server.api.me() }
    }

    @Test fun secretGroupsIncludeAnIncompleteFinalBlock() {
        assertEquals("ABCD EFGH IJKL MN", groupSecret("ABCDEFGHIJKLMN"))
        assertEquals("ABCD", groupSecret("ABCD"))
        assertEquals("A", groupSecret("A"))
        assertEquals("", groupSecret(""))
    }

    @Test fun securityMessagesUseTheWebVocabulary() {
        val messages = mapOf(
            "bad_email" to "Digite um e-mail válido.",
            "email_unavailable" to "Não foi possível usar esse e-mail. Confira o endereço ou procure o dono.",
            "invalid_code" to "Código ou link inválido, vencido ou já usado.",
            "email_required" to "Confirme seu e-mail para continuar.",
            "totp_required" to "Digite o código do app ou um código de recuperação.",
            "mail_unavailable" to "O envio de e-mail está indisponível. Procure o dono do servidor.",
            "rate_limited" to "Tentativas demais. Espere quinze minutos.",
            "bad_credentials" to "Usuário ou senha não conferem.",
            "weak_password" to "A senha precisa ter pelo menos 10 caracteres.",
        )
        messages.forEach { (code, text) -> assertEquals(text, securityMessage(code)) }
        assertEquals("Deu algo errado. Tente de novo.", securityMessage("unknown"))
        assertEquals("Sem conexão com o servidor.", securityError(java.io.IOException()))
    }

    @Test fun diagnosticsNeverIncludeLoginOrFactorSecrets() {
        com.taleshernandes.harmony.diag.Diag.clearForTest()
        val server = Server()
        server.body = """{"twofa":"totp","challenge":"private-challenge"}"""
        server.api.login("private@example.test", "private-password")
        server.status = 400; server.body = """{"error":"invalid_code"}"""
        server.api.login2FA("private-challenge", "private-code")
        val log = com.taleshernandes.harmony.diag.Diag.dump("test")
        assertTrue(log.contains("2fa pedido (totp)"))
        for (secret in listOf("private@example.test", "private-password", "private-challenge", "private-code"))
            assertFalse(log.contains(secret))
    }
}
