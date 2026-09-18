param(
    [Parameter(Mandatory=$true)][string[]]$Csv,
    [double]$AfterMilliseconds=4000,
    [int]$MinimumFrame=10
)
$ErrorActionPreference='Stop'
function Stats($Values) {
    $sorted=@($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    return [ordered]@{count=$sorted.Count; p50_ms=$sorted[[int](($sorted.Count-1)*0.50)]; p95_ms=$sorted[[int](($sorted.Count-1)*0.95)]; max_ms=$sorted[-1]}
}
$results=foreach($path in $Csv) {
    $all=@(Import-Csv -LiteralPath $path)
    $rows=@($all | Where-Object { [double]$_.elapsed_ms -gt $AfterMilliseconds -and [long]$_.frame -gt $MinimumFrame })
    $updates=@($rows | Where-Object event -eq 'Application Update')
    $gaps=@(for($i=1;$i -lt $updates.Count;$i++) {
        if([long]$updates[$i].frame -eq [long]$updates[$i-1].frame+1) {
            [double]$updates[$i].elapsed_ms-[double]$updates[$i-1].elapsed_ms
        }
    })
    $events=[ordered]@{}
    foreach($group in ($rows | Group-Object event)) { $events[$group.Name]=Stats @($group.Group | ForEach-Object {[double]$_.ms}) }
    [ordered]@{file=[IO.Path]::GetFileName($path); after_ms=$AfterMilliseconds; minimum_frame=$MinimumFrame; row_limit_reached=($all.Count -ge 100000); frame_interval=Stats $gaps; events=$events}
}
@($results) | ConvertTo-Json -Depth 8
