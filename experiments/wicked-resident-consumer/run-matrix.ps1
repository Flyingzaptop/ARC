param(
    [string]$WickedRoot='C:/Users/r3d_flzp/ARC-Hardening-GPU/WickedEngine',
    [string]$Raw='C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/resident-consumer-20260924',
    [string]$Original='C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/offload-controlled-20260923/Tests-before.exe'
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../..").Path
$destination=Join-Path $WickedRoot 'BUILD/x64/Release/Tests/Tests.exe'
$resident=Join-Path $Raw 'Tests-resident-final.exe'
$working=Join-Path $WickedRoot 'Samples/Tests'
$sampler=$null
try {
    foreach($run in @('A1','B1','C1','C2','B2','A2')) {
        $source=if($run.StartsWith('A')){$Original}else{$resident}
        Copy-Item -LiteralPath $source -Destination $destination
        $env:ARC_RESIDENT_QUEUE=if($run.StartsWith('C')){'1'}else{'0'}
        $env:ARC_RESIDENT_SCREEN='0';$env:ARC_RESIDENT_DEBUG='0'
        $env:ARC_VERIFY_FINAL_LIST='0';$env:ARC_CONTROLLED_CULL='0';$env:ARC_ASYNC_CHAIN='0'
        $out=Join-Path $Raw ($run+'.csv')
        $telemetry=Join-Path $Raw ($run+'-hardware.csv')
        $sampler=Start-Process -FilePath (Get-Command nvidia-smi.exe).Source -ArgumentList '--query-gpu=timestamp,temperature.gpu,power.draw,clocks.gr,clocks.mem,utilization.gpu','--format=csv','--loop=1' -WindowStyle Hidden -RedirectStandardOutput $telemetry -PassThru
        try {
            & "$repo/scripts/profile-wicked-cpu.ps1" -Executable $destination -WorkingDirectory $working -Output $out -Scene 18 -Seconds 15 -Mode off -AlwaysActive
        } finally {
            if($sampler -and -not $sampler.HasExited){Stop-Process -Id $sampler.Id -Force}
            $sampler=$null
        }
        [ordered]@{run=$run;resident_mode=$env:ARC_RESIDENT_QUEUE;oracle=$false;scene=18;seconds=15;window_seconds=@(6,15);exe_sha256=(Get-FileHash $destination).Hash;queue_shader_sha256=(Get-FileHash "$working/shaders/hlsl6/arcResidentQueueCS.cso").Hash;arguments='alwaysactive';telemetry_file=[IO.Path]::GetFileName($telemetry)} | ConvertTo-Json | Set-Content (Join-Path $Raw ($run+'-launch.json'))
    }
} finally {
    if($sampler -and -not $sampler.HasExited){Stop-Process -Id $sampler.Id -Force}
    Copy-Item -LiteralPath $Original -Destination $destination
}
