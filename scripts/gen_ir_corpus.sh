#!/usr/bin/env bash
# Lower the C/C++ IR corpus (testdata/conformance, testdata/pir) with the
# pinned LLVM (third_party/llvm/PIN.toml) the way lower_to_ir does, and
# optionally parse-check every .ll.
#   LLVM=<llvm root>       default /workspace/opt/llvm23
#   GCC_CXX=<libstdc++ include/c++/<ver> dir>   auto-detected if unset
#   SYSROOT=<sysroot>      optional (conda toolchains)
#   OUT=<dir>              default build/ir-corpus
#   PARSECHECK=<binary>    optional: run it on each .ll (nonzero = parse error)
#   JOBS=<n>               default 3
# The PRISM C++ models come first on the include path (as in lower_to_ir),
# then libstdc++. Units under */c23/* are built -std=c23, other C as c17.
# `import std;` units get the modelled std module (models/cxx/std.cppm).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LLVM=${LLVM:-/workspace/opt/llvm23}
OUT=${OUT:-$ROOT/build/ir-corpus}
JOBS=${JOBS:-3}
MODELS=$ROOT/src/prism/pir/models/cxx
if [ -z "${GCC_CXX:-}" ]; then
  GCC_CXX=$(dirname "$(dirname "$(find /usr/include/c++ /usr/lib/gcc /workspace/opt/llvm18/lib/gcc -path '*/bits/c++config.h' 2>/dev/null | sort -V | tail -1)")")
  GCC_CXX=${GCC_CXX%/*-*-*}  # strip the target dir
fi
[ -f "$GCC_CXX/vector" ] || { echo "no libstdc++ found (set GCC_CXX)" >&2; exit 3; }
TGT=$(ls -d "$GCC_CXX"/*-linux-gnu* 2>/dev/null | head -1)
if [ -z "${SYSROOT:-}" ] && [ -d /workspace/opt/llvm18/x86_64-conda-linux-gnu/sysroot ]; then
  SYSROOT=/workspace/opt/llvm18/x86_64-conda-linux-gnu/sysroot
fi
SR=${SYSROOT:+--sysroot=$SYSROOT}
CXXF="-nostdinc++ -isystem $MODELS -isystem $GCC_CXX ${TGT:+-isystem $TGT} -isystem $GCC_CXX/backward $SR"
rm -rf "$OUT"; mkdir -p "$OUT"
PCM=$OUT/std.pcm
"$LLVM/bin/clang++" $CXXF -std=c++23 -w --precompile -o "$PCM" "$MODELS/std.cppm" 2>"$OUT/std.pcm.err" || echo "std module did not build (see $OUT/std.pcm.err)" >&2
export LLVM OUT CXXF SR PCM ROOT PARSECHECK=${PARSECHECK:-}
one() {
  s=$1; n=$(echo "$s" | tr / _)
  case $s in
    *.cpp) c="$LLVM/bin/clang++ $CXXF -std=c++23"; grep -q '^[[:space:]]*import[[:space:]]\+std' "$s" && c="$c -fmodule-file=std=$PCM"
           p="function(mem2reg),coro-early,cgscc(coro-split),coro-cleanup,function(lower-switch,fix-irreducible,loop-simplify,lcssa,instnamer)";;
    */c23/*) c="$LLVM/bin/clang $SR -std=c23 -isystem $ROOT/src/prism/pir/models/libc/c23"; p="mem2reg,lower-switch,loop-simplify,lcssa,instnamer";;
    *) c="$LLVM/bin/clang $SR -std=c17"; p="mem2reg,lower-switch,loop-simplify,lcssa,instnamer";;
  esac
  f="-S -emit-llvm -O0 -Xclang -disable-O0-optnone -fno-discard-value-names -gline-tables-only -w -o $OUT/$n.0.ll"
  ok=0; timeout 60 $c $f "$s" 2>"$OUT/$n.err" && ok=1
  # pre-C++17 code (dynamic exception specs, register): retry as C++14, as lower_to_ir does
  if [ $ok = 0 ] && grep -q "ISO C++17 does not allow" "$OUT/$n.err"; then
    timeout 60 ${c/-std=c++23/-std=c++14} $f "$s" 2>"$OUT/$n.err" && ok=1
  fi
  if [ $ok = 1 ] && "$LLVM/bin/opt" -passes="$p" -S -o "$OUT/$n.ll" "$OUT/$n.0.ll" 2>>"$OUT/$n.err"; then
    rm -f "$OUT/$n.0.ll" "$OUT/$n.err"
    if [ -n "$PARSECHECK" ] && ! "$PARSECHECK" "$OUT/$n.ll" >/dev/null 2>"$OUT/$n.pc"; then echo "PARSE $s"; else rm -f "$OUT/$n.pc"; echo "OK $s"; fi
  else echo "COMPILE $s"; fi
}
export -f one
cd "$ROOT"
find testdata/conformance testdata/pir \( -name '*.c' -o -name '*.cpp' \) | sort | xargs -P "$JOBS" -I{} bash -c 'one "$@"' _ {} > "$OUT/results.txt"
echo "ok=$(grep -c ^OK "$OUT/results.txt") compile_fail=$(grep -c ^COMPILE "$OUT/results.txt") parse_fail=$(grep -c ^PARSE "$OUT/results.txt")"
