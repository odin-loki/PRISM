#!/usr/bin/env bash
set -euo pipefail
PRISM="${PRISM:-/tmp/prism-link/prism}"
OUT="${OUT:-/tmp/prism-smoke}"
ROOT="/mnt/c/Users/odinl/OneDrive/Desktop/Code Analysis"
cd "$ROOT"
mkdir -p "$OUT"

run() {
  local name="$1" src="$2" stages="$3"
  echo "======== $name ========"
  local dest="$OUT/$name"
  rm -rf "$dest"
  "$PRISM" "$ROOT/$src" --no-llm --jobs 4 --out "$dest" --stage "$stages" || true
  grep -E '^\| |confidence|FAILED|PROVED|CRASH|NEEDS-HARNESS|RACE|INT-' "$dest/report.md" | head -40
}

run abs testdata/abs_ok.c "inventory,classify,lints,bmc,concolic,fuzz,rapid,unify"
run div testdata/div_param.c "inventory,classify,bmc,fuzz,unify"
run ovf testdata/add_overflow.c "inventory,classify,bmc,fuzz,unify"
run oob testdata/oob_write.c "inventory,classify,bmc,fuzz,unify"
run thr testdata/iso_thread_race.c "inventory,classify,lints,thread,bmc"
run join testdata/thrd_join_api.c "inventory,classify,lints"
echo SMOKE_CORE_DONE
