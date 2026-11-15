package com.taleshernandes.harmony.net

import com.taleshernandes.harmony.model.User
import org.json.JSONObject

data class AccountStatus(val user: User, val emailRequired: Boolean)
data class FactorResult(val user: User?, val secret: String, val uri: String,
    val codeRequired: Boolean, val recoveryCodes: List<String>)

fun groupSecret(secret: String): String = secret.chunked(4).joinToString(" ")

fun securityMessage(reason: String?): String = when (reason) {
    "mail_unavailable" -> "O envio de e-mail está indisponível. Procure o dono do servidor."
    "bad_email" -> "Digite um e-mail válido."
    "email_unavailable" -> "Não foi possível usar esse e-mail. Confira o endereço ou procure o dono."
    "invalid_code" -> "Código ou link inválido, vencido ou já usado."
    "email_required" -> "Confirme seu e-mail para continuar."
    "totp_required" -> "Digite o código do app ou um código de recuperação."
    "rate_limited" -> "Tentativas demais. Espere quinze minutos."
    "delivery_failed" -> "Não foi possível entregar."
    "bad_credentials" -> "Usuário ou senha não conferem."
    "weak_password" -> "A senha precisa ter pelo menos 10 caracteres."
    "offline" -> "Sem conexão com o servidor."
    "unauthenticated" -> "Sua sessão terminou. Entre de novo."
    else -> "Deu algo errado. Tente de novo."
}

fun securityError(e: Exception): String = when (e) {
    is ApiError -> securityMessage(e.reason)
    is Unauthorized -> securityMessage("unauthenticated")
    else -> securityMessage("offline")
}

fun accountStatus(body: String): AccountStatus =
    AccountStatus(Json.userOf(body), JSONObject(body).optBoolean("email_required"))

/** A challenge is not a session; it must never be saved in the vault. */
fun loginResponse(body: String, cookie: String?): LoginResult {
    val json = JSONObject(body)
    val method = json.optString("twofa")
    val challenge = json.optString("challenge")
    if (method in listOf("email", "totp") && challenge.isNotEmpty())
        return LoginResult.SecondFactor(method, challenge)
    return if (!cookie.isNullOrEmpty()) LoginResult.Ok(Json.userOf(body), cookie)
        else LoginResult.Failed("O servidor não devolveu a sessão.")
}
