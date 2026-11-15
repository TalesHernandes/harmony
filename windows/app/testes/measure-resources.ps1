param([Parameter(Mandatory=$true)][string]$Exe,[string]$Arguments='', [Parameter(Mandatory=$true)][string]$Name,[int]$TimeoutSeconds=90)
$ErrorActionPreference='Stop'
if($Name -notmatch '^[A-Za-z0-9][A-Za-z0-9_-]*$'){throw 'Use a simple measurement name.'}
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$resolved=[IO.Path]::GetFullPath($Exe)
if(-not $resolved.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Only this worktree executable may be launched.'}
$destination=Join-Path $PSScriptRoot 'medidas-otimizacao'
New-Item -ItemType Directory -Force $destination | Out-Null
foreach($suffix in @('-stages.jsonl','-external.jsonl','-stdout.log')){if(Test-Path -LiteralPath (Join-Path $destination ($Name+$suffix))){throw 'Choose a new name to keep runs separate.'}}
$env:HARMONY_RESOURCE_REPORT=Join-Path $destination ($Name+'-stages.jsonl')
$env:HARMONY_TEST_PARENT_PID=[string]$PID
$env:HARMONY_URL='http://127.0.0.1:9'
$env:LOCALAPPDATA=Join-Path $destination 'isolated-localappdata'
$env:APPDATA=Join-Path $destination 'isolated-appdata'
New-Item -ItemType Directory -Force $env:LOCALAPPDATA,$env:APPDATA | Out-Null
# Measurements contain counters only; app stderr stays local and is not copied into the report.
$launch=@{FilePath=$resolved;WindowStyle='Hidden';PassThru=$true;WorkingDirectory=$root;RedirectStandardOutput=(Join-Path $destination ($Name+'-stdout.log'));RedirectStandardError=(Join-Path $destination ($Name+'-stderr.log'))}
if($Arguments){$launch.ArgumentList=$Arguments}
$process=Start-Process @launch
$null=$process.Handle
$clock=[Diagnostics.Stopwatch]::StartNew()
$previousCpu=0.0;$previousMs=0.0
try {
 while(-not $process.HasExited){
  if($clock.Elapsed.TotalSeconds -gt $TimeoutSeconds){throw 'Measurement deadline exceeded.'}
  $process.Refresh()
  try {
   $ms=$clock.Elapsed.TotalMilliseconds;$cpu=$process.TotalProcessorTime.TotalMilliseconds
   $sample=[ordered]@{seconds=$ms/1000;pid=$process.Id;private_bytes=$process.PrivateMemorySize64;working_set_bytes=$process.WorkingSet64;handles=$process.HandleCount;threads=$process.Threads.Count;main_window_handle=$process.MainWindowHandle.ToInt64();cpu_percent_core=100*($cpu-$previousCpu)/[Math]::Max(1,($ms-$previousMs))}
   if($previousMs -eq 0){$sample.cpu_percent_core=$null}
   $previousCpu=$cpu;$previousMs=$ms
   $counters=Get-Counter -Counter '\GPU Process Memory(*)\Dedicated Usage','\GPU Process Memory(*)\Shared Usage','\GPU Engine(*)\Utilization Percentage' -ErrorAction SilentlyContinue
   $matching=@($counters.CounterSamples | Where-Object {$_.InstanceName -like ('pid_'+$process.Id+'_*')})
   $sample.gpu_seconds=$clock.Elapsed.TotalSeconds
   $sample.gpu=@($matching | ForEach-Object {@{counter=$_.Path.Substring($_.Path.LastIndexOf('\')+1);instance=$_.InstanceName;value=$_.CookedValue}})
   $sample | ConvertTo-Json -Depth 5 -Compress | Add-Content -Encoding UTF8 (Join-Path $destination ($Name+'-external.jsonl'))
  } catch {if(-not $process.HasExited){throw}}
  Start-Sleep -Milliseconds 250
 }
 $process.WaitForExit();$process.Refresh()
 'exit_code='+$process.ExitCode | Set-Content (Join-Path $destination ($Name+'-exit.txt'))
 Write-Output ($Name+' exit='+$process.ExitCode)
} finally {
 if(-not $process.HasExited){$process.Refresh();if($process.Path -eq $resolved){$process.Kill();$process.WaitForExit()}}
}

if($process.ExitCode -ne 0){exit $process.ExitCode}
