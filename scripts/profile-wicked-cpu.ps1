param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$WorkingDirectory,
    [Parameter(Mandatory=$true)][string]$Output,
    [ValidateSet(1,18)][int]$Scene=1,
    [ValidateRange(2,15)][int]$Seconds=8,
    [ValidateSet('off','observe')][string]$Mode='off',
    [switch]$AlwaysActive
)
$ErrorActionPreference='Stop'
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$WorkingDirectory=(Resolve-Path -LiteralPath $WorkingDirectory).Path
$Output=[IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $Output) { throw "Refusing to overwrite $Output" }
New-Item -ItemType Directory -Path (Split-Path -Parent $Output) -Force | Out-Null
$names=@('ARC_WICKED_CPU_PROFILE','ARC_WICKED_CPU_SCENE','ARC_WICKED_CPU_SECONDS','ARC_WICKED_EXPERIMENT_MODE','ARC_WICKED_HOOK_TIMING','ARC_WICKED_CRASH_OUTPUT')
$saved=@{}
foreach ($name in $names) { $saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process') }
$process=$null
try {
    $env:ARC_WICKED_CPU_PROFILE=$Output
    $env:ARC_WICKED_CPU_SCENE=[string]$Scene
    $env:ARC_WICKED_CPU_SECONDS=[string]$Seconds
    $env:ARC_WICKED_EXPERIMENT_MODE=$Mode
    $env:ARC_WICKED_HOOK_TIMING='0'
    $env:ARC_WICKED_CRASH_OUTPUT=$Output+'.crash.txt'
    $launch=@{FilePath=$Executable; WorkingDirectory=$WorkingDirectory; WindowStyle='Hidden'; PassThru=$true}
    if ($AlwaysActive) { $launch.ArgumentList='alwaysactive' }
    $hash=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
    $started=Get-Date
    $process=Start-Process @launch
    if (-not $process.WaitForExit(($Seconds+20)*1000)) { throw 'CPU diagnostic exceeded its deadline' }
    $exitCode=$process.ExitCode
    [ordered]@{schema=1; diagnostic_only=$true; executable_sha256=$hash; scene=$Scene; seconds=$Seconds; mode=$Mode; always_active=[bool]$AlwaysActive; started=$started.ToString('o'); exit_code=$exitCode} |
        ConvertTo-Json | Set-Content -LiteralPath ($Output+'.json') -Encoding utf8
    if ($exitCode -ne 0) { throw "CPU diagnostic failed: $exitCode" }
    if (-not (Test-Path -LiteralPath $Output)) { throw 'CPU diagnostic did not produce output' }
    Write-Output $Output
} finally {
    if ($null -ne $process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
}
