param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference='Stop'
$source=Join-Path $env:USERPROFILE 'harmony-deps\dfn-0.5.6'
$pinned=@{
 'df.dll'=@(16678912,'4040ce3da1dd1c26e15d2615f49fc38a92566a0384a0c4f1d149b8fe852f320f')
 'df.dll.lib'=@(2876,'6afecdaa8ad52375bc080f993069ed232508184f3e730e9bc5f47b361e6dc25f')
 'DeepFilterNet3_onnx.tar.gz'=@(7983136,'c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616')
}
foreach($name in $pinned.Keys) {
 $file=Join-Path $source $name
 if((Get-Item -LiteralPath $file).Length -ne $pinned[$name][0] -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pinned[$name][1]) { throw "Pinned DFN artifact mismatch: $name" }
}
New-Item -ItemType Directory -Force $Destination | Out-Null
foreach($name in @('df.dll','DeepFilterNet3_onnx.tar.gz')) { Copy-Item -LiteralPath (Join-Path $source $name) -Destination $Destination -Force }
$licenses=Join-Path $Destination 'licenses\DeepFilterNet-0.5.6'
New-Item -ItemType Directory -Force $licenses | Out-Null
foreach($name in @('LICENSE','LICENSE-MIT','LICENSE-APACHE','SHA256SUMS','COMO-FOI-FEITO.txt')) { Copy-Item -LiteralPath (Join-Path $source $name) -Destination $licenses -Force }
