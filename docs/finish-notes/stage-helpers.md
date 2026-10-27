# Stage helpers

Slice on `finish/stage-helpers`, started at `e59a8b3df`. Three gaps from the
`prism/contracts.py` and `prism/concrete.py` rows of `docs/CPP_PORT_PLAN.md`.
The appendix still describes the audit at `911b9f94f`. On this tree the first
and third are already closed in the engine; the decreases baseline is left
on purpose.

## Case-insensitive clause keywords

Python `_COMMENT_CLAUSE` and `_ACSL_KEYWORD` compile with `re.I`. The C++
patterns in `parse_comments` / `parse_acsl_body` (`src/prism/stages/common.cpp`)
already carry a leading `(?i)`:

- `^(requires|ensures|invariant|decreases|assigns)\s+(.+)`
- `(?://|/\*|\*)\s*(requires|ensures|invariant|decreases|diff)\s*:...`

`prism::Regex` compiles with `PCRE2_UTF | PCRE2_UCP` only. `(?i)` is an inline
option on these two patterns. It is not `PCRE2_CASELESS` on the `Regex`
constructor, so the other patterns in this file stay case-sensitive. The
comment marks and the captured value contain no cased letters, so a
keyword-only group would match the same text. The captured keyword is folded
with `lower_copy` before it is stored. Locked by
`tests/cpp/test_contracts.cpp` (`contract_case.c`: `Requires` / `ENSURES`,
and ACSL `Requires` / `Ensures`).

## Decreases baseline

Left as written:

```cpp
bmc_with_assume(fn, unwind, requires_, ensures, std::nullopt, invariant);
```

Python is `bmc_function_with_assume(..., decreases=None)`. `invariant` is the
next parameter and defaults to `None`, so that baseline drops the invariant
as well as the decreases clause. Requires and ensures are still passed.

C++ passes the invariant through. That is the check minus only decreases.
It is not a default decreases value that Python omitted: decreases is
`std::nullopt` on both sides.

Keeping the invariant does not raise a proof and does not drop the decreases
check.

- The main query still runs `bmc_with_assume` with the decreases clause, and
  still instruments it when the measure encodes.
- `bmc_with_assume` rewrites a proof to `PROVED-ASSUMING` whenever an
  invariant (or a requires) is present. The baseline therefore cannot come
  back `PROVED-UNBOUNDED` while the invariant is set, so `closed_without`
  stays false.
- `decreases_assumed` is then true only when the main result's original
  status is already a proof, and the only status change is
  `PROVED-UNBOUNDED` to `PROVED-ASSUMING`.

Passing `std::nullopt` for the invariant would copy Python's default. It
would not repair a wrong proof or a skipped check. It would only let
`decreases_assumed` become false in the case where the unbounded baseline
exists because the invariant was omitted. The flag is allowed to lower a
status. It is not a reason to delete a clause from the baseline.

No planted function shows the flag flipping a verdict the other way. The
status tests in `tests/cpp/test_contracts.cpp` (`countdown`,
`countdown_complex`, `countdown_lin`) stay on the C++ side of D126: a linear
measure is checked, and a proof that uses a variant is not
`PROVED-UNBOUNDED`.

## 64-bit concrete arguments

`decode_args`, `Args`, `St`, and `execute` are already 64-bit on this path.
At the audit, `decode_args` returned `std::map<std::string, int>` and stored
`static_cast<int>(raw)` for `sz >= 8`. Now:

- `Args` is `std::map<std::string, int64_t>` (`common.hpp`).
- An 8-byte parameter keeps `raw` (an unsigned value as its bit pattern).
  Narrower parameters still go through `i32`.
- `St` stores `int64_t` and `norm` leaves a width of 64 unchanged.
- `execute` / `eval_cond` take `Args`.

`interesting_seeds` packs each slot at its own width and adds the 64-bit
extremes when any parameter is 8 bytes. Locked by
`tests/cpp/test_concrete.cpp` (`decode_args: 8-byte parameters keep their
full value`, `add_ll` overflow from those bytes).

Not widened here:

- `concrete_execute` in `include/prism/stages.hpp` still takes
  `std::map<std::string, int>` and returns `rc` as `int`. That header is
  outside this slice. The soundness oracle only feeds it 32-bit edges.
- Concolic seeds still narrow with `i32` (`concolic.cpp`). The port plan
  calls that 32-bit on purpose, and that file is outside this slice.
- BMC symbolic widths are unchanged.

## Left outside this slice

No edits to `fuse.cpp`, `concolic.cpp`, `execute_cex.cpp`, `bmc.cpp`,
`pipeline.cpp`, `memory.cpp`, `encode.cpp`, `CMakeLists.txt`, or
`third_party/`. Source cache, report-shape `extra` types, and the speed-only
notes in the same port-plan cells are untouched.
