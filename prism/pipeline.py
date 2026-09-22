"""One command. Stages that cannot run write NOTRUN, never look clean."""

from __future__ import annotations

from pathlib import Path
import json
import time

from prism import confidence, laws
from prism.adapters import run_compiler, run_cppcheck, run_dafny, run_esbmc
from prism.adapters_extra import run_optional_tools
from prism.agent import dafny_specs, execute_cex, hypothesize, rlef_repair
from prism.ai import LlamaEngine
from prism.checkers import run_lints
from prism.config import Config
from prism.contracts import prove_contracts
from prism.wp import run_wp
from prism.cparse import TU_EXTS, extract_functions, iter_sources
from prism.diff import run_diff
from prism.fuse import run_fuse
from prism.harness import run_harness_bmc
from prism.interval import run_interval
from prism.ltl import run_ltl
from prism.bmc import run_bmc
from prism.inline import inline_static
from prism.concolic import run_concolic
from prism.models import Finding, FunctionInfo, RunReport, StageResult
from prism.muttest import run_muttest
from prism.pbsd import run_pbsd_lints
from prism.polyglot import run_polyglot
from prism.rapid import run_rapid
from prism.sarif import write_sarif
from prism.sanitize import run_sanitize
from prism.taint import run_taint
from prism.thread import run_thread
from prism.taxonomy import coverage_from_report
from prism import journal


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
    "polyglot",
    "esbmc",
    "dafny",
    "contracts",
    "wp",
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


# LLM silence/hypothesis statuses. Anything else (PROVED/FAILED/CLEAN/…) is a lie.
_LLM_KEEP_STATUS = frozenset({
    laws.NOTRUN, laws.ERROR, laws.TIMEOUT, laws.HYPOTHESIS, laws.READS,
})


def llm_forced_reads(findings: list[Finding]) -> list[Finding]:
    """The llm stage is READS. A lying backend cannot COVER or prove."""
    for f in findings:
        f.stage = "llm"
        f.strength = laws.STRENGTH_READS
        if f.status not in _LLM_KEEP_STATUS:
            f.status = laws.HYPOTHESIS
    return findings


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
        if (
            prev is not None
            and prev.status in {"ok", "NOTRUN"}
            and not (name == "classify" and not self.report.functions)
        ):
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
        if cfg.resume:
            # stages.jsonl is the live log. report.json is only a fallback when
            # the log file is absent — a failed/partial jsonl must not revive
            # ok/NOTRUN rows from a previous complete report.json.
            jsonl_present = journal.stages_present(cfg.out)
            self._resume = journal.completed_ok(cfg.out) if jsonl_present else {}
            fns = journal.read_functions(cfg.out)
            old = RunReport.load(cfg.out / "report.json")
            if old is not None and not jsonl_present:
                self._resume = {
                    s.name: s for s in old.stages
                    if s.status in {"ok", "NOTRUN"}
                }
                if not fns:
                    fns = list(old.functions)
            if fns:
                self.report.functions = list(fns)
            if self._resume:
                src = "stages.jsonl" if jsonl_present else "report.json"
                self.report.notes.append(f"resumed from {cfg.out / src}")
        else:
            journal.reset(cfg.out)
        sources = iter_sources(root)
        functions: list[FunctionInfo] = list(self.report.functions)
        # inventory and classify both need every source parsed; parse each
        # file once and share it (classify re-parses only what inventory did
        # not, e.g. when inventory was resumed or excluded).
        parsed: dict[Path, list[FunctionInfo]] = {}

        def source_rel(p: Path) -> str:
            try:
                return str(p.relative_to(root)) if root.is_dir() else p.name
            except ValueError:
                return p.name

        def parse(p: Path, rel: str) -> list[FunctionInfo]:
            fns = parsed.get(p)
            if fns is None:
                fns = parsed[p] = extract_functions(p, rel)
            return fns

        def inventory() -> list[Finding]:
            out: list[Finding] = []
            for p in sources:
                rel = source_rel(p)
                fns = parse(p, rel)
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
                rel = source_rel(p)
                fns = parse(p, rel)
                functions.extend(fns)
                for fn in fns:
                    out.append(Finding(
                        stage="classify", status=fn.kind, file=rel, function=fn.name,
                        line=fn.line, cls=fn.kind, message=fn.signature,
                        strength=laws.STRENGTH_FINDS, extra={"static": fn.static},
                    ))
            self.report.functions = functions
            journal.write_functions(cfg.out, functions)
            return out

        self._stage("classify", classify)
        parsed.clear()

        def lints() -> list[Finding]:
            return run_lints(sources, root if root.is_dir() else root.parent,
                             jobs=cfg.jobs)

        self._stage("lints", lints)
        self._stage("taint", lambda: run_taint(functions))
        self._stage("thread", lambda: run_thread(functions))
        self._stage("interval", lambda: run_interval(functions))
        self._stage("warnings", lambda: run_compiler(sources, cfg))
        self._stage("cppcheck", lambda: run_cppcheck(sources, cfg))
        self._stage("pbsd", lambda: run_pbsd_lints(sources, cfg))
        self._stage("sanitize", lambda: run_sanitize(sources, cfg))
        self._stage("optional", lambda: run_optional_tools(sources, cfg))
        self._stage("polyglot", lambda: run_polyglot(root, cfg))
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
        self._stage("wp", lambda: run_wp(functions, cfg.unwind))

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
            return llm_forced_reads(hypothesize(self._engine(), functions, budget=4))

        self._stage("llm", llm)

        def execute() -> list[Finding]:
            fails = [
                f for s in self.report.stages for f in s.findings
                if f.status in {laws.FAILED, laws.CRASH} and f.function
                and "=" in (f.counterexample or "")
            ]
            fails.sort(key=lambda f: (0 if f.status == laws.CRASH else 1, f.stage))
            eng = self._engine() if cfg.llm and fails else None
            return execute_cex(fails, functions, llm=cfg.llm, engine=eng)

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
                message=f"taxonomy {covered}/{len(rows)} COVERED, {len(gaps)} GAP (not a proof)",
                strength=laws.STRENGTH_FINDS,
                extra={"gaps": [g["id"] for g in gaps], "not_a_proof": "true"},
            )]

        self._stage("unify", unify)
        confidence.apply(self.report)
        self.report.save(cfg.out / "report.json")
        _write_md(self.report, cfg.out / "report.md")
        write_sarif(self.report, cfg.out / "report.sarif")
        return self.report


def _write_md(report: RunReport, path: Path) -> None:
    lines = [
        "# PRISM report",
        "",
        f"root: `{report.root}`",
        "",
        "| visibility | answer | resolution | **confidence** |",
        "|---|---|---|---|",
        f"| {report.visibility} | {report.answer} | {report.resolution} | **{report.confidence}** |",
        "",
        "Confidence is a product. 0 means no data, not clean.",
        "",
        "## Stages",
        "",
        "| stage | status | records | seconds | note |",
        "|---|---|---:|---:|---|",
    ]
    for s in report.stages:
        note = s.detail or s.install
        lines.append(f"| {s.name} | {s.status} | {s.records} | {s.elapsed:.2f} | {note} |")
    lines += ["", "## Findings", ""]
    for s in report.stages:
        for f in s.findings:
            if f.status in {laws.NOTRUN, laws.CLEAN} and s.name in {"inventory", "classify", "unify"}:
                continue
            # Same rows as prism.gui.finding_rows: UNKNOWN/TIMEOUT stay visible.
            # A whitelist that dropped them made a present-but-silent adapter
            # look like an empty ok stage in report.md.
            loc = f"{f.file}:{f.line}" if f.line else (f.file or "")
            cex = f"  cex `{f.counterexample}`" if f.counterexample else ""
            lines.append(f"- `{f.status}` **{s.name}** {loc} `{f.function or ''}` {f.cls} — {f.message}{cex}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_pipeline(cfg: Config) -> RunReport:
    return Pipeline(cfg).run()
