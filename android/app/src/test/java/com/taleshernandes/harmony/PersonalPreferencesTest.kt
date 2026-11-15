package com.taleshernandes.harmony

import com.taleshernandes.harmony.model.*
import com.taleshernandes.harmony.net.Api
import com.taleshernandes.harmony.net.SESSION_COOKIE
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.After
import org.junit.Assert.*
import org.junit.Test

class PersonalPreferencesTest {
 @After fun clear() { PersonalPreferences.clear() }
 @Test fun silenceKeepsScreenAudioAndBlockingSilencesBoth() {
  PersonalPreferences.replace(listOf(PersonPreference("bob",true,false)))
  assertTrue(PersonalPreferences.silent("bob",false))
  assertFalse(PersonalPreferences.silent("bob",true))
  assertFalse(PersonalPreferences.silent("alice",false))
  PersonalPreferences.replace(listOf(PersonPreference("bob",false,true)))
  assertTrue(PersonalPreferences.silent("bob",false))
  assertTrue(PersonalPreferences.silent("bob",true))
  PersonalPreferences.replace(emptyList())
  assertFalse(PersonalPreferences.silent("bob",false))
 }
 @Test fun anUnknownListSilencesNobody() {
  PersonalPreferences.clear()
  assertFalse(PersonalPreferences.silent("bob",false))
  assertFalse(PersonalPreferences.silent("bob",true))
  PersonalPreferences.replace(emptyList())
  assertFalse(PersonalPreferences.silent("bob",true))
 }
 @Test fun blockedMessagesDoNotCountUnreadOrMentions() {
  PersonalPreferences.replace(listOf(PersonPreference("bob",false,true)))
  val msg = Message(20,"room", Author("bob","Bob"),"@alice", 0L, null, false, "", emptyList(), listOf("alice"), emptyList(), null)
  val state = RoomsState(channels=listOf(Channel("room","text","geral","")))
  val after = state.apply(GatewayEvent.MessageNew(msg),"alice",null)
  assertEquals(0,after.channels.single().unread)
  assertEquals(0,after.channels.single().mentions)
  assertEquals(20L,after.channels.single().lastMessageId)
 }
 @Test fun routesAreSessionOwnedAndReadBackBothFlags() {
  val paths = mutableListOf<String>()
  val client = OkHttpClient.Builder().addInterceptor { chain ->
   val r = chain.request(); paths.add(r.method + " " + r.url.encodedPath)
   assertEquals("$SESSION_COOKIE=fixture",r.header("Cookie"))
   Response.Builder().request(r).protocol(Protocol.HTTP_1_1).code(200).message("OK")
    .body("""{"items":[{"username":"bob","muted":true,"blocked":false}]}""".toResponseBody()).build()
  }.build()
  val api=Api(client,"http://localhost",{"fixture"},"test")
  assertTrue(api.personPreferences().single().muted)
  api.personPreference("bob",true,true)
  api.personPreference("bob",false,false)
  assertEquals(listOf("GET /api/me/blocks","PUT /api/me/blocks/bob","DELETE /api/me/mutes/bob"),paths)
 }
}
