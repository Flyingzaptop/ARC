param(
    [Parameter(Mandatory=$true)][string]$Manifest,
    [string]$WorkloadId = '',
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [ValidateRange(1,10)][int]$Rounds = 2,
    [ValidateRange(0,1800)][int]$WarmupSeconds = 5,
    [ValidateRange(6,3600)][int]$Seconds = 20,
    [string]$FrontendDll = ''
)

$ErrorActionPreference = 'Stop'
if ($Seconds -le $WarmupSeconds) { throw 'Seconds must exceed WarmupSeconds' }
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$manifestPath = (Resolve-Path -LiteralPath $Manifest).Path
$document = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($document.workloads) {
    if (-not $WorkloadId) { throw 'WorkloadId is required for a multi-workload manifest' }
    $matches = @($document.workloads | Where-Object id -eq $WorkloadId)
    if ($matches.Count -ne 1) { throw "Expected one workload with id $WorkloadId" }
    $spec = $matches[0]
} else {
    $spec = $document
}
$outRoot = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
$order = @('native','passthrough','observe','optimize','optimize','observe','passthrough','native')
$runs = [Collections.Generic.List[object]]::new()
$index = 0

foreach ($round in 1..$Rounds) {
    foreach ($mode in $order) {
        $index++
        $runDir = Join-Path $outRoot ('r{0:d2}-{1:d2}-{2}' -f $round,$index,$mode)
        New-Item -ItemType Directory -Force -Path $runDir | Out-Null
        $exe = [IO.Path]::GetFullPath((Join-Path $repo ([string]($spec.exe))))
        $work = [IO.Path]::GetFullPath((Join-Path $repo ([string]($spec.working_directory))))
        $arguments = @($spec.arguments | ForEach-Object {
            ([string]$_).Replace('{ROOT}',$repo).Replace('{OUT}',$runDir)
        })
        $environment = @{}
        if ($spec.environment) {
            foreach ($item in $spec.environment.PSObject.Properties) {
                $environment[$item.Name] = [string]$item.Value
            }
        }
        Write-Output "ARC2 matrix $($spec.id) round=$round run=$index mode=$mode"
        & (Join-Path $PSScriptRoot 'run-testbed.ps1') -Testbed ([string]($spec.testbed)) `
            -Workload ([string]($spec.id)) -Mode $mode -Exe $exe -WorkingDirectory $work `
            -Arguments $arguments -Environment $environment -OutputDir $runDir `
            -WarmupSeconds $WarmupSeconds -Seconds $Seconds -FrontendDll $FrontendDll `
            *> (Join-Path $runDir 'runner.log')
        $run = Get-Content -LiteralPath (Join-Path $runDir 'run.json') -Raw | ConvertFrom-Json
        $trace = Join-Path $runDir 'arc2.json'
        if (Test-Path -LiteralPath $trace -PathType Leaf) {
            Compress-Archive -LiteralPath $trace -DestinationPath (Join-Path $runDir 'arc2-ir.zip') -CompressionLevel Optimal
        }
        $runs.Add([ordered]@{
            round = $round; order_index = $index; mode = $mode
            run_json = (Join-Path $runDir 'run.json')
            exit_code = $run.exit_code; shutdown = $run.shutdown
            native_present_rows = $run.native_present_rows
            frontend_present_rows = $run.present_rows
            timing_eligible = $run.eligible_for_timing_analysis
        })
        [ordered]@{schema='arc2-counterbalanced-v1'; workload=$spec.id; rounds=$Rounds;
                   warmup_seconds=$WarmupSeconds; seconds=$Seconds; order=$order;
                   runs=$runs} | ConvertTo-Json -Depth 8 |
            Set-Content -LiteralPath (Join-Path $outRoot 'matrix.json') -Encoding utf8
    }
}
