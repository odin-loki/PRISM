#!/bin/bash
# scripts/pir_phase2_rerun.sh TAG TREE(linux|freebsd) [extra prism args...]   -- rerun the W8 stage1 units that finished, same flags/budgets as the W8 run
# (tools/pir_scale_run.sh of OS-Proving) plus the harness-phase-2 flags given as extra args. At most 4 workers.
TAG=$1; TREE=$2; shift 2
BIN=${BIN:-$HOME/pbuild/pwharn2/prism}
REPO=/mnt/d/ParanoidBSD/wt/pwharn2
S=/mnt/d/ParanoidBSD/clang-work
OUT=$S/h2-out/$TAG-$TREE
export PRISM_FUNCTION_BUDGET=6 PRISM_HOUDINI_BUDGET=20 PRISM_MEM_BUDGET_MB=6000
until bash /mnt/d/ParanoidBSD/wt/capacity_ok.sh; do sleep 120; done
mkdir -p "$OUT"
args=()
for a in "$@"; do args+=(--prism-arg "$a"); done
exec nice -n 19 bash $REPO/scripts/pir_batch.sh --db $S/h2-prep/stage1-$TREE-done.json --out "$OUT" --prism "$BIN" --max-workers 4 --workers 4 --mem-mb 6000 \
  --unit-timeout 1500 --timeout 5 --retry-failed \
  --prism-arg --kernel-api --prism-arg $TREE --prism-arg --pir-discharge --prism-arg --pir-container-of --prism-arg --pir-vacuity --prism-arg --assume-unknown-calls "${args[@]}" \
  > "$OUT.run.log" 2>&1
