#!/usr/bin/env bash
# Reproducible WSL build for a source tree on a data disk (/mnt/d/...):
# build dir in /dev/shm (RAM), backed up to a cache dir on the data disk so a WSL
# restart (which empties /dev/shm) costs a restore, not a full Z3 rebuild.
#   scripts/wsl_build_shm.sh [target...]      (default: prism prism_tests)
# env: PRISM_BUILD_DIR (default /dev/shm/prism-kernel-build), PRISM_BUILD_BACKUP
# (default <repo>/../prism-kernel-build-backup), PRISM_JOBS (default: from MemAvailable, max 8).
# Compiler: clang 23 (FORGE needs it): PRISM_CC / PRISM_CXX override. Never builds into ~/PRISM/build-wsl. ccache is used when installed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BD="${PRISM_BUILD_DIR:-/dev/shm/prism-kernel-build}"
BK="${PRISM_BUILD_BACKUP:-$(dirname "$ROOT")/prism-kernel-build-backup}"
# jobs: floor((MemAvailable - 10 GB) / PRISM_JOB_GB), at most PRISM_MAX_JOBS (8 while other agents build)
AVAIL_GB=$(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo)
JOBS="${PRISM_JOBS:-$(( (AVAIL_GB - 10) / ${PRISM_JOB_GB:-3} ))}"
(( JOBS > ${PRISM_MAX_JOBS:-8} )) && JOBS=${PRISM_MAX_JOBS:-8}
(( JOBS < 1 )) && JOBS=1
if [[ ! -d "$BD" && -d "$BK" ]]; then
  echo "restoring build dir from $BK"; mkdir -p "$BD"; cp -a "$BK/." "$BD/"
fi
LAUNCH=()
if command -v ccache >/dev/null; then
  export CCACHE_DIR="${CCACHE_DIR:-$(dirname "$ROOT")/ccache}"
  LAUNCH=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi
cmake -S "$ROOT" -B "$BD" -G Ninja -DCMAKE_C_COMPILER="${PRISM_CC:-/home/odin/opt/llvm23/bin/clang}" -DCMAKE_CXX_COMPILER="${PRISM_CXX:-/home/odin/opt/llvm23/bin/clang++}" -DCMAKE_BUILD_TYPE=Release \
  -DPRISM_CUDA=OFF -DPRISM_QT=OFF -DPRISM_Z3=ON "${LAUNCH[@]}"
TARGETS=("$@"); [[ ${#TARGETS[@]} -eq 0 ]] && TARGETS=(prism prism_tests)
cmake --build "$BD" --target "${TARGETS[@]}" -j"$JOBS"
if [[ "${PRISM_BACKUP:-1}" == 1 ]]; then
  mkdir -p "$BK"; rsync -a --delete "$BD/" "$BK/" 2>/dev/null || cp -a "$BD/." "$BK/"
fi
echo "built in $BD"
