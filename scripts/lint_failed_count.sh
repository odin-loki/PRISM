#!/usr/bin/env bash
# Count FAILED findings in report.json for lints/warnings stages (needs jq).
set -euo pipefail
report="${1:-report.json}"
if ! command -v jq >/dev/null 2>&1; then
  echo "lint_failed_count.sh: jq not found; install jq or pass prism stderr" >&2
  exit 127
fi
jq '[.stages[]?.findings[]? | select(.status=="FAILED" and (.stage=="lints" or .stage=="warnings"))] | length' "$report"
