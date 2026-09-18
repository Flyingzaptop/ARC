param(
    [string]$RepoRoot = '',
    [string]$RunStamp = '',
    [string]$Remote = 'origin'
)

$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'

function Run-Git {
    param(
        [Parameter(Mandatory=$true)][string]$WorkingDir,
        [Parameter(Mandatory=$true)][string[]]$GitArgs,
        [switch]$AllowFailure
    )
    $previous=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        $output=& git.exe -C $WorkingDir $GitArgs 2>&1
        $code=$LASTEXITCODE
    } finally {
        $ErrorActionPreference=$previous
    }
    if(-not$AllowFailure -and $code-ne 0){
        throw "git $($GitArgs -join ' ') failed ($code): $($output -join ' ')"
    }
    return [pscustomobject]@{Code=$code;Output=@($output)}
}

if([string]::IsNullOrWhiteSpace($RepoRoot)){
    $RepoRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}else{
    $RepoRoot=(Resolve-Path -LiteralPath $RepoRoot).Path
}

$localRoot=Join-Path $RepoRoot 'results\stage14_5-wicked-local'
if(-not(Test-Path -LiteralPath $localRoot)){throw "Stage 14.5 local results root not found: $localRoot"}

if([string]::IsNullOrWhiteSpace($RunStamp)){
    $candidate=Get-ChildItem -LiteralPath $localRoot -Directory |
        Sort-Object Name -Descending |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'acceptance.json') } |
        Select-Object -First 1
    if(-not$candidate){throw 'No publishable Stage 14.5 Wicked run was found.'}
    $runDir=$candidate.FullName
    $RunStamp=$candidate.Name
}else{
    $runDir=Join-Path $localRoot $RunStamp
}

$required=@('wicked-engine.json','manifest.json','acceptance.json','SUMMARY.md','benchmark.log')
foreach($name in $required){
    $path=Join-Path $runDir $name
    if(-not(Test-Path -LiteralPath $path)){throw "Missing result artifact: $path"}
}

$manifest=Get-Content -LiteralPath (Join-Path $runDir 'manifest.json') -Raw | ConvertFrom-Json
$sourceSha=[string]$manifest.source_sha
if([string]::IsNullOrWhiteSpace($sourceSha)){throw 'manifest.json does not contain source_sha.'}
Run-Git -WorkingDir $RepoRoot -GitArgs @('cat-file','-e',"$sourceSha^{commit}") | Out-Null

$branch="results/stage14_5-wicked-$RunStamp"
$remoteProbe=Run-Git -WorkingDir $RepoRoot -GitArgs @('ls-remote','--heads',$Remote,"refs/heads/$branch") -AllowFailure
if($remoteProbe.Code-eq 0 -and ($remoteProbe.Output-join '').Trim().Length-gt 0){
    $branch="$branch-recovered-$(Get-Date -Format 'HHmmss')"
}

$publishRoot=Join-Path $env:TEMP "arc-stage14_5-wicked-$RunStamp-$(Get-Random)"
try{
    Write-Host "Publishing Stage 14.5 Wicked run $RunStamp" -ForegroundColor Cyan
    Run-Git -WorkingDir $RepoRoot -GitArgs @('worktree','add','--force','--detach',$publishRoot,$sourceSha) | Out-Null

    $destinationRel="results/stage14_5-wicked/$RunStamp"
    $destination=Join-Path $publishRoot $destinationRel.Replace('/','\')
    New-Item -ItemType Directory -Force $destination | Out-Null
    Copy-Item -Path (Join-Path $runDir '*') -Destination $destination -Recurse -Force

    Run-Git -WorkingDir $publishRoot -GitArgs @('add','--',$destinationRel) | Out-Null
    Run-Git -WorkingDir $publishRoot -GitArgs @('commit','-m',"Publish Stage 14.5 Wicked acceptance $RunStamp") | Out-Null
    Run-Git -WorkingDir $publishRoot -GitArgs @('push',$Remote,"HEAD:refs/heads/$branch") | Out-Null

    $remoteUrl=(Run-Git -WorkingDir $RepoRoot -GitArgs @('remote','get-url',$Remote)).Output -join ''
    $remoteUrl=$remoteUrl.Trim()
    if($remoteUrl-match '^git@github.com:(.+)\.git$'){$repoUrl="https://github.com/$($Matches[1])"}
    elseif($remoteUrl-match '^https://github.com/(.+)\.git$'){$repoUrl="https://github.com/$($Matches[1])"}
    elseif($remoteUrl-match '^https://github.com/(.+)$'){$repoUrl="https://github.com/$($Matches[1])"}
    else{$repoUrl='https://github.com/Flyingzaptop/ARC'}

    $url="$repoUrl/tree/$branch/results/stage14_5-wicked/$RunStamp"
    Set-Content -LiteralPath (Join-Path $localRoot 'last-result-url.txt') -Value $url -Encoding ascii

    $lastSummary=Join-Path $localRoot 'last-summary.txt'
    if(Test-Path -LiteralPath $lastSummary){
        $summary=Get-Content -LiteralPath $lastSummary -Raw
        if($summary-match '(?m)^Results URL:.*$'){
            $summary=[regex]::Replace($summary,'(?m)^Results URL:.*$',"Results URL: $url")
        }else{
            $summary=$summary.TrimEnd()+[Environment]::NewLine+"Results URL: $url"+[Environment]::NewLine
        }
        Set-Content -LiteralPath $lastSummary -Value $summary -Encoding utf8
    }
    Write-Host "Published results: $url" -ForegroundColor Green
}finally{
    if(Test-Path -LiteralPath $publishRoot){
        Run-Git -WorkingDir $RepoRoot -GitArgs @('worktree','remove','--force',$publishRoot) -AllowFailure | Out-Null
    }
}
