# libz3 only (-j1). Prefer scripts\win_z3_one.ps1 for the full pipeline (one worker).
param(
    [string]$BuildDir = 'C:\Temp\prism-build-win-z3',
    [int]$Jobs = 32,
    [string]$Log = '',
    [switch]$NoLock
)

$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Log) { $Log = Join-Path $Repo 'z3-single-build.log' }

$oneArgs = @{
    BuildDir  = $BuildDir
    Jobs      = $Jobs
    Log       = $Log
    Libz3Only = $true
}
if ($NoLock) { $oneArgs['NoLock'] = $true }

& (Join-Path $PSScriptRoot 'win_z3_one.ps1') @oneArgs
exit $LASTEXITCODE
