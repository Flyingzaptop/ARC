param([int]$Rounds = 5, [int]$Objects = 24, [int]$ObjectMiB = 2)
$ErrorActionPreference = 'Stop'
if ($Rounds -lt 3 -or $Rounds -gt 20) { throw 'Rounds must be 3..20' }
$results = @()
for ($round = 0; $round -lt $Rounds; $round++) {
    foreach ($mode in @('baseline', 'arc')) {
        $env:ARC_D3D12_DEBUG = '1'
        & ./build/Release/dx12-residency-lab.exe $mode $Objects $ObjectMiB
        if ($LASTEXITCODE -ne 0) { throw "Residency lab failed: $mode round $round" }
        $result = Get-Content -Raw "traces/residency-lab-$mode.json" | ConvertFrom-Json
        $result | Add-Member -NotePropertyName round -NotePropertyValue $round
        $results += $result
    }
}
$results | ConvertTo-Json -Depth 6 | Set-Content traces/residency-benchmark.json -Encoding utf8
$results | Format-Table round,mode,valid,usage_allocated,usage_after_evict,bytes_evicted,reloads,false_evictions,late_residency,p50_ms,p95_ms,p99_ms
