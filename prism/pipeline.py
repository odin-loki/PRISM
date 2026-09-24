"""One command. Stages that cannot run write NOTRUN, never look clean."""

from __future__ import annotations

from pathlib import Path
import json
import time

from prism import confidence, laws, shipdocs
from prism.adapters import run_compiler, run_cppcheck, run_dafny, run_esbmc
from prism.adapters_extra import run_optional_tools
from prism.agent import dafny_specs, execute_cex, hypothesize, rlef_repair
from prism.ai import LlamaEngine
from prism.checkers import run_lints
from prism.config import Config
from prism.contracts import prove_contracts
from prism.wp import run_wp
from prism.cparse import TU_EXTS, extract_functions, iter_sources, parse_gap_findings
from prism.diff import run_diff
from prism.fuse import run_fuse
from prism.harness import run_harness_bmc
from prism.interval import run_interval
from prism.ltl import run_ltl
from prism.bmc import run_bmc, with_cxx_std
from prism.inline import inline_static
from prism.concolic import run_concolic
from prism.models import Finding, FunctionInfo, RunReport, StageResult
from prism.muttest import run_muttest
from prism.pbsd import run_pbsd_lints
from prism.polyglot import is_known_source, run_polyglot
from prism.rapid import run_rapid
from prism.sarif import write_sarif
from prism.sanitize import run_sanitize
from prism.taint import run_taint
from prism.thread import run_thread
from prism.taxonomy import coverage_from_report
from prism import journal
from prism import sandbox
from prism import scope


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
    "pir",
    "conc",
    "harness",
    "review",
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


# Law 9 (docs/PLAN.md "Running on untrusted code"): what each stage may
# execute. "none" = pure analysis / parse / compile-only; "whole" = the
# stage only runs scanned code and is NOTRUN without --allow-exec; "part" =
# the analysis half runs, the execute half is NOTRUN without the flag.
# Same table in src/prism/pipeline.cpp (kExecStages).
EXEC_STAGES: dict[str, str] = {
    "pbsd": "part",        # imports the configured ParanoidBSD tools/verify modules
    "sanitize": "whole",   # compiles + runs `// prism: run` functions under ASan/UBSan/TSan
    "optional": "part",    # klee (native external calls), tree-local .cocci scripts
    "pir": "part",         # C++ engine: Clang->PIR->Z3 runs; lli translation validation does not
    "polyglot": "part",    # perl -c, cargo clippy, eslint (Tool.executes)
    "fuzz": "part",        # concrete oracle runs; compiled harness / AFL++ / libFuzzer do not
    "diff": "whole",       # compiles + runs both functions
    "rapid": "part",       # interpreter runs; gcc fallback does not
    "muttest": "part",     # interpreter runs; gcc fallback does not
    "execute": "part",     # concrete cex replay runs; LLM-written C does not
    "repair": "whole",     # compiles + runs LLM-written candidates
}


def run_pir_notrun(cfg: Config | None = None) -> list[Finding]:
    """The pir stage (Clang -> LLVM IR -> PIR -> Z3) exists only in the C++ engine.

    Roadmap D8 freezes this engine as a differential oracle, so it keeps the
    stage in STAGE_ORDER and says in one NOTRUN row that it did not run it
    (Law 7), instead of skipping it quietly. --certified / --solver-cache are
    accepted for CLI parity and recorded here: nothing was certified.
    """
    extra = {"install": "run the C++ engine (build/prism) for the pir stage"}
    if cfg is not None and cfg.certified:
        extra["certified_mode"] = "on"
        extra["certify_note"] = "not certified: the pir stage did not run (C++ engine only)"
    if cfg is not None and cfg.solver_cache:
        extra["solver_cache"] = str(cfg.solver_cache)
    return [Finding(
        stage="pir", status=laws.NOTRUN, file="", function=None, line=None, cls="",
        message="C++ engine only (Python engine frozen as oracle, roadmap D8)",
        strength=laws.STRENGTH_PROVES,
        extra=extra,
    )]


ASTLINT_LAYER = "clang-ast"


def is_layer_row(f: Finding) -> bool:
    """A NOTRUN row describing a sub-layer of a stage that did run."""
    return f.status == laws.NOTRUN and "layer" in (f.extra or {})


def run_astlint_notrun(sources: list[Path]) -> list[Finding]:
    """The Clang-AST lint layer of the lints stage (roadmap 2.8) exists only in
    the C++ engine (src/prism/astlint.cpp). Roadmap D8: the regex lints run
    here as before, and one NOTRUN row says the AST layer did not (Law 7).
    """
    if not any(p.suffix.lower() in TU_EXTS for p in sources):
        return []
    return [Finding(
        stage="lints", status=laws.NOTRUN, file="", function=None, line=None, cls="",
        message="Clang-AST lints: C++ engine only (Python engine frozen as oracle, "
                "roadmap D8); regex lints only",
        strength=laws.STRENGTH_FINDS,
        extra={"layer": ASTLINT_LAYER,
               "install": "run the C++ engine (build/prism) for the Clang-AST lints"},
    )]


def run_review_notrun() -> list[Finding]:
    """The review stage (vacuity audit, approved/drafted contracts, assumption
    audit, proof store / PROOF-REGRESSION; roadmap 9.2, 9.3, 4.2) exists only in
    the C++ engine (src/prism/ai/review.cpp). Roadmap D8: one NOTRUN row here.
    """
    return [Finding(
        stage="review", status=laws.NOTRUN, file="", function=None, line=None, cls="",
        message="C++ engine only (Python engine frozen as oracle, roadmap D8)",
        strength=laws.STRENGTH_PROVES,
        extra={"install": "run the C++ engine (build/prism) for the review stage"},
    )]


def run_conc_notrun() -> list[Finding]:
    """The conc stage (lazy sequentialisation of threads) exists only in the C++ engine.

    Roadmap D8 freezes this engine as a differential oracle; the stage keeps
    its STAGE_ORDER slot and records one NOTRUN row (Law 7).
    """
    return [Finding(
        stage="conc", status=laws.NOTRUN, file="", function=None, line=None, cls="",
        message="C++ engine only (Python engine frozen as oracle, roadmap D8)",
        strength=laws.STRENGTH_FINDS,
        extra={"install": "run the C++ engine (build/prism) for the conc stage"},
    )]


def exec_gate_note(stage: str, findings: list[Finding], what: str) -> list[Finding]:
    """A "part" stage that held back its execute half says so once (Law 7).

    The stage's modules mark the skipped half with extra.exec = NOTRUN;
    this adds the stage-level NOTRUN row with the --allow-exec hint.
    """
    marked = any((f.extra or {}).get("exec") == laws.NOTRUN for f in findings)
    noted = any(
        f.status == laws.NOTRUN and (f.extra or {}).get("reason") == sandbox.EXEC_REASON
        for f in findings
    )
    if marked and not noted:
        findings.append(sandbox.exec_notrun(stage, what))
    return findings


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
        # A NOTRUN row for a sub-layer (extra.layer, e.g. the Clang-AST lints)
        # does not make a stage whose main body ran NOTRUN.
        if findings and all(f.status == laws.NOTRUN and not is_layer_row(f)
                            for f in findings):
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
        # Law 9: the exec policy holds for this run only (modules without a
        # Config read it through prism.sandbox.allowed()).
        with sandbox.policy(self.cfg.allow_exec):
            return self._run()

    def _run(self) -> RunReport:
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
                out.extend(parse_gap_findings(p, rel))
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
            # Law 7: a skipped vendor/build directory holding sources is
            # written down, not skipped quietly (prism/scope.py).
            for rel_dir, n in scope.skipped_dirs(root, is_known_source):
                out.append(Finding(
                    stage="inventory", status=laws.UNKNOWN, file="", function=None,
                    line=None, cls="", message=scope.skipped_message(rel_dir, n),
                    strength=laws.STRENGTH_FINDS,
                    extra={"skipped": rel_dir, "files": str(n)},
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
            out = run_lints(sources, root if root.is_dir() else root.parent,
                            jobs=cfg.jobs)
            return out + run_astlint_notrun(sources)

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
            return run_bmc(inline_static(with_cxx_std(functions, cfg.root)), cfg.unwind)

        bmc_rec = self._stage("bmc", bmc)
        self._stage("pir", lambda: run_pir_notrun(cfg))
        self._stage("conc", run_conc_notrun)
        self._stage("harness", lambda: run_harness_bmc(functions, cfg.unwind))
        self._stage("review", run_review_notrun)
        self._stage("concolic", lambda: run_concolic(functions, budget=32))

        def fuzz() -> list[Finding]:
            src_root = root if root.is_dir() else root.parent
            return exec_gate_note("fuzz", run_fuse(
                functions, bmc_rec.findings, src_root,
                budget=cfg.fuzz_budget, iters=cfg.fuzz_iters,
                engine=self._engine() if cfg.llm else None,
            ), "fuzz (compiled harness, AFL++, libFuzzer)")

        self._stage("fuzz", fuzz)
        self._stage("diff", lambda: run_diff(
            functions, root if root.is_dir() else root.parent,
        ))
        self._stage("rapid", lambda: exec_gate_note(
            "rapid", run_rapid(functions, trials=64), "rapid (gcc fallback harness)"))
        self._stage("muttest", lambda: exec_gate_note(
            "muttest", run_muttest(functions, trials=32), "muttest (gcc fallback harness)"))

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
            return execute_cex(fails, functions, llm=cfg.llm, engine=eng,
                               allow_exec=cfg.allow_exec)

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
            return rlef_repair(self._engine(), src, fb, cfg.repair_rounds,
                               allow_exec=cfg.allow_exec)

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

        # Verdict audit (docs/VERDICTS.md): before unify so taxonomy and
        # confidence only see admitted verdicts; again after, for unify itself.
        laws.audit_report(self.report)
        self._stage("unify", unify)
        laws.audit_report(self.report)
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
        # Roadmap 3.2 / 8.3 / 6.4: the trusted base and the verdict
        # definitions are written next to this file (prism/shipdocs.py).
        f"Trusted base: [{shipdocs.TRUSTED_BASE_FILE}]({shipdocs.TRUSTED_BASE_FILE}) says what a proof "
        f"in this report depends on. Every verdict links to its definition in "
        f"[{shipdocs.VERDICTS_FILE}]({shipdocs.VERDICTS_FILE}).",
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
            lines.append(_md_finding(s.name, f))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    shipdocs.write_shipped_docs(path.parent)


def _md_finding(stage: str, f: Finding) -> str:
    """One report.md bullet. Empty location/function/class parts are left
    out (a summary row has no file); report.json keeps every field."""
    parts = [f"- `{f.status}` **{stage}**"]
    if f.file:
        parts.append(f"{f.file}:{f.line}" if f.line else f.file)
    if f.function:
        parts.append(f"`{f.function}`")
    if f.cls:
        parts.append(f.cls)
    line = " ".join(parts) + f" — {f.message}"
    if f.counterexample:
        line += f"  cex `{f.counterexample}`"
    anchor = shipdocs.verdict_anchor(f.status)
    if anchor:
        line += f"  ([{f.status}]({shipdocs.VERDICTS_FILE}#{anchor}))"
    return line


def run_pipeline(cfg: Config) -> RunReport:
    return Pipeline(cfg).run()
