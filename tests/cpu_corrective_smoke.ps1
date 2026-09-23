param(
    [string]$DynamoRIO='C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/deps/DynamoRIO-Windows-11.3.0-1',
    [string]$BuildDir=(Join-Path $PSScriptRoot '../build/cpu_backend'),
    [Parameter(Mandatory=$true)][string]$EvidenceDir,
    [switch]$SkipLifecycle
)
$ErrorActionPreference='Stop'
$build=[IO.Path]::GetFullPath($BuildDir)
$evidence=[IO.Path]::GetFullPath($EvidenceDir)
New-Item -ItemType Directory -Path $evidence | Out-Null
if(-not $SkipLifecycle){
    & (Join-Path $PSScriptRoot 'cpu_lifecycle_smoke.ps1') -DynamoRIO $DynamoRIO -BuildDir $build |
        ConvertTo-Json -Depth 5 | Set-Content (Join-Path $evidence 'cpu-lifecycle-results.json') -Encoding utf8
    foreach($case in @('protect-other','protect','protect-same','late-hot','return-evicted')){
        foreach($ext in @('json','log')){
            Copy-Item -LiteralPath (Join-Path $build "lifecycle-$case.$ext") -Destination (Join-Path $evidence "cpu-$case.$ext")
        }
    }
}
foreach($case in @('auto','study-two','fp-state','cost-churn')){
    $mode=if($case -eq 'study-two'){'study'}else{'apply'}
    $report=Join-Path $evidence "cpu-$case.json"
    $arguments=@('-msgbox_mask','0','-stderr_mask','15','-no_follow_children',
        '-c',(Join-Path $build 'Release/arc_cpu_client.dll'),'-mode',$mode,'-actuator','auto',
        '-out',$report,'--',(Join-Path $build 'Release/cpu_native_fixture.exe'))
    if($case -ne 'auto'){$arguments+="--$case"}
    & (Join-Path $DynamoRIO 'bin64/drrun.exe') @arguments 2>&1 | Set-Content (Join-Path $evidence "cpu-$case.log")
    if($LASTEXITCODE -ne 0){throw "$case native state oracle failed"}
    $r=Get-Content -LiteralPath $report -Raw|ConvertFrom-Json
    if($case -eq 'study-two'){
        if($r.capture_completed -lt 2 -or $r.capture_limit_events -ne 100000 -or
            @($r.regions|Where-Object {$_.sampled -and $_.calls -ge 900}).Count -lt 2){
            throw 'Second known region did not receive a bounded study'
        }
    }else{
        $measured=@($r.regions|Where-Object {$_.timed_spans -ge 36 -and $_.cost_reason -in @(1,2)})
        if(-not $measured.Count){throw "$case has no measured action decision"}
        if($case -eq 'cost-churn' -and @($measured|Where-Object {$_.calls -ge 120 -and $_.calls -le 135}).Count -lt 5){
            throw 'Data-cache eviction erased cost policy history'
        }
    }
    [pscustomobject]@{Case=$case;Pass=$true;Executions=$r.executions;Studies=$r.capture_completed}
}
