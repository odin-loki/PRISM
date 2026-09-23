#!/usr/bin/env bash
# Independent re-checking of one PRISM Lean project (roadmap 8.5).
#
#   proofs/refinement/recheck.sh PROJECT_DIR ROOT_MODULE [ROOT_MODULE...]
#
# Steps, each of which must pass:
#   1. lake build of the project;
#   2. source scan: no `sorry`, `native_decide`, `bv_decide`, top-level
#      `axiom` / `unsafe` outside comments;
#   3. axiom audit of *every* declaration under the root modules
#      (axiom_report, built from proofs/refinement): only propext,
#      Classical.choice and Quot.sound;
#   4. leanchecker (the toolchain's replay checker, formerly lean4checker):
#      every declaration of every module under the roots is re-added to a
#      kernel environment built from its imports;
#   5. nanoda (independent Rust kernel, third_party/MANIFEST.toml): the
#      lean4export NDJSON export of the roots and all their dependencies is
#      type-checked from scratch.
#
# A missing checker is NOTRUN and the script exits 3 (never a silent pass),
# unless PRISM_RECHECK_ALLOW_MISSING=1 (local runs without nanoda).
# Tools are found on PATH or in ~/.prism/tools/<name>/<commit>/bin
# (python scripts/fetch_deps.py --tool lean4export / --tool nanoda).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
proj="$(cd "$1" && pwd)"; shift
roots=("$@")
[ ${#roots[@]} -gt 0 ] || { echo "usage: $0 PROJECT_DIR ROOT_MODULE..." >&2; exit 2; }
mkdir -p "$proj/.lake"
report="$proj/.lake/recheck-report.txt"
: > "$report"
say() { echo "$*" | tee -a "$report"; }
tools="${PRISM_TOOLS_DIR:-$HOME/.prism/tools}"
find_tool() {  # name bin
  local p
  p="$(command -v "$2" 2>/dev/null || true)"
  [ -n "$p" ] && { echo "$p"; return; }
  for d in "$tools/$1"/*/bin; do [ -x "$d/$2" ] && { echo "$d/$2"; return; }; done
  echo ""
}
missing=0

say "== recheck $proj (${roots[*]})"
say "toolchain: $(cd "$proj" && lean --version)"

say "== 1. lake build"
(cd "$proj" && lake build) > "$proj/.lake/recheck-build.log" 2>&1 || { tail -40 "$proj/.lake/recheck-build.log"; say "FAIL: lake build"; exit 1; }
if grep -q "declaration uses 'sorry'" "$proj/.lake/recheck-build.log"; then say "FAIL: sorry"; exit 1; fi
say "ok"

say "== 2. source scan"
python3 - "$proj" <<'PY' | tee -a "$report"
import pathlib, re, sys
bad = []
for p in sorted(pathlib.Path(sys.argv[1]).rglob("*.lean")):
    if ".lake" in p.parts:
        continue
    text = re.sub(r"/-.*?-/", "", p.read_text(encoding="utf-8"), flags=re.S)
    text = re.sub(r"--[^\n]*", "", text)
    for m in re.finditer(r"\b(sorry|native_decide|bv_decide)\b|^\s*(axiom|unsafe)\b", text, flags=re.M):
        bad.append(f"{p}: {m.group(0).strip()}")
print("\n".join(bad) or "ok")
sys.exit(1 if bad else 0)
PY

say "== 3. axiom audit (every declaration)"
ar="$here/.lake/build/bin/axiom_report"
[ -x "$ar" ] || (cd "$here" && lake build axiom_report >/dev/null)
(cd "$proj" && lake env "$ar" "${roots[@]}") | tee -a "$report"

say "== 4. leanchecker (kernel replay of the .olean files)"
(cd "$proj" && lake env leanchecker "${roots[@]}") 2>&1 | tee -a "$report"
say "ok: leanchecker replayed every module under ${roots[*]}"

say "== 5. nanoda (independent kernel)"
le="$(find_tool lean4export lean4export)"
nd="$(find_tool nanoda nanoda_bin)"
if [ -z "$le" ] || [ -z "$nd" ]; then
  say "NOTRUN: lean4export/nanoda_bin not found (python scripts/fetch_deps.py --tool lean4export; --tool nanoda)"
  missing=1
else
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT
  (cd "$proj" && lake env "$le" "${roots[@]}") > "$tmp/export.ndjson"
  # Lean.trustCompiler is permitted for the *environment* only: core Init
  # declares it (for native_decide). Step 3 already rejects any PRISM
  # declaration that depends on it.
  cat > "$tmp/nanoda.json" <<EOF
{
  "export_file_path": "$tmp/export.ndjson",
  "use_stdin": false,
  "permitted_axioms": ["propext", "Classical.choice", "Quot.sound", "Lean.trustCompiler"],
  "unpermitted_axiom_hard_error": false,
  "nat_extension": true,
  "string_extension": true,
  "print_success_message": true
}
EOF
  if ! "$nd" "$tmp/nanoda.json" > "$tmp/nanoda.out" 2>&1; then
    tail -20 "$tmp/nanoda.out" | tee -a "$report"
    say "FAIL: nanoda rejected the export"
    exit 1
  fi
  tail -5 "$tmp/nanoda.out" | tee -a "$report"
  say "export: $(wc -l < "$tmp/export.ndjson") NDJSON records"
fi

if [ "$missing" -ne 0 ] && [ "${PRISM_RECHECK_ALLOW_MISSING:-0}" != "1" ]; then
  say "recheck: INCOMPLETE (a checker was NOTRUN)"
  exit 3
fi
if [ "$missing" -ne 0 ]; then
  say "recheck: OK for the checkers that ran (nanoda NOTRUN, allowed by PRISM_RECHECK_ALLOW_MISSING=1)"
else
  say "recheck: OK"
fi
