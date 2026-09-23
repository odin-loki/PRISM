#!/usr/bin/env bash
# Drop the fifteen mined source trees from ALL of PRISM's git history
# (roadmap Part 1.1). This rewrites every commit id. The owner runs it once,
# on a fresh mirror clone, and approves the force-push by hand.
#
#   git clone --mirror https://github.com/<owner>/PRISM.git prism-rewrite.git
#   cd prism-rewrite.git
#   bash /path/to/PRISM/scripts/rewrite_history.sh
#   # inspect, then (owner only):  git push --force --mirror origin
#
# Needs git-filter-repo (pip install git-filter-repo). Nothing is pushed by
# this script. See docs/SUPPLY_CHAIN.md "History rewrite".
set -euo pipefail

PATHS=(
  third_party/AFLplusplus/
  third_party/Frama-C/
  third_party/FuSeBMC/
  third_party/Fuzz4All/
  third_party/cbmc/
  third_party/coccinelle/
  third_party/codeql/
  third_party/cppcheck/
  third_party/dafny/
  third_party/esbmc/
  third_party/infer/
  third_party/klee/
  third_party/rapidcheck/
  third_party/semgrep/
  third_party/strix/
)

if ! git filter-repo --version >/dev/null 2>&1; then
  echo "git filter-repo is not installed (pip install git-filter-repo)" >&2
  exit 2
fi
if [ "$(git rev-parse --is-bare-repository)" != "true" ]; then
  echo "run this inside a fresh 'git clone --mirror' (bare) repository, not a working checkout" >&2
  exit 2
fi

cat <<EOF

  *** DESTRUCTIVE: REWRITES THE WHOLE HISTORY OF $(git rev-parse --absolute-git-dir) ***

  Every commit id changes. Every clone, fork, open pull request and CI cache
  keyed on a commit id becomes stale. After the (manual) force-push all
  collaborators must re-clone. These paths will be removed from every commit:

$(printf '    %s\n' "${PATHS[@]}")
  Size before:
$(git count-objects -vH | sed 's/^/    /')

EOF
read -r -p 'Type "I UNDERSTAND" to rewrite this mirror: ' answer
if [ "$answer" != "I UNDERSTAND" ]; then
  echo "aborted; nothing changed"
  exit 1
fi

args=()
for p in "${PATHS[@]}"; do args+=(--path "$p"); done
git filter-repo --force --invert-paths "${args[@]}"
git reflog expire --expire=now --all
git gc --prune=now --aggressive

cat <<EOF

  Done. Size after:
$(git count-objects -vH | sed 's/^/    /')

  Check before pushing:
    git log --all --oneline -- ${PATHS[0]} | head   # must print nothing
    git ls-tree -r --name-only HEAD third_party | cut -d/ -f2 | sort -u

  The force-push is the owner's decision and is NOT done by this script:
    git push --force --mirror origin
EOF
