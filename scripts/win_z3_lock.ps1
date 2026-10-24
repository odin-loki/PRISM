# Shared lock for C:\Temp\prism-build-win-z3 (one cmake/ninja worker only).
function Enter-PrismWinZ3Lock {
    param([string]$BuildDir)
    $Lock = Join-Path $BuildDir '.win_z3_build.lock'
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    if (Test-Path $Lock) {
        $stale = $true
        if ((Get-Process ninja, cmake -ErrorAction SilentlyContinue).Count -gt 0) { $stale = $false }
        $pidTxt = Get-Content $Lock -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($pidTxt -match '^\d+$') {
            if (Get-Process -Id ([int]$pidTxt) -ErrorAction SilentlyContinue) { $stale = $false }
        }
        if (-not $stale) {
            throw "Z3 build lock active ($Lock). Only one worker allowed; stop other cmake/ninja on this dir."
        }
        Remove-Item -Force $Lock
    }
    Set-Content -Path $Lock -Value "$PID`n$(Get-Date -Format o)"
    return $Lock
}

function Exit-PrismWinZ3Lock {
    param([string]$Lock)
    Remove-Item -Force $Lock -ErrorAction SilentlyContinue
}
