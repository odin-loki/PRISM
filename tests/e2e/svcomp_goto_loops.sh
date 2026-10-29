#!/usr/bin/env bash
# End-to-end: goto-built loops (cfa-reducer shape). The pinned true task must
# answer true; a mutant with a wrong closed form must answer false via replay.
# Usage: tests/e2e/svcomp_goto_loops.sh PRISM_BINARY
set -euo pipefail
prism=${1:?prism binary}
root=$(cd "$(dirname "$0")/../.." && pwd)
sv=$root/testdata/conformance/sv-comp
prp=$sv/properties/unreach-call.prp
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
cd "$t"
res() { "$prism" svcomp --allow-exec --prop "$prp" "$1" 2>&1 | sed -n 's/^PRISM-SVCOMP-RESULT: //p'; }
ok=$(res "$sv/loop-new/gauss_sum.i.p+cfa-reducer.c")
sed '44s/(main__n + 1)/(main__n - 1)/' "$sv/loop-new/gauss_sum.i.p+cfa-reducer.c" > bad.c
grep -q '(main__n - 1)' bad.c
bad=$(res bad.c)
echo "true task: $ok; mutant: $bad"
[ "$ok" = true ] && [ "$bad" = "false(unreach-call)" ]
