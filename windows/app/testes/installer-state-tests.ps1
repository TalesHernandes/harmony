$ErrorActionPreference='Stop'
$test=Join-Path (Split-Path $PSScriptRoot -Parent) ('test-output\w7-state-' + [Guid]::NewGuid().ToString('N'))
$root=Join-Path $test 'programs\harmony'
$native=Join-Path $root 'native'
$key='HKCU:\Software\harmony-installer-tests\' + [IO.Path]::GetFileName($test)
$priorRoot=$env:HARMONY_INSTALLER_TEST_ROOT; $priorOrigin=$env:HARMONY_URL
$gate=$null; $gateOwned=$false; $child=$null
try {
    New-Item -ItemType Directory -Force $native | Out-Null
    [IO.File]::WriteAllText((Join-Path $test '.harmony-installer-test'),'disposable state regression')
    [IO.File]::WriteAllText((Join-Path $root '.harmony-installer'),'installer fixture')
    $env:HARMONY_INSTALLER_TEST_ROOT=$test; $env:HARMONY_URL='http://127.0.0.1:18197'
    # Persist the registry adapter in the fixture; never write to real HKCU.
    $adapter=Join-Path $test 'registry-adapter.ps1'
    @'
function Resolve-FixtureRegistry($Path) {
    if ($Path -like 'HKCU:*') { return Join-Path (Join-Path $env:HARMONY_INSTALLER_TEST_ROOT 'registry') $Path.Substring(6) }
    return $Path
}
function New-Item { param($Path,$ItemType,$Target,[switch]$Force,$ErrorAction)
    $PSBoundParameters.Path=Resolve-FixtureRegistry $Path
    if ($Path -like 'HKCU:*') { $PSBoundParameters.ItemType='Directory' }
    Microsoft.PowerShell.Management\New-Item @PSBoundParameters
}
function Test-Path { param($LiteralPath,$Path,$PathType='Any')
    if (-not $LiteralPath) { $LiteralPath=$Path }
    Microsoft.PowerShell.Management\Test-Path -LiteralPath (Resolve-FixtureRegistry $LiteralPath) -PathType $PathType
}
function New-ItemProperty { param($Path,$Name,$Value,$PropertyType,[switch]$Force)
    $directory=Resolve-FixtureRegistry $Path
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    [IO.File]::WriteAllText((Join-Path $directory ($Name+'.json')), (ConvertTo-Json -InputObject $Value))
}
function Get-ItemProperty { param($Path,$Name,$ErrorAction)
    $values=@{}
    foreach ($file in Get-ChildItem -LiteralPath (Resolve-FixtureRegistry $Path) -Filter '*.json' -ErrorAction SilentlyContinue) { $values[$file.BaseName]=Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json }
    [pscustomobject]$values
}
function Remove-ItemProperty { param($Path,$Name,$ErrorAction)
    $file=Join-Path (Resolve-FixtureRegistry $Path) ($Name+'.json')
    if ([IO.File]::Exists($file)) { [IO.File]::Delete($file) }
}
function Remove-Item { param($LiteralPath,[switch]$Recurse,[switch]$Force)
    Microsoft.PowerShell.Management\Remove-Item -LiteralPath (Resolve-FixtureRegistry $LiteralPath) -Recurse:$Recurse -Force:$Force
}
'@ | Set-Content -LiteralPath $adapter -Encoding UTF8
    . $adapter
    Copy-Item -LiteralPath (Join-Path (Split-Path $PSScriptRoot -Parent) 'instalar/live-app.ps1') -Destination (Join-Path $native 'live-app.ps1')
    . (Join-Path $native 'live-app.ps1')
    foreach ($version in 1,2) {
        $dir=Join-Path $native "app-$version"
        [IO.Directory]::CreateDirectory($dir) | Out-Null
        $exe=Join-Path $dir 'harmony-nativo.exe'
        [IO.File]::WriteAllText($exe,"fixture-$version")
        @{version=$version;complete=$true;files=@(@{path='harmony-nativo.exe';bytes=(Get-Item $exe).Length;sha256=(Get-FileHash $exe).Hash})} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $dir 'manifest.json') -Encoding UTF8
    }
    Promote-HarmonyVersion $native 2
    $uninstall="$key\Uninstall\harmony"; $run="$key\Run"
    New-Item -Path $uninstall -Force | Out-Null
    New-Item -Path $run -Force | Out-Null
    New-ItemProperty -Path $uninstall -Name DisplayVersion -Value '2' -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $run -Name harmony -Value 'unconfirmed' -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $run -Name unrelated -Value 'retained' -PropertyType String -Force | Out-Null
    $old=@{version=1;directory='app-1'} | ConvertTo-Json
    [IO.File]::WriteAllText((Join-Path $native 'current.json'),(@{version=2;directory='app-2';live='app'} | ConvertTo-Json))
    $registry=@{}
    $registry[$uninstall]=@{DisplayVersion=@{value='1';kind=1}}
    $registry[$run]=@{harmony=@{value='original';kind=1}}
    $state=@{previous=$old;release=2;registry=$registry;links=@{}} | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText((Join-Path $root 'recovery.json'),$state)
    $sha=[Security.Cryptography.SHA256]::Create()
    try { $id=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($root.ToLowerInvariant()))).Replace('-','') } finally { $sha.Dispose() }
    $gate=[Threading.Mutex]::new($false,('Local\harmony-installer-install-'+$id)); $gateOwned=$gate.WaitOne(1000)
    if(-not $gateOwned) { throw 'Fixture transition lock unavailable.' }
    $wrapper=Join-Path $test 'recover-test.ps1'
    [IO.File]::WriteAllText($wrapper, ('param([string]$Root)'+"`n. '"+$adapter+"'`ntry { & '"+(Join-Path (Split-Path $PSScriptRoot -Parent) 'instalar\bridge-host.ps1')+"' -Root `$Root -Recover } catch { Write-Error (`$_ | Out-String); Write-Error `$_.ScriptStackTrace; exit 1 }"))
    $arguments='-NoProfile -ExecutionPolicy Bypass -File "{0}" -Root "{1}"' -f $wrapper,$root
    $child=Start-Process powershell.exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $test 'child.err') -RedirectStandardOutput (Join-Path $test 'child.log')
    $null=$child.Handle
    Start-Sleep -Milliseconds 800
    if($child.HasExited -or (Get-Content (Join-Path $native 'current.json') -Raw | ConvertFrom-Json).version -ne 2) { throw 'Recovery bypassed the active transition lock.' }
    $gate.ReleaseMutex();$gateOwned=$false
    if(-not $child.WaitForExit(15000)) { throw 'Recovery helper timed out.' }
    $child.Refresh()
    if($child.ExitCode) { Get-Content (Join-Path $test 'child.err'); throw 'Recovery helper failed.' }
    $pointer=Get-Content (Join-Path $native 'current.json') -Raw | ConvertFrom-Json
    if($pointer.version -ne 1 -or (Get-ItemProperty $uninstall).DisplayVersion -ne '1' -or (Get-ItemProperty $run).harmony -ne 'original' -or (Get-ItemProperty $run).unrelated -ne 'retained' -or (Test-Path -LiteralPath (Join-Path $root 'recovery.json'))) { throw 'Recovery failed to restore committed state.' }
    if ($pointer.live -ne 'app' -or [IO.File]::ReadAllText((Join-Path $native 'app/harmony-nativo.exe')) -ne 'fixture-1') { throw 'Rollback did not repromote the previous version to the fixed path.' }
    Write-Output 'PASS: rollback repromotes the previous version before committing current.json.'
    Write-Output 'PASS: interrupted transaction restores current.json and owned registry values; unrelated startup retained (filesystem registry adapter).'
    Write-Output 'PASS: transition mutex prevents a second helper from mutating an active installation.'
} finally {
    if($gateOwned) { $gate.ReleaseMutex() };if($gate) { $gate.Dispose() }
    if($child -and -not $child.HasExited) { $child.Kill();$child.WaitForExit() }
    $env:HARMONY_INSTALLER_TEST_ROOT=$priorRoot; $env:HARMONY_URL=$priorOrigin
    $resolved=[IO.Path]::GetFullPath($test)
    if(-not $resolved.StartsWith([IO.Path]::GetFullPath((Join-Path (Split-Path $PSScriptRoot -Parent) 'test-output'))+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture cleanup.' }
    if(Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
