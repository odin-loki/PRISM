# After libz3.lib exists: prism_tests + pir e2e. Prefer win_z3_one.ps1 -RunPirE2e (single worker).
# Does not use Get-Process ninja (other tools on PATH also run ninja).
param(
    [string]$BuildDir = 'C:\Temp\prism-build-win-z3',
    [int]$Jobs = 32,
    [string]$Log = ''
)

$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Log) { $Log = Join-Path $Repo 'z3-followup.log' }

function Log($m) {
    $line = "$(Get-Date -Format o) $m"
    Add-Content -Path $Log -Value $line
    Write-Host $line
}

$lib = Join-Path $BuildDir 'third_party\z3\libz3.lib'
if (-not (Test-Path $lib)) {
    Log "libz3 missing ($lib); run win_msbuild_smoke.ps1 -Z3 first"
    exit 1
}
Log "libz3 ok: $lib"

& (Join-Path $PSScriptRoot 'win_z3_one.ps1') -BuildDir $BuildDir -Jobs $Jobs -Log $Log -SkipLibz3 -RunPirE2e
exit $LASTEXITCODE
