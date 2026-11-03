#!/usr/bin/env bash
# Two-file memory test: PIR on priority_queue_contracts.cpp + vector_resize.cpp
# with a 3000 MB budget and 2 jobs; the peak RSS of the process tree (Z3
# worker children included) must stay under budget. Slow (~6 min); run by
# hand or nightly: tests/heavy/peak_budget.sh [path/to/prism]
set -u
P=${1:-./build/prism}; D=$(dirname "$0"); O=$(mktemp -d); IN=$(mktemp -d)
cp "$D"/*.cpp "$IN"/
PRISM_MEM_BUDGET_MB=3000 "$P" "$IN" --no-llm --stage inventory,classify,pir \
  --out "$O" --jobs 2 >"$O/run.log" 2>&1 & pid=$!
tree() { local p=$1 s; s=$(awk '/VmRSS/{print $2}' /proc/$p/status 2>/dev/null); s=${s:-0}
  for c in $(cat /proc/$p/task/*/children 2>/dev/null); do s=$((s+$(tree $c))); done; echo $s; }
peak=0
while kill -0 $pid 2>/dev/null; do r=$(tree $pid); [ "$r" -gt "$peak" ] && peak=$r; sleep 0.2; done
wait $pid; rc=$?
echo "rc=$rc peak $((peak/1024))MB (budget 3000MB)"
[ $rc -eq 0 ] && [ $((peak/1024)) -lt 3000 ] && [ -s "$O/VERDICTS.md" ]
