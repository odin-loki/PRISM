#!/usr/bin/env bash
set -euo pipefail
report="${1:-report.json}"
jq -r '.stages[]?.findings[]? | select(.status=="FAILED") | "\(.file):\(.line) \(.cls) \(.message)"' "$report" | sort -u
