# Run pir e2e after win_msbuild_smoke.ps1 -Z3 (does not invoke cmake/ninja itself).
param(
    [string]$BuildDir = 'C:\Temp\prism-build-win-z3',
    [string]$Log = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Log) { $Log = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')).Path 'win-z3-pipeline.log' }

function Write-Log($msg) {
    $line = "$(Get-Date -Format o) $msg"
    Add-Content -Path $Log -Value $line
    Write-Host $line
}

$exe = Join-Path $BuildDir 'prism_tests.exe'
if (-not (Test-Path $exe)) {
    throw "missing $exe; run scripts/win_msbuild_smoke.ps1 -BuildDir $BuildDir -Z3 first (one build at a time)"
}

$llvm = 'C:\llvm18\bin'
if (Test-Path $llvm) {
    $env:PATH = "$llvm;$env:PATH"
    Write-Log "LLVM 18 on PATH"
} else {
    Write-Log "WARN: C:\llvm18\bin missing; pir e2e may NOTRUN"
}

Write-Log "pir e2e filter on $exe"
& $exe '-tc=pir e2e*' 2>&1 | Tee-Object -FilePath $Log -Append
Write-Log "pir e2e exit $LASTEXITCODE"
exit $LASTEXITCODE
