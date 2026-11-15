#!/usr/bin/env bash
# Batch driver for the pir stage over a compile-commands database
# (docs/PIR.md "Batch mode"). One `prism` process per translation unit, up to
# N at once; one JSON line per function in append-only shards; a manifest of
# finished units, so a stopped run resumes where it was.
#
#   scripts/pir_batch.sh --db compile_commands.json --out DIR [options]
#   scripts/pir_batch.sh --out DIR --summary          # counts over the shards
#
# Memory: every worker runs with PRISM_MEM_BUDGET_MB, PRISM's own budget: a
# function whose check reaches it ends as UNKNOWN with outcome "memory-limit",
# never retried with more. The budget is not a fixed number: by default
# (--mem-mb auto) the first units run alone with a generous budget, and the
# budget becomes 1.5 x their measured peak RSS + 256 MB; the worker count is
# floor((MemAvailable - min-avail) / budget), capped at --max-workers.
# Do NOT use `ulimit -v`/`prlimit --as`: PRISM derives its budget from
# RLIMIT_AS and reports "memory budget reached" at a fraction of the
# address-space limit, and Z3 fails at tiny RSS under it.
# A worker starts only when MemAvailable stays above --min-avail-mb (default
# 10 GB) after every running worker has grown to its full budget.
set +u

DB="" OUT="" PRISM="" WORKERS=auto MEM_MB=auto MAX_WORKERS=5 CAL_UNITS=3 MIN_AVAIL_MB=10240 UNIT_TIMEOUT=900 QUERY_TIMEOUT=30
SHARDS=16 LIMIT=0 RETRY_FAILED=0 SUMMARY=0 ONLY="" EXTRA=()
HARNESS=1 STRIP=""

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; cat <<'EOF'
Options:
  --db FILE            compile_commands.json (units = its "file" entries, .c/.cc/.cpp/.cxx)
  --out DIR            results: DIR/shards/shard-NN.jsonl, DIR/manifest.tsv, DIR/logs/
  --prism BIN          prism binary (default: build*/prism next to the repo, else PATH)
  --workers N|auto     processes at once (default auto: floor((MemAvailable - min-avail) / budget), capped)
  --max-workers N      cap for auto (default 5 = the current process limit; at most 32)
  --mem-mb N|auto      PRISM_MEM_BUDGET_MB per worker; auto (default) = 1.5 x peak RSS (+256, at least 1024 MB) measured on the
                       first --calibrate units (run alone, 4096 MB budget) + 256 MB, kept in OUT/calibration.env
  --calibrate N        units used to measure the peak for auto (default 3)
  --min-avail-mb N     never let MemAvailable fall under this while starting workers (default 10240)
  --unit-timeout S     wall seconds per unit before it is killed and recorded (default 900)
  --timeout S          solver seconds per query (prism --timeout, default 30)
  --shards N           number of result shards (default 16; a unit always goes to the same one)
  --only REGEX         only units whose path matches
  --limit N            stop after N units started this run (0 = all)
  --retry-failed       run again the units whose manifest line is not "done"
  --no-harness         do not pass --pir-harness (pointer functions stay NEEDS-HARNESS)
  --strip-prefix DIR   make `unit` (and `key`) relative to DIR (the source tree root, the OS-Proving path); default: the absolute path
  --prism-arg ARG      extra argument for prism (repeatable), e.g. --prism-arg --pir-harness-depth --prism-arg 3
  --summary            print counts over DIR/shards and exit
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --db) DB=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    --prism) PRISM=$2; shift 2;;
    --workers) WORKERS=$2; shift 2;;
    --max-workers) MAX_WORKERS=$2; shift 2;;
    --calibrate) CAL_UNITS=$2; shift 2;;
    --mem-mb) MEM_MB=$2; shift 2;;
    --min-avail-mb) MIN_AVAIL_MB=$2; shift 2;;
    --unit-timeout) UNIT_TIMEOUT=$2; shift 2;;
    --timeout) QUERY_TIMEOUT=$2; shift 2;;
    --shards) SHARDS=$2; shift 2;;
    --only) ONLY=$2; shift 2;;
    --limit) LIMIT=$2; shift 2;;
    --retry-failed) RETRY_FAILED=1; shift;;
    --no-harness) HARNESS=0; shift;;
    --strip-prefix) STRIP=$2; shift 2;;
    --prism-arg) EXTRA+=("$2"); shift 2;;
    --summary) SUMMARY=1; shift;;
    -h|--help) usage; exit 0;;
    *) echo "pir_batch.sh: unknown option $1" >&2; usage >&2; exit 2;;
  esac
done

[ -n "$OUT" ] || { echo "pir_batch.sh: --out DIR is required" >&2; exit 2; }
command -v jq >/dev/null || { echo "pir_batch.sh: jq is required" >&2; exit 2; }

summary() {
  local files=("$OUT"/shards/shard-*.jsonl)
  [ -e "${files[0]}" ] || { echo "no shards under $OUT/shards"; return 1; }
  # a later line for the same unit+function replaces an earlier one (a retried unit)
  jq -s '
    map(select(.record=="function")) | group_by(.unit + "\u0000" + .function) | map(last) as $f
    | {
        functions: ($f | length),
        by_verdict: ($f | group_by(.verdict) | map({key: .[0].verdict, value: length}) | from_entries),
        by_level: ($f | group_by(.level // "none") | map({key: (.[0].level // "none"), value: length}) | from_entries),
        by_outcome: ($f | group_by(.outcome) | map({key: .[0].outcome, value: length}) | from_entries),
        auto_harness: ($f | map(select(.harness.auto)) | length),
        with_vcs: ($f | map(select(.vcs != null and .vcs > 0)) | length),
        lean_inside_fragment: ($f | map(select(.lean.supported == true)) | length),
        lean_outside_fragment: ($f | map(select(.lean.supported == false)) | length),
        lean_outside_by_reason: ($f | map(select(.lean.supported == false)) | group_by(.lean.reason[0:90])
                                  | map({key: .[0].lean.reason[0:90], value: length}) | from_entries),
        max_peak_mb: ($f | map(.peak_mb // 0) | max),
        total_time_s: ($f | map(.time_s // 0) | add)
      }' "${files[@]}"
  if [ -s "$OUT/manifest.tsv" ]; then
    echo "units by manifest status:"
    awk -F'\t' '{c[$2]++} END {for (k in c) printf "  %s\t%d\n", k, c[k]}' "$OUT/manifest.tsv" | sort
  fi
}

if [ "$SUMMARY" = 1 ]; then summary; exit $?; fi

[ -n "$DB" ] && [ -f "$DB" ] || { echo "pir_batch.sh: --db FILE (compile_commands.json) is required" >&2; exit 2; }
if [ -z "$PRISM" ]; then
  here=$(cd "$(dirname "$0")/.." && pwd)
  for c in "$here"/build*/prism /dev/shm/prism-build/prism; do [ -x "$c" ] && { PRISM=$c; break; }; done
  [ -n "$PRISM" ] || PRISM=$(command -v prism || true)
fi
[ -x "$PRISM" ] || { echo "pir_batch.sh: no prism binary (use --prism)" >&2; exit 2; }
DB=$(readlink -f "$DB"); OUT=$(mkdir -p "$OUT" && readlink -f "$OUT")
mkdir -p "$OUT/shards" "$OUT/logs" "$OUT/work"
touch "$OUT/manifest.tsv"

avail_mb() { awk '/^MemAvailable:/ {print int($2/1024)}' /proc/meminfo; }
# RSS (MB) of every process of the workers' process groups
running_rss_mb() {
  local sum=0 pid kb
  for pid in "${!PIDS[@]}"; do
    kb=$(ps -o rss= -g "$pid" 2>/dev/null | awk '{s+=$1} END {print s+0}')
    sum=$((sum + kb / 1024))
  done
  echo "$sum"
}

# ---- worklist -------------------------------------------------------------
declare -A DONE
while IFS=$'\t' read -r unit status _; do
  [ -z "$unit" ] && continue
  if [ "$RETRY_FAILED" = 1 ] && [ "$status" != "done" ]; then continue; fi
  DONE["$unit"]=1
done < "$OUT/manifest.tsv"

mapfile -t UNITS < <(jq -r '.[] | (if (.file|startswith("/")) then .file else (.directory + "/" + .file) end)' "$DB" \
  | grep -E '\.(c|cc|cpp|cxx)$' | sort -u)
[ -n "$ONLY" ] && mapfile -t UNITS < <(printf '%s\n' "${UNITS[@]}" | grep -E "$ONLY")
TOTAL=${#UNITS[@]}
echo "pir_batch: $TOTAL units in $DB, manifest has ${#DONE[@]} finished; out=$OUT"

EXTRA_STR=$(printf "%s
" "${EXTRA[@]+"${EXTRA[@]}"}")
export STRIP PRISM DB OUT SHARDS MEM_MB UNIT_TIMEOUT QUERY_TIMEOUT HARNESS EXTRA_STR
declare -A PIDS UNIT_OF START_OF
STARTED=0

# One unit. Runs in the background; appends its records to its shard in one
# locked write, then its manifest line (the manifest line is the commit).
run_unit() {
  local unit=$1 tag work t0 rc n shard status
  tag=$(printf '%s' "$unit" | cksum | cut -d' ' -f1)
  shard=$(printf '%s/shards/shard-%02d.jsonl' "$OUT" $((tag % SHARDS)))
  work="$OUT/work/$tag"; rm -rf "$work"; mkdir -p "$work"
  t0=$(date +%s)
  local args=(--stage pir --jobs 1 --timeout "$QUERY_TIMEOUT" --compile-commands "$DB" --jsonl "$work/unit.jsonl" --out "$work/out")
  [ "$HARNESS" = 1 ] && args+=(--pir-harness)
  if [ -n "$EXTRA_STR" ]; then mapfile -t more <<< "$EXTRA_STR"; args+=("${more[@]}"); fi
  PRISM_MEM_BUDGET_MB=$MEM_MB timeout -k 10 "$UNIT_TIMEOUT" "$PRISM" "$unit" "${args[@]}" > "$OUT/logs/$tag.log" 2>&1
  rc=$?
  status=done
  if [ -s "$work/unit.jsonl" ] && [ $rc -ne 124 ] && [ $rc -lt 128 ]; then
    :
  else
    # no complete result for the unit: say so in the shard (never silent)
    case $rc in
      124) status=timeout;;
      137|143) status=killed;;
      *) status="crash-$rc";;
    esac
    jq -nc --arg unit "$unit" --arg st "$status" --argjson rc "$rc" --argjson mb "$MEM_MB" \
      '{schema:"prism-pir-jsonl/1", record:"unit", unit:$unit, function:null, engine:"prism-pir", verdict:"ERROR",
        level:null, outcome:(if $st=="timeout" then "timeout" elif $st=="killed" then "killed" else "crash" end),
        message:("prism process ended without a complete result (" + $st + ", exit " + ($rc|tostring) + "; PRISM_MEM_BUDGET_MB=" + ($mb|tostring) + ")"),
        assumptions:[], lean:{supported:null}}' >> "$work/unit.jsonl"
  fi
  # `unit` as the path the OS-Proving key uses: absolute, or relative to --strip-prefix; `key` = unit:start-end
  jq -c --arg abs "$unit" --arg pre "${STRIP%/}/" '.unit = (if $pre != "/" and ($abs | startswith($pre)) then $abs[($pre | length):] else $abs end)
      | .key = (if .start_line != null and .end_line != null then .unit + ":" + (.start_line | tostring) + "-" + (.end_line | tostring) else null end)' \
    "$work/unit.jsonl" > "$work/unit.keyed" && mv "$work/unit.keyed" "$work/unit.jsonl"
  # measured peak RSS of this unit (the largest per-function peak PRISM recorded), for calibration
  jq -r 'select(.peak_mb != null) | .peak_mb' "$work/unit.jsonl" 2>/dev/null | sort -n | tail -1 \
    | awk -v u="$unit" 'NF {print u "\t" $1}' >> "$OUT/peaks.tsv"
  n=$(wc -l < "$work/unit.jsonl")
  ( flock 9; cat "$work/unit.jsonl" >> "$shard" ) 9>>"$shard.lock"
  printf '%s\t%s\t%s\t%s\t%s\n' "$unit" "$status" "$n" $(( $(date +%s) - t0 )) "$(date -u +%FT%TZ)" >> "$OUT/manifest.tsv"
  rm -rf "$work"
}

reap() {
  local pid
  for pid in "${!PIDS[@]}"; do
    kill -0 "$pid" 2>/dev/null || { wait "$pid" 2>/dev/null; unset "PIDS[$pid]"; }
  done
}

# ---- memory budget and worker count ---------------------------------------
if [ "$MEM_MB" = auto ] && [ -s "$OUT/calibration.env" ]; then . "$OUT/calibration.env"; fi
if [ "$MEM_MB" = auto ]; then
  # calibration: the first units alone with a generous budget; keep their results (they are real)
  export MEM_MB=4096
  cal=0
  for unit in "${UNITS[@]}"; do
    [ "$cal" -ge "$CAL_UNITS" ] && break
    [ -n "${DONE[$unit]:-}" ] && continue
    echo "pir_batch: calibration unit $((cal + 1))/$CAL_UNITS: $unit"
    run_unit "$unit"; DONE["$unit"]=1; cal=$((cal + 1)); STARTED=$((STARTED + 1))
  done
  peak=$(awk -F'\t' '{ if ($2 > m) m = $2 } END { print m + 0 }' "$OUT/peaks.tsv" 2>/dev/null)
  [ "${peak:-0}" -gt 0 ] || peak=512
  MEM_MB=$(( peak * 3 / 2 + 256 ))
  [ "$MEM_MB" -lt 1024 ] && MEM_MB=1024   # floor: the first units are often the smallest
  printf 'MEM_MB=%s\nMEASURED_PEAK_MB=%s\n' "$MEM_MB" "$peak" > "$OUT/calibration.env"
  echo "pir_batch: measured peak RSS $peak MB over $cal units -> PRISM_MEM_BUDGET_MB=$MEM_MB"
fi
export MEM_MB
if [ "$WORKERS" = auto ]; then
  [ "$MAX_WORKERS" -gt 32 ] && MAX_WORKERS=32
  WORKERS=$(( ($(avail_mb) - MIN_AVAIL_MB) / MEM_MB ))
  [ "$WORKERS" -gt "$MAX_WORKERS" ] && WORKERS=$MAX_WORKERS
  [ "$WORKERS" -lt 1 ] && WORKERS=1
fi
echo "pir_batch: workers=$WORKERS budget=${MEM_MB}MB min-avail=${MIN_AVAIL_MB}MB"

for unit in "${UNITS[@]}"; do
  [ -n "${DONE[$unit]:-}" ] && continue
  [ "$LIMIT" -gt 0 ] && [ "$STARTED" -ge "$LIMIT" ] && break
  while :; do
    reap
    n=${#PIDS[@]}
    if [ "$n" -lt "$WORKERS" ]; then
      # every running worker may still grow to its budget; the new one too
      grow=$(( n * MEM_MB - $(running_rss_mb) ))
      [ "$grow" -lt 0 ] && grow=0
      if [ $(( $(avail_mb) - grow - MEM_MB )) -ge "$MIN_AVAIL_MB" ]; then break; fi
    fi
    sleep 3
  done
  setsid bash -c "$(declare -f run_unit); run_unit \"\$1\"" _ "$unit" \
    < /dev/null > /dev/null 2>&1 &
  PIDS[$!]=1; UNIT_OF[$!]=$unit; START_OF[$!]=$(date +%s)
  STARTED=$((STARTED + 1))
done
while [ ${#PIDS[@]} -gt 0 ]; do reap; sleep 2; done
echo "pir_batch: started $STARTED units this run; results in $OUT/shards, manifest $OUT/manifest.tsv"
summary
