$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$msbuild = $env:PROOF_MSBUILD
if (-not $msbuild -or -not (Test-Path -LiteralPath $msbuild)) { throw 'Run build.cmd to locate MSBuild with vswhere.' }
Write-Output "MSBuild: $msbuild"
# All writable caches and logs stay in this proof's directory.
$env:NUGET_PACKAGES = Join-Path $root '.nuget-packages'
$env:TEMP = Join-Path $root 'temp'
$env:TMP = $env:TEMP
$env:APPDATA = Join-Path $root 'temp\appdata'
$env:LOCALAPPDATA = Join-Path $root 'temp\localappdata'
New-Item -ItemType Directory -Force $env:LOCALAPPDATA | Out-Null
New-Item -ItemType Directory -Force $env:APPDATA | Out-Null
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
$clock = [Diagnostics.Stopwatch]::StartNew()
& (Join-Path $root 'restore-offline.ps1')
foreach ($target in @('Rebuild')) {
    $remaining = 540000 - [int]$clock.ElapsedMilliseconds
    if ($remaining -le 0) { throw 'Build exceeded nine minutes.' }
    # Skip user SDK discovery; use the installed, explicitly pinned SDK instead.
    $arguments = '"{0}" /t:{1} /p:Configuration=Release /p:Platform=x64 /p:WindowsSdkDir_10= /p:WindowsSdkDir="C:/Program Files (x86)/Windows Kits/10/" /p:TrackFileAccess=false /p:MinimalRebuildFromTracking=false /p:RestorePackagesConfig=true /p:RestoreConfigFile="{2}" /p:RestoreRepositoryPath="{3}" /m:2 /nologo /v:minimal' -f (Join-Path $root 'telas.vcxproj'), $target, (Join-Path $root 'nuget.config'), $env:NUGET_PACKAGES
    $p = Start-Process -FilePath $msbuild -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $root "build-$target.log") -RedirectStandardError (Join-Path $root "build-$target.err")
    $null = $p.Handle
    if (-not $p.WaitForExit($remaining)) { & taskkill.exe /PID $p.Id /T /F | Out-Null; throw 'Build exceeded nine minutes.' }
    $p.Refresh()
    Get-Content (Join-Path $root "build-$target.log")
    Get-Content (Join-Path $root "build-$target.err")
    if ($p.ExitCode -ne 0) { throw "MSBuild $target failed ($($p.ExitCode)); no other package may be added." }
}
