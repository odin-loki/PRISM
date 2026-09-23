"""Per-file subprocess stages run on cfg.jobs threads without changing output.

warnings (run_compiler), sanitize and clang-tidy fan their per-file
subprocesses out over ``prism.config.ordered_map``. Findings must be the
same list, in the same order, as a serial (jobs=1) run, even when later
files finish first.

python -m unittest tests.test_py_parallel_order
"""

from __future__ import annotations

import subprocess
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.config import Config, ordered_map


def _key(findings):
    return [(f.stage, f.status, f.file, f.function, f.line, f.cls, f.message,
             f.evidence, f.extra) for f in findings]


class TestOrderedMap(unittest.TestCase):
    def test_keeps_input_order_when_late_items_finish_first(self):
        def slow_first(x):
            time.sleep(0.02 * (5 - x))
            return x * 10
        self.assertEqual(ordered_map(slow_first, range(5), 4), [0, 10, 20, 30, 40])

    def test_serial_when_one_job(self):
        seen = []
        ordered_map(lambda x: seen.append(threading.get_ident()), range(4), 1)
        self.assertEqual(set(seen), {threading.get_ident()})

    def test_first_error_in_input_order_propagates(self):
        def boom(x):
            if x == 1:
                time.sleep(0.05)
                raise ValueError("first")
            if x == 3:
                raise KeyError("later")
            return x
        with self.assertRaisesRegex(ValueError, "first"):
            ordered_map(boom, range(5), 4)

    def test_bad_jobs_value_is_serial(self):
        self.assertEqual(ordered_map(str, [1, 2], None), ["1", "2"])
        self.assertEqual(ordered_map(str, [], 8), [])


def _files(n):
    return [Path(f"/src/u{i}.c") for i in range(n)]


class TestStagesSameOutputAnyJobs(unittest.TestCase):
    def test_warnings_identical_for_jobs_1_and_4(self):
        from prism import adapters

        def fake_run(cmd, **_k):
            name = cmd[-1]
            idx = int(Path(name).stem[1:])
            time.sleep(0.002 * (8 - idx))  # later files finish first
            if idx == 2:
                raise subprocess.TimeoutExpired(cmd, 30)
            if idx == 5:
                raise OSError("exec format error")
            # Same message from every unit: dedup keeps the first in order.
            err = f"{name}:{idx + 1}:1: warning: unused variable [-Wunused]\n" \
                  "/src/common.h:3:1: warning: shared header\n"
            return mock.Mock(returncode=1 if idx == 6 else 0, stdout="", stderr=err)

        def run(jobs):
            cfg = Config(jobs=jobs)
            with mock.patch("prism.adapters._host_compilers", return_value=["gcc", "clang"]), \
                 mock.patch("prism.adapters.subprocess.run", side_effect=fake_run):
                return adapters.run_compiler(_files(8), cfg)

        serial, parallel = run(1), run(4)
        self.assertTrue(serial)
        self.assertEqual(_key(serial), _key(parallel))
        self.assertEqual(sum(1 for f in serial if f.file == "/src/common.h"), 1)
        self.assertIn(laws.TIMEOUT, {f.status for f in serial})
        self.assertIn(laws.NOTRUN, {f.status for f in serial})

    def test_sanitize_identical_for_jobs_1_and_4(self):
        from prism import sanitize

        def fake_cr(cc, p, flags, timeout, call=None):
            idx = int(p.stem[1:])
            time.sleep(0.002 * (6 - idx))
            st = laws.FAILED if (idx + len(flags)) % 3 == 0 else laws.CLEAN
            return st, f"{flags[0]} {p.name}", f"evidence {idx}"

        def run(jobs):
            with mock.patch("prism.sanitize._find_cc", return_value="/bin/gcc"), \
                 mock.patch("prism.sanitize._probe_sanitizer",
                            side_effect=lambda _cc, flags: flags != sanitize._UB_FLAGS), \
                 mock.patch("prism.sanitize._opted_in_callable",
                            side_effect=lambda p: f"fn_{p.stem}"), \
                 mock.patch("prism.sanitize._compile_and_run", side_effect=fake_cr), \
                 mock.patch.object(Path, "is_file", return_value=True):
                return sanitize.run_sanitize(_files(6), Config(jobs=jobs, allow_exec=True))

        serial, parallel = run(1), run(4)
        self.assertEqual(_key(serial), _key(parallel))
        self.assertEqual(
            [f.extra["sanitizer"] for f in serial],
            ["asan"] * 6 + ["ubsan"] + ["tsan"] * 6,
        )
        self.assertEqual(serial[6].status, laws.NOTRUN)
        self.assertEqual([f.function for f in serial[:6]], [f"fn_u{i}" for i in range(6)])

    def test_clang_tidy_identical_for_jobs_1_and_4(self):
        from prism.adapters_extra import _run_clang_tidy

        def fake_run(cmd, timeout):
            idx = int(Path(cmd[1]).stem[1:])
            time.sleep(0.002 * (6 - idx))
            if idx == 1:
                raise subprocess.TimeoutExpired(cmd, timeout)
            text = f"{cmd[1]}:1:1: warning: w{idx}\n" if idx % 2 else ""
            return mock.Mock(returncode=0 if idx != 4 else 3, stdout=text, stderr="")

        def run(jobs):
            with mock.patch("prism.adapters_extra._run", side_effect=fake_run):
                return _run_clang_tidy("/usr/bin/clang-tidy", _files(6), Config(jobs=jobs))

        serial, parallel = run(1), run(4)
        self.assertEqual(_key(serial), _key(parallel))
        self.assertEqual(serial[0].status, laws.TIMEOUT)


if __name__ == "__main__":
    unittest.main()
