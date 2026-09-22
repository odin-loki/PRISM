"""The vocabulary a result is allowed to use.

Borrowed from ParanoidBSD's verify tree and not relaxed. A status that
is not in this file is a bug in the caller, not a new kind of truth.
"""

from __future__ import annotations

# Formal / BMC — never merge any pair of these.
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

FORMAL_VERDICTS = {
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

ANSWERED = {PROVED_UNBOUNDED, PROVED, PROVED_ASSUMING, BOUNDED, FAILED}
NO_ANSWER = {TIMEOUT, ERROR, NOFUNC, NOTRUN, NEEDS_HARNESS, UNKNOWN}

FUZZ_VERDICTS = {CRASH, CLEAN, NOSEED, SANFAIL, ERROR, TIMEOUT, NOTRUN}

LLM_VERDICTS = {HYPOTHESIS, READS, ERROR, TIMEOUT, NOTRUN}


def is_proof(status: str) -> bool:
    return status in {PROVED_UNBOUNDED, PROVED, PROVED_ASSUMING}


def refuse_merge(a: str, b: str) -> None:
    """Two different claims about the same function stay two claims."""
    if a == b:
        return
    proofs = {PROVED_UNBOUNDED, PROVED, PROVED_ASSUMING, BOUNDED}
    if a in proofs and b in proofs:
        raise ValueError(f"refusing to merge formal statuses {a} and {b}")
    if {a, b} & {CLEAN} and {a, b} & proofs:
        raise ValueError(f"refusing to promote fuzz {a}/{b} to a proof")
