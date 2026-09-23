#!/usr/bin/env bash
# Build the PIR semantics proofs and audit them.
# Fails if: the build fails, any declaration uses `sorry`, a source file
# contains an escape hatch (sorry/admit/axiom/native_decide/bv_decide/
# implemented_by/extern), or any audited theorem depends on an axiom other
# than Lean's standard propext, Classical.choice and Quot.sound.
set -euo pipefail
cd "$(dirname "$0")"

echo "== lake build"
lake build 2>&1 | tee build.log
if grep -q "declaration uses 'sorry'" build.log || grep -q 'declaration uses `sorry`' build.log; then
  echo "FAIL: a declaration uses sorry" >&2
  exit 1
fi

echo "== escape-hatch scan"
# Comments are allowed to mention the words; code is not.  Strip line
# comments and block comments before scanning.
bad=0
for f in PrismSem.lean PrismSem/*.lean Audit.lean; do
  code=$(python3 - "$f" <<'PY'
import re, sys
s = open(sys.argv[1], encoding="utf-8").read()
s = re.sub(r"/-.*?-/", "", s, flags=re.S)
s = re.sub(r"--[^\n]*", "", s)
print(s)
PY
)
  if echo "$code" | grep -nE '\b(sorry|admit|native_decide|bv_decide|implemented_by|extern)\b|^\s*(axiom|unsafe)\b' ; then
    echo "FAIL: escape hatch in $f" >&2
    bad=1
  fi
done
[ "$bad" = 0 ] || exit 1

echo "== axiom audit"
lake env lean Audit.lean 2>&1 | tee axioms.txt
n=$(grep -c "depends on axioms\|does not depend on any axioms" axioms.txt || true)
expected=$(grep -c '^#print axioms' Audit.lean)
if [ "$n" != "$expected" ]; then
  echo "FAIL: expected $expected axiom reports, got $n" >&2
  exit 1
fi
if grep "depends on axioms" axioms.txt \
   | sed -e 's/.*depends on axioms: \[//' -e 's/\]$//' | tr ',' '\n' | sed 's/^ *//' \
   | grep -vxE 'propext|Classical\.choice|Quot\.sound' ; then
  echo "FAIL: non-standard axiom (listed above)" >&2
  exit 1
fi
echo "OK: $n theorems, axioms within {propext, Classical.choice, Quot.sound}"
