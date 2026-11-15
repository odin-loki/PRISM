#!/usr/bin/env bash
# Share of the IR census's "UNENCODED: unknown external call" functions that the kernel specs
# resolve (OS-Proving data/clang/census/*.tsv and pilot/*.tsv, columns: file name start end status
# reason ir_instr inline_asm unknown_ext_calls unknown_names ...; unknown_names is ';'-separated).
# A function is *resolved by specs* when every unknown external callee has a spec for its kernel
# (a path containing /hbsd/ or /freebsd/ or /sys/ is FreeBSD, else Linux). The census classes are an
# approximation of PRISM (see data/clang/INDEX.md): this counts what could be encoded, not verdicts.
#   tools/pir/kernel_census_coverage.sh CENSUS.tsv [linux|freebsd|auto]
set -euo pipefail
T="${1:?census tsv}"; OSM="${2:-auto}"
HERE="$(cd "$(dirname "$0")" && pwd)"
LX="$("$HERE/kernel_spec_names.sh" linux)"; FB="$("$HERE/kernel_spec_names.sh" freebsd)"
awk -F'\t' -v lx="$LX" -v fb="$FB" -v osm="$OSM" '
BEGIN { n=split(lx,a," "); for(i=1;i<=n;i++) SL[a[i]]=1; n=split(fb,a," "); for(i=1;i<=n;i++) SF[a[i]]=1 }
NR==1 { next }
{
  st=$5; asm=$8+0; unk=$9+0; names=$10
  os=osm; if (os=="auto") os = ($1 ~ /\/hbsd\/|\/freebsd\// ) ? "freebsd" : "linux"
  total++
  nn = split(names, nm, ";"); allres=1; ndist=0; nres=0
  for (i=1;i<=nn;i++) if (nm[i]!="") { ndist++; r = (os=="linux") ? (nm[i] in SL) : (nm[i] in SF); if (r) nres++; else allres=0 }
  if (st=="encodable-candidate") { cand++ }
  else if (st=="UNENCODED:unknown-external-call") {
    unkfn++; dn+=ndist; dr+=nres
    if (allres && ndist>0) { resolved++ }
    if (asm==0) {} 
  } else if (st=="unsupported:inline-asm") {
    asmfn++
    if (allres) asm_ok++          # every unknown callee (if any) has a spec: asm havoc (R3) + specs
  }
  seen[os]++
}
END {
  printf "functions %d: encodable-candidate %d, unknown-external-call %d, inline-asm %d\n", total, cand, unkfn, asmfn
  printf "unknown-external-call functions resolved by specs (all callees specified): %d of %d = %.1f%%\n", resolved, unkfn, unkfn? 100*resolved/unkfn:0
  printf "distinct unknown callee names (summed per function) covered by specs: %d of %d = %.1f%%\n", dr, dn, dn? 100*dr/dn:0
  printf "inline-asm functions encodable with the asm model and the specs (upper bound): %d of %d = %.1f%%\n", asm_ok, asmfn, asmfn? 100*asm_ok/asmfn:0
  printf "encodable candidates before %d (%.1f%%), after specs + asm model %d (%.1f%%); with --assume-unknown-calls too: %d (%.1f%%)\n", cand, 100*cand/total, cand+resolved+asm_ok, 100*(cand+resolved+asm_ok)/total, cand+unkfn+asmfn, 100*(cand+unkfn+asmfn)/total
}' "$T"
