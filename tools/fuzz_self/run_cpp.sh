#!/usr/bin/env bash
# Build and run the C++ self-fuzz target (roadmap 6.2; docs/FUZZ_SELF.md).
#
#   tools/fuzz_self/run_cpp.sh [BUILD_DIR] [SECONDS]
#
# Configures BUILD_DIR with -DPRISM_FUZZ=ON (clang, no Z3: the parsers under
# test do not need it), builds prism_fuzz_self, makes a seed corpus with
# tools/fuzz_self/make_corpus.py and runs libFuzzer in fork mode, keeping
# going after timeouts and crashes so one finding does not hide the next.
# Artifacts (crash-*, timeout-*, oom-*) land in BUILD_DIR/fuzz-artifacts/.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$repo/build-fuzz}"
seconds="${2:-300}"
cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DPRISM_CUDA=OFF -DPRISM_LLAMA=OFF -DPRISM_QT=OFF -DPRISM_Z3=OFF -DPRISM_TESTS=OFF -DPRISM_FUZZ=ON
cmake --build "$build" --target prism_fuzz_self
if [ ! -x "$build/prism_fuzz_self" ]; then
  echo "prism_fuzz_self was not built (compiler without -fsanitize=fuzzer): NOTRUN" >&2
  exit 3
fi
python3 "$repo/tools/fuzz_self/make_corpus.py" "$build/fuzz-seeds"
mkdir -p "$build/fuzz-corpus" "$build/fuzz-artifacts"
cp -n "$build/fuzz-seeds/libfuzzer/"* "$build/fuzz-corpus/" 2>/dev/null || true
"$build/prism_fuzz_self" -fork=1 -ignore_timeouts=1 -ignore_ooms=1 -ignore_crashes=1 \
  -max_total_time="$seconds" -timeout=25 -rss_limit_mb=2048 -max_len=65536 \
  -artifact_prefix="$build/fuzz-artifacts/" "$build/fuzz-corpus"
ls -l "$build/fuzz-artifacts/"
