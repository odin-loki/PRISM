#!/usr/bin/env bash
# Build and run the C++ self-fuzz target (roadmap 6.2; docs/FUZZ_SELF.md).
#
#   tools/fuzz_self/run_cpp.sh [BUILD_DIR] [SECONDS]
#
# Configures BUILD_DIR with -DPRISM_FUZZ=ON (clang, no Z3: the parsers under
# test do not need it) and runs the CMake target fuzz-self-run: the seed
# corpus (prism_fuzz_corpus) and one libFuzzer campaign in fork mode.
# Exit 3 (NOTRUN) when the compiler has no libFuzzer.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$repo/build-fuzz}"
seconds="${2:-300}"
cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DPRISM_CUDA=OFF -DPRISM_LLAMA=OFF -DPRISM_QT=OFF -DPRISM_Z3=OFF -DPRISM_TESTS=OFF -DPRISM_FUZZ=ON \
  -DPRISM_FUZZ_SECONDS="$seconds"
if ! grep -q '^PRISM_HAS_LIBFUZZER:INTERNAL=1' "$build/CMakeCache.txt"; then
  echo "prism_fuzz_self cannot be built (compiler without -fsanitize=fuzzer): NOTRUN" >&2
  exit 3
fi
# The report seeds come from a PRISM run on testdata/ (PRISM_BIN, else the prism next to the tools).
cmake --build "$build" --target prism
cmake --build "$build" --target fuzz-self-run
ls -l "$build/fuzz-artifacts/"
