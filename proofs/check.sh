#!/usr/bin/env bash
# Build the Lean proof of the verdict laws and check it is complete and in
# step with the code. Used by .github/workflows/proofs.yml; run locally the
# same way (needs elan: https://github.com/leanprover/elan).
#
#   1. no `sorry` / `native_decide` in any source (comments stripped);
#   2. `lake build` succeeds with no warning (Lean warns on every sorry);
#   3. every main theorem's axioms are within {propext, Classical.choice,
#      Quot.sound} (Prism/Axioms.lean; sorryAx or anything else fails);
#   4. the exported truth tables equal tests/data/verdict_tables.json, which
#      the C++ doctest and tests/test_verdict.py check the code against.
#
# --write regenerates tests/data/verdict_tables.json instead of step 4's diff.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(dirname "$here")"
tables="$repo/tests/data/verdict_tables.json"
cd "$here"
fail() { echo "proofs: FAIL: $*" >&2; exit 1; }

echo "== 1. no sorry in sources"
python3 - <<'PY' || fail "sorry/native_decide in a Lean source"
import pathlib, re, sys
bad = []
for p in pathlib.Path(".").rglob("*.lean"):
    if ".lake" in p.parts:
        continue
    text = re.sub(r"--[^\n]*|/-.*?-/", "", p.read_text(encoding="utf-8"), flags=re.S)
    for m in re.finditer(r"\b(sorry|native_decide)\b", text):
        bad.append(f"{p}: {m.group(1)}")
print("\n".join(bad) or "ok")
sys.exit(1 if bad else 0)
PY

echo "== 2. lake build"
log="$(mktemp)"
lake build 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
[ "$status" -eq 0 ] || fail "lake build exited $status"
if grep -Eiq "warning|declaration uses 'sorry'" "$log"; then
  fail "lake build printed a warning (see above)"
fi

echo "== 3. axioms"
lake env lean Prism/Axioms.lean > "$log" 2>&1 || { cat "$log"; fail "Axioms.lean did not check"; }
cat "$log"
expected=$(grep -c '^#print axioms' Prism/Axioms.lean)
seen=$(grep -Ec "depends on axioms|does not depend on any axioms" "$log")
[ "$seen" -eq "$expected" ] || fail "expected $expected axiom reports, saw $seen"
python3 - "$log" <<'PY' || fail "a theorem uses an axiom outside the allowed set"
import re, sys
allowed = {"propext", "Classical.choice", "Quot.sound"}
bad = []
for line in open(sys.argv[1], encoding="utf-8"):
    m = re.search(r"'([^']+)' depends on axioms: \[(.*)\]", line)
    if m:
        extra = {a.strip() for a in m.group(2).split(",")} - allowed
        if extra:
            bad.append(f"{m.group(1)}: {sorted(extra)}")
print("\n".join(bad) or "ok: only propext / Classical.choice / Quot.sound")
sys.exit(1 if bad else 0)
PY

echo "== 4. truth tables"
out="$(mktemp)"
lake exe verdict_tables > "$out"
if [ "${1:-}" = "--write" ]; then
  cp "$out" "$tables"
  echo "wrote $tables"
else
  diff -u "$tables" "$out" || fail "tests/data/verdict_tables.json differs from the Lean model (run proofs/check.sh --write)"
  echo "ok: tests/data/verdict_tables.json equals the Lean model"
fi
rm -f "$log" "$out"
echo "proofs: OK"
