param(
    [string]$ArcRepoRoot = '',
    [string]$ExternalRoot = ''
)

$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$UpstreamUrl='https://github.com/microsoft/DirectX-Graphics-Samples.git'
$UpstreamSha='213dd4fd4918ea009dd8f35adee1aff1f2ecaba4'

if([string]::IsNullOrWhiteSpace($ArcRepoRoot)){
    $ArcRepoRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}else{
    $ArcRepoRoot=(Resolve-Path -LiteralPath $ArcRepoRoot).Path
}
if([string]::IsNullOrWhiteSpace($ExternalRoot)){
    $ExternalRoot=Join-Path $env:TEMP "arc-stage14-external-build-$PID"
}

$pf86=[Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
$vswhere=Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
if(-not(Test-Path -LiteralPath $vswhere)){throw 'vswhere.exe not found.'}
$installation=(& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1)
if(-not$installation){throw 'Visual Studio/MSBuild installation not found.'}
$msbuild=Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
if(-not(Test-Path -LiteralPath $msbuild)){throw "MSBuild not found: $msbuild"}

$nugetCmd=Get-Command nuget.exe -ErrorAction SilentlyContinue
if($nugetCmd){
    $nuget=$nugetCmd.Source
}else{
    $nuget=Join-Path $env:TEMP 'arc-nuget.exe'
    if(-not(Test-Path -LiteralPath $nuget)){
        Invoke-WebRequest 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile $nuget
    }
}

try{
    if(Test-Path -LiteralPath $ExternalRoot){Remove-Item -LiteralPath $ExternalRoot -Recurse -Force}
    & git.exe clone --filter=blob:none --no-checkout $UpstreamUrl $ExternalRoot
    if($LASTEXITCODE-ne 0){throw "External clone failed: $LASTEXITCODE"}
    & git.exe -C $ExternalRoot sparse-checkout init --cone
    if($LASTEXITCODE-ne 0){throw "sparse-checkout init failed: $LASTEXITCODE"}
    & git.exe -C $ExternalRoot sparse-checkout set Samples/Desktop/D3D12HelloWorld/src/HelloTexture
    if($LASTEXITCODE-ne 0){throw "sparse-checkout set failed: $LASTEXITCODE"}
    & git.exe -C $ExternalRoot fetch origin $UpstreamSha --depth=1
    if($LASTEXITCODE-ne 0){throw "Pinned fetch failed: $LASTEXITCODE"}
    & git.exe -C $ExternalRoot checkout --detach $UpstreamSha
    if($LASTEXITCODE-ne 0){throw "Pinned checkout failed: $LASTEXITCODE"}

    $actual=(& git.exe -C $ExternalRoot rev-parse HEAD).Trim()
    if($actual-ne$UpstreamSha){throw "Upstream SHA mismatch: $actual"}

    $overlay=Join-Path $ArcRepoRoot 'scripts\apply-directx-hello-texture-integration.ps1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $overlay -ExternalRoot $ExternalRoot -ArcRepoRoot $ArcRepoRoot
    if($LASTEXITCODE-ne 0){throw "Overlay failed: $LASTEXITCODE"}

    $sample=Join-Path $ExternalRoot 'Samples\Desktop\D3D12HelloWorld\src\HelloTexture'
    $project=Join-Path $sample 'D3D12HelloTexture.vcxproj'
    $packages=Join-Path (Split-Path -Parent $sample) 'packages'

    & $nuget restore $project -PackagesDirectory $packages -NonInteractive
    if($LASTEXITCODE-ne 0){throw "NuGet restore failed: $LASTEXITCODE"}

    & $msbuild $project /m /t:Build /p:Configuration=Release /p:Platform=x64 "/p:ArcRepoRoot=$ArcRepoRoot" /verbosity:minimal
    if($LASTEXITCODE-ne 0){throw "External renderer build failed: $LASTEXITCODE"}

    $exe=Join-Path $sample 'bin\x64\Release\D3D12HelloTexture.exe'
    if(-not(Test-Path -LiteralPath $exe)){throw "External renderer executable missing: $exe"}
    Write-Host "External renderer build check PASS: $actual" -ForegroundColor Green
}finally{
    if(Test-Path -LiteralPath $ExternalRoot){Remove-Item -LiteralPath $ExternalRoot -Recurse -Force -ErrorAction SilentlyContinue}
}
