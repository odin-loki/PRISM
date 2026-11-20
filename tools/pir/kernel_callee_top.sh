#!/usr/bin/env bash
# Top unresolved callees by call count from the OS-Proving clang pass (data/clang/<os>/calls.tsv.gz,
# functions.tsv.gz): every callee except compiler builtins and header `static inline` functions
# (a unit defines those itself), with its call count and number of distinct callers.
#   tools/pir/kernel_callee_top.sh DATA_CLANG_DIR linux|freebsd [N=150]
# Output: callee <TAB> calls <TAB> callers <TAB> spec  (spec = the callee has a spec in
# src/prism/pir/models/kernel for that kernel, 1/0).
set -euo pipefail
D="${1:?data/clang directory}"; OS="${2:?linux|freebsd}"; N="${3:-150}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SPECS="$("$HERE/kernel_spec_names.sh" "$OS")"
INL="$(mktemp)"; trap 'rm -f "$INL" ' EXIT
zcat "$D/$OS/functions.tsv.gz" | awk -F'\t' 'NR>1 && $6==1 && $7==1 {print $2}' | sort -u > "$INL"
zcat "$D/$OS/calls.tsv.gz" | awk -F'\t' -v inl="$INL" -v specs="$SPECS" '
  BEGIN { while ((getline l < inl) > 0) I[l]=1; n=split(specs, a, " "); for (i=1;i<=n;i++) S[a[i]]=1 }
  NR>1 { if ($5 ~ /^__builtin_|^llvm\./ || I[$5]) next; c[$5]+=$6; f[$5]++ }
  END { for (k in c) printf "%s\t%d\t%d\t%d\n", k, c[k], f[k], (k in S) ? 1 : 0 }' |
  sort -t$'\t' -k2,2nr | head -n "$N"
