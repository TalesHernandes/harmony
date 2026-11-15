[CmdletBinding()]
param([string]$Destino)
$ErrorActionPreference = 'Stop'
if (-not $Destino) { $Destino = Join-Path $env:LOCALAPPDATA 'Programs\harmony-prova' }
$root = [IO.Path]::GetFullPath($Destino)
$installer = Join-Path $PSScriptRoot 'instalar.ps1'
$app = $null
$updatedApp = $null
$report = Join-Path $PSScriptRoot 'prova-instalacao.json'
$step = 'install version 1'
if (Test-Path -LiteralPath $root) { throw 'Use a fresh destination for the complete installation proof.' }
. (Join-Path $PSScriptRoot 'window-probe.ps1')
try {
    & $installer -Versao 1 -Destino $root -Relatorio (Join-Path $PSScriptRoot 'instalacao-1.json')
    # Opening this window is the requested live-update experiment. Never run harmony-w1.exe.
    $step = 'launch version 1'
    $app = & (Join-Path $root 'launch.ps1') -PassThru
    $null = $app.Handle
    Start-Sleep -Seconds 3
    if ($app.HasExited) { throw 'Version 1 did not remain open; cannot validate live update.' }
    $window1 = [InstalledProofWindow]::Find($app.Id)
    if (-not $window1) { throw 'Version 1 has no Harmony proof window.' }
    $step = 'update with version 1 open'
    & $installer -Versao 2 -Destino $root -Relatorio (Join-Path $PSScriptRoot 'atualizacao-2.json')
    if ($app.HasExited -or -not [InstalledProofWindow]::IsWindow($window1) -or -not (Test-Path (Join-Path $root 'app-1\telas.exe'))) { throw 'Running version was not retained.' }
    $pointer = Get-Content (Join-Path $root 'current.json') -Raw | ConvertFrom-Json
    if ($pointer.version -ne 2) { throw 'Pointer did not switch to version 2.' }
    $step = 'launch selected version 2'
    $updatedApp = & (Join-Path $root 'launch.ps1') -PassThru
    $null = $updatedApp.Handle
    Start-Sleep -Seconds 3
    if ($updatedApp.HasExited -or $updatedApp.Path -ne (Join-Path $root 'app-2\telas.exe')) { throw 'Launcher did not open version 2.' }
    $window2 = [InstalledProofWindow]::Find($updatedApp.Id)
    if (-not $window2) { throw 'Version 2 has no Harmony proof window.' }
    [InstalledProofWindow]::Close($window2) | Out-Null
    if (-not $updatedApp.WaitForExit(10000)) { throw 'Version 2 did not close.' }
    $step = 'close version 1 and clean up'
    [InstalledProofWindow]::Close($window1) | Out-Null
    if (-not $app.WaitForExit(10000)) { throw 'Close version 1 manually and run instalar.ps1 -Limpar.' }
    & $installer -Limpar -Destino $root
    if (Test-Path (Join-Path $root 'app-1')) { throw 'Version 1 remains in use; cleanup deferred.' }
    $step = 'measure installed version 2'
    & (Join-Path $PSScriptRoot '..\telas\medir.ps1') -Exe (Join-Path $root 'app-2\telas.exe') -Arquivo (Join-Path $PSScriptRoot 'medidas-instalado.json')
    $installedFiles = @(Get-ChildItem -LiteralPath $root -Recurse -File -Force)
    $installedMb = ($installedFiles | Measure-Object Length -Sum).Sum / 1MB
    $step = 'remove'
    & $installer -Remover -Destino $root
    if (Test-Path -LiteralPath $root) { throw 'Destination still exists after removal.' }
    [ordered]@{passed=$true; destination=$root; version_1_open_during_update=$true; pointer_version=2; launcher_opened_version_2=$true; old_version_removed_after_exit=$true; installed_after_cleanup_mb=$installedMb; installed_after_cleanup_file_count=$installedFiles.Count; installed_measurement_completed=$true; removed=$true} |
        ConvertTo-Json | Set-Content -Encoding UTF8 $report
} catch {
    [ordered]@{passed=$false; destination=$root; failed_step=$step; error=$_.Exception.Message; details=($_ | Out-String)} |
        ConvertTo-Json | Set-Content -Encoding UTF8 $report
    throw
} finally {
    if ($app -and -not $app.HasExited) { $app.CloseMainWindow() | Out-Null }
    if ($updatedApp -and -not $updatedApp.HasExited) { $updatedApp.CloseMainWindow() | Out-Null }
}
