# Solver slice: Bitwuzla dialect and certified scheduling

No new portfolio timing. The 4,179-VC comparison was not rerun.

## SMT-LIB2 sent to Bitwuzla

In-process Z3 still solves the original formula. Bitwuzla and any other SMT-LIB2 member read one script, `external_smt2` (`src/prism/solver/query.cpp`).

This Z3's benchmark printer emits `(check-sat)`, not `(check-sat-using)`, and it does not emit a named tactic. The script is still passed through `bitwuzla_dialect`: a top-level `(check-sat-using ...)` becomes `(check-sat)`, and a top-level Z3 tactic command (`bit-blast`, `simplify`, `apply`, `qfbv`, `tseitin-cnf`, ...) is dropped. Commands inside an assertion, a quoted symbol, or a string are left alone.

`portable_smt2` already rewrote 1-argument `and`/`or` and `bvsmul_noovfl` / `bvsmul_noudfl` / `bvumul_noovfl`. 0-argument `and`/`or` were still apps, so the walker returned them unchanged and Z3 printed `(and )` / `(or )`. They are now `true` / `false` before printing. That is the same identity the header already stated.

## Certified requests

The learned scheduler (the GBDT in `predict.cpp`) only orders members and sets the head start. It was skipped when a certificate was actually attempted (`want_cert`). A certified request whose formula is not certifiable (arrays, floating point, arithmetic, ...) has `want_cert == false`, so the model still ran. Every `SolveOptions::certified` request now keeps the rules scheduler: feature estimates, then the bucket history. `PRISM_SOLVER_PREDICT=0` is unchanged for plain requests.

A plain UNSAT does not satisfy a certified query. That path was already in place and was not changed:

- `SolveResult::certified` is set only after cake_lpr (and Lean's checker, on the Lean bit-blaster path) accepts the LRAT proof of the CNF just built.
- A cached plain UNSAT on a certifiable certified request is not a cache hit. The query is solved again. The plain answer stands only when that gives no answer, and `certified` stays false.
- A cached certified entry is re-checked; the JSON flag alone is not the certificate.
- `known_unsat` runs only the certificate member. If that member does not produce an accepted proof, the caller's plain UNSAT stands uncertified.

History is part of the rules scheduler (what runs when the learned model is off). Certified requests still use it. The model does not.
