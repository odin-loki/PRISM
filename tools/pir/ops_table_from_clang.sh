#!/usr/bin/env bash
# ops_table_from_clang.sh OPS.tsv[.gz] [FILE_REGEX] > ops-table.tsv
#
# Converts the clang pass's ops data (OS-Proving data/clang/linux/ops.tsv.gz, or freebsd/ops.tsv.gz:
# tu, table_var, table_type, file, line, field, function) into the --ops-table file read by
# `prism --stage pir --ops-table FILE` (docs/PIR.md "Indirect calls through ops tables"):
#
#   struct_type<TAB>field_name<TAB>target1,target2,...<TAB>role<TAB>field_index
#
# struct_type is the IR spelling of the struct ("%struct.e1000_mac_operations": `struct X` and
# `const struct X[N]` become %struct.X). Rows are grouped per (struct_type, field_name); targets are
# the distinct function names that initialise that field, in first-seen order. role is the field
# name (the clang data does not decide roles). field_index is left empty: the clang data carries no
# field positions, so the pir stage numbers the fields of a struct in the order they are listed
# in this file (docs/PIR.md). Rows without a field name (positional initialisers), without a
# function, or whose table type is not a plain struct are dropped and counted on stderr.
#
# FILE_REGEX (optional, awk regex on the file column) keeps only one subsystem, e.g.
#   ops_table_from_clang.sh ops.tsv.gz '/drivers/net/ethernet/intel/e1000e/' > e1000e.ops.tsv
set -euo pipefail
IN="${1:?usage: ops_table_from_clang.sh OPS.tsv[.gz] [FILE_REGEX] > ops-table.tsv}"
RX="${2:-.}"
cat_in() { case "$IN" in *.gz) gzip -dc "$IN";; *) cat "$IN";; esac; }
cat_in | awk -F'\t' -v rx="$RX" '
BEGIN { OFS = "\t"; dropped = 0; n = 0 }
NR == 1 && $1 == "tu" { next }
{
    file = $4; ty = $3; field = $6; fn = $7
    if (file !~ rx || field == "" || fn == "") next
    if (field !~ /^[A-Za-z_][A-Za-z0-9_]*$/ || fn !~ /^[A-Za-z_][A-Za-z0-9_]*$/) { dropped++; next }
    sub(/\[[0-9]*\]$/, "", ty)          # a table of structs: the element type
    sub(/^const /, "", ty)
    if (ty !~ /^struct [A-Za-z_][A-Za-z0-9_]*$/) { dropped++; next }
    sub(/^struct /, "%struct.", ty)
    key = ty SUBSEP field
    if (!(key in seen)) { seen[key] = 1; order[++n] = key; ks[key] = ty; kf[key] = field }
    tk = key SUBSEP fn
    if (!(tk in tseen)) { tseen[tk] = 1; tl[key] = (key in tl) ? tl[key] "," fn : fn }
}
END {
    for (i = 1; i <= n; i++) {
        key = order[i]
        print ks[key], kf[key], tl[key], kf[key], ""
    }
    printf "ops_table_from_clang: %d fields kept, %d rows dropped\n", n, dropped > "/dev/stderr"
}'
