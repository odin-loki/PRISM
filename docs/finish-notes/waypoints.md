# SV-COMP waypoints

`nondet_waypoints`, `nondet_locations` and `reach_error_call_site` in `src/prism/svcomp/waypoints.cpp` already place a waypoint only from a location on the counterexample. This slice leaves that file as it is and locks the empty-counterexample rule in `tests/cpp/test_svcomp_waypoints.cpp`.

## Behavior

- An empty nondet trace yields no waypoints. A call that happens to sit on line 1 is not turned into a waypoint, and a task with no `reach_error()` call yields no target site.
- One counterexample location (`line:col` that is the start of that call) yields one waypoint. Its file is the task file and its line is that location's line. Two call sites and no location still yield nothing.
- A missing or zero line (`0:11`, or a token that is not `line:col`) is not line 1. `nondet_locations` stores `0:0` as an absent location. With two call sites that absence yields no waypoint. With one call on a later line, the waypoint uses that call's line.

An empty counterexample is not a proof. `decide` still answers `true` only for a covering proof of `main`, and `false` only for a replayed refutation. That mapping was not changed here.

## Tests

`tests/cpp/test_svcomp_waypoints.cpp`, registered in `CMakeLists.txt` next to `tests/cpp/test_svcomp_decide.cpp`. The older `tests/cpp/test_svcomp.cpp` still holds ambiguous call sites, debug locations, line markers and the violation-target column.

- An empty trace yields no waypoints, including when the only call is on line 1, and `reach_error_call_site` is empty when the task has no call.
- One location on the second of two `__VERIFIER_nondet_int()` calls yields one waypoint with that file and line 4.
- `0:11` is an absent location, not line 1. Two sites then yield no waypoint. The only call, on line 4, stays on line 4 when its line is missing or zero.

## Left alone

`waypoints.cpp` was not edited. Also left alone: `decide.cpp`, `witness.cpp`, `invariants.cpp`, `replay.cpp`, `cli_svcomp.cpp`, `stage.cpp`, and `third_party/`.
