"""One command. Stages that cannot run write NOTRUN, never look clean."""

from __future__ import annotations

from pathlib import Path
import json
import time

from helix import confidence, laws
from helix.adapters import run_compiler, run_cppcheck, run_dafny, run_esbmc
from helix.adapters_extra import run_optional_tools
from helix.agent import dafny_specs, hypothesize, interpreter_loop, rlef_repair
from helix.ai import LlamaEngine
from helix.checkers import run_lints
from helix.config import Config
from helix.contracts import prove_contracts
from helix.cparse import TU_EXTS, extract_functions, iter_sources
from helix.diff import run_diff
from helix.fuse import run_fuse
from helix.harness import run_harness_bmc
from helix.interval import run_interval
from helix.ltl import run_ltl
from helix.bmc import run_bmc
from helix.inline import inline_static
from helix.concolic import run_concolic
from helix.models import Finding, FunctionInfo, RunReport, StageResult
from helix.muttest import run_muttest
from helix.pbsd import run_pbsd_lints
from helix.rapid import run_rapid
from helix.sanitize import run_sanitize
from helix.taint import run_taint
from helix.thread import run_thread
from helix.taxonomy import coverage_from_report
from helix import journal


STAGE_ORDER = [
    "inventory",
    "classify",
    "lints",
    "taint",
    "thread",
    "interval",
    "warnings",
    "cppcheck",
    "pbsd",
    "sanitize",
    "optional",
    "esbmc",
    "dafny",
    "contracts",
    "bmc",
    "harness",
    "concolic",
    "fuzz",
    "diff",
    "rapid",
    "muttest",
    "ltl",
    "llm",
    "execute",
    "repair",
    "unify",
]


class Pipeline:
    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self.engine: LlamaEngine | None = None
        self.report = RunReport(root=str(cfg.root))
        self._resume: dict[str, StageResult] = {}

    def _engine(self) -> LlamaEngine:
        if self.engine is None:
            self.engine = LlamaEngine(self.cfg)
        return self.engine

    def _emit(self, rec: StageResult) -> StageResult:
        self.report.stages.append(rec)
        journal.append_stage(self.cfg.out, rec)
        return rec

    def _stage(self, name: str, fn) -> StageResult:
        if not self.cfg.want(name):
            return self._emit(StageResult(
                name=name, status="skipped",
                detail="excluded by --stage/--skip",
            ))
        prev = self._resume.get(name)
        if prev is not None and prev.status in {"ok", "NOTRUN"}:
            return self._emit(prev)
        t0 = time.time()
        try:
            findings = fn() or []
        except Exception as ex:
            return self._emit(StageResult(
                name=name, status="failed", detail=str(ex),
                started=t0, elapsed=time.time() - t0,
            ))
        status = "ok"
        install = ""
        detail = ""
        if findings and all(f.status == laws.NOTRUN for f in findings):
            status = "NOTRUN"
            installs = [
                (f.extra or {}).get("install", "")
                for f in findings if (f.extra or {}).get("install")
            ]
            install = "; ".join(dict.fromkeys(installs))
            detail = "; ".join(f"{f.stage}: {f.message}" for f in findings[:12])
        return self._emit(StageResult(
            name=name, status=status, detail=detail, started=t0,
            elapsed=time.time() - t0, findings=findings,
            records=len(findings), install=install,
        ))

    def run(self) -> RunReport:
        cfg = self.cfg
        root = cfg.root
        cfg.out.mkdir(parents=True, exist_ok=True)
        if not cfg.resume:
            journal.reset(cfg.out)
        if cfg.resume:
            old = RunReport.load(cfg.out / "report.json")
            if old is not None:
                self._resume = {s.name: s for s in old.stages}
                self.report.functions = list(old.functions)
                self.report.notes.append(f"resumed from {cfg.out / 'report.json'}")
        sources = iter_sources(root)
        functions: list[FunctionInfo] = list(self.report.functions)

        def inventory() -> list[Finding]:
            out: list[Finding] = []
            for p in sources:
                try:
                    rel = str(p.relative_to(root)) if root.is_dir() else p.name
                except ValueError:
                    rel = p.name
                fns = extract_functions(p, rel)
                if not fns:
                    if p.suffix.lower() in TU_EXTS:
                        out.append(Finding(
                            stage="inventory", status=laws.ERROR, file=rel,
                            function=None, line=None, cls="EMPTY-TU",
                            message="no functions parsed (not a clean unit)",
                            strength=laws.STRENGTH_FINDS,
                        ))
                    continue
                out.append(Finding(
                    stage="inventory", status=laws.CLEAN, file=rel,
                    function=None, line=None, cls="",
                    message="translation unit", strength=laws.STRENGTH_FINDS,
                ))
            return out

        self._stage("inventory", inventory)

        def classify() -> list[Finding]:
            nonlocal functions
            functions = []
            out = []
            for p in sources:
                try:
                    rel = str(p.relative_to(root)) if root.is_dir() else p.name
                except ValueError:
                    rel = p.name
                fns = extract_functions(p, rel)
                functions.extend(fns)
                for fn in fns:
                    out.append(Finding(
                        stage="classify", status=fn.kind, file=rel, function=fn.name,
                        line=fn.line, cls=fn.kind, message=fn.signature,
                        strength=laws.STRENGTH_FINDS, extra={"static": fn.static},
                    ))
            self.report.functions = functions
            return out

        self._stage("classify", classify)

        def lints() -> list[Finding]:
            return run_lints(sources, root if root.is_dir() else root.parent)

        self._stage("lints", lints)
        self._stage("taint", lambda: run_taint(functions))
        self._stage("thread", lambda: run_thread(functions))
        self._stage("interval", lambda: run_interval(functions))
        self._stage("warnings", lambda: run_compiler(sources, cfg))
        self._stage("cppcheck", lambda: run_cppcheck(sources, cfg))
        self._stage("pbsd", lambda: run_pbsd_lints(sources, cfg))
        self._stage("sanitize", lambda: run_sanitize(sources, cfg))
        self._stage("optional", lambda: run_optional_tools(sources, cfg))
        self._stage("esbmc", lambda: run_esbmc(sources, cfg))
        self._stage("dafny", lambda: run_dafny(sources, cfg))

        def contracts() -> list[Finding]:
            out = prove_contracts(functions, cfg.unwind)
            if cfg.llm:
                out.extend(dafny_specs(
                    self._engine(),
                    [f for f in functions if f.kind in {"SCALAR", "VOID"}],
                    3,
                ))
            return out

        self._stage("contracts", contracts)

        def bmc() -> list[Finding]:
            return run_bmc(inline_static(functions), cfg.unwind)

        bmc_rec = self._stage("bmc", bmc)
        self._stage("harness", lambda: run_harness_bmc(functions, cfg.unwind))
        self._stage("concolic", lambda: run_concolic(functions, budget=32))

        def fuzz() -> list[Finding]:
            src_root = root if root.is_dir() else root.parent
            return run_fuse(
                functions, bmc_rec.findings, src_root,
                budget=cfg.fuzz_budget, iters=cfg.fuzz_iters,
                engine=self._engine() if cfg.llm else None,
            )

        self._stage("fuzz", fuzz)
        self._stage("diff", lambda: run_diff(
            functions, root if root.is_dir() else root.parent,
        ))
        self._stage("rapid", lambda: run_rapid(functions, trials=64))
        self._stage("muttest", lambda: run_muttest(functions, trials=32))

        def ltl() -> list[Finding]:
            base = root if root.is_dir() else root.parent
            specs = sorted({*base.glob("*.ltl"), *base.glob("**/*.ltl")})
            return run_ltl(functions, specs)

        self._stage("ltl", ltl)

        def llm() -> list[Finding]:
            if not cfg.llm:
                return [Finding(stage="llm", status=laws.NOTRUN, file="", function=None,
                                line=None, cls="", message="--no-llm",
                                strength=laws.STRENGTH_READS)]
            return hypothesize(self._engine(), functions, budget=4)

        self._stage("llm", llm)

        def execute() -> list[Finding]:
            from helix.concrete import execute as cexec
            fails = [
                f for s in self.report.stages for f in s.findings
                if f.status in {laws.FAILED, laws.CRASH} and f.function
                and "=" in (f.counterexample or "")
            ]
            fails.sort(key=lambda f: (0 if f.status == laws.CRASH else 1, f.stage))
            out: list[Finding] = []
            for f0 in fails[:16]:
                fn = next(
                    (x for x in functions
                     if x.name == f0.function and (x.file == f0.file or Path(x.file).name == Path(f0.file).name)),
                    None,
                )
                if not fn or fn.kind == "POINTER":
                    continue
                args: dict[str, int] = {}
                blob = f0.counterexample or ""
                for part in blob.replace(";", ",").split(","):
                    if "=" not in part:
                        continue
                    k, v = part.split("=", 1)
                    k = k.strip().split()[-1]
                    try:
                        args[k] = int(str(v).strip().split()[0], 0)
                    except ValueError:
                        continue
                if not args:
                    continue
                rec = cexec(fn, args)
                if rec.ub:
                    out.append(Finding(
                        stage="execute", status=laws.CRASH, file=fn.file,
                        function=fn.name, line=fn.line, cls=rec.ub,
                        message=f"cex replay trapped {rec.ub}",
                        strength=laws.STRENGTH_FINDS,
                        counterexample=blob[:200],
                        extra={"oracle": "concrete-replay"},
                    ))
                else:
                    out.append(Finding(
                        stage="execute", status=laws.CLEAN, file=fn.file,
                        function=fn.name, line=fn.line, cls="",
                        message="cex did not trap in concrete replay (not a proof)",
                        strength=laws.STRENGTH_FINDS,
                        extra={"oracle": "concrete-replay"},
                    ))
            if cfg.llm and fails:
                f0 = fails[0]
                prompt = (
                    f"Write a C main() that demonstrates this finding is real or not.\n"
                    f"{f0.file}:{f0.line} {f0.function} {f0.cls}: {f0.message}\n"
                    f"counterexample: {f0.counterexample}"
                )
                out.extend(interpreter_loop(self._engine(), prompt, rounds=2))
            if not out:
                return [Finding(
                    stage="execute", status=laws.NOTRUN, file="", function=None,
                    line=None, cls="", message="no FAILED/CRASH cex to replay",
                    strength=laws.STRENGTH_READS,
                )]
            return out

        self._stage("execute", execute)

        def repair() -> list[Finding]:
            if not cfg.llm:
                return [Finding(stage="repair", status=laws.NOTRUN, file="", function=None,
                                line=None, cls="", message="--no-llm",
                                strength=laws.STRENGTH_READS)]
            fails = [f for s in self.report.stages for f in s.findings
                     if f.status in {laws.FAILED, laws.CRASH} and f.file]
            if not fails:
                return [Finding(stage="repair", status=laws.NOTRUN, file="", function=None,
                                line=None, cls="", message="nothing to repair",
                                strength=laws.STRENGTH_READS)]
            f0 = fails[0]
            src_path = Path(f0.file)
            if not src_path.exists():
                src_path = (root / f0.file) if root.is_dir() else root
            try:
                src = src_path.read_text(encoding="utf-8", errors="replace")
            except OSError as ex:
                return [Finding(stage="repair", status=laws.ERROR, file=str(src_path),
                                function=None, line=None, cls="", message=str(ex),
                                strength=laws.STRENGTH_READS)]
            fb = f"{f0.stage} {f0.status} {f0.cls} {f0.message} {f0.counterexample}"
            return rlef_repair(self._engine(), src, fb, cfg.repair_rounds)

        self._stage("repair", repair)

        def unify() -> list[Finding]:
            rows = coverage_from_report(self.report)
            (cfg.out / "taxonomy.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")
            gaps = [r for r in rows if r["verdict"] == "GAP"]
            covered = sum(1 for r in rows if r["verdict"] == "COVERED")
            return [Finding(
                stage="unify", status=laws.CLEAN, file="", function=None, line=None,
                cls="",
                message=f"taxonomy {covered}/{len(rows)} COVERED, {len(gaps)} GAP",
                strength=laws.STRENGTH_FINDS,
                extra={"gaps": [g["id"] for g in gaps]},
            )]

        self._stage("unify", unify)
        confidence.apply(self.report)
        self.report.save(cfg.out / "report.json")
        _write_md(self.report, cfg.out / "report.md")
        return self.report


def _write_md(report: RunReport, path: Path) -> None:
    lines = [
        f"# Helix report",
        f"",
        f"root: `{report.root}`",
        f"",
        f"| visibility | answer | resolution | **confidence** |",
        f"|---|---|---|---|",
        f"| {report.visibility} | {report.answer} | {report.resolution} | **{report.confidence}** |",
        f"",
        f"Confidence is a product. 0 means no data, not clean.",
        f"",
        f"## Stages",
        f"",
        f"| stage | status | records | seconds | note |",
        f"|---|---|---:|---:|---|",
    ]
    for s in report.stages:
        note = s.detail or s.install
        lines.append(f"| {s.name} | {s.status} | {s.records} | {s.elapsed:.2f} | {note} |")
    lines += ["", "## Findings", ""]
    for s in report.stages:
        for f in s.findings:
            if f.status in {laws.NOTRUN, laws.CLEAN} and s.name in {"inventory", "classify", "unify"}:
                continue
            if f.status in {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING, laws.BOUNDED, laws.FAILED, laws.CRASH, laws.HYPOTHESIS, laws.NOTRUN, laws.ERROR, laws.NEEDS_HARNESS, laws.CLEAN}:
                loc = f"{f.file}:{f.line}" if f.line else (f.file or "")
                cex = f"  cex `{f.counterexample}`" if f.counterexample else ""
                lines.append(f"- `{f.status}` **{s.name}** {loc} `{f.function or ''}` {f.cls} — {f.message}{cex}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_pipeline(cfg: Config) -> RunReport:
    return Pipeline(cfg).run()
