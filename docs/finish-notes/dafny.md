# Dafny specs without a model

`dafny_specs` (`src/prism/stages/hypothesize.cpp`) proposes Dafny-style contracts. Law 4: model output is `HYPOTHESIS` or `READS` and cannot cover `FUNC-CONTRACT`. A missing model is `NOTRUN`.

The contracts stage (`src/prism/pipeline.cpp`) already calls `dafny_specs(fns, 3, cfg)` when `cfg.llm` is on, and only for functions whose kind is `SCALAR` or `VOID`. That call site was left as it stands.

## Statuses

With no FORGE model (`.fpk`), `dafny_specs` returns one finding before it walks the function list:

| field | value |
|---|---|
| stage | `contracts` |
| status | `NOTRUN` |
| strength | `READS` |
| class | `FUNC-CONTRACT` |
| message | `no FORGE model (.fpk) configured` |

A later `complete()` that fails with an HTTP-shaped error is also `NOTRUN` / `READS` / `FUNC-CONTRACT`. Any other `complete()` error is `ERROR` / `READS` / `FUNC-CONTRACT`. A reply is `HYPOTHESIS` / `READS` / `FUNC-CONTRACT` (`Dafny-style spec (hypothesis, not proved)`).

Those are the only statuses `dafny_specs` assigns. `PROVED`, `PROVED-UNBOUNDED`, `PROVED-CERTIFIED`, and `CLEAN` are not among them. `hypothesize.cpp` was left unchanged.

## Test

`tests/cpp/test_agent.cpp` is already on the explicit `prism_tests` source list, so the case was added there.

`agent: dafny_specs with no FORGE model is one NOTRUN FUNC-CONTRACT read` builds a `Config` with an empty `fpk` (not `default_config()`, which fills `fpk` from `PRISM_FPK` or a guessed blob). It calls `prism::dafny_specs` on one `SCALAR` and one `VOID` function and expects the single `NOTRUN` / `READS` / `FUNC-CONTRACT` row above. No model is downloaded.
