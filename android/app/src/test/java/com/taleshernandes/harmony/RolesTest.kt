package com.taleshernandes.harmony

import com.taleshernandes.harmony.model.*
import com.taleshernandes.harmony.net.*
import okhttp3.*
import okhttp3.ResponseBody.Companion.toResponseBody
import okio.Buffer
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class RolesTest {
    private val me = User("mod", "Moderator", false)
    private val owner = User("owner", "Owner", true)
    private fun role(id: String, position: Int, color: String = "", hoist: Boolean = false,
        permissions: Set<String> = emptySet(), members: List<String> = listOf("mod")) =
        Role(id, id, color, position, hoist, permissions, members)

    @Test fun firstColoredRoleWinsRegardlessOfWireOrderAndUncoloredRolesFallThrough() {
        val roles = listOf(role("low", 2, "#2ecc71"), role("top", 0), role("middle", 1, "#3498db"))
        assertEquals("#3498db", colorOf("mod", roles))
        assertEquals("#2ecc71", colorOf("mod", roles.filterNot { it.id == "middle" }))
        assertEquals("", colorOf("mod", roles.filter { it.id == "top" }))
        assertEquals("", colorOf("other", roles))
    }

    @Test fun highestHoistGroupsOnlineAndAwayButNeverOffline() {
        val members = listOf("mod", "away", "off", "normal").map { Member(it, it, false, null) }
        val roles = listOf(role("low", 2, hoist = true, members = listOf("mod", "away", "off")),
            role("top", 0, hoist = true, members = listOf("mod")))
        val groups = memberGroups(members, "mod", mapOf("away" to "away", "normal" to "online"), roles).toMap()
        assertEquals(listOf("mod"), groups["top"]!!.map { it.username })
        assertEquals(listOf("away"), groups["low"]!!.map { it.username })
        assertEquals(listOf("off"), groups["Offline"]!!.map { it.username })
        assertEquals(listOf("normal"), groups["Online"]!!.map { it.username })
    }

    @Test fun textModerationAndPrivateConversationRulesStaySeparate() {
        val text = Channel("t", "text", "t", "")
        val dm = text.copy(kind = "dm")
        val voice = text.copy(kind = "voice")
        for (permission in listOf("manage_messages", "manage_channels", "manage_roles")) {
            val roles = listOf(role("r", 0, permissions = setOf(permission)))
            val messages = permission == "manage_messages"
            assertEquals(messages, canDeleteMessage(me, roles, text, "other"))
            assertFalse(canDeleteMessage(me, roles, dm, "other"))
            assertTrue(canDeleteMessage(me, roles, dm, me.username))
            assertTrue(canDeleteMessage(owner, roles, dm, "other"))
            assertEquals(messages, canPin(messages, text))
            assertTrue(canPin(messages, dm))
            assertFalse(canPin(messages, voice))
        }
        assertFalse(canDeleteMessage(me, emptyList(), text, "other"))
        assertFalse(canPin(false, text))
    }

    @Test fun roleOnlyModeratorCannotEditAtOrAboveOwnTopOrGrantMissingPermissions() {
        val roles = listOf(role("above", 0, members = emptyList()), role("own", 1, permissions = setOf("manage_roles")),
            role("below", 2, permissions = setOf("manage_messages"), members = listOf("owner")))
        assertEquals(listOf("below"), roles.filter { canEditRole(me, roles, it) }.map { it.id })
        assertEquals(listOf("manage_roles"), editablePermissions(me, roles).map { it.id })
        assertFalse(canChangeRoleMember(me, roles, roles[2], false, true))
        assertTrue(canChangeRoleMember(me, roles, roles[2], false, false))
        assertFalse(canChangeRoleMember(me, roles, roles[2], true, false))
        assertEquals(roles, roles.filter { canEditRole(owner, roles, it) })
        assertEquals(3, editablePermissions(owner, roles).size)
        assertTrue(canChangeRoleMember(owner, roles, roles[0], true, true))
        assertFalse(canEditRole(me, emptyList(), roles[2]))
    }

    @Test fun profileOwnerCanGiveEveryMissingRoleInOrderIncludingToSelf() {
        val roles = listOf(role("low", 2, members = emptyList()), role("owned", 1, members = listOf("target")),
            role("high", 0, permissions = setOf("manage_messages"), members = emptyList()))
        assertEquals(listOf("high", "low"), giveableRoles(owner, roles, "target", false).map { it.id })
        assertEquals(listOf("high", "owned", "low"), giveableRoles(owner, roles, "owner", true).map { it.id })
        assertTrue(roles.all { canChangeRoleMember(owner, roles, it, true, false) })
    }

    @Test fun profileModeratorOnlyGivesMissingLowerRolesWithPermissionsTheyHave() {
        val roles = listOf(role("higher", 0, members = emptyList()),
            role("own", 1, permissions = setOf("manage_roles")),
            role("missingPermission", 2, permissions = setOf("manage_messages"), members = listOf("target")),
            role("available", 3, permissions = setOf("manage_roles"), members = emptyList()),
            role("alreadyOwned", 4, members = listOf("target")),
            role("forbidden", 5, permissions = setOf("manage_channels"), members = emptyList()))
        assertEquals(listOf("available"), giveableRoles(me, roles, "target", false).map { it.id })
        assertEquals(emptyList<Role>(), giveableRoles(me, roles, "owner", true))
        assertFalse(canChangeRoleMember(me, roles, roles[0], false, false))
        assertFalse(canChangeRoleMember(me, roles, roles[1], false, false))
        // Taking a lower role never requires holding its permissions.
        assertTrue(canChangeRoleMember(me, roles, roles[2], false, false))
        assertFalse(canChangeRoleMember(me, roles, roles[2], true, false))
        val ordinary = User("ordinary", "Ordinary", false)
        assertEquals(emptyList<Role>(), giveableRoles(ordinary, roles, "target", false))
        assertFalse(roles.any { canChangeRoleMember(ordinary, roles, it, false, false) })
    }

    @Test fun gatewayReplacesAllRolesAndRevokesPermissionsImmediately() {
        val snapshot = Json.roleSnapshot("""{"roles":[{"id":"r","name":"Mod","position":0,"color":"#2ecc71","hoist":true,"permissions":["manage_messages","manage_channels"],"members":["mod"]}],"colors":["#2ecc71"]}""")
        var state = RoomsState(roles = snapshot.roles, roleColors = snapshot.colors, canManage = true)
        assertTrue(canDeleteMessage(me, state.roles, Channel("t", "text", "t", ""), "other"))
        state = state.apply(Json.event("""{"type":"roles","data":{"roles":[]}}""")!!, me.username, null)
        assertEquals(emptyList<Role>(), state.roles)
        assertFalse(state.canManage)
        assertEquals(listOf("#2ecc71"), state.roleColors)
        assertFalse(canDeleteMessage(me, state.roles, Channel("t", "text", "t", ""), "other"))
        assertTrue(state.copy(roleOwner = true).apply(GatewayEvent.Roles(emptyList()), owner.username, null).canManage)
    }

    @Test fun oneAuthenticatedPutContainsEveryRoleAfterDragAndPreservesConflictReason() {
        val roles = listOf(role("a", 0), role("b", 1), role("c", 2))
        val ids = moveRole(roles, "c", "a", false)!!
        assertEquals(listOf("c", "a", "b"), ids)
        assertEquals(listOf("b", "c", "a"), moveRole(roles, "a", "c", true))
        assertNull(moveRole(roles, "a", "a", false))
        assertNull(moveRole(roles, "missing", "a", false))
        var calls = 0
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            calls++
            val req = chain.request()
            assertEquals("PUT", req.method)
            assertEquals("/api/roles/order", req.url.encodedPath)
            assertEquals("harmony_session=test", req.header("Cookie"))
            val body = JSONObject(Buffer().also { req.body!!.writeTo(it) }.readUtf8()).getJSONArray("roles")
            assertEquals(ids, (0 until body.length()).map { body.getString(it) })
            Response.Builder().request(req).protocol(Protocol.HTTP_1_1).code(409).message("Conflict")
                .body("""{"error":"stale_roles"}""".toResponseBody()).build()
        }.build()
        try {
            Api(client, "https://example.test", { "test" }, "test").roleAction("order", "PUT", body = roleOrderBody(ids))
            fail("Conflict was swallowed")
        } catch (e: ApiError) { assertEquals("stale_roles", e.reason) }
        assertEquals(1, calls)
    }
}
