#!/usr/bin/env bash
# PRISM vs Frama-C/WP on the OS-Proving core tier (docs/VERIFY_PRISM.md B3).
#
# usage: scripts/os_proving_compare.sh --units DIR --labels labels.tsv.gz [--prism BIN] [--out DIR] [-j N]
#                                       [--source unit.final.c] [--limit N]
#
#   --units   directory of OS-Proving unit directories (u001/, u002/, ...), each with
#             meta.tsv and the unit source with its ACSL contract (unit.final.c, else
#             unit.c). The sources are only on the machine that built the units.
#   --labels  OS-Proving data/proving/core/labels.tsv.gz (id, name, kind, shape,
#             codebase, label, tool, assumptions, reason).
#
# PRISM runs `inventory,classify,contracts,pir --no-llm` over the staged units. Two
# comparisons, kept apart because they are about different properties:
#   B3a functional: PRISM contracts stage (requires/ensures from the ACSL block) vs the
#       WP label. PRISM proof-class vs WP FALSE, or PRISM FAILED vs WP PROVED*, is an
#       incident for one of the two tools: disagree_contracts.tsv.
#   B3b safety: PRISM pir (UB, overflow, bounds) vs the WP label. Informational: WP
#       proved the functional contract, pir checks runtime errors (WP proves those only
#       with -wp-rte). pir FAILED where WP PROVED is listed in pir_failed_wp_proved.tsv.
# Outputs in --out: compare.tsv (one row per labelled function), summary.txt (the two
# confusion tables), the disagreement lists, and prism-out/ (the PRISM run).
# Needs bash, jq, gzip, and a built prism. Nothing here changes a verdict.
set -euo pipefail

units="" labels="" prism="${PRISM:-$(dirname "$0")/../build/prism}" out="os-proving-compare" jobs=0
src_name="unit.final.c" limit=0
while [ $# -gt 0 ]; do
  case "$1" in
    --units) units="$2"; shift 2 ;;
    --labels) labels="$2"; shift 2 ;;
    --prism) prism="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    -j|--jobs) jobs="$2"; shift 2 ;;
    --source) src_name="$2"; shift 2 ;;
    --limit) limit="$2"; shift 2 ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$units" ] && [ -d "$units" ] || { echo "--units DIR is required" >&2; exit 2; }
[ -n "$labels" ] && [ -f "$labels" ] || { echo "--labels FILE is required" >&2; exit 2; }
[ -x "$prism" ] || { echo "prism not found or not executable: $prism (--prism BIN)" >&2; exit 2; }
command -v jq >/dev/null || { echo "jq is required" >&2; exit 2; }

mkdir -p "$out/stage"
: > "$out/staged.tsv"
n=0 missing=0
for d in "$units"/*/; do
  [ -f "$d/meta.tsv" ] || continue
  id=$(cut -f1 "$d/meta.tsv" | head -1)
  name=$(cut -f2 "$d/meta.tsv" | head -1)
  src="$d/$src_name"; [ -f "$src" ] || src="$d/unit.c"
  if [ ! -f "$src" ]; then missing=$((missing + 1)); continue; fi
  cp "$src" "$out/stage/u$id.c"
  printf 'u%s.c\t%s\t%s\n' "$id" "$name" "$id" >> "$out/staged.tsv"
  n=$((n + 1))
  [ "$limit" -gt 0 ] && [ "$n" -ge "$limit" ] && break
done
echo "staged $n units ($missing without a source file)" >&2
[ "$n" -gt 0 ] || { echo "no unit has a source file ($src_name or unit.c)" >&2; exit 1; }

"$prism" "$out/stage" --no-llm --stage inventory,classify,contracts,pir -j "$jobs" --out "$out/prism-out" \
  > "$out/prism.log" 2>&1 || true
rep="$out/prism-out/report.json"
[ -f "$rep" ] || { echo "PRISM wrote no report (see $out/prism.log)" >&2; exit 1; }

# PRISM rows: file, function, stage, status (first row per key and stage)
jq -r '[.stages[]?.findings[]?, .findings[]?] | .[] | select(.stage == "contracts" or .stage == "pir")
       | select(.function != null) | [(.file | split("/") | last), .function, .stage, .status] | @tsv' "$rep" \
  | sort -u -t$'\t' -k1,1 -k2,2 -k3,3 > "$out/prism_rows.tsv"

# labels: id -> (name, kind, codebase, label)
gzip -dc "$labels" | awk -F'\t' '{print $1"\t"$2"\t"$3"\t"$5"\t"$6}' | sort -t$'\t' -k1,1 > "$out/labels.tsv"

awk -F'\t' -v OFS='\t' '
  FILENAME == ARGV[1] { file[$3] = $1; next }                       # staged: file name id
  FILENAME == ARGV[2] { key = $1 "\t" $2; st[key, $3] = $4; next }  # prism rows
  {                                                                  # labels
    id = $1; if (!(id in file)) next
    key = file[id] "\t" $2
    c = ((key, "contracts") in st) ? st[key, "contracts"] : "-"
    p = ((key, "pir") in st) ? st[key, "pir"] : "-"
    print id, $2, $3, $4, $5, c, p
  }' "$out/staged.tsv" "$out/prism_rows.tsv" "$out/labels.tsv" > "$out/compare.tsv.body"
{ printf 'id\tfunction\tkind\tcodebase\twp_label\tprism_contracts\tprism_pir\n'; cat "$out/compare.tsv.body"; } > "$out/compare.tsv"
rm -f "$out/compare.tsv.body"

proofish='^(PROVED|PROVED-ASSUMING|PROVED-UNBOUNDED|PROVED-CERTIFIED)$'
awk -F'\t' -v OFS='\t' -v P="$proofish" 'NR > 1 && $6 ~ P && ($5 == "FALSE" || $5 == "FAILED")' "$out/compare.tsv" \
  > "$out/disagree_contracts.tsv"
awk -F'\t' -v OFS='\t' -v P="$proofish" 'NR > 1 && $6 == "FAILED" && $5 ~ /^PROVED/' "$out/compare.tsv" \
  >> "$out/disagree_contracts.tsv"
awk -F'\t' -v OFS='\t' 'NR > 1 && $7 == "FAILED" && $5 ~ /^PROVED/' "$out/compare.tsv" > "$out/pir_failed_wp_proved.tsv"

{
  echo "OS-Proving core tier vs PRISM ($(date -u +%Y-%m-%dT%H:%M:%SZ), $("$prism" --version 2>/dev/null | head -1))"
  echo "units staged: $n; labelled functions compared: $(($(wc -l < "$out/compare.tsv") - 1))"
  echo
  echo "B3a functional contract: WP label (rows) x PRISM contracts verdict (columns)"
  awk -F'\t' 'NR > 1 { t[$5 "\t" $6]++; r[$5]; c[$6] }
    END { printf "%-16s", "wp \\ prism"; for (j in c) printf "%18s", j; print "";
          for (i in r) { printf "%-16s", i; for (j in c) printf "%18d", t[i "\t" j]; print "" } }' "$out/compare.tsv"
  echo
  echo "B3b safety (informational): WP label (rows) x PRISM pir verdict (columns)"
  awk -F'\t' 'NR > 1 { t[$5 "\t" $7]++; r[$5]; c[$7] }
    END { printf "%-16s", "wp \\ prism"; for (j in c) printf "%18s", j; print "";
          for (i in r) { printf "%-16s", i; for (j in c) printf "%18d", t[i "\t" j]; print "" } }' "$out/compare.tsv"
  echo
  echo "disagreements to triage (B3a): $(wc -l < "$out/disagree_contracts.tsv")  -> disagree_contracts.tsv"
  echo "pir FAILED where WP PROVED (B3b, read the cex): $(wc -l < "$out/pir_failed_wp_proved.tsv")  -> pir_failed_wp_proved.tsv"
} > "$out/summary.txt"
cat "$out/summary.txt"
