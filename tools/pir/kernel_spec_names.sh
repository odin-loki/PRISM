#!/usr/bin/env bash
# Names of the functions the kernel specs of one kernel define (extern "C" definitions at column 0
# of src/prism/pir/models/kernel/<os>_*.cpp, static helpers excluded).
#   tools/pir/kernel_spec_names.sh linux|freebsd
set -euo pipefail
OS="${1:?linux|freebsd}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cat "$ROOT"/src/prism/pir/models/kernel/"${OS}"_*.cpp | tr -d '\r' |
  awk '/^(static|#|\/\*|\*|extern|\}|[ \t])/ {next}
       match($0, /[A-Za-z_][A-Za-z0-9_]*\(/) { s=substr($0, RSTART, RLENGTH-1); if ($0 ~ /\{/ || $0 ~ /\)[ \t]*\{?$/) print s }' |
  sort -u | tr '\n' ' '
echo
