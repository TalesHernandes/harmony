# Run on the interactive desktop after building the application.
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent
$work = Join-Path $root 'windows\app\queixas\w4'
$destination = Join-Path $work ('monitor-r23-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
if (Test-Path -LiteralPath $destination) { throw "Output already exists: $destination" }
New-Item -ItemType Directory -Path $destination | Out-Null
$names = @('HARMONY_WGC_REGULAR_TIMESTAMPS', 'HARMONY_SFU_SYNC_DIAG', 'W4_NO_PIXELS', 'W4_MONITOR', 'W4_HEADED', 'W4_EXPECT_REFRESH_HZ', 'W4_GAME_MONITOR', 'W4_VIEWER_MONITOR', 'W4_GAME_EXPECT_REFRESH_HZ', 'W4_VIEWER_EXPECT_REFRESH_HZ')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
Push-Location $root
try {
    $env:HARMONY_SFU_SYNC_DIAG = '1'
    $env:W4_NO_PIXELS = '0'
    $env:W4_HEADED = '1'
    $env:HARMONY_WGC_REGULAR_TIMESTAMPS = '0'
    # Fixed shuffled blocks: each combination occurs once in every block.
    $schedule = @('PS', 'SS', 'PP', 'SP', 'PP', 'SP', 'SS', 'PS', 'SP', 'PS', 'PP', 'SS')
    $counts = @{}
    $env:W4_MONITOR = $null
    $env:W4_EXPECT_REFRESH_HZ = $null
    foreach ($pair in $schedule) {
            $counts[$pair] = 1 + [int]$counts[$pair]
            $round = $counts[$pair]
            $env:W4_GAME_MONITOR = if ($pair[0] -eq 'P') { 'primary' } else { 'secondary' }
            $env:W4_VIEWER_MONITOR = if ($pair[1] -eq 'P') { 'primary' } else { 'secondary' }
            $env:W4_GAME_EXPECT_REFRESH_HZ = if ($pair[0] -eq 'P') { '240' } else { '75' }
            $env:W4_VIEWER_EXPECT_REFRESH_HZ = if ($pair[1] -eq 'P') { '240' } else { '75' }
            $mode = "game-$($env:W4_GAME_MONITOR)-viewer-$($env:W4_VIEWER_MONITOR)"
            $prefix = Join-Path $destination ("monitor-{0}-round-{1}" -f $mode, $round)
            $json = Join-Path $work 'autoteste-tela.json'
            & node windows/app/testes/autoteste-processes.mjs --preflight *> ($prefix + '-preflight.log')
            if ($LASTEXITCODE -ne 0) { throw "Preflight refused next round; inspect $prefix-preflight.log" }
            $started = [DateTime]::UtcNow
            & cmd.exe /d /c windows\app\testes\autoteste-tela.cmd --minutos 1 --camera *> ($prefix + '-console.log')
            $code = $LASTEXITCODE
            if (!(Test-Path -LiteralPath $json) -or (Get-Item -LiteralPath $json).LastWriteTimeUtc -lt $started) {
                throw "No fresh autotest result: mode=$mode round=$round exit=$code"
            }
            Copy-Item -LiteralPath $json -Destination ($prefix + '.json')
            $native = Join-Path $work 'autoteste-tela-nativo.json'
            if ((Test-Path -LiteralPath $native) -and (Get-Item -LiteralPath $native).LastWriteTimeUtc -ge $started) {
                Copy-Item -LiteralPath $native -Destination ($prefix + '-native.json')
            }
            # autoteste-tela.cmd places the server log here and overwrites it each run.
            $serverLog = Join-Path $root 'windows\app\test-output\temp\harmony-e2e-server.log'
            if (!(Test-Path -LiteralPath $serverLog) -or (Get-Item -LiteralPath $serverLog).LastWriteTimeUtc -lt $started) {
                throw "No fresh server log: mode=$mode round=$round exit=$code"
            }
            Copy-Item -LiteralPath $serverLog -Destination ($prefix + '-servidor.log')
            $result = Get-Content -LiteralPath $json -Raw | ConvertFrom-Json
            Write-Output ("mode={0} round={1} exit={2} passed={3}" -f $mode, $round, $code, $result.passed)
            # A failed acceptance threshold is a result; a broken harness is not.
            & node windows/app/testes/autoteste-processes.mjs --check-result ($prefix + '.json') *> ($prefix + '-continuation.log')
            if ($LASTEXITCODE -ne 0) { throw "Comparison cannot continue; inspect $prefix-continuation.log" }
            if ($result.cleanup_failure) {
                Write-Warning ("Cleanup failed; fresh preflight passed; result remains failed: {0}" -f $result.cleanup_failure)
            }
    }
    Write-Output "Comparison saved: $destination"
} finally {
    Pop-Location
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
}
