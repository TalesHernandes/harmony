$ErrorActionPreference = 'Stop'
$fixtures = Join-Path $PSScriptRoot 'test-fixtures'
$dest = Join-Path $PSScriptRoot 'teste-destino'
if (Test-Path -LiteralPath $dest) { throw 'Remove the previous test installation before running this test.' }
New-Item -ItemType Directory -Force (Join-Path $fixtures 'emoji') | Out-Null
# Non-executable fixtures: this test never claims to validate WinUI installation.
[IO.File]::WriteAllText((Join-Path $fixtures 'telas.exe'), 'NOT AN EXECUTABLE: installation transaction test')
[IO.File]::WriteAllText((Join-Path $fixtures 'runtime.dll'), 'test runtime file')
[IO.File]::WriteAllText((Join-Path $fixtures 'emoji\sample.svg'), '<svg/>')
[IO.File]::WriteAllText((Join-Path $fixtures 'voice-fixture.bin'), 'NOT AN EXECUTABLE: copied only')
$harmony = Join-Path $fixtures 'voice-fixture.bin'
& (Join-Path $PSScriptRoot 'instalar.ps1') -Versao 1 -Origem $fixtures -Harmony $harmony -Destino $dest | Out-Null
$manifest = Get-Content (Join-Path $dest 'app-1\manifest.json') -Raw | ConvertFrom-Json
if ($manifest.files.Count -ne 5) { throw 'Unexpected manifest count.' }
$handle = [IO.File]::Open((Join-Path $dest 'app-1\runtime.dll'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
try {
    & (Join-Path $PSScriptRoot 'instalar.ps1') -Versao 2 -Origem $fixtures -Harmony $harmony -Destino $dest | Out-Null
    if (-not (Test-Path (Join-Path $dest 'app-1\runtime.dll'))) { throw 'Locked version was removed.' }
    $pointer = Get-Content (Join-Path $dest 'current.json') -Raw | ConvertFrom-Json
    if ($pointer.version -ne 2) { throw 'Pointer not updated.' }
} finally { $handle.Dispose() }
& (Join-Path $PSScriptRoot 'instalar.ps1') -Limpar -Destino $dest
if (Test-Path (Join-Path $dest 'app-1')) { throw 'Explicit cleanup did not remove the unlocked version.' }
# Simulate an interrupted copy. It cannot become the selected version.
New-Item -ItemType Directory (Join-Path $dest '.staging-interrupted') | Out-Null
[IO.File]::WriteAllText((Join-Path $dest '.staging-interrupted\telas.exe'), 'incomplete')
# Also simulate interruption after a completed directory rename, before pointer commit.
Copy-Item -LiteralPath (Join-Path $dest 'app-2') -Destination (Join-Path $dest 'app-3') -Recurse
& (Join-Path $PSScriptRoot 'instalar.ps1') -Versao 3 -Origem $fixtures -Harmony $harmony -Destino $dest | Out-Null
if (Test-Path (Join-Path $dest 'app-1')) { throw 'Unlocked old version not cleaned.' }
if (Test-Path (Join-Path $dest '.staging-interrupted')) { throw 'Interrupted staging not cleaned.' }
# Corruption must be rejected before Start-Process.
[IO.File]::AppendAllText((Join-Path $dest 'app-3\runtime.dll'), 'corrupt')
$rejected = $false
try { & (Join-Path $dest 'launch.ps1') } catch { $rejected = $_.Exception.Message -like 'Invalid installed file:*' }
if (-not $rejected) { throw 'Corrupted file was not rejected.' }
& (Join-Path $PSScriptRoot 'instalar.ps1') -Remover -Destino $dest
if (Test-Path -LiteralPath $dest) { throw 'Removal incomplete.' }
[ordered]@{
    passed=$true; fixture_only=$true; real_telas_exe_tested=$false
    manifest_verified=$true; locked_version_retained=$true; pointer_updated=$true
    stale_copy_ignored_and_cleaned=$true; corrupted_payload_rejected=$true; uninstall_verified=$true
    interrupted_before_pointer_recovered=$true
    explicit_cleanup_verified=$true
} | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $PSScriptRoot 'teste-logica.json')
Get-Content (Join-Path $PSScriptRoot 'teste-logica.json')
