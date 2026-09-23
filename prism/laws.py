"""The vocabulary a result is allowed to use, and the verdict lattice.

Borrowed from ParanoidBSD's verify tree and not relaxed. A status that
is not in this file is a bug in the caller, not a new kind of truth.

The lattice functions (merge law, rewrite rule, admission gate, audit,
confidence) mirror the C++ module src/prism/verdict/ and the Lean model
proofs/Prism/Verdict.lean, where the laws are proved. tests/test_verdict.py
checks every function here against tests/data/verdict_tables.json (the
model's truth tables) entry by entry. docs/VERDICTS.md.
"""

from __future__ import annotations

from typing import Any

from prism.models import Finding

# Formal / BMC — never merge any pair of these.
PROVED_CERTIFIED = "PROVED-CERTIFIED"  # UNSAT checked by a verified checker (cake_lpr / Lean LRAT)
PROVED_UNBOUNDED = "PROVED-UNBOUNDED"  # k-induction closed; no unwind bound
PROVED = "PROVED"                      # all properties hold, loops closed in k
PROVED_ASSUMING = "PROVED-ASSUMING"    # proved under an explicit requires
BOUNDED = "BOUNDED"                    # nothing found within k; that is all
FAILED = "FAILED"                      # counterexample
UNKNOWN = "UNKNOWN"                    # solver ran, did not conclude
TIMEOUT = "TIMEOUT"
ERROR = "ERROR"
NOFUNC = "NOFUNC"
NOTRUN = "NOTRUN"
NEEDS_HARNESS = "NEEDS-HARNESS"

# Fuzz — CLEAN is not a proof.
CRASH = "CRASH"
CLEAN = "CLEAN"
NOSEED = "NOSEED"
SANFAIL = "SANFAIL"

# LLM — cannot promote a class to COVERED.
HYPOTHESIS = "HYPOTHESIS"
READS = "READS"

# Strength a silence is worth.
STRENGTH_PROVES = "PROVES"
STRENGTH_FINDS = "FINDS"
STRENGTH_SOME = "SOME"
STRENGTH_READS = "READS"

# extra key/value of a finding whose UNSAT result a verified checker
# accepted against the exact CNF PRISM produced (roadmap 3.2).
CERTIFICATE_KEY = "certificate"
CERTIFICATE_CHECKED = "checked"

# Table order of the Lean model (Verdict.all) and the C++ enum.
VERDICTS = (
    PROVED_CERTIFIED, PROVED_UNBOUNDED, PROVED, PROVED_ASSUMING, BOUNDED,
    FAILED, UNKNOWN, TIMEOUT, ERROR, NOFUNC, NOTRUN, NEEDS_HARNESS,
    CRASH, CLEAN, NOSEED, SANFAIL, HYPOTHESIS, READS,
)

PROOFS = frozenset({PROVED_CERTIFIED, PROVED_UNBOUNDED, PROVED, PROVED_ASSUMING})
# Formal claims: no two distinct ones ever merge (Law 2).
FORMAL = PROOFS | {BOUNDED}
MODEL = frozenset({HYPOTHESIS, READS})
DEFECTS = frozenset({FAILED, CRASH, SANFAIL})

FORMAL_VERDICTS = {
    PROVED_CERTIFIED,
    PROVED_UNBOUNDED,
    PROVED,
    PROVED_ASSUMING,
    BOUNDED,
    FAILED,
    UNKNOWN,
    TIMEOUT,
    ERROR,
    NOFUNC,
    NOTRUN,
    NEEDS_HARNESS,
}

ANSWERED = {PROVED_CERTIFIED, PROVED_UNBOUNDED, PROVED, PROVED_ASSUMING, BOUNDED, FAILED}
NO_ANSWER = {TIMEOUT, ERROR, NOFUNC, NOTRUN, NEEDS_HARNESS, UNKNOWN}

FUZZ_VERDICTS = {CRASH, CLEAN, NOSEED, SANFAIL, ERROR, TIMEOUT, NOTRUN}

LLM_VERDICTS = {HYPOTHESIS, READS, ERROR, TIMEOUT, NOTRUN}

_RANK = {PROVED_CERTIFIED: 5, PROVED_UNBOUNDED: 4, PROVED: 3, PROVED_ASSUMING: 2, BOUNDED: 1}


def is_proof(status: str) -> bool:
    return status in PROOFS


def is_formal(status: str) -> bool:
    return status in FORMAL


def rank(status: str) -> int:
    """Strength of a formal claim: 5 PROVED-CERTIFIED .. 1 BOUNDED; 0 otherwise."""
    return _RANK.get(status, 0)


# Refusal names (Lean Refusal.name).
REFUSE_NONE = "none"
REFUSE_FORMAL = "formal"
REFUSE_PROMOTE_FUZZ = "promote-fuzz"
REFUSE_PROMOTE_MODEL = "promote-model"
REFUSE_NOTRUN_CLEAN = "notrun-clean"


def merge_refusal(a: str, b: str) -> str:
    """Why two claims about one function must stay two claims ("none" if they may merge)."""
    if a == b:
        return REFUSE_NONE
    fa, fb = a in FORMAL, b in FORMAL
    if fa and fb:
        return REFUSE_FORMAL
    if (a == CLEAN and fb) or (b == CLEAN and fa):
        return REFUSE_PROMOTE_FUZZ
    if (a in MODEL and fb) or (b in MODEL and fa):
        return REFUSE_PROMOTE_MODEL
    if (a == NOTRUN and (b in ANSWERED or b == CLEAN)) or (
        b == NOTRUN and (a in ANSWERED or a == CLEAN)
    ):
        return REFUSE_NOTRUN_CLEAN
    return REFUSE_NONE


def refuse_merge(a: str, b: str) -> None:
    """Two different claims about the same function stay two claims."""
    why = merge_refusal(a, b)
    if why == REFUSE_FORMAL:
        raise ValueError(f"refusing to merge formal statuses {a} and {b}")
    if why == REFUSE_PROMOTE_FUZZ:
        raise ValueError(f"refusing to promote fuzz {a}/{b} to a proof")
    if why == REFUSE_PROMOTE_MODEL:
        raise ValueError(f"refusing to promote model output {a}/{b} to a proof")
    if why == REFUSE_NOTRUN_CLEAN:
        other = b if a == NOTRUN else a
        raise ValueError(
            f"refusing to merge NOTRUN with {other}: a stage that did not run is never clean"
        )


def may_rewrite(frm: str, to: str) -> bool:
    """May a recorded status later be rewritten? Formal claims only weaken;
    nothing becomes a formal claim by rewriting; no-answer stays no-answer."""
    if frm not in VERDICTS or to not in VERDICTS:
        return False
    if frm == to:
        return True
    if to in FORMAL:
        return frm in FORMAL and rank(to) < rank(frm)
    if frm in FORMAL:
        return to in {UNKNOWN, ERROR}
    if frm in NO_ANSWER:
        return to in NO_ANSWER
    return True


# Origins (Lean Origin.name). Only a solver or an external prover may prove.
ORIGIN_SOLVER = "solver"
ORIGIN_EXTERNAL_PROVER = "external-prover"
ORIGIN_FUZZER = "fuzzer"
ORIGIN_MODEL = "model"
ORIGIN_LINT = "lint"
ORIGIN_EXECUTION = "execution"
ORIGIN_PIPELINE = "pipeline"
ORIGINS = (
    ORIGIN_SOLVER, ORIGIN_EXTERNAL_PROVER, ORIGIN_FUZZER, ORIGIN_MODEL,
    ORIGIN_LINT, ORIGIN_EXECUTION, ORIGIN_PIPELINE,
)
PROVING_ORIGINS = frozenset({ORIGIN_SOLVER, ORIGIN_EXTERNAL_PROVER})

# The audit table: which origin each pipeline stage's findings have.
# Same rows as src/prism/verdict/verdict.cpp and Stage.origin in Lean.
STAGE_ORIGIN: dict[str, str] = {
    "inventory": ORIGIN_PIPELINE,
    "classify": ORIGIN_PIPELINE,
    "lints": ORIGIN_LINT,
    "taint": ORIGIN_LINT,
    "thread": ORIGIN_LINT,
    "interval": ORIGIN_LINT,
    "warnings": ORIGIN_LINT,
    "cppcheck": ORIGIN_LINT,
    "pbsd": ORIGIN_LINT,
    "sanitize": ORIGIN_EXECUTION,
    "optional": ORIGIN_EXTERNAL_PROVER,
    "polyglot": ORIGIN_LINT,
    "esbmc": ORIGIN_EXTERNAL_PROVER,
    "dafny": ORIGIN_EXTERNAL_PROVER,
    "contracts": ORIGIN_SOLVER,
    "wp": ORIGIN_SOLVER,
    "bmc": ORIGIN_SOLVER,
    "pir": ORIGIN_SOLVER,
    "harness": ORIGIN_SOLVER,
    "review": ORIGIN_SOLVER,
    "concolic": ORIGIN_EXECUTION,
    "fuzz": ORIGIN_FUZZER,
    "diff": ORIGIN_EXECUTION,
    "rapid": ORIGIN_FUZZER,
    "muttest": ORIGIN_EXECUTION,
    "ltl": ORIGIN_SOLVER,
    "llm": ORIGIN_MODEL,
    "execute": ORIGIN_EXECUTION,
    "repair": ORIGIN_MODEL,
    "unify": ORIGIN_PIPELINE,
    "other": ORIGIN_PIPELINE,
}


def stage_origin(stage: str) -> str:
    return STAGE_ORIGIN.get(stage, ORIGIN_PIPELINE)


def admit(origin: str, status: str, certificate_checked: bool) -> str:
    """The gate every formal claim passes. Non-formal statuses pass unchanged
    (NOTRUN stays NOTRUN). A formal claim from an origin that may not prove
    becomes UNKNOWN. PROVED-CERTIFIED needs the solver origin and a checked
    certificate, otherwise it falls back to PROVED — never upward."""
    if status not in FORMAL:
        return status
    if origin not in PROVING_ORIGINS:
        return UNKNOWN
    if status == PROVED_CERTIFIED:
        return PROVED_CERTIFIED if origin == ORIGIN_SOLVER and certificate_checked else PROVED
    return status


def audit(stage: str, status: str, certificate_checked: bool) -> tuple[str, bool]:
    """(status after admit, violation) for one finding of `stage`."""
    r = admit(stage_origin(stage), status, certificate_checked)
    return r, r != status


def audit_report(report: Any) -> int:
    """Final pipeline pass (src/prism/laws.cpp audit_report). Every finding
    goes through audit(); a violation demotes it, records
    extra.audit_original, and appends an ERROR finding "verdict audit:
    <stage> may not emit <status>" to that stage. Idempotent."""
    violations = 0
    for s in report.stages:
        errors: list[Any] = []
        for f in s.findings:
            if f.status not in VERDICTS:
                continue
            extra = f.extra or {}
            cert = str(extra.get(CERTIFICATE_KEY, "")) == CERTIFICATE_CHECKED
            new, bad = audit(s.name, f.status, cert)
            if not bad:
                continue
            violations += 1
            msg = f"verdict audit: {s.name} may not emit {f.status}"
            if f.status == PROVED_CERTIFIED and stage_origin(s.name) in PROVING_ORIGINS:
                msg += " without a checked certificate"
            errors.append(Finding(
                stage=s.name, status=ERROR, file=f.file, function=f.function, line=f.line,
                cls="", message=msg, strength=STRENGTH_FINDS,
                extra={"audit": "verdict", "original_status": f.status},
            ))
            if f.extra is None:
                f.extra = {}
            f.extra["audit_original"] = f.status
            f.status = new
            f.message = f"[{msg}] {f.message}"
        if errors:
            s.findings.extend(errors)
            s.records = len(s.findings)
    return violations


def confidence(visibility: float, answer: float, resolution: float) -> float:
    """Law 5: visibility x answer x resolution; exactly 0 without visibility."""
    if not visibility > 0.0:
        return 0.0
    return visibility * answer * resolution


def score_counts(
    n_fun: int, classified: int, attempted: int, answered: int, resolved: int,
) -> tuple[float, float, float, float]:
    """(visibility, answer, resolution, confidence); each factor is 0 without data."""
    def ratio(n: int, d: int) -> float:
        return n / d if d else 0.0

    v = ratio(classified, n_fun)
    a = ratio(answered, attempted)
    r = ratio(resolved, answered)
    return v, a, r, confidence(v, a, r)
