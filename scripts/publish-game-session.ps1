param(
    [Parameter(Mandatory=$true)][string]$Session,
    [Parameter(Mandatory=$true)][string]$RepoRoot
)

$ErrorActionPreference = 'Stop'
$Session = (Resolve-Path -LiteralPath $Session).Path
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

Push-Location $RepoRoot
try {
    $sourceBranch = (& git.exe branch --show-current).Trim()
    $sourceSha = (& git.exe rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $sourceSha) { throw 'Could not resolve source commit.' }

    $tracked = (& git.exe status --porcelain --untracked-files=no | Out-String).Trim()
    if (-not [string]::IsNullOrWhiteSpace($tracked)) { throw 'Tracked source files are dirty; refusing to publish a game session.' }

    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
    $branch = "results/game-session-$stamp"
    $sessionName = Split-Path -Leaf $Session
    $destination = "results/game-sessions/$stamp-$sessionName"

    & git.exe switch -c $branch $sourceSha | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Could not create $branch" }
    New-Item -ItemType Directory -Force $destination | Out-Null

    $published = @()
    $skipped = @()
    foreach ($file in Get-ChildItem -LiteralPath $Session -File) {
        # GitHub rejects individual blobs >=100 MiB. Keep a safety margin.
        if ($file.Length -gt 90MB) {
            $skipped += [ordered]@{ name=$file.Name; bytes=$file.Length; reason='over_90MiB' }
            continue
        }
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $destination $file.Name) -Force
        $published += [ordered]@{ name=$file.Name; bytes=$file.Length }
    }

    $manifest = [ordered]@{
        schema = 1
        created_utc = [DateTime]::UtcNow.ToString('o')
        source_commit = $sourceSha
        source_branch = $sourceBranch
        results_branch = $branch
        original_session = $Session
        published = $published
        skipped = $skipped
    }
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $destination 'manifest.json') -Encoding utf8

    & git.exe add -- $destination
    if ($LASTEXITCODE -ne 0) { throw 'git add failed.' }
    & git.exe commit -m "Publish ARC game session $stamp" | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'git commit failed.' }
    & git.exe push -u origin $branch | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'git push failed.' }

    $remote = (& git.exe remote get-url origin).Trim()
    if ($remote -match '^git@github.com:(.+)\.git$') { $remote = "https://github.com/$($Matches[1])" }
    elseif ($remote -match '^https://github.com/(.+)\.git$') { $remote = "https://github.com/$($Matches[1])" }
    $url = "$remote/tree/$branch/$destination"
    $url | Set-Content -LiteralPath (Join-Path $Session 'results-url.txt') -Encoding ascii
    Write-Host "Results URL: $url" -ForegroundColor Green
} finally {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'SilentlyContinue'
    if ($sourceBranch) { & git.exe switch $sourceBranch *> $null }
    $ErrorActionPreference = $saved
    Pop-Location
}
