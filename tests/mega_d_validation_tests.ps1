$ErrorActionPreference='Stop'
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$dir=Join-Path $tempRoot ('arc-mega-d-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $dir|Out-Null
try {
    $nativeNodes=@(1..3|ForEach-Object {[pscustomobject]@{id=$_;queue=1;kind=3;gpu_ms=0.5;local_raster_coverage_upper=$null}})
    $native=[pscustomobject]@{hardware=$true;readback_verified=$true;frequency=1000000;timestamp_ticks=@(100,600,700,1200,1300,1800);graph=[pscustomobject]@{observer_only=$true;capture_complete=$true;errors=0;nodes=$nativeNodes;edges=@(@{producer=1;consumer=2;resource=2},@{producer=2;consumer=3;resource=3})}}
    $nodes=@(1..10|ForEach-Object {[pscustomobject]@{id=$_;queue=1;kind=0;gpu_ms=$null;local_raster_coverage_upper=$null;raster=$null}})
    $nodes[0].gpu_ms=.5;$nodes[0].local_raster_coverage_upper=.1
    $nodes[0].raster=@{known=$true;width=100;height=100;viewport=@(0,0,100,100);scissor=@(0,0,25,40)}
    $nodes[9].kind=5
    $edges=@(1..9|ForEach-Object {@{producer=$_;consumer=$_+1;resource=$_}})
    $renderer=[pscustomobject]@{source_sha='fixture';present_succeeded=$true;present_work=10;present_slice_complete=$false;shader_access_complete=$false;present_ancestors=@(1..9);timestamp_samples=@(@{work=1;queue=1;begin=100;end=600;frequency=1000000});graph=@{observer_only=$true;capture_complete=$true;errors=0;nodes=$nodes;edges=$edges}}
    $native|ConvertTo-Json -Depth 12|Set-Content (Join-Path $dir 'native.json')
    function Validate([bool]$ExpectPass){
        $renderer|ConvertTo-Json -Depth 12|Set-Content (Join-Path $dir 'wicked.json')
        $passed=$false
        try {& (Join-Path $PSScriptRoot '../scripts/validate-mega-d.ps1') -NativeJson (Join-Path $dir 'native.json') -WickedJson (Join-Path $dir 'wicked.json') -ExpectedSha 'fixture' -Output (Join-Path $dir 'acceptance.json')|Out-Null;$passed=$true}catch{}
        if($passed -ne $ExpectPass){throw "Unexpected validator result: $passed"}
    }
    Validate $true
    $nodes[0].gpu_ms=1;Validate $false;$nodes[0].gpu_ms=.5
    $nodes[0].raster.scissor=@(0,0,50,40);Validate $false;$nodes[0].raster.scissor=@(0,0,25,40)
    $renderer.present_ancestors=@(1,2);Validate $false;$renderer.present_ancestors=@(1..9)
    $renderer.present_slice_complete=$true;Validate $false;$renderer.present_slice_complete=$false
    $renderer.timestamp_samples[0].queue=2;Validate $false;$renderer.timestamp_samples[0].queue=1
    $renderer.graph.errors=1;Validate $false;$renderer.graph.errors=0
    Validate $true
    Write-Output 'Mega D independent validator and tamper rejection PASS'
} finally {
    $resolved=[IO.Path]::GetFullPath($dir)
    if(-not $resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -or (Split-Path $resolved -Leaf) -notlike 'arc-mega-d-*'){throw 'Unexpected temporary path'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
