#!/usr/bin/env bash
# Native WSL build on ext4 (e.g. ~/PRISM/build-wsl). Never run cmake/ninja on
# OneDrive (/mnt/c/.../OneDrive/...); sync first: scripts/wsl_sync_from_windows.sh
# See AGENTS.md.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ "$ROOT" == /mnt/* ]]; then
  echo "refusing to build under DrvFS ($ROOT); use a clone in \$HOME/PRISM" >&2
  exit 1
fi
if [[ ! -f "$ROOT/third_party/MANIFEST.toml" ]]; then
  echo "missing third_party/ — use a full git checkout (not a partial copy)" >&2
  exit 1
fi
BD="${1:-build-wsl}"
cmake -B "$BD" -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DPRISM_CUDA=OFF -DPRISM_QT=OFF -DPRISM_Z3=ON
cmake --build "$BD" --target prism_tests -j"$(nproc)"
echo "run: $BD/prism_tests"
