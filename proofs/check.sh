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
# lean_audit (src/tools/lean_audit.cpp): the comment-aware source scan and the
# axiom-log audit, a standard-library-only C++23 file built on first use.
lean_audit() {
  local src="$repo/src/tools/lean_audit.cpp" bin="$repo/proofs/.lake/lean_audit"
  if [ ! -x "$bin" ] || [ "$src" -nt "$bin" ]; then
    mkdir -p "$(dirname "$bin")"
    "${CXX:-c++}" -std=c++23 -O2 -o "$bin.tmp.$$" "$src" && mv -f "$bin.tmp.$$" "$bin" || return 2
  fi
  "$bin" "$@"
}

echo "== 1. no sorry in sources"
lean_audit scan --words sorry,native_decide . || fail "sorry/native_decide in a Lean source"

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
lean_audit axioms "$log" || fail "a theorem uses an axiom outside the allowed set"

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
