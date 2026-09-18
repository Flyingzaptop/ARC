param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$WorkRoot = (Join-Path $env:TEMP 'ARC-Stage14_5-Wicked-BuildCheck'),
    [switch]$KeepWork
)

$ErrorActionPreference = 'Stop'
$WickedRepo = 'https://github.com/turanszkij/WickedEngine.git'
$WickedSha = '0b4dd9ebe0025a4a6d8f17c52c943c40d96d62a7'

if ($env:OS -ne 'Windows_NT') {
    Write-Host 'Stage 14.5 Wicked build check is Windows-only.'
    exit 0
}

$RepoRoot = (Resolve-Path $RepoRoot).Path
if (Test-Path $WorkRoot) { Remove-Item $WorkRoot -Recurse -Force }
New-Item -ItemType Directory -Path $WorkRoot | Out-Null

$arcBuild = Join-Path $WorkRoot 'arc-build'
$wicked = Join-Path $WorkRoot 'WickedEngine'

try {
    Write-Host '=== ARC /MT Release build ==='
    $cmakeArgs = @(
        '-S', $RepoRoot,
        '-B', $arcBuild,
        '-A', 'x64',
        '-DARC_BUILD_TESTS=ON',
        '-DARC_GPU_TESTS=OFF',
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded'
    )
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw 'ARC configure failed.' }

    & cmake --build $arcBuild --config Release --target arc-core arc-dx12-observer
    if ($LASTEXITCODE -ne 0) { throw 'ARC Release build failed.' }

    Write-Host '=== Sparse checkout pinned Wicked Engine ==='
    & git clone --filter=blob:none --no-checkout $WickedRepo $wicked
    if ($LASTEXITCODE -ne 0) { throw 'Wicked clone failed.' }
    & git -C $wicked sparse-checkout init --cone
    if ($LASTEXITCODE -ne 0) { throw 'Wicked sparse checkout init failed.' }
    & git -C $wicked sparse-checkout set WickedEngine Samples/Tests
    if ($LASTEXITCODE -ne 0) { throw 'Wicked sparse checkout selection failed.' }
    & git -C $wicked checkout --detach $WickedSha
    if ($LASTEXITCODE -ne 0) { throw 'Wicked pinned checkout failed.' }

    $actual = (& git -C $wicked rev-parse HEAD).Trim()
    if ($actual -ne $WickedSha) { throw "Wicked SHA mismatch: $actual" }

    Write-Host '=== Apply ARC Wicked overlay ==='
    $overlayArgs = @{
        WickedRoot = $wicked
        ArcRoot = $RepoRoot
        ArcBuildRoot = $arcBuild
    }
    & (Join-Path $RepoRoot 'scripts\apply-wicked-engine-integration.ps1') @overlayArgs
    if ($LASTEXITCODE -ne 0) { throw 'Wicked overlay failed.' }

    $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found.' }
    $msbuild = (& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1)
    if (-not $msbuild) { throw 'MSBuild.exe not found.' }

    $msbuildVersionText = (& $msbuild -version -nologo | Select-Object -Last 1).Trim()
    if ($msbuildVersionText -notmatch '^(\d+)\.') { throw "Unable to parse MSBuild version: $msbuildVersionText" }
    $msbuildMajor = [int]$Matches[1]
    if ($msbuildMajor -ge 18) { $platformToolset = 'v145' }
    elseif ($msbuildMajor -ge 17) { $platformToolset = 'v143' }
    else { throw "MSBuild $msbuildVersionText is too old for the Stage 14.5 C++23 bridge." }

    Write-Host "MSBuild: $msbuild ($msbuildVersionText / $platformToolset)"
    Write-Host '=== Build patched Wicked Samples/Tests ==='
    $msbuildArgs = @(
        (Join-Path $wicked 'Samples\Tests\Tests.vcxproj'),
        '/m',
        '/p:Configuration=Release',
        '/p:Platform=x64',
        "/p:PlatformToolset=$platformToolset",
        "/p:SolutionDir=$wicked\",
        "/p:ArcRepoRoot=$RepoRoot",
        "/p:ArcBuildRoot=$arcBuild"
    )
    & $msbuild @msbuildArgs
    if ($LASTEXITCODE -ne 0) { throw 'Patched Wicked Tests build failed.' }

    $exe = Join-Path $wicked 'BUILD\x64\Release\Tests\Tests.exe'
    if (-not (Test-Path $exe)) { throw "Expected executable missing: $exe" }

    Write-Host 'Stage 14.5 Wicked build check: PASS'
    Write-Host "Executable: $exe"
}
finally {
    if (-not $KeepWork -and (Test-Path $WorkRoot)) {
        Remove-Item $WorkRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
