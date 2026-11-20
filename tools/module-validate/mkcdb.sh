#!/bin/bash
# mkcdb.sh MAKE_N_FILE BUILD_DIR SIBLING_REL_C OUT.json : a one-entry compile_commands.json for a kernel unit, taken from the text
# of `make -n` (the OS-Proving clang pass keeps it as linux-make-n.txt). prism-module clones that entry for its harness units.
set -e
mk="$1"; bd="$2"; rel="$3"; out="$4"
line=$(grep -m1 -F "CC [M]  ${rel%.c}.o" "$mk" || grep -m1 -F "CC      ${rel%.c}.o" "$mk" || true)
[ -n "$line" ] || { echo "no entry for $rel" >&2; exit 1; }
cmd=$(printf '%s' "$line" | sed -e "s/^.*; clang /clang /" -e "s/ -c -o .*\$//")
src=$(printf '%s' "$line" | sed -e "s/.* -c -o [^ ]* \([^ ]*\) .*/\1/")
cmd="$cmd -c -o /dev/null $src"
jq -n --arg d "$bd" --arg c "$cmd" --arg f "$src" '[{directory:$d, command:$c, file:$f}]' > "$out"
echo "$src"
