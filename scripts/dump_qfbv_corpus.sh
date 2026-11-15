#!/usr/bin/env bash
# Dump PRISM's QF_BV verification-condition queries as SMT2 files and split
# them train / validation / test BY SOURCE FILE (never by query: the queries of
# one function are near-copies of each other, a split by query leaks).
#
#   scripts/dump_qfbv_corpus.sh --prism build/prism --out /dev/shm/qfbv-corpus \
#       [--split "80 10 10"] [--unwind 8] [--timeout 30] [--jobs 4] \
#       [--compile-commands DB] [--max-bytes 8388608] UNIT.c [UNIT.c ...]
#   scripts/dump_qfbv_corpus.sh ... --units-from LIST.txt     # one path per line
#
# The queries are the ones the pir stage sends to the solver: `prism --pir-vcs
# UNIT --out DIR` (src/prism/pir/bench.cpp) writes every VC of every encodable
# function of a unit, each satisfiable exactly when its property is violated.
# --compile-commands DB gives each unit its own flags (a Linux kernel unit needs
# it, docs/PIR.md "GNU C units and a unit's own flags").
#
# Output (OUT/):
#   train/ validation/ test/    <unit>.<function>.<k>.smt2   (QF_BV only)
#   manifest.tsv               split  unit  query  bytes
#   rejected.tsv               query  reason   (not QF_BV, or over --max-bytes)
#   summary.txt                counts and bytes per split, units per split
#
# The split is a hash of the unit's path relative to --root (default: the
# unit as given), so adding units never moves an old unit between splits.
# With few units a split may be empty; the summary says so.
#
# Resource rules: every prism process runs under `ulimit -v` (default 2 GiB,
# PRISM_CORPUS_ULIMIT_KB), at most --jobs (<= 8) at once. Put OUT in RAM or on
# a big disk (/dev/shm, D:), not on a small system drive. Nothing here solves a
# query; the corpus is an input for training (docs/FASTSMT_PORT_NOTES.md).
set -euo pipefail

prism="" out="" split="80 10 10" unwind=8 timeout=30 jobs=4 db="" maxb=8388608 root=""
units=()
while [ $# -gt 0 ]; do
  case "$1" in
    --prism) prism="$2"; shift 2;;
    --out) out="$2"; shift 2;;
    --split) split="$2"; shift 2;;
    --unwind) unwind="$2"; shift 2;;
    --timeout) timeout="$2"; shift 2;;
    --jobs) jobs="$2"; shift 2;;
    --compile-commands) db="$2"; shift 2;;
    --max-bytes) maxb="$2"; shift 2;;
    --root) root="$2"; shift 2;;
    --units-from) while IFS= read -r l; do [ -n "$l" ] && units+=("$l"); done < "$2"; shift 2;;
    -h|--help) sed -n '2,32p' "$0"; exit 0;;
    --*) echo "unknown option $1" >&2; exit 2;;
    *) units+=("$1"); shift;;
  esac
done
[ -x "$prism" ] || { echo "--prism BINARY required" >&2; exit 2; }
[ -n "$out" ] || { echo "--out DIR required" >&2; exit 2; }
[ ${#units[@]} -gt 0 ] || { echo "no units given" >&2; exit 2; }
[ "$jobs" -le 8 ] || { echo "--jobs is capped at 8" >&2; exit 2; }
read -r s_train s_valid s_test <<< "$split"
[ $((s_train + s_valid + s_test)) -eq 100 ] || { echo "--split must sum to 100" >&2; exit 2; }
ulimit -v "${PRISM_CORPUS_ULIMIT_KB:-2097152}"

mkdir -p "$out/train" "$out/validation" "$out/test"
work="$(mktemp -d "${TMPDIR:-/dev/shm}/qfbv-corpus.XXXXXX")"
trap 'rm -rf "$work"' EXIT
: > "$out/manifest.tsv"; : > "$out/rejected.tsv"

split_of() {  # unit key -> train|validation|test
  local h; h=$(printf '%s' "$1" | cksum | cut -d' ' -f1); h=$((h % 100))
  if [ "$h" -lt "$s_train" ]; then echo train
  elif [ "$h" -lt $((s_train + s_valid)) ]; then echo validation
  else echo test; fi
}

# QF_BV only: no Int/Real/Array/FloatingPoint sorts, no uninterpreted sorts, no
# function with arguments, no quantifier.
is_qfbv() {
  ! grep -Eq '\bInt\b|\bReal\b|\(Array |\(_ FloatingPoint|Float(16|32|64|128)\b|\(declare-sort|\(declare-fun [^ ]+ \([^)]|\((forall|exists) ' "$1"
}

dump_one() {
  local unit="$1" i="$2"
  local d="$work/u$i"; mkdir -p "$d"
  local args=(--pir-vcs "$unit" --out "$d" --unwind "$unwind" --timeout "$timeout")
  [ -n "$db" ] && args+=(--compile-commands "$db")
  "$prism" "${args[@]}" > "$d/vcs.json" 2> "$d/stderr.txt" || echo "warn: prism --pir-vcs failed for $unit (rc $?)" >&2
}
export -f dump_one 2>/dev/null || true

i=0; running=0
for u in "${units[@]}"; do
  [ -f "$u" ] || { echo "warn: no such unit $u" >&2; continue; }
  dump_one "$u" "$i" &
  i=$((i + 1)); running=$((running + 1))
  if [ "$running" -ge "$jobs" ]; then wait -n || true; running=$((running - 1)); fi
done
wait || true

i=0
for u in "${units[@]}"; do
  d="$work/u$i"; i=$((i + 1))
  [ -d "$d" ] || continue
  key="$u"; [ -n "$root" ] && key="${u#"$root"/}"
  sp=$(split_of "$key")
  for q in "$d"/*.smt2; do
    [ -f "$q" ] || continue
    b=$(basename "$q"); sz=$(wc -c < "$q")
    if [ "$sz" -gt "$maxb" ]; then printf '%s\tover --max-bytes (%s)\n' "$b" "$sz" >> "$out/rejected.tsv"; continue; fi
    if ! is_qfbv "$q"; then printf '%s\tnot QF_BV\n' "$b" >> "$out/rejected.tsv"; continue; fi
    # a unit name may repeat across directories: keep the key in the file name
    safe=$(printf '%s' "$key" | tr '/ ' '__')
    mv "$q" "$out/$sp/${safe}.${b#*.}"
    printf '%s\t%s\t%s\t%s\n' "$sp" "$key" "$sp/${safe}.${b#*.}" "$sz" >> "$out/manifest.tsv"
  done
done

{
  echo "units: ${#units[@]}   split ${s_train}/${s_valid}/${s_test} by unit (cksum of the unit path)"
  for sp in train validation test; do
    n=$(awk -F'\t' -v s="$sp" '$1==s' "$out/manifest.tsv" | wc -l)
    b=$(awk -F'\t' -v s="$sp" '$1==s{t+=$4} END{print t+0}' "$out/manifest.tsv")
    u=$(awk -F'\t' -v s="$sp" '$1==s{print $2}' "$out/manifest.tsv" | sort -u | wc -l)
    echo "$sp: $n queries from $u units, $b bytes"
  done
  echo "rejected: $(wc -l < "$out/rejected.tsv") (rejected.tsv)"
} | tee "$out/summary.txt"
