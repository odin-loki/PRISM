# SV-COMP property files

`parse_property` and `supported_property` in `src/prism/svcomp/property.cpp` already reject a property outside the supported set. This slice leaves that file as it is and locks the rejection in `tests/cpp/test_svcomp_property.cpp`.

## What an unknown property becomes

`parse_property` returns `unsupported` when the text matches none of the three supported formulas, or more than one of them.

The supported set stays these three:

| Id | Formula |
|---|---|
| `no-overflow` | `LTL(G ! overflow)` |
| `unreach-call` | `LTL(G ! call(reach_error()))` |
| `valid-memsafety` | `LTL(G valid-free)`, `LTL(G valid-deref)`, or `LTL(G valid-memtrack)` |

`valid-memsafety` and `no-overflow` are different ids. `supported_property` is true only for the three ids above.

`decide` answers `unknown` for any other id. The reason is `property <id> is not supported`. That holds when `bmc` reports `PROVED` of `main` (a `no-overflow` proof would be `true`) and when `bmc` reports a replayed `INT-SIGNED-OVF` (a `no-overflow` refutation would be `false(no-overflow)`). The same proof is `unknown` for `valid-memsafety`. The same overflow refutation is `unknown` for `valid-memsafety`.

`solve` answers `unknown` for a property file that parses as `unsupported`, with reason `unsupported property file <name>`. A property file that cannot be read answers `error`.

Outside the set, and still `unsupported`: termination (`LTL(F end)`), `def-behavior`, `no-data-race` (`LTL(G ! data-race)`), `valid-memcleanup`, `LTL(G valid-memsafety)`, `LTL(G ! unsigned-overflow)`, `LTL(G ! call(overflow()))`, `LTL(G ! call(__VERIFIER_error()))`, coverage properties, and a file that matches two supported formulas. A subproperty id such as `valid-deref` or `valid-free` is an unknown id.

## Tests

`tests/cpp/test_svcomp_property.cpp`, registered in `CMakeLists.txt` next to `tests/cpp/test_svcomp_waypoints.cpp`. The older `tests/cpp/test_svcomp.cpp` still maps the three shipped `.prp` files and the termination formula.

## Left alone

`property.cpp` was not edited. Also left alone: `decide.cpp`, `witness.cpp`, `replay.cpp`, `cli_svcomp.cpp`, `stage.cpp`, and `third_party/`.
