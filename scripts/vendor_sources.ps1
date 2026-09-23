$ErrorActionPreference = "Continue"
$root = Join-Path (Split-Path -Parent $PSScriptRoot) "third_party"
$log = Join-Path $root "vendor.log"
Set-Content -Path $log -Value "vendor start $(Get-Date -Format o)"

$repos = @(
  @{ url = "https://github.com/emil-e/rapidcheck.git"; dir = "rapidcheck" },
  @{ url = "https://github.com/meyerphi/strix.git"; dir = "strix" },
  @{ url = "https://github.com/kaled-alshmrany/FuSeBMC.git"; dir = "FuSeBMC" },
  @{ url = "https://github.com/ise-uiuc/Fuzz4All.git"; dir = "Fuzz4All" },
  @{ url = "https://github.com/coccinelle/coccinelle.git"; dir = "coccinelle" },
  @{ url = "https://github.com/danmar/cppcheck.git"; dir = "cppcheck" },
  @{ url = "https://github.com/AFLplusplus/AFLplusplus.git"; dir = "AFLplusplus" },
  @{ url = "https://github.com/klee/klee.git"; dir = "klee" },
  @{ url = "https://github.com/esbmc/esbmc.git"; dir = "esbmc" },
  @{ url = "https://github.com/diffblue/cbmc.git"; dir = "cbmc" },
  @{ url = "https://github.com/Frama-C/Frama-C-snapshot.git"; dir = "Frama-C" },
  @{ url = "https://github.com/dafny-lang/dafny.git"; dir = "dafny" },
  @{ url = "https://github.com/semgrep/semgrep.git"; dir = "semgrep" },
  @{ url = "https://github.com/facebook/infer.git"; dir = "infer" },
  @{ url = "https://github.com/github/codeql.git"; dir = "codeql" }
)

foreach ($r in $repos) {
  $dest = Join-Path $root $r.dir
  $already = (Test-Path (Join-Path $dest "README.md")) -or (Test-Path (Join-Path $dest "CMakeLists.txt")) -or (Test-Path (Join-Path $dest "LICENSE"))
  if ($already) {
    Add-Content $log "SKIP exists $($r.dir)"
    Write-Host "SKIP $($r.dir)"
    continue
  }
  if (Test-Path $dest) {
    Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
  }
  Add-Content $log "CLONE $($r.url)"
  Write-Host "CLONE $($r.dir)"
  git -c core.longpaths=true clone --depth 1 --single-branch $r.url $dest
  $code = $LASTEXITCODE
  if ($code -ne 0) {
    Add-Content $log "FAIL $($r.dir) exit $code"
    Write-Host "FAIL $($r.dir) $code"
    continue
  }
  $gitdir = Join-Path $dest ".git"
  if (Test-Path $gitdir) {
    Remove-Item -Recurse -Force $gitdir
  }
  Add-Content $log "OK $($r.dir)"
  Write-Host "OK $($r.dir)"
}

Add-Content $log "VENDOR DONE $(Get-Date -Format o)"
Write-Host "VENDOR DONE"
