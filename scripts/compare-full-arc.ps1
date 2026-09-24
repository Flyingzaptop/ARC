param(
 [Parameter(Mandatory=$true)][string]$Output,
 [string]$Package='C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/full-arc-20260924/package'
)
$ErrorActionPreference='Stop'
if(Get-Process Tests,FFX_BRIXELIZER_GI_DX12 -ErrorAction SilentlyContinue){throw 'A benchmark renderer is already running'}
$manifest=Get-Content -LiteralPath (Join-Path $Package 'manifest.json') -Raw | ConvertFrom-Json
foreach($entry in $manifest.PSObject.Properties){
 if((Get-FileHash -LiteralPath (Join-Path $Package $entry.Name)).Hash -ne $entry.Value.sha256){throw "Package hash mismatch: $($entry.Name)"}
}
$repo=(Resolve-Path "$PSScriptRoot/..").Path
$destinations=@{
 'arcResidentQueueCS.cso'='C:/Users/r3d_flzp/ARC-Hardening-GPU/WickedEngine/Samples/Tests/shaders/hlsl6/arcResidentQueueCS.cso'
 'arcResidentPixelsCS.cso'='C:/Users/r3d_flzp/ARC-Hardening-GPU/WickedEngine/Samples/Tests/shaders/hlsl6/arcResidentPixelsCS.cso'
 'Tests.exe'='C:/Users/r3d_flzp/ARC-Hardening-GPU/WickedEngine/BUILD/x64/Release/Tests/Tests.exe'
 'FFX_BRIXELIZER_GI_DX12.exe'='C:/Users/r3d_flzp/ARC-Hardening-GPU/benchmarks/FidelityFX-1.1.4/sdk/bin/FFX_BRIXELIZER_GI_DX12.exe'
}
$backup=Join-Path ([IO.Path]::GetTempPath()) ('arc-full-restore-'+[Guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $backup | Out-Null
try {
 foreach($name in $destinations.Keys){
  Copy-Item -LiteralPath $destinations[$name] -Destination (Join-Path $backup $name)
  Copy-Item -LiteralPath (Join-Path $Package $name) -Destination $destinations[$name]
 }
 foreach($renderer in @('wicked','cauldron')){
  $result=Join-Path $Output $renderer
  & C:/Python314/python.exe "$repo/scripts/run-full-arc-comparison.py" $result --renderer $renderer --static-camera --package $Package
  if($LASTEXITCODE -ne 0){throw "Comparison failed: $renderer"}
  & C:/Python314/python.exe "$repo/scripts/analyze-full-arc-comparison.py" $result --renderer $renderer
  if($LASTEXITCODE -ne 0){throw "Analysis failed: $renderer"}
 }
} finally {
 foreach($name in $destinations.Keys){
  $saved=Join-Path $backup $name
  if(Test-Path -LiteralPath $saved){Copy-Item -LiteralPath $saved -Destination $destinations[$name]}
 }
}
