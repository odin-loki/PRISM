# One cmake/ninja process on $BuildDir: libz3 -> prism_tests -> optional pir e2e (-Jobs parallel compiles).
param(
    [string]$BuildDir = 'C:\Temp\prism-build-win-z3',
    [int]$Jobs = 32,
    [switch]$RunPirE2e,
    [switch]$Libz3Only,
    [switch]$SkipLibz3,
    [switch]$NoLock,
    [string]$Log = ''
)

$ErrorActionPreference = 'Stop'
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}
. (Join-Path $PSScriptRoot 'win_z3_lock.ps1')
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Log) { $Log = Join-Path $Repo 'z3-single-build.log' }
$Vcvars = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$Cmake = 'C:\Program Files\CMake\bin\cmake.exe'

function Log($m) {
    $line = "$(Get-Date -Format o) $m"
    Add-Content -Path $Log -Value $line
    Write-Host $line
}

function Invoke-VcBuild([string]$Body) {
    $line = "call `"$Vcvars`" && $Body"
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $out = cmd.exe /c $line 2>&1
    $ErrorActionPreference = $prevEap
    if ($out) { $out | Tee-Object -FilePath $Log -Append }
    $code = $LASTEXITCODE
    if ($code -lt 0) { $code = 1 }
    if ($code -ne 0) { throw "command failed ($code): $Body" }
}

$lib = Join-Path $BuildDir 'third_party\z3\libz3.lib'
$lock = $null
if (-not $NoLock) { $lock = Enter-PrismWinZ3Lock $BuildDir }
try {
    if (-not $SkipLibz3) {
        Log "=== win_z3_one: libz3 -j$Jobs ==="
        Invoke-VcBuild "`"$Cmake`" --build `"$BuildDir`" --target libz3 -j $Jobs"
        if (-not (Test-Path $lib)) { throw "missing $lib" }
        Log "libz3 ok ($((Get-Item $lib).Length) bytes)"
    } elseif (-not (Test-Path $lib)) {
        throw "missing $lib"
    }

    if ($Libz3Only) { exit 0 }

    Log "=== win_z3_one: prism_tests -j$Jobs ==="
    Invoke-VcBuild "`"$Cmake`" --build `"$BuildDir`" --target prism_tests -j $Jobs"
    if (-not (Test-Path (Join-Path $BuildDir 'prism_tests.exe'))) { throw 'missing prism_tests.exe' }

    if ($RunPirE2e) {
        Log '=== win_z3_one: pir e2e ==='
        & (Join-Path $PSScriptRoot 'win_z3_pipeline.ps1') -BuildDir $BuildDir -Log $Log
        exit $LASTEXITCODE
    }
} finally {
    if ($lock) { Exit-PrismWinZ3Lock $lock }
}
