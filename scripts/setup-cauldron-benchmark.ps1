param([Parameter(Mandatory=$true)][string]$Destination, [string]$Python='python')
$ErrorActionPreference='Stop'
$target=[IO.Path]::GetFullPath($Destination)
if(Test-Path -LiteralPath $target){throw 'Use a fresh dedicated benchmark directory'}
New-Item -ItemType Directory -Path $target | Out-Null
$archive=Join-Path $target 'FidelityFX-SDK-v1.1.4.zip'
Invoke-WebRequest 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v1.1.4/FidelityFX-SDK-v1.1.4.zip' -OutFile $archive
# Locally pinned digest of the official release archive. This release does not
# publish an independent digest in GitHub release metadata.
if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne '0216556BFB0E243CEC30004A2A98D38F4E3F7406CB7938E3C1B85C758E95D952'){throw 'Archive digest mismatch'}
$sdk=Join-Path $target 'sdk'
Expand-Archive -LiteralPath $archive -DestinationPath $sdk
Push-Location $sdk
try {
    & '.\sdk\tools\media_delivery\MediaDelivery.exe' '--target-sha256=7c82a9704f11c082ad12115eadb944e7884a73cb678bb51e8c7486a3190c7f98'
    if($LASTEXITCODE){throw 'Media delivery failed'}
    & $Python (Join-Path $PSScriptRoot 'prepare-cauldron-benchmark.py') $sdk
    if($LASTEXITCODE){throw 'Benchmark adapter preparation failed'}
    $vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products '*' -property installationPath
    $cmake=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    & $cmake -S sdk -B build-sdk-arc -A x64 -DFFX_API_BACKEND=DX12_X64 -DFFX_BRIXELIZER=ON -DFFX_BRIXELIZER_GI=ON -DFFX_AUTO_COMPILE_SHADERS=ON
    if($LASTEXITCODE){throw 'SDK configuration failed'}
    & $cmake --build build-sdk-arc --config Release -j 4
    if($LASTEXITCODE){throw 'SDK build failed'}
    & $cmake -S . -B build-arc -A x64 -DBUILD_TYPE=SAMPLES_DX12 -DFFX_BRIXELIZER_GI=ON -Wno-dev
    if($LASTEXITCODE){throw 'Host configuration failed'}
    & $cmake --build build-arc --config ReleaseDX12 --target FFX_BRIXELIZER_GI -j 4
    if($LASTEXITCODE){throw 'Host build failed'}
} finally {Pop-Location}
Write-Output "Benchmark ready: $sdk"
