$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
foreach($directory in @('src/core','src/backends','include/arc')) {
    foreach($file in Get-ChildItem (Join-Path $root $directory) -File -Recurse) {
        if([IO.File]::ReadAllText($file.FullName) -match 'ArcWickedSemanticAudit|audit::catalog|truth_live_|truth_samples_|ARCWickedResourceTruth') {
            throw "Evaluation-only labels crossed the core/host boundary: $($file.FullName)"
        }
    }
}
$header=[IO.File]::ReadAllText((Join-Path $root 'include/arc/resource_semantics.hpp'))
if($header -match 'std::string|string_view|const char\*') { throw 'Resource inference API must not accept semantic name labels.' }
Write-Host 'wicked-audit-boundary-tests: PASS'
