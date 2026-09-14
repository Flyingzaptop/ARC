param([string]$Output = 'build/hardware-profile.json', [string]$StorageFile = 'build/Release/arc-calibrate.exe')
$ErrorActionPreference = 'Stop'
& ./build/Release/arc-calibrate.exe $Output $StorageFile
if ($LASTEXITCODE -ne 0) { throw 'CPU/RAM/storage calibration failed' }
& ./build/Release/dx12-memory-pressure.exe baseline 300
if ($LASTEXITCODE -ne 0) { throw 'GPU calibration workload failed' }
$profile = Get-Content -Raw -LiteralPath $Output | ConvertFrom-Json
$gpuMeasurements = Get-Content -Raw traces/dx12-memory-pressure-baseline-metrics.json | ConvertFrom-Json
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$os = Get-CimInstance Win32_OperatingSystem
$profile | Add-Member -NotePropertyName gpu_proxies_ms -NotePropertyValue $gpuMeasurements.gpu_proxies_ms -Force
$profile | Add-Member -NotePropertyName cpu_name -NotePropertyValue $cpu.Name -Force
$profile | Add-Member -NotePropertyName physical_cores -NotePropertyValue $cpu.NumberOfCores -Force
$profile | Add-Member -NotePropertyName os_build -NotePropertyValue $os.BuildNumber -Force
$profile | Add-Member -NotePropertyName timestamp_utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
$profile | Add-Member -NotePropertyName arc_commit -NotePropertyValue ((git rev-parse HEAD).Trim()) -Force
$profile | Add-Member -NotePropertyName storage_read_file -NotePropertyValue ([IO.Path]::GetFullPath($StorageFile)) -Force
$profile | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Output -Encoding utf8
Write-Output "Complete calibration profile: $Output"
