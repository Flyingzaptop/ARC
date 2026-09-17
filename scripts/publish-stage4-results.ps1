param(
    [string]$SourceDirectory = 'traces',
    [string]$Remote = 'origin'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Push-Location $repoRoot
try {
    $required = @(
        'stage4-acceptance.json',
        'STAGE4_ACCEPTANCE.md',
        'observer-benchmark-v2.json',
        'observer-benchmark-v2-raw.json',
        'live-runtime-lab.json',
        'runtime-event-bridge-lab.json',
        'stage2-final-acceptance.json',
        'residency-benchmark-summary.json',
        'residency-frontier-summary.json',
        'residency-frontier.csv',
        'tiled-texture-lab.json',
        'global-memory-lab.json',
        'hardware-profile.json'
    )
    foreach ($name in $required) {
        $path = Join-Path $SourceDirectory $name
        if (-not (Test-Path -LiteralPath $path)) { throw "Missing result artifact: $path" }
    }

    $trackedChanges = (git status --porcelain --untracked-files=no | Out-String).Trim()
    if (-not [string]::IsNullOrWhiteSpace($trackedChanges)) {
        throw 'Refusing to publish results while tracked source files have uncommitted changes.'
    }

    $baseCommit = (git rev-parse HEAD).Trim()
    $originalBranch = (git rev-parse --abbrev-ref HEAD).Trim()
    if ($originalBranch -eq 'HEAD') { throw 'Cannot publish results from detached HEAD.' }
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
    $resultBranch = "results/stage4-$stamp"
    $destination = "results/stage4/$stamp"

    git checkout -b $resultBranch $baseCommit | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Failed to create results branch.' }
    try {
        New-Item -ItemType Directory -Force $destination | Out-Null
        foreach ($name in $required) {
            Copy-Item -LiteralPath (Join-Path $SourceDirectory $name) -Destination (Join-Path $destination $name) -Force
        }
        $manifest = [ordered]@{
            schema = 1
            created_utc = [DateTime]::UtcNow.ToString('o')
            source_commit = $baseCommit
            source_branch = $originalBranch
            result_branch = $resultBranch
            files = $required
        }
        $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $destination 'manifest.json') -Encoding utf8

        git add -- $destination
        if ($LASTEXITCODE -ne 0) { throw 'git add failed.' }
        git commit -m "Publish Stage 4 validation results $stamp" | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'git commit failed.' }
        git push -u $Remote $resultBranch | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'git push failed.' }

        $remoteUrl = (git remote get-url $Remote).Trim()
        $repoUrl = $remoteUrl
        if ($repoUrl -match '^git@github.com:(.+)\.git$') { $repoUrl = "https://github.com/$($Matches[1])" }
        elseif ($repoUrl -match '^https://github.com/(.+)\.git$') { $repoUrl = "https://github.com/$($Matches[1])" }
        $branchUrl = "$repoUrl/tree/$resultBranch/$destination"
        Write-Host ''
        Write-Host "Published results branch: $resultBranch"
        Write-Host "Results URL: $branchUrl"
        $branchUrl | Set-Content -LiteralPath (Join-Path $SourceDirectory 'stage4-results-url.txt') -Encoding ascii
    } finally {
        git checkout $originalBranch | Out-Host
        if ($LASTEXITCODE -ne 0) { Write-Warning "Could not return to source branch $originalBranch" }
    }
} finally {
    Pop-Location
}
