package com.taleshernandes.harmony.model

import org.json.JSONArray
import org.json.JSONObject

data class Role(val id: String, val name: String, val color: String = "", val position: Int = 0,
    val hoist: Boolean = false, val permissions: Set<String> = emptySet(), val members: List<String> = emptyList())
data class RoleSnapshot(val roles: List<Role>, val colors: List<String>)
data class RolePermission(val id: String, val name: String, val detail: String)

val ROLE_PERMISSIONS = listOf(
    RolePermission("manage_channels", "Gerenciar canais", "Criar, renomear, organizar e apagar canais e categorias."),
    RolePermission("manage_messages", "Gerenciar mensagens", "Apagar a mensagem dos outros e fixar mensagens nos canais."),
    RolePermission("manage_roles", "Gerenciar cargos", "Criar, editar e dar cargos abaixo do seu cargo mais alto."),
)

fun rolesOf(username: String?, roles: List<Role>) = roles.filter { username in it.members }.sortedBy { it.position }
fun colorOf(username: String?, roles: List<Role>) = rolesOf(username, roles).firstOrNull { it.color.isNotEmpty() }?.color.orEmpty()
fun permissionsOf(username: String?, owner: Boolean, roles: List<Role>): Set<String> =
    if (owner) ROLE_PERMISSIONS.map { it.id }.toSet() else rolesOf(username, roles).flatMap { it.permissions }.toSet()
fun topOf(me: User, roles: List<Role>) = if (me.isAdmin) -1 else rolesOf(me.username, roles).firstOrNull()?.position ?: Int.MAX_VALUE
fun canEditRole(me: User, roles: List<Role>, role: Role) =
    "manage_roles" in permissionsOf(me.username, me.isAdmin, roles) && role.position > topOf(me, roles)
fun editablePermissions(me: User, roles: List<Role>) = ROLE_PERMISSIONS.filter { it.id in permissionsOf(me.username, me.isAdmin, roles) }
fun canChangeRoleMember(me: User, roles: List<Role>, role: Role, targetOwner: Boolean, giving: Boolean) =
    canEditRole(me, roles, role) && (!targetOwner || me.isAdmin) &&
        (!giving || permissionsOf(me.username, me.isAdmin, roles).containsAll(role.permissions))

fun giveableRoles(me: User, roles: List<Role>, username: String, targetOwner: Boolean): List<Role> =
    roles.filter { username !in it.members && canChangeRoleMember(me, roles, it, targetOwner, true) }.sortedBy { it.position }

fun canDeleteMessage(me: User, roles: List<Role>, channel: Channel, author: String) =
    author == me.username || me.isAdmin || (channel.isText && "manage_messages" in permissionsOf(me.username, false, roles))

/** Offline always wins over a hoisted role. Empty groups are omitted by the view. */
fun memberGroups(members: List<Member>, me: String?, presence: Map<String, String>, roles: List<Role>): List<Pair<String, List<Member>>> {
    val apart = roles.filter { it.hoist }.sortedBy { it.position }
    val groups = List(apart.size + 3) { mutableListOf<Member>() }
    for (member in members) {
        val state = presence[member.username] ?: if (member.username == me) "online" else "offline"
        val visible = state in listOf("online", "in_call", "away")
        val hoist = if (visible) rolesOf(member.username, apart).firstOrNull() else null
        val at = if (hoist != null) apart.indexOf(hoist) else apart.size + when (state) {
            "online", "in_call" -> 0
            "away" -> 1
            else -> 2
        }
        groups[at].add(member)
    }
    return (apart.map { it.name } + listOf("Online", "Ausente", "Offline")).mapIndexed { i, label -> label to groups[i].toList() }
}

/** The complete order is sent once, when the handle is released. */
fun moveRole(roles: List<Role>, id: String, onto: String, after: Boolean): List<String>? {
    val ids = roles.sortedBy { it.position }.map { it.id }
    if (id == onto || id !in ids || onto !in ids) return null
    val next = ids.filterNot { it == id }.toMutableList()
    next.add(next.indexOf(onto) + if (after) 1 else 0, id)
    return next.takeIf { it != ids }
}
fun roleOrderBody(ids: List<String>) = JSONObject().put("roles", JSONArray(ids))
