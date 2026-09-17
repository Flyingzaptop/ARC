param(
    [Parameter(Mandatory=$true)][string]$Baseline,
    [Parameter(Mandatory=$true)][string]$ArcObserve
)

$ErrorActionPreference = 'Stop'
function Load-Summary([string]$Path) {
    $file = if (Test-Path -LiteralPath $Path -PathType Container) { Join-Path $Path 'summary.json' } else { $Path }
    if (-not (Test-Path -LiteralPath $file)) { throw "Missing summary: $file" }
    return Get-Content -Raw -LiteralPath $file | ConvertFrom-Json
}
$b = Load-Summary $Baseline
$a = Load-Summary $ArcObserve
if (-not $b.valid -or -not $a.valid) { throw 'Both captures must be valid.' }

$result = [ordered]@{
    schema = 1
    baseline = [ordered]@{
        average_fps = [double]$b.average_fps
        one_percent_low_fps = [double]$b.one_percent_low_fps
        p99_frame_ms = [double]$b.p99_frame_ms
        stutter_percent = [double]$b.stutter_percent
    }
    arc_observe = [ordered]@{
        average_fps = [double]$a.average_fps
        one_percent_low_fps = [double]$a.one_percent_low_fps
        p99_frame_ms = [double]$a.p99_frame_ms
        stutter_percent = [double]$a.stutter_percent
    }
    delta = [ordered]@{
        average_fps = [double]$a.average_fps - [double]$b.average_fps
        average_fps_percent = if ([double]$b.average_fps -ne 0) { 100.0 * ([double]$a.average_fps - [double]$b.average_fps) / [double]$b.average_fps } else { $null }
        one_percent_low_fps = [double]$a.one_percent_low_fps - [double]$b.one_percent_low_fps
        p99_frame_ms = [double]$a.p99_frame_ms - [double]$b.p99_frame_ms
        stutter_percent = [double]$a.stutter_percent - [double]$b.stutter_percent
    }
}
$result | ConvertTo-Json -Depth 8
