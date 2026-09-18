param(
    [Parameter(Mandatory=$true)][string]$RunDirectory,
    [string]$RepoRoot = '',
    [string]$RunStamp = '',
    [string]$Remote = 'origin'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Run-Git {
    param(
        [Parameter(Mandatory=$true)][string]$WorkingDir,
        [Parameter(Mandatory=$true)][string[]]$GitArgs,
        [switch]$AllowFailure
    )
    $previous = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = & git.exe -C $WorkingDir $GitArgs 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    if (-not $AllowFailure -and $code -ne 0) {
        throw "git $($GitArgs -join ' ') failed ($code): $($output -join ' ')"
    }
    [pscustomobject]@{ Code = $code; Output = @($output) }
}

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
} else {
    $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
}
$RunDirectory = (Resolve-Path -LiteralPath $RunDirectory).Path

$required = @('native.json','attribution.json','renderer.json','manifest.json','acceptance.json')
foreach ($name in $required) {
    $path = Join-Path $RunDirectory $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing Mega D artifact: $path"
    }
}

$manifest = Get-Content -LiteralPath (Join-Path $RunDirectory 'manifest.json') -Raw | ConvertFrom-Json
$acceptance = Get-Content -LiteralPath (Join-Path $RunDirectory 'acceptance.json') -Raw | ConvertFrom-Json
$sourceSha = [string]$manifest.source_sha
if ($sourceSha -notmatch '^[0-9a-fA-F]{40}$') { throw 'manifest.json has no valid source_sha.' }
if ([string]$acceptance.source_sha -ne $sourceSha) { throw 'Acceptance/source SHA mismatch.' }

Run-Git -WorkingDir $RepoRoot -GitArgs @('cat-file','-e',"$sourceSha^{commit}") | Out-Null

if ([string]::IsNullOrWhiteSpace($RunStamp)) {
    $leaf = Split-Path $RunDirectory -Leaf
    if ($leaf -match '^\d{8}-\d{6}$') { $RunStamp = $leaf }
    else { $RunStamp = Get-Date -Format 'yyyyMMdd-HHmmss' }
}
if ($RunStamp -notmatch '^\d{8}-\d{6}$') { throw 'RunStamp must be yyyyMMdd-HHmmss.' }

$branch = "results/mega-d-$RunStamp"
$probe = Run-Git -WorkingDir $RepoRoot -GitArgs @('ls-remote','--heads',$Remote,"refs/heads/$branch") -AllowFailure
if ($probe.Code -eq 0 -and ($probe.Output -join '').Trim().Length -gt 0) {
    throw "Immutable result branch already exists: $branch"
}

$publishRoot = Join-Path $env:TEMP "arc-mega-d-$RunStamp-$(Get-Random)"
try {
    Write-Host "Publishing Mega D evidence $RunStamp" -ForegroundColor Cyan
    Run-Git -WorkingDir $RepoRoot -GitArgs @('worktree','add','--force','--detach',$publishRoot,$sourceSha) | Out-Null

    $destinationRel = "results/mega-d/$RunStamp"
    $destination = Join-Path $publishRoot $destinationRel.Replace('/','\')
    New-Item -ItemType Directory -Force $destination | Out-Null
    Copy-Item -Path (Join-Path $RunDirectory '*') -Destination $destination -Recurse -Force

    $summary = @(
        '# Mega D hardware evidence'
        ''
        "- Source SHA: $sourceSha"
        "- Verdict: $($acceptance.verdict)"
        "- Scope: $($acceptance.scope)"
        "- Native readback verified: $($acceptance.native_readback_verified)"
        "- Renderer nodes: $($acceptance.renderer_nodes)"
        "- Renderer edges: $($acceptance.renderer_edges)"
        "- Raster-bound work: $($acceptance.renderer_regions)"
        "- Timed work: $($acceptance.renderer_timed_work)"
        "- Present ancestors: $($acceptance.renderer_present_ancestors)"
        "- Complete renderer attribution claimed: $($acceptance.renderer_complete_attribution)"
        "- Optimization enabled: $($acceptance.optimization_enabled)"
        ''
        'This branch is immutable evidence. A PASS is scoped to the Mega D observer prototype and does not claim complete bindless shader attribution, visual importance, perceptual equivalence, or optimization speedup.'
    )
    Set-Content -LiteralPath (Join-Path $destination 'SUMMARY.md') -Value $summary -Encoding utf8

    Run-Git -WorkingDir $publishRoot -GitArgs @('add','--',$destinationRel) | Out-Null
    Run-Git -WorkingDir $publishRoot -GitArgs @('commit','-m',"Publish Mega D evidence $RunStamp") | Out-Null
    Run-Git -WorkingDir $publishRoot -GitArgs @('push',$Remote,"HEAD:refs/heads/$branch") | Out-Null

    $remoteUrl = ((Run-Git -WorkingDir $RepoRoot -GitArgs @('remote','get-url',$Remote)).Output -join '').Trim()
    if ($remoteUrl -match '^git@github.com:(.+)\.git$') { $repoUrl = "https://github.com/$($Matches[1])" }
    elseif ($remoteUrl -match '^https://github.com/(.+)\.git$') { $repoUrl = "https://github.com/$($Matches[1])" }
    elseif ($remoteUrl -match '^https://github.com/(.+)$') { $repoUrl = "https://github.com/$($Matches[1])" }
    else { $repoUrl = 'https://github.com/Flyingzaptop/ARC' }

    $url = "$repoUrl/tree/$branch/results/mega-d/$RunStamp"
    Write-Host "Published Mega D evidence: $url" -ForegroundColor Green
} finally {
    if (Test-Path -LiteralPath $publishRoot) {
        Run-Git -WorkingDir $RepoRoot -GitArgs @('worktree','remove','--force',$publishRoot) -AllowFailure | Out-Null
    }
}
