"""bmc: SV-COMP's error function and __VERIFIER_assert (docs/SVCOMP.md).

- A reachable `reach_error()` / `__VERIFIER_error()` call is a FAILED
  FUNC-CONTRACT with the check name `reach_error` (as in pir), not the silent
  end of a path; an unreachable one lets the proof cover unreach-call. A
  static definition of the error function is never inlined over the call.
  `abort()` and `__assert_fail()` still only end the path.
- A canonical `__VERIFIER_assert(int cond)` (`if (!cond) { reach_error(); }`)
  is checked as `assert((int)(E))` at each call statement; call sites with
  no definition in the unit (extern only) use the same rewrite; any other
  definition stays unmodelled.

Both engines: the Python engine directly, the C++ engine through PRISM_BIN
(or build/prism) on the same files (tests/cpp/test_main.cpp has the same
cases as doctests).
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from prism import laws
from prism.bmc import HAS_Z3, run_bmc
from prism.cparse import extract_functions

from tests.test_bmc_goto_shift import _cpp_bmc, _cpp_prism

DEFS = "extern void abort(void);\nvoid reach_error() { abort(); }\nextern int __VERIFIER_nondet_int(void);\n"
VASSERT = ("void __VERIFIER_assert(int cond) {\n  if (!(cond)) {\n    ERROR: {reach_error();abort();}\n"
           "  }\n  return;\n}\n")

CASES: dict[str, tuple[str, set[str], str]] = {
    # name: (source, main's statuses, class when FAILED); a direct abort() is
    # never FAILED (the Python engine calls it an unmodelled libc effect)
    "reach_bad": (DEFS + "int main(void) {\n  int x = __VERIFIER_nondet_int();\n  if (x > 10 && x < 20) {\n"
                  "    if (x == 15) reach_error();\n  }\n  return 0;\n}\n", {laws.FAILED}, "FUNC-CONTRACT"),
    "reach_ok": (DEFS + "int main(void) {\n  int x = __VERIFIER_nondet_int();\n  int i = 0;\n"
                 "  if (x > 10 && x < 20) {\n    if (x == 25) reach_error();\n  }\n"
                 "  while (i < 5) { i = i + 1; }\n  if (i != 5) reach_error();\n  return 0;\n}\n",
                 {laws.PROVED, laws.PROVED_UNBOUNDED}, ""),
    "reach_old": ("extern void __VERIFIER_error(void);\nextern int __VERIFIER_nondet_int(void);\n"
                  "int main(void) {\n  int x = __VERIFIER_nondet_int();\n  if (x == 3) { __VERIFIER_error(); }\n"
                  "  return 0;\n}\n", {laws.FAILED}, "FUNC-CONTRACT"),
    "reach_static": ("static void reach_error(void) {}\nextern int __VERIFIER_nondet_int(void);\n"
                     "int main(void) {\n  int x = __VERIFIER_nondet_int();\n  if (x == 3) reach_error();\n"
                     "  return 0;\n}\n", {laws.FAILED}, "FUNC-CONTRACT"),
    "reach_abort": ("extern void abort(void);\nextern int __VERIFIER_nondet_int(void);\n"
                    "int main(void) {\n  int x = __VERIFIER_nondet_int();\n  if (x == 3) abort();\n"
                    "  return 0;\n}\n", {laws.PROVED, laws.PROVED_UNBOUNDED, laws.NEEDS_HARNESS}, ""),
    "vassert_ok": (DEFS + VASSERT + "int main(void) {\n  int x = __VERIFIER_nondet_int();\n  int i = 0;\n"
                   "  while (i < 5) { i = i + 1; }\n  __VERIFIER_assert (i == 5);\n  return 0;\n}\n",
                   {laws.PROVED, laws.PROVED_UNBOUNDED}, ""),
    "vassert_bad": (DEFS + VASSERT + "int main(void) {\n  int x = __VERIFIER_nondet_int();\n"
                    "  __VERIFIER_assert(x != 5);\n  return 0;\n}\n", {laws.FAILED}, "FUNC-CONTRACT"),
    "vassert_other": (DEFS + "int hits;\nvoid __VERIFIER_assert(int cond) {\n  if (!(cond)) {\n    hits++;\n"
                      "  }\n  return;\n}\nint main(void) {\n  int x = __VERIFIER_nondet_int();\n"
                      "  __VERIFIER_assert(x != 5);\n  return 0;\n}\n", {laws.NEEDS_HARNESS}, ""),
    "vassert_extern": (DEFS + "extern void __VERIFIER_assert(int cond);\nint main(void) {\n"
                       "  int x = __VERIFIER_nondet_int();\n  __VERIFIER_assert(x == x);\n  return 0;\n}\n",
                       {laws.PROVED, laws.PROVED_UNBOUNDED}, ""),
}


def _write(d: Path, name: str) -> Path:
    p = d / name / f"{name}.c"
    p.parent.mkdir()
    p.write_text(CASES[name][0], encoding="utf-8")
    return p


@unittest.skipUnless(HAS_Z3, "z3 not installed")
class TestReachErrorPython(unittest.TestCase):
    def test_cases(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            for name, (_src, want, cls) in CASES.items():
                p = _write(Path(d), name)
                rows = {f.function: f for f in run_bmc(extract_functions(p, p.name), 8)}
                main = rows["main"]
                self.assertIn(main.status, want, f"{name}: {main.message}")
                if main.status == laws.FAILED:
                    self.assertEqual(main.cls, cls, name)
                    want_prop = "assert" if name.startswith("vassert") else "reach_error"
                    self.assertTrue(main.message.startswith(want_prop + ":"), f"{name}: {main.message}")


@unittest.skipUnless(_cpp_prism() is not None, "C++ prism binary not built")
class TestReachErrorParity(unittest.TestCase):
    def test_same_verdicts(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            for name, (_src, want, cls) in CASES.items():
                p = _write(Path(d), name)
                main = _cpp_bmc(p.parent)["main"]
                self.assertIn(main["status"], want, f"{name}: {main.get('message')}")
                if main["status"] == laws.FAILED:
                    self.assertEqual(main["cls"], cls, name)
                    want_prop = "assert" if name.startswith("vassert") else "reach_error"
                    self.assertTrue(main["message"].startswith(want_prop + ":"), name)


if __name__ == "__main__":
    unittest.main()
