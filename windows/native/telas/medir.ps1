[CmdletBinding()]
param([string]$Arquivo, [string]$Exe)
$ErrorActionPreference = 'Stop'
if (-not $Arquivo) { $Arquivo = Join-Path $PSScriptRoot 'medidas.json' }
if (-not $Exe) { $Exe = Join-Path $PSScriptRoot 'out\telas.exe' }
$output = [IO.Path]::GetFullPath($Arquivo)
if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output }
$p = Start-Process -FilePath $Exe -ArgumentList "--medir `"$output`"" -WorkingDirectory ([IO.Path]::GetDirectoryName($Exe)) -PassThru
$null = $p.Handle
if (-not $p.WaitForExit(210000)) { Stop-Process -Id $p.Id; throw 'Measurement timed out.' }
if (-not (Test-Path -LiteralPath $output)) {
    [ordered]@{passed=$false; exit_code=$p.ExitCode; error="No measurement result (exit $($p.ExitCode))."} | ConvertTo-Json | Set-Content -LiteralPath $output -Encoding UTF8
    throw "No measurement result (exit $($p.ExitCode))."
}
$result = Get-Content -LiteralPath $output -Raw -Encoding UTF8 | ConvertFrom-Json
$result | ConvertTo-Json -Depth 5
if ($result.error -or $result.animation.distinct_rendering_frames -le 1 -or $result.wheel.distinct_rendering_frames -le 1 -or -not $result.wheel_sendinput_ok) { throw 'Measurement failed; see result.' }
