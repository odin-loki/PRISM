"""python -m helix PATH [--gui] [--no-llm] [--stage bmc,lints]"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from helix.config import Config
from helix.pipeline import STAGE_ORDER, run_pipeline


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="helix",
        description="Hybrid code-testing pipeline: BMC, fuzz, LTL, Qwen 3.5 9B.",
    )
    p.add_argument("path", nargs="?", default="testdata",
                   help="file or directory of C/C++ (default: testdata)")
    p.add_argument("--out", default="helix-out")
    p.add_argument("--no-llm", action="store_true")
    p.add_argument("--gui", action="store_true")
    p.add_argument("--stage", default="", help="comma-separated stage names")
    p.add_argument("--skip", default="", help="comma-separated stage names")
    p.add_argument("--unwind", type=int, default=8)
    p.add_argument("--fuzz-budget", type=float, default=4.0)
    p.add_argument("--fuzz-iters", type=int, default=512)
    p.add_argument("--repair-rounds", type=int, default=2)
    p.add_argument("--resume", action="store_true",
                   help="reuse ok/NOTRUN stages from --out/report.json")
    p.add_argument("--list-stages", action="store_true")
    args = p.parse_args(argv)

    if args.list_stages:
        print("\n".join(STAGE_ORDER))
        return 0

    if args.gui:
        from helix.gui import launch
        return launch(args)

    cfg = Config(
        root=Path(args.path).resolve(),
        out=Path(args.out).resolve(),
        llm=not args.no_llm,
        unwind=args.unwind,
        fuzz_budget=args.fuzz_budget,
        fuzz_iters=args.fuzz_iters,
        repair_rounds=args.repair_rounds,
        stages=[s.strip() for s in args.stage.split(",") if s.strip()] or None,
        skip=[s.strip() for s in args.skip.split(",") if s.strip()],
        resume=args.resume,
    )
    report = run_pipeline(cfg)
    print(f"confidence {report.confidence}  "
          f"(vis {report.visibility} x ans {report.answer} x res {report.resolution})")
    print(f"report {cfg.out / 'report.md'}")
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
    if any(s.status == "failed" for s in report.stages):
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
