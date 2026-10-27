# Concolic CLEAN is not a proof

Slice on `finish/concolic-not-proof`, started at `66d1b3373`. A finished
concolic run with no UB was already `laws::CLEAN`, and that status is
already not a proof, so `src/prism/stages/concolic.cpp` was not edited.

## What a quiet run reports

`concolic_function` drains its seed queue. When every concrete execution
returns with an empty `ub` string, the finding is:

| field | value |
| --- | --- |
| stage | `concolic` |
| status | `laws::CLEAN` |
| strength | `FINDS` |
| message | `no UB in N concolic inputs (not a proof)` |

`laws::is_proof` is true only for `PROVED-CERTIFIED`, `PROVED-UNBOUNDED`,
`PROVED`, and `PROVED-ASSUMING`. `CLEAN` is outside that set, so
`laws::is_proof` is false.

A pointer parameter stays `NEEDS-HARNESS` (Law 6). That gate returns
before the body runs. It was already landed and is unchanged.

## Test

`tests/cpp/test_concolic_proof.cpp` builds a scalar `FunctionInfo` the
same way `tests/cpp/test_concolic.cpp` fills one by hand: kind `SCALAR`,
name `scalar_ok`, parameter `int x`, body `return x;`. There is no
branch, and the call is `run_concolic` with budget 1, so the neighbour
loop does not ask the solver. The case runs on a build without Z3.

The single row is `CLEAN`, `laws::is_proof` is false, and the message
contains `not a proof`.

Registered in `CMakeLists.txt` next to `test_concolic.cpp`.
