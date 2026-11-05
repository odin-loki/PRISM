#!/usr/bin/env bash
# Write the FORGE commit PRISM builds against into cmake/FORGE_PIN.
#   scripts/bump_forge_pin.sh          # the current tip of odin-loki/FORGE main
#   scripts/bump_forge_pin.sh SHA      # an exact commit (full 40-hex SHA)
# cmake/forge.cmake and every workflow read that file; commit the one-line
# change as its own reviewed PR so CI shows whether the new FORGE builds.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PIN="$ROOT/cmake/FORGE_PIN"
URL="https://github.com/odin-loki/FORGE.git"

if [[ $# -gt 1 ]]; then
  echo "usage: $0 [SHA]" >&2
  exit 2
fi
if [[ $# -eq 1 ]]; then
  new="$1"
else
  new="$(git ls-remote "$URL" refs/heads/main | cut -f1)"
fi
if [[ ! "$new" =~ ^[0-9a-f]{40}$ ]]; then
  echo "not a full 40-hex commit SHA: '$new'" >&2
  exit 2
fi
old="$(tr -d '[:space:]' < "$PIN" 2>/dev/null || true)"
if [[ "$old" == "$new" ]]; then
  echo "FORGE pin already at $new"
  exit 0
fi
printf '%s\n' "$new" > "$PIN"
echo "FORGE pin: ${old:-none} -> $new"
echo "review: https://github.com/odin-loki/FORGE/compare/${old:-main}...$new"
