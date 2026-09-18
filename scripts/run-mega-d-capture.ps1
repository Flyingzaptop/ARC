param([Parameter(Mandatory=$true)][string]$Executable,
      [Parameter(Mandatory=$true)][string]$WorkingDirectory,
      [Parameter(Mandatory=$true)][string]$NativeExecutable,
      [Parameter(Mandatory=$true)][string]$OutputDirectory,
      [int]$Seconds=20)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$sha=(& git -C $repo rev-parse HEAD).Trim()
if($LASTEXITCODE -ne 0){throw 'Source identity unavailable'}
if(@(& git -C $repo diff --name-only HEAD).Count){throw 'Commit tracked source before capture'}
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new output directory; prior evidence is immutable'}
$run=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $run | Out-Null
$settings=@{ARC_SOURCE_SHA=$sha;ARC_WICKED_ATTRIBUTION_OUTPUT=(Join-Path $run 'attribution.json');ARC_WICKED_OUTPUT=(Join-Path $run 'renderer.json');ARC_WICKED_EXPERIMENT_MODE='observe';ARC_WICKED_SECONDS=[string]$Seconds;ARC_WICKED_HOOK_TIMING='0';ARC_WICKED_WARMUP_SECONDS='3';ARC_WICKED_SCENE_OFFSET='0';ARC_WICKED_SCENE_SETTLE_MS='1500'}
$backup=@{}
try {
    & $NativeExecutable (Join-Path $run 'native.json')
    if($LASTEXITCODE -ne 0){throw "Native GPU test failed: $LASTEXITCODE"}
    foreach($key in $settings.Keys){$backup[$key]=[Environment]::GetEnvironmentVariable($key,'Process');[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
    [ordered]@{source_sha=$sha;wicked_sha='0b4dd9ebe0025a4a6d8f17c52c943c40d96d62a7';binary_sha256=(Get-FileHash -LiteralPath $Executable).Hash;native_binary_sha256=(Get-FileHash -LiteralPath $NativeExecutable).Hash;mode='observe';instrumented=$true;build_provenance='prebuilt executables; caller must build from this source';settings=$settings}|ConvertTo-Json -Depth 4|Set-Content (Join-Path $run 'manifest.json')
    $process=Start-Process -FilePath $Executable -ArgumentList 'alwaysactive' -WorkingDirectory $WorkingDirectory -PassThru
    $timer=[Diagnostics.Stopwatch]::StartNew()
    while(-not $process.WaitForExit(1000)){if($timer.Elapsed.TotalSeconds -gt 240){$process.Kill();throw 'Owned renderer process exceeded 240 seconds'}}
    $process.Refresh()
    if($process.ExitCode -ne 0){throw "Renderer exited $($process.ExitCode)"}
    $renderer=Get-Content (Join-Path $run 'renderer.json') -Raw|ConvertFrom-Json
    if($renderer.valid -ne $true -or $renderer.arc_source_sha -ne $sha -or $renderer.quality_actions -ne 0){throw 'Invalid observer run'}
    & (Join-Path $PSScriptRoot 'validate-mega-d.ps1') -NativeJson (Join-Path $run 'native.json') -WickedJson (Join-Path $run 'attribution.json') -ExpectedSha $sha -Output (Join-Path $run 'acceptance.json')
} catch {
    [ordered]@{schema=1;verdict='FAIL';source_sha=$sha;error=$_.Exception.Message}|ConvertTo-Json|Set-Content (Join-Path $run 'acceptance.json')
    throw
} finally {
    foreach($key in $backup.Keys){[Environment]::SetEnvironmentVariable($key,$backup[$key],'Process')}
}
