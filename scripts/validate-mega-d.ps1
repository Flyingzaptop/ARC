param([Parameter(Mandatory=$true)][string]$NativeJson,
      [Parameter(Mandatory=$true)][string]$WickedJson,
      [Parameter(Mandatory=$true)][string]$ExpectedSha,
      [Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
function Require([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Validate-Graph($Graph){
    Require ($Graph.observer_only -ceq $true) 'Observer declaration missing'
    Require ($Graph.capture_complete -ceq $true -and $Graph.errors -eq 0) 'Capture overflow/error'
    $nodes=@{}; $timed=0; $regions=0
    foreach($n in $Graph.nodes){
        Require ($n.id -gt 0 -and -not $nodes.ContainsKey([string]$n.id)) 'Duplicate/invalid work ID'
        $nodes[[string]$n.id]=$n
        if($null -ne $n.gpu_ms){Require (-not [double]::IsNaN([double]$n.gpu_ms) -and -not [double]::IsInfinity([double]$n.gpu_ms) -and $n.gpu_ms -ge 0) 'Invalid cost';++$timed}
        if($null -ne $n.local_raster_coverage_upper){Require ($n.kind -eq 0 -and $n.local_raster_coverage_upper -ge 0 -and $n.local_raster_coverage_upper -le 1) 'Invalid region';++$regions}
    }
    foreach($e in $Graph.edges){Require ($nodes.ContainsKey([string]$e.producer) -and $nodes.ContainsKey([string]$e.consumer) -and $e.producer -lt $e.consumer) 'Invalid/cyclic dependency'}
    return [pscustomobject]@{nodes=$nodes;timed=$timed;regions=$regions}
}
$native=Get-Content -LiteralPath $NativeJson -Raw|ConvertFrom-Json
$wicked=Get-Content -LiteralPath $WickedJson -Raw|ConvertFrom-Json
$n=Validate-Graph $native.graph; $w=Validate-Graph $wicked.graph
Require ($native.hardware -ceq $true -and $native.readback_verified -ceq $true) 'Native data correctness missing'
Require ($n.nodes.Count -eq 3 -and @($native.graph.edges).Count -eq 2 -and $n.timed -eq 3) 'Native known chain mismatch'
for($i=0;$i -lt 3;++$i){
    $ms=([decimal]$native.timestamp_ticks[$i*2+1]-[decimal]$native.timestamp_ticks[$i*2])*1000/[decimal]$native.frequency
    Require ([math]::Abs([double]$ms-[double]$native.graph.nodes[$i].gpu_ms) -lt 0.00001) 'Native timestamp mismatch'
}
Require ($wicked.source_sha -ceq $ExpectedSha) 'Wrong renderer source'
Require ($wicked.present_succeeded -ceq $true -and $w.nodes.ContainsKey([string]$wicked.present_work)) 'Present missing'
Require ($wicked.present_slice_complete -ceq $false -and $wicked.shader_access_complete -ceq $false) 'Adapter must disclose incomplete shader access'
Require ($w.nodes.Count -ge 10 -and @($wicked.graph.edges).Count -ge 1 -and $w.regions -ge 1) 'Insufficient renderer evidence'
Require ($w.timed -ge 1 -and $w.timed -le 64 -and @($wicked.timestamp_samples).Count -eq $w.timed) 'Sparse GPU timing missing'
$seen=@{}
foreach($s in $wicked.timestamp_samples){
    Require ($s.frequency -gt 0 -and [decimal]$s.end -ge [decimal]$s.begin -and $w.nodes.ContainsKey([string]$s.work) -and -not $seen.ContainsKey([string]$s.work)) 'Invalid raw query'
    $seen[[string]$s.work]=$true
    $ms=([decimal]$s.end-[decimal]$s.begin)*1000/[decimal]$s.frequency
    $node=$w.nodes[[string]$s.work]
    Require ($node.queue -eq $s.queue -and [math]::Abs([double]$ms-[double]$node.gpu_ms) -lt 0.00001) 'Renderer timestamp attribution mismatch'
}
$result=[ordered]@{schema=1;verdict='PASS';scope='Mega D observer prototype; conservative whole-resource candidate graph, local raster bounds, sparse measured cost';source_sha=$ExpectedSha;native_nodes=$n.nodes.Count;native_readback_verified=$true;renderer_nodes=$w.nodes.Count;renderer_edges=@($wicked.graph.edges).Count;renderer_regions=$w.regions;renderer_timed_work=$w.timed;renderer_present_ancestors=@($wicked.present_ancestors).Count;renderer_complete_attribution=$false;optimization_enabled=$false;stage15_complexity_deferred=$true}
$result|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $Output -Encoding UTF8
$result|ConvertTo-Json -Depth 5
