#!/usr/bin/env bash
# Merge gate for engine changes (docs/VERIFY_PRISM.md "Merge gate"). A change to
# the engine merges only when this passes against the binary it replaces.
#
# usage: scripts/verify_gate.sh --candidate BIN --baseline BIN [--quick | --full] [-j N]
#                               [--stages a,b] [--corpus DIR]... [--tests BIN [--tests-filter F]]
#                               [--qa BIN] [--out DIR]
#
# Checks (each PASS / FAIL / SKIP, the gate fails on any FAIL):
#   G1 soundness   prism-qa conformance with the candidate: 0 wrong proofs (B1).
#                  --quick: testdata/conformance/prism only; --full: every suite.
#   G2 no lost or flipped verdict vs the baseline on the corpus (default
#                  testdata/pir, testdata/planted): per (stage, file, function,
#                  line) the status must be equal, or stronger in the allowed
#                  direction (UNKNOWN/TIMEOUT/NEEDS-HARNESS -> an answer;
#                  BOUNDED -> PROVED*/FAILED). PROVED* <-> FAILED, an answer
#                  -> no answer, or a row that disappears is a FAIL. Messages
#                  are not compared (they carry timings).
#   G3 --jobs 1 and --jobs N give the same rows (C1).
#   G4 a cold run and a warm re-run (same solver cache) give the same
#                  statuses (C2, C3).
#   G5 every function of the inventory has a pir row when pir ran (D1).
#   G6 doctests (with --tests): only the known pre-existing failures may fail.
# Outputs in --out: one report per run, diff files, gate.txt. Needs bash, jq.
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cand="" base="" mode=quick jobs=2 stages="inventory,classify,pir" out="gate-out" tests="" tests_filter="*" qa=""
corpus=()
while [ $# -gt 0 ]; do
  case "$1" in
    --candidate) cand="$2"; shift 2 ;;
    --baseline) base="$2"; shift 2 ;;
    --quick) mode=quick; shift ;;
    --full) mode=full; shift ;;
    -j|--jobs) jobs="$2"; shift 2 ;;
    --stages) stages="$2"; shift 2 ;;
    --corpus) corpus+=("$2"); shift 2 ;;
    --tests) tests="$2"; shift 2 ;;
    --tests-filter) tests_filter="$2"; shift 2 ;;
    --qa) qa="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -x "$cand" ] || { echo "--candidate BIN is required" >&2; exit 2; }
[ -x "$base" ] || { echo "--baseline BIN is required" >&2; exit 2; }
command -v jq >/dev/null || { echo "jq is required" >&2; exit 2; }
[ ${#corpus[@]} -gt 0 ] || corpus=("$root/testdata/pir" "$root/testdata/planted")
[ -n "$qa" ] || qa="$(dirname "$cand")/prism-qa"
[ -x "$qa" ] || qa="$root/build/prism-qa"
mkdir -p "$out"
: > "$out/gate.txt"
fail=0
note() { echo "$1" | tee -a "$out/gate.txt"; }
result() {  # name status detail
  note "$(printf '%-4s %-5s %s' "$1" "$2" "$3")"
  [ "$2" = FAIL ] && fail=1
  return 0
}

# rows: stage file function line status (sorted), from a report
rows() {
  jq -r '[.stages[]?.findings[]?, .findings[]?] | .[] | select(.function != null)
         | [.stage, .file, .function, (.line // 0 | tostring), .status] | @tsv' "$1" | sort
}
run() {  # bin corpus-dir out-dir jobs cache-dir [env...]
  local bin=$1 src=$2 o=$3 j=$4 cache=$5; shift 5
  rm -rf "$o"
  env "$@" "$bin" "$src" --no-llm --stage "$stages" -j "$j" --out "$o" --solver-cache "$cache" > "$o.log" 2>&1
  [ -f "$o/report.json" ]
}

note "PRISM merge gate ($mode) $(date -u +%Y-%m-%dT%H:%M:%SZ)"
note "candidate: $cand ($("$cand" --version 2>/dev/null | head -1))"
note "baseline:  $base ($("$base" --version 2>/dev/null | head -1))"
note "stages: $stages; corpus: ${corpus[*]}"

# ---- G1 soundness
if [ -x "$qa" ]; then
  suite_args=()
  [ "$mode" = quick ] && suite_args=(--suite "$root/testdata/conformance/prism")
  if "$qa" conformance --prism "$cand" "${suite_args[@]}" -j "$jobs" --out "$out/conformance" > "$out/conformance.log" 2>&1; then
    result G1 PASS "conformance: 0 wrong proofs ($mode)"
  else
    wp=$(grep -o 'RELEASE GATE FAILED: [0-9]* wrong proof' "$out/conformance.log" | head -1)
    result G1 FAIL "${wp:-conformance run failed (see conformance.log)}"
  fi
else
  result G1 FAIL "prism-qa not found (--qa BIN): soundness not checked"
fi

# ---- G2 vs baseline, G3 jobs, G4 cache, G5 coverage
g2_bad=0 g3_bad=0 g4_bad=0 g5_bad=0
for src in "${corpus[@]}"; do
  tag=$(basename "$src")
  run "$base" "$src" "$out/$tag-base" "$jobs" "$out/$tag-cache-base" PRISM_PIR_FN_CACHE=0 || { result G2 FAIL "$tag: baseline wrote no report"; continue; }
  run "$cand" "$src" "$out/$tag-cand" "$jobs" "$out/$tag-cache-cand" || { result G2 FAIL "$tag: candidate wrote no report"; continue; }
  rows "$out/$tag-base/report.json" > "$out/$tag-base.rows"
  rows "$out/$tag-cand/report.json" > "$out/$tag-cand.rows"
  # G2: join on the key (first four columns)
  awk -F'\t' -v OFS='\t' '
    function cls(s) { if (s ~ /^PROVED/) return "P"; if (s == "BOUNDED") return "B"; if (s == "FAILED" || s == "CRASH" || s == "SANFAIL") return "F"; return "N" }
    FNR == NR { b[$1 FS $2 FS $3 FS $4] = $5; next }
    { k = $1 FS $2 FS $3 FS $4; c[k] = $5 }
    END {
      for (k in b) {
        if (!(k in c)) { print "LOST-ROW", k, b[k], "-"; continue }
        x = cls(b[k]); y = cls(c[k])
        if (b[k] == c[k]) continue
        if ((x == "P" && y == "F") || (x == "F" && y == "P")) print "FLIP", k, b[k], c[k]
        else if (x != "N" && y == "N") print "LOST", k, b[k], c[k]
        else if (x == "B" && y == "B") print "SAME-CLASS", k, b[k], c[k]
        else if ((x == "P" || x == "F") && y == "B") print "WEAKER", k, b[k], c[k]
        else if (x == "P" && y == "P") print "PROOF-KIND", k, b[k], c[k]
        else print "STRONGER", k, b[k], c[k]
      }
      for (k in c) if (!(k in b)) print "NEW-ROW", k, "-", c[k]
    }' "$out/$tag-base.rows" "$out/$tag-cand.rows" | sort > "$out/$tag.g2.tsv"
  bad=$(grep -c -E '^(LOST-ROW|FLIP|LOST|WEAKER|PROOF-KIND)' "$out/$tag.g2.tsv" || true)
  better=$(grep -c -E '^STRONGER' "$out/$tag.g2.tsv" || true)
  # a PROOF-KIND change (e.g. PROVED -> PROVED-ASSUMING) is a FAIL too: read it
  [ "$bad" -gt 0 ] && g2_bad=$((g2_bad + bad))
  note "     G2 $tag: $(wc -l < "$out/$tag-base.rows") rows; $bad lost/flipped/weaker; $better stronger (see $tag.g2.tsv)"
  # G3: candidate at -j 1
  if run "$cand" "$src" "$out/$tag-cand-j1" 1 "$out/$tag-cache-j1"; then
    rows "$out/$tag-cand-j1/report.json" > "$out/$tag-cand-j1.rows"
    if ! diff -q "$out/$tag-cand.rows" "$out/$tag-cand-j1.rows" > /dev/null; then
      g3_bad=$((g3_bad + 1)); diff "$out/$tag-cand-j1.rows" "$out/$tag-cand.rows" > "$out/$tag.g3.diff"
    fi
  else g3_bad=$((g3_bad + 1)); fi
  # G4: warm re-run on the candidate's cache
  if run "$cand" "$src" "$out/$tag-cand-warm" "$jobs" "$out/$tag-cache-cand"; then
    rows "$out/$tag-cand-warm/report.json" > "$out/$tag-cand-warm.rows"
    if ! diff -q "$out/$tag-cand.rows" "$out/$tag-cand-warm.rows" > /dev/null; then
      g4_bad=$((g4_bad + 1)); diff "$out/$tag-cand.rows" "$out/$tag-cand-warm.rows" > "$out/$tag.g4.diff"
    fi
  else g4_bad=$((g4_bad + 1)); fi
  # G5: inventory functions without a pir row (only when pir ran)
  if [[ ",$stages," == *",pir,"* ]]; then
    jq -r '.functions[]? | select(.file != null and .name != null) | [.file, .name] | @tsv' "$out/$tag-cand/report.json" 2>/dev/null \
      | sort -u > "$out/$tag.inv"
    awk -F'\t' '$1 == "pir" { print $2 "\t" $3 }' "$out/$tag-cand.rows" | sort -u > "$out/$tag.pirfn"
    if [ -s "$out/$tag.inv" ]; then
      comm -23 "$out/$tag.inv" "$out/$tag.pirfn" > "$out/$tag.g5.missing"
      m=$(wc -l < "$out/$tag.g5.missing")
      [ "$m" -gt 0 ] && g5_bad=$((g5_bad + m))
    fi
  fi
done
[ "$g2_bad" -eq 0 ] && result G2 PASS "no lost, flipped or weaker verdict vs the baseline" \
                     || result G2 FAIL "$g2_bad lost/flipped/weaker verdict(s) vs the baseline (*.g2.tsv)"
[ "$g3_bad" -eq 0 ] && result G3 PASS "--jobs 1 and --jobs $jobs give the same rows" \
                     || result G3 FAIL "--jobs 1 vs --jobs $jobs differ (*.g3.diff)"
[ "$g4_bad" -eq 0 ] && result G4 PASS "cold and warm runs give the same rows" \
                     || result G4 FAIL "cold vs warm differ (*.g4.diff)"
if [[ ",$stages," == *",pir,"* ]]; then
  [ "$g5_bad" -eq 0 ] && result G5 PASS "every inventory function has a pir row (or the inventory lists none)" \
                       || result G5 FAIL "$g5_bad inventory function(s) without a pir row (*.g5.missing)"
else result G5 SKIP "pir not in --stages"; fi

# ---- G6 doctests
if [ -n "$tests" ]; then
  # failures that are on the original code too (docs/VERIFY_PRISM.md "Merge gate")
  known='astlint: ported regex classes|svcomp fuzz_replay|cxx expected model: operator<'
  "$tests" -tc="$tests_filter" > "$out/tests.log" 2>&1 || true
  awk '/^TEST CASE:/ { tc = substr($0, 13) } /ERROR:/ { print tc }' "$out/tests.log" | sort -u > "$out/tests.failed"
  unexpected=$(grep -v -E "$known" "$out/tests.failed" | grep -c . || true)
  summary=$(grep "test cases:" "$out/tests.log" | tail -1)
  [ "$unexpected" -eq 0 ] && result G6 PASS "${summary:-doctests ran} (known failures only)" \
                           || result G6 FAIL "$unexpected unexpected failing test case(s) (tests.failed)"
else result G6 SKIP "no --tests BIN"; fi

note ""
if [ "$fail" -eq 0 ]; then note "GATE PASSED"; exit 0; else note "GATE FAILED"; exit 1; fi
