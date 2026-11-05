#!/usr/bin/env bash
# Regenerate the tables in docs/USER_GUIDE.md that copy facts from the code:
#   - the stage-order sentence, from include/prism/pipeline.hpp STAGE_ORDER;
#   - the environment-variable table, from prism::env_vars() in
#     src/prism/config.cpp.
# Each table sits between "<!-- generated:NAME -->" and "<!-- /generated:NAME -->".
# Run it after changing either source; `--check` exits 1 when the guide is stale.
#   scripts/gen_doc_tables.sh [--check]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GUIDE="$ROOT/docs/USER_GUIDE.md"
CHECK=0
[[ "${1:-}" == "--check" ]] && CHECK=1

stages="$(awk '/STAGE_ORDER\[\] = \{/,/nullptr\};/' "$ROOT/include/prism/pipeline.hpp" |
  grep -o '"[a-z_-]*"' | tr -d '"' | paste -sd, - | sed 's/,/, /g')"
[[ -n "$stages" ]] || { echo "no STAGE_ORDER in include/prism/pipeline.hpp" >&2; exit 2; }

stage_block="$(mktemp)"
env_block="$(mktemp)"
out="$(mktemp)"
trap 'rm -f "$stage_block" "$env_block" "$out"' EXIT

# docscan.cpp stage_order_listed() reads "...: a, b, c. What"; keep ". What" on one line.
printf 'The stages run in a fixed order (`--list-stages`): %s.@What each stage does is in [ARCHITECTURE.md](ARCHITECTURE.md#stages).\n' \
  "$stages" | fold -s -w 78 | sed 's/ *$//; s/\.@What/. What/' > "$stage_block"

{
  echo '| variable | meaning |'
  echo '|---|---|'
  awk '/env_vars\(\) \{/,/return v;/' "$ROOT/src/prism/config.cpp" |
    sed -n 's/^[[:space:]]*{"\([A-Za-z0-9_]*\)", "\(.*\)"},[[:space:]]*$/\1\t\2/p' |
    sort |
    awk -F'\t' '{ gsub(/\|/, "\\|", $2); gsub(/\\"/, "\"", $2); printf "| `%s` | %s |\n", $1, $2 }'
} > "$env_block"
[[ $(wc -l < "$env_block") -gt 2 ]] || { echo "no env_vars() rows in src/prism/config.cpp" >&2; exit 2; }

awk -v sf="$stage_block" -v ef="$env_block" '
  function dump(f,   l) { while ((getline l < f) > 0) print l; close(f) }
  /^<!-- generated:stage-order -->$/ { print; dump(sf); skip = 1; next }
  /^<!-- generated:env-vars -->$/    { print; dump(ef); skip = 1; next }
  /^<!-- \/generated:/               { skip = 0 }
  !skip { print }
' "$GUIDE" > "$out"

for m in stage-order env-vars; do
  grep -q "^<!-- generated:$m -->\$" "$GUIDE" || { echo "docs/USER_GUIDE.md has no generated:$m block" >&2; exit 2; }
done

if cmp -s "$out" "$GUIDE"; then
  echo "docs/USER_GUIDE.md: generated tables are current"
  exit 0
fi
if [[ $CHECK -eq 1 ]]; then
  echo "docs/USER_GUIDE.md: generated tables are stale; run scripts/gen_doc_tables.sh" >&2
  diff -u "$GUIDE" "$out" >&2 || true
  exit 1
fi
cp "$out" "$GUIDE"
echo "docs/USER_GUIDE.md: generated tables updated"
