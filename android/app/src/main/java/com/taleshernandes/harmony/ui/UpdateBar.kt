package com.taleshernandes.harmony.ui

import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import com.taleshernandes.harmony.update.Updater

private fun mb(bytes: Long, whole: Boolean = false): String {
    val v = bytes / 1048576.0
    return if (whole && v >= 10) String.format(ptBr, "%.0f", v) else String.format(ptBr, "%.1f", v)
}

/**
 * The update strip (design screen 7): an island in the layout under the
 * header, pushing the list down. Nothing to say, nothing drawn.
 */
@Composable
fun UpdateStrip(state: Updater.State, onInstall: () -> Unit, onCancel: () -> Unit, modifier: Modifier = Modifier) {
    when (state) {
        Updater.State.Idle -> Unit
        // TODO(design): dragging the strip aside to put it off until tomorrow — the updater has no way to defer.
        is Updater.State.Available -> Strip(
            Glyphs.ArrowDownToLine, "Versão nova disponível",
            "${state.release.versionName}${if (state.release.size > 0) " · ${mb(state.release.size, true)} MB" else ""}",
            modifier, action = "Atualizar", onAction = onInstall,
        )
        is Updater.State.NeedsPermission -> Strip(
            Glyphs.ArrowDownToLine, "Versão nova disponível",
            "${state.release.versionName} · o Android precisa da sua licença",
            modifier, action = "Permitir", onAction = onInstall,
        )
        is Updater.State.Downloading -> {
            val f = if (state.total > 0) (state.done.toFloat() / state.total).coerceIn(0f, 1f) else 0f
            Strip(
                Glyphs.ArrowDownToLine, "Baixando a versão ${state.release.versionName}",
                if (state.total > 0) "${mb(state.done)} de ${mb(state.total, true)} MB" else null,
                modifier, action = "Cancelar", actionMain = false, onAction = onCancel, progress = f,
            )
        }
        // TODO(design): the "pronta" state's Instalar button — the updater hands the APK
        // to the installer as soon as it is checked, so there is nothing to press here.
        is Updater.State.Installing -> Strip(
            Glyphs.ArrowDownToLine, "Versão ${state.release.versionName} baixada", "O Android vai pedir para instalar", modifier,
        )
        is Updater.State.Failed -> Strip(
            Glyphs.CircleExclamation, if (state.byFile) "A versão ${state.release.versionName} não instalou" else "O download parou",
            state.why, modifier, danger = true,
            action = if (state.byFile) "Pelo arquivo" else "Tentar de novo", actionMain = false, onAction = onInstall,
        )
    }
}
