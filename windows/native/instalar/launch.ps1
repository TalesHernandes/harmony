param([switch]$PassThru)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $root '.harmony-proof-install'))) { throw 'Installation marker missing.' }
# Validate the selected complete version on every launch. Staging directories are ignored.
$pointer = Get-Content -LiteralPath (Join-Path $root 'current.json') -Raw | ConvertFrom-Json
if ($pointer.directory -cne "app-$([int]$pointer.version)") { throw 'Invalid version pointer.' }
$directory = Join-Path $root $pointer.directory
$manifest = Get-Content -LiteralPath (Join-Path $directory 'manifest.json') -Raw | ConvertFrom-Json
if (-not $manifest.complete -or [int]$manifest.version -ne [int]$pointer.version) { throw 'Incomplete version refused.' }
$seen = @{}
foreach ($entry in $manifest.files) {
    $file = [IO.Path]::GetFullPath((Join-Path $directory $entry.path))
    if (-not $file.StartsWith($directory + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest path outside version refused.' }
    if ($seen.ContainsKey($file)) { throw 'Duplicate manifest entry.' }; $seen[$file] = $true
    if ((Get-Item -LiteralPath $file).Length -ne $entry.bytes -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Invalid installed file: $($entry.path)" }
}
$exe = Join-Path $directory 'telas.exe'
if (-not $seen.ContainsKey($exe)) { throw 'telas.exe missing from manifest.' }
# The voice/GPU proof is copied for size only and is never executed here.
Start-Process -FilePath $exe -WorkingDirectory $directory -PassThru:$PassThru
