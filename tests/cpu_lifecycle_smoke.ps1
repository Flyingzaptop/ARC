param(
    [string]$DynamoRIO = 'C:\Users\r3d_flzp\ARC-Hardening-GPU\universal-optimizer\deps\DynamoRIO-Windows-11.3.0-1',
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build\cpu_backend')
)
$ErrorActionPreference = 'Stop'
$build = [IO.Path]::GetFullPath($BuildDir)
$runner = Join-Path $DynamoRIO 'bin64\drrun.exe'
$client = Join-Path $build 'Release\arc_cpu_client.dll'
$fixture = Join-Path $build 'Release\cpu_native_fixture.exe'
foreach ($case in @('protect-other','protect','protect-same','late-hot','return-evicted')) {
    $report = Join-Path $build "lifecycle-$case.json"
    $output = & $runner -msgbox_mask 0 -stderr_mask 15 -no_follow_children -c $client -mode apply `
        -actuator incremental -out $report -- $fixture "--$case" 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Native $case exited $LASTEXITCODE" }
    $output | Set-Content -LiteralPath (Join-Path $build "lifecycle-$case.log") -Encoding utf8
    $result = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    if ($case -eq 'protect-other') {
        if ($result.range_invalidations -ne 0 -or $result.executions -lt 1) {
            throw 'Unrelated protection retired a CPU candidate'
        }
    } elseif ($case -in @('protect','protect-same')) {
        if ($result.range_invalidations -lt 1 -or $result.candidate_replacements -lt 1 -or
            -not @($result.regions | Where-Object { $_.generation -gt 1 -and $_.active }).Count) {
            throw 'Own protection did not produce a fresh active generation'
        }
    } elseif ($case -eq 'return-evicted') {
        $match=[regex]::Match(($output -join "`n"),'revisited_offset=(\d+)')
        if(-not $match.Success){throw 'Missing return-region oracle'}
        $offset=[long]$match.Groups[1].Value
        if(-not @($result.regions | Where-Object {$_.module_offset -eq $offset -and $_.executions -gt 0}).Count){
            throw 'Originally admitted, evicted region could not return'
        }
    } else {
        $late = @($result.regions | Where-Object {
            $_.calls -ge 250 -and $_.calls -lt 512 -and $_.executions -gt 0
        })
        if ($result.regions_discovered -le 64 -or $result.candidate_replacements -lt 2 -or
            $result.observed_calls -lt 256 -or $late.Count -lt 2) {
            throw 'Late hot regions failed to displace cold candidates'
        }
    }
    [pscustomobject]@{Case=$case; Discovered=$result.regions_discovered;
        Invalidations=$result.range_invalidations; Replacements=$result.candidate_replacements;
        ObservedCalls=$result.observed_calls; Executions=$result.executions}
}
