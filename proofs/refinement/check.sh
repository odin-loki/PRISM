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

echo "== lake build"
lake build 2>&1 | tee build.log
status=${PIPESTATUS[0]}
[ "$status" -eq 0 ] || { echo "FAIL: lake build exited $status" >&2; exit 1; }
if grep -Eiq "warning|declaration uses 'sorry'" build.log; then
  echo "FAIL: lake build printed a warning (see above)" >&2
  exit 1
fi

echo "== escape-hatch scan"
python3 - <<'PY'
import pathlib, re, sys
bad = []
for p in sorted(pathlib.Path(".").rglob("*.lean")):
    if ".lake" in p.parts:
        continue
    text = re.sub(r"/-.*?-/", "", p.read_text(encoding="utf-8"), flags=re.S)
    text = re.sub(r"--[^\n]*", "", text)
    for m in re.finditer(r"\b(sorry|admit|native_decide|bv_decide|implemented_by|extern)\b"
                         r"|^\s*(axiom|unsafe)\b", text, flags=re.M):
        bad.append(f"{p}: {m.group(0).strip()}")
print("\n".join(bad) or "ok")
sys.exit(1 if bad else 0)
PY

echo "== axiom audit"
lake env lean Audit.lean 2>&1 | tee axioms.txt
expected=$(grep -c '^#print axioms' Audit.lean)
seen=$(grep -Ec "depends on axioms|does not depend on any axioms" axioms.txt || true)
[ "$seen" = "$expected" ] || { echo "FAIL: expected $expected axiom reports, saw $seen" >&2; exit 1; }
python3 - axioms.txt <<'PY'
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
