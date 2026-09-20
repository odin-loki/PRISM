"""C++ fuse AFL source-contract. python -m unittest tests.test_prism_afl -q

Helix helix/fuse.py + helix/afl.py is law. After C++ fuse AFL lands,
stages_rest.cpp fuse_one must match: PRISM_AFL or HELIX_AFL opt-in,
extra afl_available, CLEAN is not a proof, POINTER stays NEEDS-HARNESS
(same as tests/test_fuse.py). Missing AFL in C++ fails these checks.
"""

from __future__ import annotations

import inspect
import os
import re
import unittest
from pathlib import Path
from unittest.mock import patch

from helix import laws
from helix.afl import afl_available, run_afl_fuzz
from helix.cparse import extract_functions
from helix.fuse import _fuse_one, run_fuse
from helix.models import Finding

ROOT = Path(__file__).resolve().parents[1]
STAGES_REST = ROOT / "src" / "prism" / "stages_rest.cpp"
TD = ROOT / "testdata"
_PROOF = {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//.*?$", "", src, flags=re.M)


def _brace_body(src: str, sig: str) -> str:
    i = src.find(sig)
    if i < 0:
        raise AssertionError(f"missing {sig!r} in {STAGES_REST.name}")
    brace = src.find("{", i)
    if brace < 0:
        raise AssertionError(f"no body after {sig!r}")
    depth = 0
    for j, ch in enumerate(src[brace:], brace):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return src[brace : j + 1]
    raise AssertionError(f"unbalanced braces after {sig!r}")


def _between(src: str, start: str, end: str) -> str:
    i = src.find(start)
    if i < 0:
        raise AssertionError(f"missing {start!r}")
    j = src.find(end, i)
    if j < 0:
        raise AssertionError(f"missing {end!r} after {start!r}")
    return src[i:j]


def fn(name: str):
    for p in TD.glob("*.c"):
        for f in extract_functions(p, p.name):
            if f.name == name:
                return f, p
    raise AssertionError(name)


def _mentions_afl_env(text: str) -> bool:
    return "PRISM_AFL" in text or "HELIX_AFL" in text


class TestCppFuseAflSourceContract(unittest.TestCase):
    """C++ fuse_one must grow the Helix AFL opt-in; absence fails honestly."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.src = _read(STAGES_REST)
        cls.code = _strip_comments(cls.src)
        cls.fuse_one = _brace_body(cls.src, "Finding fuse_one(")

    def test_stages_rest_mentions_prism_or_helix_afl(self):
        self.assertTrue(
            _mentions_afl_env(self.src),
            msg="stages_rest.cpp must mention PRISM_AFL or HELIX_AFL (Helix HELIX_AFL=1)",
        )
        self.assertTrue(
            _mentions_afl_env(self.fuse_one) or _mentions_afl_env(self.code),
            msg="fuse AFL env gate must live in stages_rest.cpp fuse path",
        )
        code = _strip_comments(self.fuse_one)
        self.assertTrue(
            _mentions_afl_env(code) or _mentions_afl_env(self.code),
            msg="PRISM_AFL / HELIX_AFL must not exist only as a comment",
        )

    def test_extra_afl_available(self):
        py = inspect.getsource(_fuse_one)
        self.assertIn('extra["afl_available"]', py)
        self.assertIn("afl_available", py)
        self.assertIn("HELIX_AFL", py)

        self.assertTrue(
            "afl_available" in self.fuse_one,
            msg='C++ fuse_one extra must set "afl_available" when afl-fuzz is on PATH',
        )
        self.assertTrue(
            "afl_available" in _strip_comments(self.fuse_one),
            msg="afl_available must not exist only as a comment",
        )

    def test_clean_is_not_a_proof(self):
        self.assertFalse(laws.is_proof(laws.CLEAN))
        self.assertNotIn(laws.CLEAN, _PROOF)

        afl_src = inspect.getsource(run_afl_fuzz)
        self.assertIn("laws.CLEAN", afl_src)
        self.assertIn("not a proof", afl_src)
        self.assertNotIn("laws.PROVED", afl_src)

        fuse_src = inspect.getsource(_fuse_one)
        self.assertIn("not a proof", fuse_src)
        self.assertIn("laws.CLEAN", fuse_src)

        cpp = _strip_comments(self.fuse_one)
        self.assertIn("laws::CLEAN", cpp)
        self.assertIn("not a proof", cpp)
        self.assertNotIn("laws::PROVED", cpp.split("laws::CLEAN", 1)[1][:400]
                         if "laws::CLEAN" in cpp else cpp)

    def test_pointer_still_needs_harness_in_fuse(self):
        cpp = _strip_comments(self.fuse_one)
        ptr = cpp.find('fn.kind == "POINTER"')
        self.assertGreaterEqual(ptr, 0, msg="C++ fuse_one must keep POINTER NEEDS-HARNESS")
        self.assertIn("laws::NEEDS_HARNESS", cpp)
        self.assertIn("POINTER: FuSeBMC harness would invent a buffer or pass NULL", cpp)
        ptr_arm = _between(cpp, 'fn.kind == "POINTER"', 'fn.kind == "OTHER"')
        self.assertIn("return nh(", ptr_arm)
        self.assertIn("NEEDS_HARNESS", self.fuse_one)
        self.assertNotIn('extra["engine"]', ptr_arm)
        self.assertNotIn("laws::ERROR", ptr_arm)
        after_ptr = cpp[ptr:]
        self.assertTrue(
            "return nh(" in after_ptr[:400] or "NEEDS_HARNESS" in after_ptr[:400],
            msg="POINTER branch must return NEEDS-HARNESS, not run AFL",
        )

        env_at = min(
            (i for i in (cpp.find("PRISM_AFL"), cpp.find("HELIX_AFL")) if i >= 0),
            default=-1,
        )
        if env_at >= 0:
            self.assertLess(
                ptr,
                env_at,
                msg="POINTER must stay NEEDS-HARNESS before the AFL env gate",
            )

    def test_compile_afl_harness_sanitizer_fallbacks(self):
        """Helix helix/afl.py comments: ASan+UBSan, then UBSan, then ASan, then bare."""
        from helix.afl import _compile_afl_harness

        py = inspect.getsource(_compile_afl_harness)
        self.assertIn("-fsanitize=address,undefined", py)
        self.assertIn("-fsanitize=undefined", py)
        self.assertIn("-fsanitize=address", py)
        self.assertIn("bare", py.lower())

        body = _brace_body(self.src, "compile_afl_harness(")
        code = _strip_comments(body)
        i_both = code.find('"-fsanitize=address,undefined"')
        i_ub = code.find('"-fsanitize=undefined"')
        i_as = code.find('"-fsanitize=address"')
        i_bare = code.find("run_argv(cmd, {}, 30.0)")
        self.assertGreaterEqual(i_both, 0, msg="must try -fsanitize=address,undefined first")
        self.assertGreaterEqual(i_ub, 0, msg="must fall back to -fsanitize=undefined")
        self.assertGreaterEqual(i_as, 0, msg="must fall back to -fsanitize=address")
        self.assertGreaterEqual(i_bare, 0, msg="must fall back to bare cmd")
        self.assertLess(i_both, i_ub)
        self.assertLess(i_ub, i_as)
        self.assertLess(i_as, i_bare)
        self.assertIn('"-fno-sanitize-recover=address,undefined"', code)
        self.assertIn('"-fno-sanitize-recover=undefined"', code)
        self.assertIn('"-fno-sanitize-recover=address"', code)

    def test_compile_afl_no_c_compiler_is_notrun(self):
        """compile_afl_harness {false, "no C compiler on PATH"} → caller NOTRUN."""
        from helix.afl import run_afl_fuzz as helix_run

        py = inspect.getsource(helix_run)
        self.assertIn('"no c compiler"', py)
        self.assertIn("laws.NOTRUN", py)

        compile_body = _brace_body(self.src, "compile_afl_harness(")
        compile_code = _strip_comments(compile_body)
        self.assertIn('"no C compiler on PATH"', compile_code)
        self.assertIn("return {false, \"no C compiler on PATH\"}", compile_code)
        self.assertNotIn("laws::ERROR", compile_code)
        self.assertNotIn("laws::CLEAN", compile_code)
        self.assertNotIn("laws::NOTRUN", compile_code)

        run = _brace_body(self.src, "std::optional<Finding> run_afl_fuzz(")
        run_code = _strip_comments(run)
        self.assertIn('low.find("no c compiler")', run_code)
        mapped = _between(run_code, 'low.find("no c compiler")', "return f;")
        self.assertIn("laws::NOTRUN", mapped)
        self.assertIn('afl_base(laws::NOTRUN, "", "AFL: no C compiler on PATH")', mapped)
        self.assertNotIn("laws::ERROR", mapped)
        self.assertNotIn("laws::CLEAN", mapped)
        self.assertNotIn('f.extra["engine"] = "afl"', mapped)
        after = run_code[run_code.find('low.find("no c compiler")'):]
        self.assertLess(after.find("laws::NOTRUN"), after.find("laws::ERROR"))

    def test_afl_missing_compiler_is_notrun_never_error(self):
        body = _brace_body(_read(STAGES_REST), "std::optional<Finding> run_afl_fuzz(")
        self.assertIn("AFL: no C compiler on PATH", body)
        self.assertIn("NOTRUN", body)
        self.assertIn("install gcc or clang", body)
        self.assertNotIn(
            'afl_base(laws::ERROR, "", "AFL: no C compiler on PATH")',
            body,
        )
        fuse = _brace_body(_read(STAGES_REST), "Finding fuse_one(")
        self.assertIn('extra["afl"] = "NOTRUN"', fuse)
        self.assertIn('it->second != "NOTRUN"', fuse)

    def test_afl_notrun_must_not_claim_engine_afl(self):
        body = _brace_body(_read(STAGES_REST), "std::optional<Finding> run_afl_fuzz(")
        code = _strip_comments(body)
        self.assertIn("st != laws::NOTRUN", code)
        self.assertIn('f.extra["engine"] = "afl"', code)
        engine_at = code.find('f.extra["engine"] = "afl"')
        guard = code.find("st != laws::NOTRUN")
        self.assertGreaterEqual(guard, 0)
        self.assertLess(guard, engine_at)
        fuse = _brace_body(_read(STAGES_REST), "Finding fuse_one(")
        fuse_code = _strip_comments(fuse)
        self.assertIn('extra.erase("engine")', fuse)
        self.assertIn("adapter_install(\"afl-fuzz\")", fuse)
        self.assertIn("opted_afl", fuse)
        notrun_afl = fuse_code.find('afl_last->status == laws::NOTRUN')
        self.assertGreaterEqual(notrun_afl, 0)
        arm = _between(fuse_code, "afl_last->status == laws::NOTRUN", "afl_last->status == laws::CRASH")
        self.assertIn('extra["afl"] = "NOTRUN"', arm)
        self.assertIn('extra.erase("engine")', arm)
        self.assertNotIn('extra["engine"] = "afl"', arm)
        flag = fuse_code.find('afl_flag->second == "NOTRUN"')
        self.assertGreaterEqual(flag, 0)
        flag_arm = fuse_code[flag : flag + 200]
        self.assertIn('extra.erase("engine")', flag_arm)

    def test_opted_afl_without_afl_fuzz_sets_extra_afl_notrun(self):
        """HELIX_AFL / PRISM_AFL=1 with no afl-fuzz → extra["afl"]="NOTRUN"."""
        fuse = _strip_comments(self.fuse_one)
        self.assertIn('env_flag_is_one("PRISM_AFL")', fuse)
        self.assertIn('env_flag_is_one("HELIX_AFL")', fuse)
        self.assertIn('env_flag_is_one("HELIX_LIBFUZZER")', fuse)
        self.assertIn('env_flag_is_one("PRISM_LIBFUZZER")', fuse)
        self.assertIn("opted_afl && !afl_on_path", fuse)
        arm = _between(fuse, "opted_afl && !afl_on_path", "afl_on_path && !use_afl")
        self.assertIn('extra["afl"] = "NOTRUN"', arm)
        self.assertIn('adapter_install("afl-fuzz")', arm)
        self.assertNotIn('extra["engine"] = "afl"', arm)
        self.assertNotIn("laws::CLEAN", arm)
        self.assertNotIn("laws::PROVED", arm)


class TestHelixAflContractCppMustMatch(unittest.TestCase):
    """Helix fuse/afl is the engine; C++ must match these statuses."""

    def test_afl_available_and_scalar_only(self):
        src = inspect.getsource(afl_available)
        self.assertIn("afl-fuzz", src)
        self.assertIn("afl-fuzz.exe", src)
        run = inspect.getsource(run_afl_fuzz)
        self.assertIn('fn.kind != "SCALAR"', run)
        self.assertIn('extra["engine"]', run)
        self.assertIn('"afl"', run)
        self.assertIn("timeout", run)

    def test_helix_extra_afl_available_when_on_path_env_off(self):
        f, p = fn("saturate")
        clean = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 0, "iters": 1, "corpus": 1},
        )
        env = {k: v for k, v in os.environ.items()
               if k not in {"HELIX_AFL", "HELIX_LIBFUZZER"}}
        with patch.dict(os.environ, env, clear=True):
            with patch("helix.fuse.afl_available", return_value="/fake/afl-fuzz.exe"):
                with patch("helix.fuse.fuzz_function", return_value=clean):
                    recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=None)
        self.assertTrue(recs[0].extra.get("afl_available"))
        self.assertEqual(recs[0].status, laws.CLEAN)
        self.assertIn("not a proof", recs[0].message)
        self.assertFalse(laws.is_proof(recs[0].status))
        self.assertNotIn(recs[0].extra.get("engine"), {"afl"})

    def test_helix_clean_afl_finding_is_not_a_proof(self):
        f, _ = fn("saturate")
        rec = Finding(
            stage="fuse", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no AFL crash in 2s (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"engine": "afl"},
        )
        self.assertEqual(rec.status, laws.CLEAN)
        self.assertIn("not a proof", rec.message)
        self.assertFalse(laws.is_proof(rec.status))
        self.assertNotIn(rec.status, _PROOF)
        self.assertNotEqual(rec.status, laws.PROVED)

    def test_pointer_needs_harness_not_error_helix(self):
        """Same contract as tests.test_fuse.TestFuseLlm.test_pointer_needs_harness_not_error."""
        f, p = fn("null_branch")
        self.assertEqual(f.kind, "POINTER")
        recs = run_fuse([f], [], p.parent, budget=0.1, iters=1, engine=None)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS, recs[0].message)
        self.assertNotEqual(recs[0].status, laws.ERROR)
        self.assertIn("POINTER", recs[0].message)
        self.assertFalse(laws.is_proof(recs[0].status))
        self.assertNotEqual(recs[0].status, laws.CLEAN)
        self.assertFalse("afl_available" in (recs[0].extra or {}))
        self.assertNotEqual((recs[0].extra or {}).get("engine"), "afl")

        env = {**os.environ, "HELIX_AFL": "1", "HELIX_LIBFUZZER": "1"}
        with patch.dict(os.environ, env, clear=True):
            recs3 = run_fuse([f], [], p.parent, budget=0.1, iters=1, engine=None)
        self.assertEqual(recs3[0].status, laws.NEEDS_HARNESS)
        self.assertNotEqual(recs3[0].status, laws.ERROR)
        self.assertNotEqual((recs3[0].extra or {}).get("engine"), "afl")
        self.assertNotEqual((recs3[0].extra or {}).get("engine"), "libfuzzer")


class TestCppLibfuzzerFuseSource(unittest.TestCase):
    """C++ fuse_one must match Helix opted HELIX_LIBFUZZER: NOTRUN never engine=libfuzzer."""

    def test_opted_libfuzzer_notrun_erases_engine(self):
        fuse = _strip_comments(_brace_body(_read(STAGES_REST), "Finding fuse_one("))
        self.assertIn('env_flag_is_one("HELIX_LIBFUZZER")', fuse)
        self.assertIn('opted_libfuzzer && fn.kind == "SCALAR"', fuse)
        arm = _between(fuse, "lf_last.status == laws::NOTRUN", "lf_last.status == laws::NEEDS_HARNESS")
        self.assertIn('extra["libfuzzer"] = "NOTRUN"', arm)
        self.assertIn('eng->second == "libfuzzer"', arm)
        self.assertIn('extra.erase(eng)', arm)
        self.assertNotIn('extra["engine"] = "libfuzzer"', arm)
        self.assertNotIn("laws::CLEAN", arm)
        self.assertNotIn("laws::PROVED", arm)

    def test_run_libfuzzer_missing_clang_is_notrun_never_engine(self):
        run = _strip_comments(_brace_body(_read(STAGES_REST), "Finding run_libfuzzer("))
        self.assertIn("clang not on PATH", run)
        self.assertIn("laws::NOTRUN", run)
        self.assertIn("st != laws::NOTRUN && st != laws::NEEDS_HARNESS", run)
        self.assertIn('f.extra["engine"] = "libfuzzer"', run)
        guard = run.find("st != laws::NOTRUN && st != laws::NEEDS_HARNESS")
        engine_at = run.find('f.extra["engine"] = "libfuzzer"')
        self.assertGreaterEqual(guard, 0)
        self.assertLess(guard, engine_at)
        no_clang = _between(run, "if (!clang)", "auto td")
        self.assertIn("laws::NOTRUN", no_clang)
        self.assertIn("clang not on PATH", no_clang)
        self.assertNotIn("laws::ERROR", no_clang)
        self.assertNotIn("laws::CLEAN", no_clang)
        self.assertNotIn('f.extra["engine"] = "libfuzzer"', no_clang)


if __name__ == "__main__":
    unittest.main()
