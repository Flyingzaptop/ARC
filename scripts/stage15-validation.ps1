# Pure validation helpers: dot-sourcing performs no builds, runs, or publication.
function Test-ExplicitBoolean($Object, [string]$Name, [bool]$Expected) {
    if ($null -eq $Object) { return $false }
    $property = $Object.PSObject.Properties[$Name]
    return $null -ne $property -and $property.Value -is [bool] -and $property.Value -eq $Expected
}

function Test-FiniteNumber($Value, [double]$Minimum, [double]$Maximum) {
    if ($null -eq $Value -or $Value -is [bool] -or $Value -is [string]) { return $false }
    if ($Value -isnot [ValueType]) { return $false }
    try { $number = [double]$Value } catch { return $false }
    return -not [double]::IsNaN($number) -and -not [double]::IsInfinity($number) -and
        $number -ge $Minimum -and $number -le $Maximum
}

function Test-DistanceMatrix($Matrix, [int]$SceneCount) {
    if ($SceneCount -le 0 -or @($Matrix).Count -ne $SceneCount) { return $false }
    foreach ($row in $Matrix) {
        if (@($row).Count -ne $SceneCount) { return $false }
        foreach ($value in $row) {
            if (-not (Test-FiniteNumber $value 0.0 1.0)) { return $false }
        }
    }
    return $true
}

function Test-WickedComparisonRun($Result, [string]$Mode, [string]$SourceSha, [int]$Offset) {
    if (-not (Test-ExplicitBoolean $Result valid $true) -or
        -not (Test-ExplicitBoolean $Result acceptance_evaluated $false) -or
        -not (Test-ExplicitBoolean $Result.hook_timings enabled $false)) { return $false }
    if ($Result.mode -ne $Mode -or $Result.arc_source_sha -ne $SourceSha -or
        $Result.experiment -ne 'three_arm_comparison' -or $Result.scene_offset -ne $Offset -or
        $Result.width -ne 1920 -or $Result.height -ne 1080) { return $false }
    foreach ($counter in @('graph_errors','failed_observations','bridge_rejections','backend_failures','invalid_samples','present_failures','device_removed_reason')) {
        if (-not (Test-FiniteNumber $Result.$counter 0 0)) { return $false }
    }
    $expected = @('model','shadows','water','volumetric','instances_65k')
    if (@($Result.scenes).Count -ne 5 -or @($Result.scenes.name | Sort-Object -Unique).Count -ne 5) { return $false }
    foreach ($scene in $Result.scenes) {
        if ($scene.name -notin $expected) { return $false }
        foreach ($count in @('gpu_samples','cpu_samples')) {
            if (-not (Test-FiniteNumber $scene.$count 30 ([double]::MaxValue))) { return $false }
        }
        foreach ($timing in @('gpu_p50_ms','gpu_p95_ms','cpu_p50_ms','cpu_p95_ms')) {
            if (-not (Test-FiniteNumber $scene.$timing ([double]::Epsilon) ([double]::MaxValue))) { return $false }
        }
    }
    return (Test-FiniteNumber $Result.calibration_p40_ms 0.001 1000) -and
        (Test-FiniteNumber $Result.calibration_p50_ms 0.001 1000)
}
