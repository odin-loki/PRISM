# Contracts gap

Slice on `finish/contracts-notrun`. `prove_contracts` in
`src/prism/stages/contracts.cpp` already writes a row when a function has a
contract and the stage cannot check it. This slice does not change that
behavior.

## Pointer parameter

A function whose kind is `POINTER`, whose body needs a pointer harness, or
whose kind is not `SCALAR` is `NEEDS-HARNESS`, class `FUNC-CONTRACT`, before
BMC. The pointer message is `POINTER: contract BMC would invent a buffer;
not a proof`. The requires, ensures, decreases, and invariant text is kept
on the finding. `prove_with_contract` uses the same kind and body gate.

A pointer parameter with no contract still produces no contracts row. The
unguarded check belongs to BMC, which is outside this slice.

## Missing backend

A scalar contract is passed to `bmc_with_assume`, which calls `bmc_one`.
A proof status with a requires or an invariant is rewritten to
`PROVED-ASSUMING`. `NOTRUN` is not a proof, so it stays `NOTRUN`, the
function name is filled in, and the finding is appended. A build without
Z3 makes `run_bmc` return one `NOTRUN` row (`"z3 not built"`,
`extra["install"]` naming `-DPRISM_Z3=ON`). That row is the contracts
result. It is not `PROVED`, `PROVED-ASSUMING`, `PROVED-UNBOUNDED`, or an
empty list.

## Tests

`tests/cpp/test_contracts_gap.cpp`, registered in `CMakeLists.txt` next to
`tests/cpp/test_contracts.cpp`. The older file is unchanged.

- `ptr_contract(int *p)` with `requires` and `ensures`, body `return 0`, is
  `NEEDS-HARNESS` on every build. `ptr_plain` has no contract and adds no
  row. The status is not `NOTRUN` and not a proof.
- `held(int x)` with a contract, beside `bare` which has none, is `NOTRUN`
  when the binary is built without Z3. The list has that one row. The
  `original_status` is `NOTRUN`. Builds with Z3 skip that case
  (`PRISM_HAS_Z3`), the same shape as `tests/cpp/test_bmc_harness.cpp`.

## Left alone

`contracts.cpp` was not edited. Also left alone: `harness_bmc.cpp`,
`bmc.cpp`, `pipeline.cpp`, `stage.cpp`, and `tests/cpp/test_contracts.cpp`.
