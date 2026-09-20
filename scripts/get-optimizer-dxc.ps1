param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$arcDestination=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $arcDestination){throw 'Use a fresh dependency directory'}
New-Item -ItemType Directory -Path $arcDestination | Out-Null
$arcArchive=Join-Path $arcDestination 'dxc_2026_05_27.zip'
$arcUrl='https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2602.24/dxc_2026_05_27.zip'
Invoke-WebRequest -Uri $arcUrl -OutFile $arcArchive
$arcHash=(Get-FileHash -LiteralPath $arcArchive -Algorithm SHA256).Hash.ToLowerInvariant()
if($arcHash -ne 'cf658aacf070d3045e31b8f1f8a696c2945f37c1095019481ef7c513368db3b4'){throw 'Official release digest mismatch'}
Expand-Archive -LiteralPath $arcArchive -DestinationPath $arcDestination
[ordered]@{release='v1.9.2602.24';url=$arcUrl;sha256=$arcHash}|ConvertTo-Json|Set-Content (Join-Path $arcDestination 'provenance.json')
Write-Output (Join-Path $arcDestination 'bin/x64/dxcompiler.dll')
