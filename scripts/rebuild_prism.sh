#!/usr/bin/env bash
set -euo pipefail
# WSL ninja tree lives on ext4. Do not cmake --build on /mnt/c (OneDrive).
BUILD=/var/tmp/prism-wsl
ROOT='/mnt/c/Users/odinl/OneDrive/Desktop/Code Analysis'
if [[ ! -f "${BUILD}/build.ninja" ]]; then
  echo "missing ${BUILD}/build.ninja — run scripts/wsl_configure_build.py" >&2
  exit 1
fi
export CMAKE_SKIP_PACKAGE_REGISTRY=ON
# Skip CMake reconfigure (it would walk huge mined third_party trees on /mnt/c).
if [[ -f "${BUILD}/CMakeFiles/cmake.check_cache" ]]; then
  touch "${BUILD}/CMakeFiles/cmake.check_cache" "${BUILD}/CMakeFiles/generate.stamp" || true
fi
ninja -C "${BUILD}" -j 8 prism prism_tests
mkdir -p "${ROOT}/build_wsl"
cp -f "${BUILD}/prism" "${ROOT}/build_wsl/prism"
cp -f "${BUILD}/prism_tests" "${ROOT}/build_wsl/prism_tests"
if [[ -f "${BUILD}/prism_gui" ]]; then
  cp -f "${BUILD}/prism_gui" "${ROOT}/build_wsl/prism_gui"
fi
"${BUILD}/prism_tests"
