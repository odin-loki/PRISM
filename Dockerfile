# syntax=docker/dockerfile:1
# Reproducible PRISM build (roadmap Part 1.3). See docs/SUPPLY_CHAIN.md.
#
#   docker build --build-arg SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) \
#                --output type=local,dest=out .
#
# Everything that feeds the binary is pinned: the base image by digest, the
# Ubuntu archive by snapshot date, LLVM by third_party/llvm/PIN.toml (tag +
# sha256, fetched by scripts/fetch_llvm.py), the linked libraries by third_party/MANIFEST.toml (checked
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
      cmake ninja-build git ca-certificates python3 xz-utils libc6-dev libstdc++-14-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

# Pinned compiler: LLVM 23.1.4 (third_party/llvm/PIN.toml). fetch_llvm.py
# refuses on a sha256 mismatch; --keep-full keeps lld and llvm-ar too.
RUN python3 /src/scripts/fetch_llvm.py --dest /opt/prism-tools --keep-full
ENV PRISM_LLVM_BIN=/opt/prism-tools/llvm-23.1.4/bin
ENV CC=${PRISM_LLVM_BIN}/clang \
    CXX=${PRISM_LLVM_BIN}/clang++ \
    AR=${PRISM_LLVM_BIN}/llvm-ar \
    RANLIB=${PRISM_LLVM_BIN}/llvm-ranlib

# Fail closed before compiling anything from third_party/: licence firewall +
# in-tree linked libraries must match the manifest (version marker and tree
# digest). prism-deps is built first from the standard library alone; it
# links no third_party/ code (PRISM_DEPS_ONLY configures only that target).
RUN cmake -S /src -B /build-deps -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=${CC} -DCMAKE_CXX_COMPILER=${CXX} \
      -DPRISM_DEPS_ONLY=ON \
 && cmake --build /build-deps --target prism-deps \
 && /build-deps/prism-deps licence-check \
 && /build-deps/prism-deps linked

# -ffile-prefix-map strips /src and the build dir from debug info, __FILE__
# and assertions; -Wl,--build-id=sha1 makes the build id a content hash;
# llvm-ar is deterministic (no timestamps/uids) by default.
# The tests find testdata/ and proofs/ from __FILE__, which the prefix map
# makes relative to /src, so prism_tests runs with /src as its directory.
ENV PRISM_REPRO_FLAGS="-ffile-prefix-map=/src=. -ffile-prefix-map=/build=build -fno-record-gcc-switches"
RUN cmake -S /src -B /build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=${CC} -DCMAKE_CXX_COMPILER=${CXX} \
      -DCMAKE_AR=${AR} -DCMAKE_RANLIB=${RANLIB} \
      -DPRISM_LLVM_PINNED_BIN=${PRISM_LLVM_BIN} \
      -DCMAKE_C_FLAGS="${PRISM_REPRO_FLAGS}" \
      -DCMAKE_CXX_FLAGS="${PRISM_REPRO_FLAGS}" \
      -DCMAKE_EXE_LINKER_FLAGS="--ld-path=${PRISM_LLVM_BIN}/ld.lld -Wl,--build-id=sha1" \
      -DCMAKE_SHARED_LINKER_FLAGS="--ld-path=${PRISM_LLVM_BIN}/ld.lld -Wl,--build-id=sha1" \
      -DPRISM_Z3=ON -DPRISM_CUDA=OFF -DPRISM_LLAMA=OFF -DPRISM_QT=OFF -DPRISM_TESTS=ON \
 && cmake --build /build \
 && (cd /src && /build/prism_tests) \
 && mkdir -p /out \
 && cp /build/prism /out/ \
 && /build-deps/prism-deps sbom --version "${PRISM_VERSION}" -o /out/prism.cdx.json \
 && touch -d "@${SOURCE_DATE_EPOCH}" /out/* \
 && (cd /out && sha256sum prism prism.cdx.json > SHA256SUMS)

# `--output type=local,dest=out` exports just the artefacts.
FROM scratch AS artefacts
COPY --from=build /out/ /
