#!/usr/bin/env bash
# Sync PRISM from the Windows checkout to ~/prism-run (ext4), configure, build,
# and run the full doctest suite (scripts/wsl_run_tests_only.sh).
set -euo pipefail

DEST="${PRISM_WSL_DIR:-${HOME}/prism-run}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -n "${PRISM_WIN_SRC:-}" ]]; then
    SRC="${PRISM_WIN_SRC}"
elif [[ "${SCRIPT_DIR}" == /mnt/* ]]; then
    SRC="$(cd "${SCRIPT_DIR}/.." && pwd)"
else
    SRC="/mnt/c/Users/odinl/OneDrive/Desktop/PRISM"
fi

if [[ ! -d "${SRC}" ]]; then
    echo "Windows checkout not found at ${SRC} (set PRISM_WIN_SRC)" >&2
    exit 1
fi

RSYNC_EXCLUDES=(
    --exclude=.git/
    --exclude=build/
    --exclude=build_wsl/
    --exclude=build_win/
    --exclude=build_z3test/
    --exclude=build_cxx/
    --exclude=build-*/
    --exclude=prism-out/
    --exclude=prism-out-*/
    --exclude=__pycache__/
    --exclude=.venv/
    --exclude=.pytest_cache/
)

mkdir -p "${DEST}"
rsync -a --delete "${RSYNC_EXCLUDES[@]}" "${SRC}/" "${DEST}/"

cd "${DEST}"
BUILD="${DEST}/build"
if [[ ! -f "${BUILD}/build.ninja" ]]; then
    cmake -B build -G Ninja \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DPRISM_CUDA=OFF \
        -DPRISM_LLAMA=OFF \
        -DPRISM_QT=OFF \
        -DPRISM_Z3=ON
fi
cmake --build build

exec "${DEST}/scripts/wsl_run_tests_only.sh"
