"""One-off: emit concolic C++ rows from tests/test_concolic.py."""
import re
from pathlib import Path

src = Path(__file__).resolve().parents[1] / "tests" / "test_concolic.py"
lines = src.read_text(encoding="utf-8").splitlines()
# stop before bulk libc unenc (dlopen is first with extra assertNotIn proof)
cut = next(i for i, L in enumerate(lines) if "test_dlopen_unenc" in L)
head = "\n".join(lines[:cut])
names = re.findall(r"""fn\(['"]([\w]+)['"]\)""", head)
for m in re.finditer(r'for name in \(([^)]+)\):', head):
    names.extend(re.findall(r'"([\w]+)"', m.group(1)))
print("cases before dlopen bulk:", len(names))
# also count unenc-only rows (all _unenc_bad with standard pattern)
all_text = src.read_text(encoding="utf-8")
unenc = sorted(set(re.findall(r'fn\(["\']([\w]*_unenc_bad)["\']', all_text)))
print("unenc_bad plants:", len(unenc))
