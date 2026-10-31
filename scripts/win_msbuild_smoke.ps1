# Native MSVC smoke build (matches CI windows-compile-smoke-cl / -cl-z3).
# Prefer a build dir outside OneDrive (e.g. C:\temp\prism-build-win-cl) to avoid LNK1104 flakes.
param(
    [string]$BuildDir = 'C:\Temp\prism-build-win-cl',
    [switch]$Z3
)

$ErrorActionPreference = 'Stop'
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Lock = Join-Path $BuildDir '.win_msbuild_smoke.lock'
if (Test-Path $Lock) {
    throw "build lock exists ($Lock): another win_msbuild_smoke is running or remove the lock"
}
New-Item -ItemType File -Force -Path $Lock | Out-Null
try {
$Vcvars = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $Vcvars)) {
    throw "vcvars64.bat not found at $Vcvars (install VS Build Tools with C++ workload)"
}
$Cmake = 'C:\Program Files\CMake\bin\cmake.exe'
if (-not (Test-Path $Cmake)) {
    throw 'Use CMake from C:\Program Files\CMake (Strawberry cmake breaks Z3 try_compile on MSVC)'
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$NinjaExe = (Get-Command ninja.exe -All -ErrorAction SilentlyContinue |
    Where-Object { $_.Source -notmatch 'Strawberry' } |
    Select-Object -First 1).Source
if (-not $NinjaExe) {
    $candidates = @(
        "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe",
        "$env:ProgramFiles\Ninja\ninja.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $NinjaExe = $c; break }
    }
}
if (-not $NinjaExe) {
    throw 'ninja.exe not found (install Ninja; avoid Strawberry ninja on PATH for Z3 MSVC builds)'
}
$NinjaDir = Split-Path -Parent $NinjaExe
$NinjaMake = "-DCMAKE_MAKE_PROGRAM=`"$($NinjaExe.Replace('\', '/'))`""

function Invoke-VcCmd([string]$Body) {
    $pathFix = "set `"PATH=$NinjaDir;!PATH!`" && "
    $line = "call `"$Vcvars`" && ${pathFix}$Body"
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $out = cmd.exe /v:on /c $line 2>&1
    $ErrorActionPreference = $prevEap
    if ($out) { $out | Write-Output }
    $code = $LASTEXITCODE
    if ($code -lt 0) { $code = 1 }
    if ($code -ne 0) { throw "command failed ($code): $Body" }
}

$z3Flag = if ($Z3) { '-DPRISM_Z3=ON' } else { '-DPRISM_Z3=OFF' }
$cfg = "`"$Cmake`" -S `"$Root`" -B `"$BuildDir`" -G Ninja -DCMAKE_BUILD_TYPE=Release $NinjaMake " +
       '-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ' +
       "-DPRISM_CUDA=OFF -DPRISM_QT=OFF $z3Flag " +
       '-DPRISM_TESTS=ON -DPRISM_TESTS_WIN_SUBSET=ON -DPRISM_LIBCLANG=OFF'
Invoke-VcCmd $cfg

if ($Z3) {
    $z3One = Join-Path $PSScriptRoot 'win_z3_one.ps1'
    & $z3One -BuildDir $BuildDir
    if ($LASTEXITCODE -ne 0) { throw "win_z3_one failed ($LASTEXITCODE)" }
} else {
    Invoke-VcCmd "`"$Cmake`" --build `"$BuildDir`" --target prism_tests -j 4"
}
} finally {
    Remove-Item -Force $Lock -ErrorAction SilentlyContinue
}
