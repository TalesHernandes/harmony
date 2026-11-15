$ErrorActionPreference = 'Stop'
$test = Join-Path $PSScriptRoot ('..\test-output\w7-download-safety-' + [Guid]::NewGuid().ToString('N'))
$test = [IO.Path]::GetFullPath($test)
$priorRoot = $env:HARMONY_INSTALLER_TEST_ROOT; $priorOrigin = $env:HARMONY_URL
$bridge = Join-Path $PSScriptRoot '..\test-output\packages\harmony-126-native.exe'
try {
    New-Item -ItemType Directory -Path $test | Out-Null
    [IO.File]::WriteAllText((Join-Path $test '.harmony-installer-test'), 'disposable download safety fixture')
    $env:HARMONY_INSTALLER_TEST_ROOT = $test; $env:HARMONY_URL = 'http://127.0.0.1:18197'
    Remove-Item Env:\HARMONY_INSTALLER_TEST_ROOT
    foreach ($exe in @($bridge)) {
        if ([IO.File]::ReadAllText($exe + '.test-only.sha256').Trim() -ne (Get-FileHash -LiteralPath $exe).Hash) { throw 'Rebuild test-only fixtures before launching.' }
        foreach ($arguments in @('--quiet', '--uninstall --quiet')) {
            $p = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -Wait
            if ($p.ExitCode -notin @(1,2)) { throw 'Test executable accepted missing isolated context.' }
        }
    }
    Write-Output 'PASS: test-only bridge refuses normal and uninstall launches without test context.'
    $env:HARMONY_INSTALLER_TEST_ROOT = $test
    $downloads = Join-Path $test 'downloads'
    $outside = Join-Path $test 'untouched'
    New-Item -ItemType Directory -Path $downloads,$outside | Out-Null
    $source = Join-Path $downloads 'bridge.exe'
    Copy-Item -LiteralPath $bridge -Destination $source
    $sentinel = Join-Path $outside 'sentinel.txt'
    [IO.File]::WriteAllText($sentinel, 'must remain unchanged')
    $programs = Join-Path $test 'programs'
    New-Item -ItemType Junction -Path $programs -Target $outside | Out-Null
    try {
        $p = Start-Process -FilePath $source -WindowStyle Hidden -PassThru -Wait
        if ($p.ExitCode -ne 1) { throw 'Destination junction was not refused.' }
        if (@(Get-ChildItem -LiteralPath $outside -Force).Count -ne 1 -or [IO.File]::ReadAllText($sentinel) -cne 'must remain unchanged') { throw 'Destination junction target was modified.' }
        Write-Output 'PASS: Downloads bridge refuses destination junction without writing through it.'
    } finally {
        # Remove the junction itself, never recursively traverse its target.
        [IO.Directory]::Delete($programs)
    }
    $alias = Join-Path $test 'download-alias'
    New-Item -ItemType Junction -Path $alias -Target $downloads | Out-Null
    try {
        $p = Start-Process -FilePath (Join-Path $alias 'bridge.exe') -WindowStyle Hidden -PassThru -Wait
        if ($p.ExitCode -ne 1 -or (Test-Path -LiteralPath (Join-Path $programs 'harmony\harmony.exe'))) { throw 'Source junction was not refused.' }
        Write-Output 'PASS: Downloads bridge refuses source junction instead of following it.'
    } finally { [IO.Directory]::Delete($alias) }
} finally {
    $env:HARMONY_INSTALLER_TEST_ROOT = $priorRoot; $env:HARMONY_URL = $priorOrigin
}
