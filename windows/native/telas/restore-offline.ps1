$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
# This is deliberately a closed local restore, not a NuGet dependency resolver.
# The nine archives are already approved, including all transitive build dependencies.
$pins = @'
Microsoft.WindowsAppSDK.WinUI 2.3.9 d36145bd6c0f46fc04e07fb4af365d093fb42c39b625948d0ead07a0b26fce96
Microsoft.WindowsAppSDK.Foundation 2.3.12 ce04d01d68cb16b5dc85a5c9efb8162dba876bc2e5b766b5968979899d4efa9e
Microsoft.WindowsAppSDK.InteractiveExperiences 2.1.9 f9159d73e3e46717f159606e43b4acf8336aaff5bdd4d395fe3a93fdb41437ac
Microsoft.WindowsAppSDK.Base 2.0.4 e3e13478c4c80c59ed5f8f89542fe49a2985daa484753e93a5858e90c2d46a4d
Microsoft.WindowsAppSDK.DWrite 2.1.0 91746abd6a780b84001f9c465507830d5a24c79912bc4aa0d35938c71c1c6b5f
Microsoft.Windows.CppWinRT 3.0.260818.1 a8993608ae9263a8288e1a1d9ee2684a5632f4acc59e1e89ddd86b3b889a7367
Microsoft.Windows.SDK.BuildTools 10.0.26100.4654 5f5783658a200f83ea2addfd406e2303a901b27ad7c49b0347c34aefcc21883d
Microsoft.Windows.SDK.BuildTools.MSIX 1.7.251221100 e61c560805029f5ae15433be52e153f3862b6faaf725292638a9ece592f7a060
Microsoft.Web.WebView2 1.0.3719.77 2f6be3a10a1a8d6d1fde986af4131dab344f8181fffe75590824e0f4b037ed73
'@
$source = "$env:USERPROFILE\harmony-deps\nuget"
$cache = Join-Path $PSScriptRoot '.nuget-packages'
[xml]$config = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'nuget.config') -Raw
$sources = @($config.configuration.packageSources.add)
if ($sources.Count -ne 1 -or $sources[0].value -ne $source -or $null -eq $config.configuration.packageSources.clear) { throw 'Only the approved offline source is allowed.' }
[xml]$packages = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'packages.config') -Raw
$declared = @($packages.packages.package)
$rows = @($pins.Trim() -split '\r?\n')
if ($declared.Count -ne $rows.Count) { throw 'Unapproved package count.' }
foreach ($row in $rows) {
    $id, $version, $sha = $row -split ' '
    if (@($declared | Where-Object { $_.id -ceq $id -and $_.version -ceq $version }).Count -ne 1) { throw "Unapproved package/version: $id" }
    $archive = Join-Path $source ($id.ToLowerInvariant() + '.' + $version + '.nupkg')
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $sha) { throw "Package checksum mismatch: $id" }
    $directory = Join-Path $cache ($id + '.' + $version)
    if (Test-Path -LiteralPath $directory) {
        $resolved = [IO.Path]::GetFullPath($directory)
        if (-not $resolved.StartsWith([IO.Path]::GetFullPath($cache) + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Cache path outside proof.' }
        if ((Get-Item -LiteralPath $directory).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Cache reparse point refused.' }
        Remove-Item -LiteralPath $directory -Recurse -Force
    }
    [IO.Compression.ZipFile]::ExtractToDirectory($archive, $directory)
    Write-Output "Restored approved local archive: $id $version (SHA-256 OK)"
}
