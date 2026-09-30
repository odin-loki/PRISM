#!/usr/bin/env bash
# Build the LLVM -> PIR refinement proofs (roadmap 8.2) and audit them.
# Fails if:
#   * the build fails or prints any warning (Lean warns on every `sorry`);
#   * a source contains an escape hatch (sorry, admit, native_decide,
#     bv_decide, implemented_by, extern, a top-level axiom or unsafe);
#   * an audited theorem depends on an axiom other than propext,
#     Classical.choice and Quot.sound;
#   * the correspondence checker disagrees with its fixtures.
set -euo pipefail
cd "$(dirname "$0")"
repo="$(cd ../.. && pwd)"
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

echo "== lake build"
lake build 2>&1 | tee build.log
status=${PIPESTATUS[0]}
[ "$status" -eq 0 ] || { echo "FAIL: lake build exited $status" >&2; exit 1; }
if grep -Eiq "warning|declaration uses 'sorry'" build.log; then
  echo "FAIL: lake build printed a warning (see above)" >&2
  exit 1
fi

echo "== escape-hatch scan"
lean_audit scan --words sorry,admit,native_decide,bv_decide,implemented_by,extern --toplevel axiom,unsafe . \
  || { echo "FAIL: escape hatch (listed above)" >&2; exit 1; }

echo "== axiom audit"
lake env lean Audit.lean 2>&1 | tee axioms.txt
expected=$(grep -c '^#print axioms' Audit.lean)
seen=$(grep -Ec "depends on axioms|does not depend on any axioms" axioms.txt || true)
[ "$seen" = "$expected" ] || { echo "FAIL: expected $expected axiom reports, saw $seen" >&2; exit 1; }
lean_audit axioms axioms.txt || { echo "FAIL: non-standard axiom (listed above)" >&2; exit 1; }

echo "== correspondence checker fixtures"
for f in fixtures/*.pirl; do
  want="${f%.pirl}.expected"
  got="$(lake exe pir_lean_check "$f" | tail -1 || true)"
  if [ "$got" != "$(cat "$want")" ]; then
    echo "FAIL: $f: got '$got', expected '$(cat "$want")'" >&2
    exit 1
  fi
  echo "ok: $f: $got"
done
echo "refinement: OK ($seen theorems audited)"
