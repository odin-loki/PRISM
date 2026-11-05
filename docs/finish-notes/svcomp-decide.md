# SV-COMP decide

`prism::svcomp::decide` in `src/prism/svcomp/decide.cpp` already followed the mapping in `include/prism/svcomp.hpp` and `docs/SVCOMP.md`. This slice leaves that function as it is and locks the answer table in `tests/cpp/test_svcomp_decide.cpp`.

The only verdict stage is `pir`. A `bmc` stage in an old report (the stage was deleted on 2026-10-31) is ignored. `valid-memsafety` still has an empty proof set, so a proof of `main` never answers `true` for it.

## Mapping

| Report | Answer |
|---|---|
| `PROVED`, `PROVED-UNBOUNDED` or `PROVED-CERTIFIED` of `main` on `pir`, for `no-overflow` or `unreach-call` | `true` |
| Same proof statuses for `valid-memsafety` | `unknown` |
| `PROVED` of a function other than `main` | `unknown` |
| `BOUNDED`, `PROVED-ASSUMING`, `HYPOTHESIS`, `READS`, `NOTRUN`, `TIMEOUT`, `UNKNOWN`, `CLEAN` on `pir` | `unknown` |
| `fuzz` or `libfuzzer` row (`CLEAN`, `PROVED`, or `FAILED` with a counterexample), even when replay would say `replayed` | `unknown` |
| `pir` `BOUNDED` beside a fuzzer `CLEAN` | `unknown` |
| No verdict stage, or `pir` present with status `NOTRUN` and no finding of `main` | `unknown` |
| A `bmc` row (`PROVED` or replayed `FAILED`) from an old report | `unknown` |
| `FAILED` of the property's class (`INT-SIGNED-OVF` for no-overflow, `FUNC-CONTRACT` `reach_error` for unreach-call) and replay `replayed` | `false(<prop>)` |
| That `FAILED` row while replay is `notrun` (`--allow-exec` off) | `unknown` |
| That `FAILED` row while replay is `not-replayed` because the program timed out | `unknown` |
| Finding status `TIMEOUT`, or a `pir` stage whose status is `TIMEOUT` or `failed` with detail `timeout` | `unknown` |
| `FAILED` of another class, replay `replayed` | `unknown` |
| `PROVED` and an unreplayed `FAILED` of the property in the same report | `unknown` |
| Same pair once the counterexample replays | `false(<prop>)` |

`false` still requires the replay key `replayed`. A counterexample string on the finding is not enough. A timeout is not `false`.

## Tests

`tests/cpp/test_svcomp_decide.cpp`, registered in `CMakeLists.txt` next to `tests/cpp/test_svcomp.cpp`. The older file still holds shifts, witness order, scoring, replay and the end-to-end task. These cases are the answer table:

- `PROVED` / `PROVED-UNBOUNDED` / `PROVED-CERTIFIED` of `main` on `pir` is `true` for `no-overflow` and `unreach-call`. The same proof on a `bmc` stage is `unknown`, and a `bmc` row does not stand in for a `pir` that did not run.
- `BOUNDED`, `HYPOTHESIS`, `READS`, `NOTRUN`, `CLEAN`, and a fuzzer row stay `unknown`.
- `FAILED` with a replayed counterexample is `false(no-overflow)` or `false(unreach-call)`. A `bmc` refutation from an old report stays `unknown`.
- The same `FAILED` row with replay `notrun` or a replay timeout stays `unknown`.
- A finding or stage timeout stays `unknown`.
- A missing verdict stage, or a verdict stage that did not run, stays `unknown`.

## Left alone

`decide.cpp` was not edited. Also left alone: `witness.cpp`, `invariants.cpp`, `replay.cpp`, `property.cpp`, `cli_svcomp.cpp`, `stage.cpp`, `pipeline.cpp`, the default data model, and `third_party/`. No subset re-score: the mapping did not change.
