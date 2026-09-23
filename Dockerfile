# syntax=docker/dockerfile:1
# Reproducible PRISM build (roadmap Part 1.3). See docs/SUPPLY_CHAIN.md.
#
#   docker build --build-arg SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) \
#                --output type=local,dest=out .
#
# Everything that feeds the binary is pinned: the base image by digest, the
# Ubuntu archive by snapshot date (so apt resolves the same clang-18 packages
# every time), the linked libraries by third_party/MANIFEST.toml (checked
# below), and the build clock by SOURCE_DATE_EPOCH.

# ubuntu:24.04 (multi-arch index digest, resolved 2026-09-23 via
# https://mirror.gcr.io/v2/library/ubuntu/manifests/24.04). Re-resolve with:
#   docker buildx imagetools inspect ubuntu:24.04 --format '{{json .Manifest.Digest}}'
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3 AS build

# Ubuntu archive snapshot (https://snapshot.ubuntu.com): the package set as
# it was at this instant. apt still verifies the signed Release files.
ARG UBUNTU_SNAPSHOT=20260920T000000Z
ARG SOURCE_DATE_EPOCH=0
ARG PRISM_VERSION=0.0.0-dev
ENV SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH} \
    PRISM_VERSION=${PRISM_VERSION} \
    DEBIAN_FRONTEND=noninteractive \
    LC_ALL=C.UTF-8 \
    TZ=UTC

RUN sed -i \
      -e "s|http://archive.ubuntu.com/ubuntu/\?|http://snapshot.ubuntu.com/ubuntu/${UBUNTU_SNAPSHOT}/|" \
      -e "s|http://security.ubuntu.com/ubuntu/\?|http://snapshot.ubuntu.com/ubuntu/${UBUNTU_SNAPSHOT}/|" \
      /etc/apt/sources.list.d/ubuntu.sources \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      clang-18 lld-18 llvm-18 cmake ninja-build python3 git ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Pinned compiler: clang 18 from the snapshot above, by absolute path.
ENV CC=/usr/bin/clang-18 \
    CXX=/usr/bin/clang++-18 \
    AR=/usr/bin/llvm-ar-18 \
    RANLIB=/usr/bin/llvm-ranlib-18

WORKDIR /src
COPY . /src

# Fail closed before compiling anything: licence firewall + in-tree linked
# libraries must match the manifest (version marker and tree digest).
RUN python3 scripts/licence_check.py \
 && python3 scripts/fetch_deps.py --linked

# -ffile-prefix-map strips /src and the build dir from debug info, __FILE__
# and assertions; -Wl,--build-id=sha1 makes the build id a content hash;
# llvm-ar is deterministic (no timestamps/uids) by default.
ENV PRISM_REPRO_FLAGS="-ffile-prefix-map=/src=. -ffile-prefix-map=/build=build -fno-record-gcc-switches"
RUN cmake -S /src -B /build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=${CC} -DCMAKE_CXX_COMPILER=${CXX} \
      -DCMAKE_AR=${AR} -DCMAKE_RANLIB=${RANLIB} \
      -DCMAKE_C_FLAGS="${PRISM_REPRO_FLAGS}" \
      -DCMAKE_CXX_FLAGS="${PRISM_REPRO_FLAGS}" \
      -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld -Wl,--build-id=sha1" \
      -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld -Wl,--build-id=sha1" \
      -DPRISM_Z3=ON -DPRISM_CUDA=OFF -DPRISM_LLAMA=OFF -DPRISM_QT=OFF -DPRISM_TESTS=ON \
 && cmake --build /build \
 && (cd /build && ./prism_tests) \
 && mkdir -p /out \
 && cp /build/prism /build/libprism_native.so /out/ \
 && python3 scripts/sbom.py --version "${PRISM_VERSION}" -o /out/prism.cdx.json \
 && touch -d "@${SOURCE_DATE_EPOCH}" /out/* \
 && (cd /out && sha256sum prism libprism_native.so prism.cdx.json > SHA256SUMS)

# `--output type=local,dest=out` exports just the artefacts.
FROM scratch AS artefacts
COPY --from=build /out/ /
