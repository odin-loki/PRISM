"""python -m prism PATH [--gui] [--no-llm] [--stage bmc,lints]"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from prism.config import Config
from prism.pipeline import STAGE_ORDER, run_pipeline
from prism.sarif import FAIL_ON, exit_code


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="prism",
        description=(
            "PRISM (Performance, Regression, Integration and Security Module) checks any "
            "codebase: deep C/C++ analysis (lints, compiler warnings, BMC, fuzzing, "
            "contracts) plus every other language through the polyglot stage (syntax, "
            "linters, type checkers) and a secrets/conflict-marker scan of every text "
            "file. What could not be checked is reported as NOTRUN, never as clean."
        ),
        epilog=(
            "Writes report.json, report.md and report.sarif (SARIF 2.1.0) under --out. "
            "Steps that execute scanned code need --allow-exec. "
            "--fail-on defect ignores warning/note/style-severity findings."
        ),
    )
    p.add_argument("path", nargs="?", default="testdata",
                   help="file or directory to check, any language (default: testdata)")
    p.add_argument("--out", default="prism-out")
    p.add_argument("--no-llm", action="store_true")
    p.add_argument("--gui", action="store_true")
    p.add_argument("--stage", default="", help="comma-separated stage names")
    p.add_argument("--skip", default="", help="comma-separated stage names")
    p.add_argument("--unwind", type=int, default=8)
    p.add_argument("--jobs", "-j", type=int, default=0,
                   help="ISO-style worker count for lints (0 = cpu/2)")
    p.add_argument("--fuzz-budget", type=float, default=4.0)
    p.add_argument("--fuzz-iters", type=int, default=512)
    p.add_argument("--repair-rounds", type=int, default=2)
    p.add_argument("--resume", action="store_true",
                   help="reuse ok/NOTRUN stages from --out/stages.jsonl (report.json fallback)")
    p.add_argument("--tool", action="append", default=[], metavar="NAME=PATH",
                   help="explicit adapter binary (searched before vendored/PATH)")
    p.add_argument("--allow-exec", action="store_true",
                   help="run code from the scanned tree (sanitizer/fuzz/diff harnesses, "
                        "perl -c, cargo clippy, eslint, LLM programs) in a sandbox; "
                        "only on code you trust. Without it those steps are NOTRUN")
    p.add_argument("--pbsd", default="", metavar="PATH",
                   help="ParanoidBSD tree for the pbsd stage (else PRISM_PBSD; no default). "
                        "Importing its modules also needs --allow-exec")
    p.add_argument("--list-stages", action="store_true")
    p.add_argument("--fail-on", choices=FAIL_ON, default="never",
                   help="exit 1 on: defect (any FAILED/CRASH/SANFAIL finding, except those "
                        "with extra.severity warning/note/style) or gap (defect, or anything "
                        "NOTRUN/ERROR/TIMEOUT). A crashed stage is exit 2.")
    args = p.parse_args(argv)

    if args.list_stages:
        print("\n".join(STAGE_ORDER))
        return 0

    if args.gui:
        try:
            from prism.gui import launch
        except ImportError as ex:
            print("NOTRUN gui: PySide6 not installed — not a clean window")
            print(f"  install: pip install PySide6  ({ex})")
            print("  or build prism_gui when Qt6 Widgets is present")
            return 0
        return launch(args)

    tools: dict[str, str] = {}
    for item in args.tool:
        if "=" not in item:
            p.error("--tool expects NAME=PATH")
        k, v = item.split("=", 1)
        k, v = k.strip(), v.strip()
        if not k or not v:
            p.error("--tool expects NAME=PATH")
        tools[k] = v

    cfg = Config(
        root=Path(args.path).resolve(),
        out=Path(args.out).resolve(),
        llm=not args.no_llm,
        unwind=args.unwind,
        jobs=args.jobs if args.jobs > 0 else max(1, (os.cpu_count() or 4) // 2),
        fuzz_budget=args.fuzz_budget,
        fuzz_iters=args.fuzz_iters,
        repair_rounds=args.repair_rounds,
        stages=[s.strip() for s in args.stage.split(",") if s.strip()] or None,
        skip=[s.strip() for s in args.skip.split(",") if s.strip()],
        resume=args.resume,
        tools=tools,
        allow_exec=args.allow_exec,
    )
    if args.pbsd:
        cfg.pbsd_root = Path(args.pbsd).resolve()
    report = run_pipeline(cfg)
    print(f"confidence {report.confidence}  "
          f"(vis {report.visibility} x ans {report.answer} x res {report.resolution})")
    print(f"report {cfg.out / 'report.md'}  (sarif {cfg.out / 'report.sarif'})")
    notrun = [s for s in report.stages if s.status == "NOTRUN"]
    if notrun:
        print("NOTRUN:")
        for s in notrun:
            print(f"  {s.name}: {s.detail}  {s.install}")
    failed = [f for s in report.stages for f in s.findings
              if f.status in {"FAILED", "CRASH"}]
    failed.sort(key=lambda f: (0 if f.status == "CRASH" else 1, f.stage, f.file or ""))
    for f in failed[:30]:
        print(f"  {f.status:12} {f.stage:10} {f.file}:{f.line} {f.function} {f.message}")
    return exit_code(report, args.fail_on)


if __name__ == "__main__":
    sys.exit(main())
