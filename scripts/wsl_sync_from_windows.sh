#!/usr/bin/env bash
# Copy sources from a Windows checkout into ~/PRISM (ext4) for fast builds.
# Edit on OneDrive (or any path under /mnt/c); run cmake/ninja only on ~/PRISM.
# See AGENTS.md.
set -euo pipefail

SRC="${1:-/mnt/c/Users/odinl/OneDrive/Desktop/PRISM}"
DEST="${2:-$HOME/PRISM}"

if [[ ! -f "$SRC/third_party/MANIFEST.toml" ]]; then
  echo "missing third_party/ in $SRC — need a full checkout" >&2
  exit 1
fi

mkdir -p "$DEST"
if [[ ! -d "$DEST/.git" ]]; then
  echo "initializing $DEST from $SRC (preserving .git)..." >&2
  rsync -a --exclude build-wsl --exclude build --exclude build-werror --exclude prism-out \
    "$SRC/" "$DEST/"
  if [[ -d "$SRC/.git" ]]; then
    rsync -a "$SRC/.git/" "$DEST/.git/"
  else
    echo "warning: no .git in $SRC; tests/cpp/test_deps.cpp allow-list needs a git checkout" >&2
  fi
else
  rsync -a --delete \
    --exclude .git \
    --exclude build-wsl \
    --exclude build \
    --exclude build-werror \
    --exclude prism-out \
    --exclude third_party \
    "$SRC/" "$DEST/"
fi

if [[ ! -f "$DEST/third_party/MANIFEST.toml" ]]; then
  echo "syncing third_party/ (large; one-time or when MANIFEST changes)..." >&2
  rsync -a "$SRC/third_party/" "$DEST/third_party/"
fi

if [[ -d "$SRC/.git" ]]; then
  rsync -a "$SRC/.git/" "$DEST/.git/"
fi
if ! git -C "$DEST" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "error: $DEST is not a git checkout after sync" >&2
  exit 1
fi

echo "synced → $DEST"
echo "build:  cd $DEST && scripts/wsl_build.sh"
