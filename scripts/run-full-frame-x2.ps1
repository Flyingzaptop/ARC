param([string]$OutputDirectory='', [ValidateSet('Release','Debug')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if(-not $OutputDirectory){$OutputDirectory=Join-Path $repo ('traces\full-frame-'+(Get-Date -Format yyyyMMdd-HHmmss))}
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new output directory; existing results are retained.'}
$run=[IO.Path]::GetFullPath($OutputDirectory)
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=(& $vswhere -latest -products '*' -property installationPath).Trim()
$cmake=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$build=Join-Path $repo 'build'
& $cmake -S $repo -B $build -A x64 -DARC_BUILD_TESTS=ON
if($LASTEXITCODE -ne 0){throw 'Configure failed'}
& $cmake --build $build --config $Configuration --target arc-full-frame-x2 arc-perceptual-trial-tests arc-predictive-perceptual-tests --parallel 2
if($LASTEXITCODE -ne 0){throw 'Build failed'}
foreach($test in @('arc-perceptual-trial-tests','arc-predictive-perceptual-tests')){
    & (Join-Path $build "$Configuration\$test.exe")
    if($LASTEXITCODE -ne 0){throw "$test failed"}
}
& python -c 'import numpy'
if($LASTEXITCODE -ne 0){throw 'Independent validation requires Python with NumPy; no packages are installed automatically.'}
New-Item -ItemType Directory -Path $run|Out-Null
$exe=Join-Path $build "$Configuration\arc-full-frame-x2.exe"
$sha=(& git -C $repo rev-parse HEAD).Trim()
[ordered]@{schema=1;source_sha=$sha;tracked_changes=@(& git -C $repo diff --name-only HEAD);binary_sha256=(Get-FileHash -LiteralPath $exe).Hash;configuration=$Configuration;started_utc=[DateTime]::UtcNow.ToString('o');games_launched=$false;scenario_order=@(0,1,2,3,4);metric='serialized full-frame render/Present throughput, not game FPS'}|ConvertTo-Json -Depth 4|Set-Content (Join-Path $run 'manifest.json')
foreach($scene in 0..4){
    $directory=Join-Path $run "scene-$scene"
    Write-Host "Full-frame scenario $scene / 4; candidate probes then counterbalanced verification"
    & $exe $directory $scene
    if($LASTEXITCODE -ne 0){throw "Renderer failed on scenario $scene; evidence retained at $directory"}
    & python (Join-Path $PSScriptRoot 'validate-full-frame-x2.py') $directory > (Join-Path $run "validation-$scene.log")
    if($LASTEXITCODE -ne 0){throw "Independent validation failed on scenario $scene"}
    $result=Get-Content (Join-Path $directory 'independent-acceptance.json') -Raw|ConvertFrom-Json
    Write-Host ('Scenario {0}: {1:N1} -> {2:N1} throughput FPS; x{3:N2}; 2x gate {4}' -f $scene,$result.baseline_throughput_fps,$result.modified_throughput_fps,$result.speedup,$result.x2_goal)
}
Write-Host "All measurements retained: $run"
