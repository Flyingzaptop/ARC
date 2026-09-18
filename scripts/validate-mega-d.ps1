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
        if($null -ne $n.local_raster_coverage_upper){
            Require ($n.kind -eq 0 -and $n.local_raster_coverage_upper -ge 0 -and $n.local_raster_coverage_upper -le 1 -and $n.raster.known -ceq $true -and $n.raster.width -gt 0 -and $n.raster.height -gt 0) 'Invalid region'
            $v=$n.raster.viewport;$s=$n.raster.scissor
            $left=[math]::Max(0,[math]::Max($v[0],$s[0]));$top=[math]::Max(0,[math]::Max($v[1],$s[1]))
            $right=[math]::Min($n.raster.width,[math]::Min(($v[0]+$v[2]),($s[0]+$s[2])))
            $bottom=[math]::Min($n.raster.height,[math]::Min(($v[1]+$v[3]),($s[1]+$s[3])))
            $bound=0.0
            if($right -gt $left -and $bottom -gt $top){$bound=([math]::Ceiling($right)-[math]::Floor($left))*([math]::Ceiling($bottom)-[math]::Floor($top))/([double]$n.raster.width*$n.raster.height)}
            Require ([math]::Abs($bound-$n.local_raster_coverage_upper) -lt 0.00001) 'Coverage does not match raw raster state'
            ++$regions
        }
    }
    foreach($e in $Graph.edges){Require ($nodes.ContainsKey([string]$e.producer) -and $nodes.ContainsKey([string]$e.consumer) -and $e.producer -lt $e.consumer) 'Invalid/cyclic dependency'}
    return [pscustomobject]@{nodes=$nodes;timed=$timed;regions=$regions}
}
$native=Get-Content -LiteralPath $NativeJson -Raw|ConvertFrom-Json
$wicked=Get-Content -LiteralPath $WickedJson -Raw|ConvertFrom-Json
$n=Validate-Graph $native.graph; $w=Validate-Graph $wicked.graph
Require ($native.hardware -ceq $true -and $native.readback_verified -ceq $true) 'Native data correctness missing'
Require ($n.nodes.Count -eq 3 -and @($native.graph.edges).Count -eq 2 -and $n.timed -eq 3) 'Native known chain mismatch'
Require ($native.graph.edges[0].producer -eq 1 -and $native.graph.edges[0].consumer -eq 2 -and $native.graph.edges[0].resource -eq 2 -and $native.graph.edges[1].producer -eq 2 -and $native.graph.edges[1].consumer -eq 3 -and $native.graph.edges[1].resource -eq 3) 'Native dependency endpoints mismatch'
for($i=0;$i -lt 3;++$i){
    $ms=([decimal]$native.timestamp_ticks[$i*2+1]-[decimal]$native.timestamp_ticks[$i*2])*1000/[decimal]$native.frequency
    Require ([math]::Abs([double]$ms-[double]$native.graph.nodes[$i].gpu_ms) -lt 0.00001) 'Native timestamp mismatch'
}
Require ($wicked.source_sha -ceq $ExpectedSha) 'Wrong renderer source'
Require ($wicked.present_succeeded -ceq $true -and $w.nodes.ContainsKey([string]$wicked.present_work)) 'Present missing'
Require ($w.nodes[[string]$wicked.present_work].kind -eq 5) 'Present node has wrong kind'
Require ($wicked.present_slice_complete -ceq $false -and $wicked.shader_access_complete -ceq $false) 'Adapter must disclose incomplete shader access'
Require ($w.nodes.Count -ge 10 -and @($wicked.graph.edges).Count -ge 1 -and $w.regions -ge 1) 'Insufficient renderer evidence'
Require ($w.timed -ge 1 -and $w.timed -le 64 -and @($wicked.timestamp_samples).Count -eq $w.timed) 'Sparse GPU timing missing'
$seen=@{}
foreach($s in $wicked.timestamp_samples){
    Require ($s.frequency -gt 0 -and [decimal]$s.end -ge [decimal]$s.begin -and $w.nodes.ContainsKey([string]$s.work) -and -not $seen.ContainsKey([string]$s.work)) 'Invalid raw query'
    $seen[[string]$s.work]=$true
    $ms=([decimal]$s.end-[decimal]$s.begin)*1000/[decimal]$s.frequency
    $node=$w.nodes[[string]$s.work]
    Require ($null -ne $node.gpu_ms -and $node.queue -eq $s.queue -and [math]::Abs([double]$ms-[double]$node.gpu_ms) -lt 0.00001) 'Renderer timestamp attribution mismatch'
}
$ancestors=@{};$pending=[Collections.Generic.Queue[long]]::new();$pending.Enqueue([long]$wicked.present_work)
while($pending.Count){$consumer=$pending.Dequeue();foreach($e in $wicked.graph.edges){if($e.consumer -eq $consumer -and -not $ancestors.ContainsKey([string]$e.producer)){$ancestors[[string]$e.producer]=$true;$pending.Enqueue([long]$e.producer)}}}
$actual=@($ancestors.Keys|ForEach-Object {[long]$_}|Sort-Object)
$claimed=@($wicked.present_ancestors|Sort-Object)
Require (($actual -join ',') -ceq ($claimed -join ',')) 'Present ancestry does not match graph'
$result=[ordered]@{schema=1;verdict='PASS';scope='Mega D observer prototype; conservative whole-resource candidate graph, local raster bounds, sparse measured cost';source_sha=$ExpectedSha;native_nodes=$n.nodes.Count;native_readback_verified=$true;renderer_nodes=$w.nodes.Count;renderer_edges=@($wicked.graph.edges).Count;renderer_regions=$w.regions;renderer_timed_work=$w.timed;renderer_present_ancestors=@($wicked.present_ancestors).Count;renderer_complete_attribution=$false;optimization_enabled=$false;stage15_complexity_deferred=$true}
$result|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $Output -Encoding UTF8
$result|ConvertTo-Json -Depth 5
