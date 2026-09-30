"""FuSeBMC LLM seeding in the Python engine. python -m unittest tests.test_concrete

The concrete oracle, the fuzzer and the no-LLM FuSeBMC rows are covered by
tests/cpp/test_concrete.cpp. What is left here mocks the Python engine's
LLM hooks (Fuzz4All seeds, ChatFuzz on stall); the C++ LlamaEngine has no
injectable backend to mock yet.
"""

from __future__ import annotations

import unittest
from pathlib import Path
from unittest.mock import patch

from prism import laws
from prism.cparse import extract_functions
from prism.fuse import run_fuse
from prism.models import Finding

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def fn(name: str):
    for p in TD.glob("*.c"):
        for f in extract_functions(p, str(p)):
            if f.name == name:
                return f, p
    raise AssertionError(name)


class TestFuseEngine(unittest.TestCase):
    def test_fuzz4all_seeds_before_fuzz(self):
        f, p = fn("saturate")
        passed_seeds: list[list[bytes] | None] = []

        class Eng:
            def available(self):
                return True

        def fake_fuzz4all(engine, fn_, nbytes):
            return [b"\x01\x00\x00\x00"]

        clean = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 0, "iters": 1, "corpus": 1},
        )

        def fake_fuzz(fn_, src, budget=0, iters=0, seeds=None):
            passed_seeds.append(seeds)
            return clean

        with patch("prism.agent.fuzz4all_seeds", side_effect=fake_fuzz4all):
            with patch("prism.fuse.fuzz_function", side_effect=fake_fuzz):
                recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=Eng())
        self.assertTrue(passed_seeds)
        self.assertIsNotNone(passed_seeds[0])
        self.assertIn(b"\x01\x00\x00\x00", passed_seeds[0])
        self.assertEqual(recs[0].extra.get("fuzz4all"), 1)
        self.assertEqual(recs[0].status, laws.CLEAN)

    def test_stall_calls_chatfuzz_once(self):
        f, p = fn("saturate")
        called = []

        class Eng:
            def available(self):
                return True

        def fake_mutants(engine, fn_, seed_hex):
            called.append((fn_.name, seed_hex))
            return [b"\x02\x00\x00\x00"]

        clean = Finding(
            stage="fuzz", status=laws.CLEAN, file=f.file, function=f.name,
            line=f.line, cls="", message="no crash (not a proof)",
            strength=laws.STRENGTH_FINDS,
            extra={"new_cov": 0, "iters": 1, "corpus": 1},
        )
        with patch("prism.fuse.fuzz_function", return_value=clean):
            with patch("prism.agent.chatfuzz_mutants", side_effect=fake_mutants):
                recs = run_fuse([f], [], p.parent, budget=0.2, iters=4, engine=Eng())
        self.assertEqual(len(called), 1, called)
        self.assertEqual(called[0][0], "saturate")
        self.assertEqual(recs[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(recs[0].status))


if __name__ == "__main__":
    unittest.main()
