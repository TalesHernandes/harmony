package com.taleshernandes.harmony.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.Checkbox
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.taleshernandes.harmony.model.*
import com.taleshernandes.harmony.net.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

internal val LocalRoles = compositionLocalOf { RoomsState() }
internal fun roleColor(hex: String, fallback: Color = Foreground): Color =
    if (hex.matches(Regex("#[0-9a-fA-F]{6}"))) Color(android.graphics.Color.parseColor(hex)) else fallback

@Composable
internal fun roleNameColor(username: String, fallback: Color = Foreground): Color = roleColor(colorOf(username, LocalRoles.current.roles), fallback)

@Composable
internal fun RoleChips(username: String, owner: Boolean, me: User?, allRoles: List<Role>, api: () -> Api?) {
    val roles = rolesOf(username, allRoles)
    val candidates = me?.let { giveableRoles(it, allRoles, username, owner) }.orEmpty()
    var adding by remember(username) { mutableStateOf(false) }
    var busy by remember(username) { mutableStateOf(false) }
    var error by remember(username) { mutableStateOf("") }
    val scope = rememberCoroutineScope()
    val currentApi by rememberUpdatedState(api)
    fun change(role: Role, giving: Boolean) {
        if (busy) return
        busy = true
        error = ""
        scope.launch {
            try {
                withContext(Dispatchers.IO) {
                    (currentApi() ?: throw Unauthorized()).roleAction(if (giving) "give" else "take",
                        if (giving) "PUT" else "DELETE", role.id, username)
                }
                adding = false
            } catch (e: CancellationException) { throw e } catch (e: Exception) {
                error = roleError(e)
            } finally { busy = false }
        }
    }
    if (roles.isEmpty() && !owner && candidates.isEmpty()) return
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionLabel("Cargos")
        FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            if (owner) RoleChip("Dono", "", dot = false)
            roles.forEach { role ->
                RoleChip(role.name, role.color, remove = if (me != null && canChangeRoleMember(me, allRoles, role, owner, false))
                    ({ change(role, false) }) else null, enabled = !busy)
            }
            if (candidates.isNotEmpty()) Row(Modifier.height(28.dp).island(Surface2)
                .clickable(enabled = !busy, role = androidx.compose.ui.semantics.Role.Button) { adding = !adding }
                .padding(horizontal = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("+ Dar cargo", style = MetaText)
            }
        }
        if (adding && candidates.isNotEmpty()) Column(Modifier.fillMaxWidth().island(Surface2).padding(8.dp)) {
            candidates.forEach { role ->
                Row(Modifier.fillMaxWidth().height(48.dp).clip(Shape8)
                    .clickable(enabled = !busy, role = androidx.compose.ui.semantics.Role.Button) { change(role, true) }
                    .padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Box(Modifier.size(10.dp).clip(CircleShape).background(roleColor(role.color, Muted)))
                    Text(role.name, style = ItemText, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
        }
        if (error.isNotEmpty()) Text(error, style = MetaText, color = DangerSoftFg)
    }
}

@Composable
private fun RoleChip(name: String, color: String, dot: Boolean = true, remove: (() -> Unit)? = null, enabled: Boolean = true) {
    Row(Modifier.height(28.dp).island(Surface2).padding(horizontal = 8.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        if (dot) Box(Modifier.size(10.dp).clip(CircleShape).background(roleColor(color, Muted)))
        Text(name, style = MetaText)
        if (remove != null) Box(Modifier.size(24.dp).clip(Shape8)
            .clickable(enabled = enabled, role = androidx.compose.ui.semantics.Role.Button, onClick = remove)
            .semantics { contentDescription = "Tirar o cargo $name" }, contentAlignment = Alignment.Center) {
            Glyph(Glyphs.Xmark, null, Muted, 12)
        }
    }
}

internal fun roleError(e: Exception): String = when ((e as? ApiError)?.reason) {
    "not_allowed" -> "Você não tem permissão para isso."
    "bad_role_name" -> "Dê um nome ao cargo, com até 32 caracteres."
    "bad_color" -> "Escolha uma das cores da lista."
    "name_taken" -> "Já existe um com esse nome."
    "role_limit" -> "O servidor já tem 50 cargos. Apague um para criar outro."
    "stale_roles" -> "A lista de cargos mudou enquanto você arrastava. Ela foi recarregada; tente de novo."
    "not_found" -> "Não encontrado."
    else -> if (e is ApiError) "Deu algo errado. Tente de novo." else accountError(e)
}

@Composable
internal fun RolesPanel(me: User, members: List<Member>, api: () -> Api?, onSnapshot: (RoleSnapshot) -> Unit) {
    val state = LocalRoles.current
    val roles = state.roles.sortedBy { it.position }
    var picked by rememberSaveable { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf("") }
    var deleting by remember { mutableStateOf(false) }
    var dragIds by remember { mutableStateOf<List<String>?>(null) }
    val scope = rememberCoroutineScope()
    val currentApi by rememberUpdatedState(api)
    val currentSnapshot by rememberUpdatedState(onSnapshot)
    val currentRoles by rememberUpdatedState(roles)
    val role = roles.firstOrNull { it.id == picked && canEditRole(me, roles, it) }
    LaunchedEffect(roles) { dragIds = null; if (role == null) { picked = ""; deleting = false } }
    fun run(action: String, method: String, id: String? = null, member: String? = null, body: JSONObject = JSONObject()) {
        if (busy) return
        busy = true
        error = ""
        scope.launch {
            try {
                val response = withContext(Dispatchers.IO) {
                    (currentApi() ?: throw Unauthorized()).roleAction(action, method, id, member, body)
                }
                if (action == "create") picked = JSONObject(response).getString("id")
                if (action == "delete") picked = ""
                // Only the gateway replaces the live list. The order response has no palette.
            } catch (e: CancellationException) { throw e } catch (e: Exception) {
                error = roleError(e)
                if (e is ApiError && e.reason == "stale_roles") {
                    try { currentSnapshot(withContext(Dispatchers.IO) { (currentApi() ?: throw Unauthorized()).roles() }) }
                    catch (cancel: CancellationException) { throw cancel }
                    catch (reload: Exception) { error += " " + roleError(reload) }
                }
            } finally { busy = false }
        }
    }
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("Cargos", style = ItemText)
        Text("Um cargo dá cor ao nome e permissões a quem o tem. O de cima vale mais: quem gerencia cargos só mexe nos que estão abaixo do seu. O dono está acima de todos e não tem cargo.", style = MetaText, color = Muted)
        if (error.isNotEmpty()) Text(error, style = MetaText, color = DangerSoftFg)
        WebButton("Criar cargo", {
            var name = "Novo cargo"
            var n = 2
            while (roles.any { it.name.equals(name, true) }) name = "Novo cargo ${n++}"
            run("create", "POST", body = JSONObject().put("name", name).put("color", "").put("hoist", false).put("permissions", JSONArray()))
        }, enabled = !busy, icon = Glyphs.Plus)
        if (roles.isEmpty()) Text("Nenhum cargo ainda.", style = MetaText, color = Muted)
        val displayed = dragIds?.mapNotNull { id -> roles.find { it.id == id } } ?: roles
        val step = with(LocalDensity.current) { 48.dp.toPx() }
        for (r in displayed) key(r.id) {
            val editable = canEditRole(me, roles, r)
            val allEditable = roles.filter { canEditRole(me, roles, it) }.map { it.id }.toSet()
            Row(Modifier.fillMaxWidth().height(48.dp).island(if (r.id == picked) Selected else Surface2),
                verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (editable) Box(Modifier.size(40.dp).semantics { contentDescription = "Arrastar ${r.name}" }
                    .pointerInput(r.id, busy, allEditable) {
                        var distance = 0f
                        detectVerticalDragGestures(
                            onDragStart = { distance = 0f; dragIds = currentRoles.map { it.id } },
                            onDragCancel = { dragIds = null },
                            onDragEnd = {
                                val order = dragIds
                                dragIds = null
                                if (!busy && order != null && order != currentRoles.map { it.id })
                                    run("order", "PUT", body = roleOrderBody(order))
                            },
                            onVerticalDrag = { change, dy ->
                                change.consume()
                                if (!busy) {
                                    distance += dy
                                    while (kotlin.math.abs(distance) >= step) {
                                        val direction = if (distance > 0) 1 else -1
                                        val ids = dragIds.orEmpty().toMutableList()
                                        val from = ids.indexOf(r.id)
                                        val to = from + direction
                                        if (from >= 0 && to in ids.indices && ids[to] in allEditable) {
                                            val ordered = ids.mapIndexed { index, id -> currentRoles.first { it.id == id }.copy(position = index) }
                                            dragIds = moveRole(ordered, r.id, ids[to], direction > 0)
                                        }
                                        distance -= direction * step
                                    }
                                }
                            })
                    }, contentAlignment = Alignment.Center) { Glyph(Glyphs.Bars, null, Muted, 16) }
                else Spacer(Modifier.width(40.dp))
                Box(Modifier.size(12.dp).clip(CircleShape).background(roleColor(r.color, Muted)))
                Row(Modifier.weight(1f).then(if (editable) Modifier.clickable { picked = if (picked == r.id) "" else r.id } else Modifier),
                    verticalAlignment = Alignment.CenterVertically) {
                    Text(r.name, color = roleColor(r.color), style = ItemText, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text("${r.members.size} ${if (r.members.size == 1) "pessoa" else "pessoas"}", style = MetaText, color = Muted, modifier = Modifier.padding(end = 8.dp))
                }
            }
        }
        role?.let { r ->
            Column(Modifier.fillMaxWidth().island(Surface2).padding(12.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                var name by remember(r.id, r.name) { mutableStateOf(r.name) }
                SectionLabel("Nome")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Field(name, { if (it.codePointCount(0, it.length) <= 32) name = it }, "Nome do cargo", Modifier.weight(1f), enabled = !busy, height = 40)
                    WebButton("Salvar", { run("rename", "PATCH", r.id, body = JSONObject().put("name", name.trim())) }, enabled = !busy && name.trim().isNotEmpty() && name.trim() != r.name, height = 40)
                }
                SectionLabel("Cor")
                ColorSwatch("", r.color, !busy) { run("color", "PATCH", r.id, body = JSONObject().put("color", "")) }
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    for (line in state.roleColors.chunked(10)) Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        line.forEach { color -> ColorSwatch(color, r.color, !busy) { run("color", "PATCH", r.id, body = JSONObject().put("color", color)) } }
                    }
                }
                Text("O nome aparece na cor do cargo mais alto que tiver cor.", style = MetaText, color = Muted)
                RoleToggle("Mostrar separado na lista de pessoas", "Quem está online com este cargo ganha um grupo próprio.", r.hoist, !busy) {
                    run("hoist", "PATCH", r.id, body = JSONObject().put("hoist", it))
                }
                SectionLabel("Permissões")
                editablePermissions(me, roles).forEach { p ->
                    RoleToggle(p.name, p.detail, p.id in r.permissions, !busy) { on ->
                        val next = if (on) r.permissions + p.id else r.permissions - p.id
                        run("permissions", "PATCH", r.id, body = JSONObject().put("permissions", JSONArray(ROLE_PERMISSIONS.map { it.id }.filter { it in next })))
                    }
                }
                Text("Convites, contas e senhas continuam só do dono, com qualquer cargo.", style = MetaText, color = Muted)
                SectionLabel("Quem tem · ${r.members.size}")
                r.members.forEach { username ->
                    val person = members.find { it.username == username }
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Avatar(person?.displayName ?: username, 32, username = username)
                        Text(person?.nickname?.ifEmpty { null } ?: person?.displayName ?: username, style = ItemText, modifier = Modifier.weight(1f))
                        if ((me.isAdmin || person != null) && canChangeRoleMember(me, roles, r, person?.isAdmin == true, false))
                            WebIcon(Glyphs.Xmark, "Tirar o cargo de $username", { if (!busy) run("take", "DELETE", r.id, username) })
                    }
                }
                val candidates = members.filter { it.username !in r.members && canChangeRoleMember(me, roles, r, it.isAdmin, true) }
                var adding by remember(r.id) { mutableStateOf(false) }
                if (candidates.isNotEmpty()) {
                    WebButton("Dar o cargo", { adding = !adding }, enabled = !busy, icon = Glyphs.Plus)
                    if (adding) candidates.forEach { person ->
                        val name = person.nickname?.ifEmpty { null } ?: person.displayName.ifEmpty { person.username }
                        Row(Modifier.fillMaxWidth().height(48.dp).clip(Shape8)
                            .clickable(enabled = !busy, role = androidx.compose.ui.semantics.Role.Button) {
                                adding = false; run("give", "PUT", r.id, person.username)
                            }.padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            Avatar(name, 32, username = person.username)
                            Text(name, style = ItemText, color = roleNameColor(person.username),
                                maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                        }
                    }
                }
                WebButton("Apagar cargo", { deleting = true }, enabled = !busy, icon = Glyphs.TrashBin, background = DangerSoft)
            }
        }
    }
    if (deleting && role != null) Confirm("Apagar este cargo?", "Quem tem o cargo perde a cor e as permissões dele na hora. Isso não tem volta.", "Apagar",
        onConfirm = { deleting = false; run("delete", "DELETE", role.id) }, onDismiss = { deleting = false },
        extra = { Text(role.name, style = ItemText, color = roleColor(role.color)) })
}

@Composable
private fun ColorSwatch(color: String, picked: String, enabled: Boolean, onPick: () -> Unit) {
    Box(Modifier.size(24.dp).semantics { contentDescription = if (color.isEmpty()) "Sem cor" else "Cor $color"; selected = color == picked }
        .border(if (color == picked) 2.dp else 0.dp, if (color == picked) Foreground else Color.Transparent, CircleShape)
        .padding(3.dp).clip(CircleShape).background(roleColor(color, Surface3)).clickable(enabled = enabled, onClick = onPick),
        contentAlignment = Alignment.Center) { if (color.isEmpty()) Text("∅", style = MetaText) }
}

@Composable
private fun RoleToggle(name: String, detail: String, checked: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().toggleable(checked, enabled = enabled, role = androidx.compose.ui.semantics.Role.Checkbox, onValueChange = onChange)
        .semantics { contentDescription = name }, verticalAlignment = Alignment.Top) {
        Checkbox(checked, null, modifier = Modifier.padding(top = 10.dp, end = 12.dp), enabled = enabled)
        Column(Modifier.weight(1f).padding(top = 10.dp)) {
            Text(name, style = ItemText)
            Text(detail, style = MetaText, color = Muted)
        }
    }
}
