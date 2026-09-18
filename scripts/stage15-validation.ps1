# Pure validation helpers: dot-sourcing performs no builds, runs, or publication.
function Test-SemanticPopulationWitness($Sem, $Captures) {
    $rows=@($Captures)
    if ($null -eq $Sem -or $Sem.capture_phase -ne 'baseline_peak_measured' -or $rows.Count -eq 0 -or
        -not (Test-FiniteNumber $Sem.capture_scene_index 0 ($rows.Count-1)) -or
        [double]$Sem.capture_scene_index -ne [int]$Sem.capture_scene_index -or
        -not (Test-FiniteNumber $Sem.capture_frame 1 ([double]::MaxValue))) { return $false }
    foreach($row in $rows) {
        if (-not (Test-ExplicitBoolean $row captured $true) -or
            -not (Test-FiniteNumber $row.live_population 1 ([double]::MaxValue))) { return $false }
    }
    $peak=($rows | Measure-Object live_population -Maximum).Maximum
    $witness=$rows[[int]$Sem.capture_scene_index]
    return $Sem.alive_resources -eq $peak -and $witness.live_population -eq $peak -and $witness.frame -eq $Sem.capture_frame
}
function Test-SemanticFamilyAudit($Audit) {
    $valid = $null -ne $Audit -and $Audit.scope -eq 'six_resource_use_families' -and
        (Test-ExplicitBoolean $Audit labels_used_for_inference $false) -and
        (Test-ExplicitBoolean $Audit fine_subtypes_validated $false) -and
        (Test-ExplicitBoolean $Audit truncated $false) -and
        (Test-FiniteNumber $Audit.metadata_mismatches 0 0) -and
        (Test-FiniteNumber $Audit.samples 20 4096) -and
        (Test-FiniteNumber $Audit.families 4 6) -and
        (Test-FiniteNumber $Audit.coverage 0.80 1) -and
        (Test-FiniteNumber $Audit.family_precision 0.90 1)
    if (-not $valid) { return $false }
    $rows=@($Audit.observations)
    if ($rows.Count -ne $Audit.samples) { return $false }
    $mapping=@(-1,-1,0,1,1,2,3,3,4,5,0,0)
    $ids=@{}; $families=@{}; $covered=0; $correct=0
    foreach($row in $rows) {
        if (-not (Test-FiniteNumber $row.resource 1 ([double]::MaxValue)) -or
            -not (Test-FiniteNumber $row.expected_family 0 5) -or
            -not (Test-FiniteNumber $row.predicted_class 0 11) -or
            -not (Test-FiniteNumber $row.confidence 0 1)) { return $false }
        if ([double]$row.expected_family -ne [int]$row.expected_family -or
            [double]$row.predicted_class -ne [int]$row.predicted_class -or $ids.ContainsKey([string]$row.resource)) { return $false }
        $ids[[string]$row.resource]=$true; $families[[string]$row.expected_family]=$true
        if ($row.predicted_class -ne 0) { ++$covered }
        if ($mapping[[int]$row.predicted_class] -eq $row.expected_family) { ++$correct }
    }
    return $covered -gt 0 -and $families.Count -eq $Audit.families -and
        $covered -eq $Audit.covered -and $correct -eq $Audit.correct -and
        [math]::Abs([double]$Audit.coverage - $covered / [double]$rows.Count) -lt 0.00001 -and
        [math]::Abs([double]$Audit.family_precision - $correct / [double]$covered) -lt 0.00001
}
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
    foreach ($counter in @('graph_errors','failed_observations','bridge_rejections','backend_failures','invalid_samples','present_failures','device_removed_reason','vsync_present_count')) {
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
