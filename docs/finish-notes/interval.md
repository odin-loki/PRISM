# interval

The interval stage is not a proof. A range that may overflow, divide by zero, or shift out of range is `FAILED`. Silence is not `PROVED`, `BOUNDED`, or `CLEAN`. Nothing in this slice emits `PROVED`.

## Empty initialiser

`decl` tells a missing initialiser from an empty one.

- `int x;` has no `=`. The name is the full range of its type.
- `int x =` (nothing after the equals) reaches `decl` as an empty initialiser string. That is the integer range `[0,0]`, the same constant an empty expression stands for. `for (int x = ;;)` uses the same path.

`1 / x` is then `INT-DIV-ZERO`. `1 / (x - 1)` is not: `x - 1` is `-1`. A skipped function would be silent on the first. The full range of a missing initialiser would alarm on the second.

## Literals that do not fit in int64

Decimal, leading-0 octal, and `0x` / `0X` hex tokens are parsed with `std::from_chars`. `std::stoll` and `std::stoull` are not called, so neither can leave the stage.

A value that does not fit in `uint64_t`, or that fits in `uint64_t` but not in `int64_t`, is `ParseFail("64-bit literal unencoded")`. The stage catches `ParseFail` and stays silent. The literal is not the value 0, so a later definite `1 / 0` in the same expression is not reported. A literal that does fit (`0x7FFFFFFFFFFFFFFF`, `0X10`, octal `010`) still reaches that division, which is `FAILED`.

A bad digit such as octal `08` stays `ParseFail("nud ...")`. That is also silence, not 0.

`0X` is one token, same as `0x`. It used to split into the integer 0 and an identifier, so `1 / 0X10` was a division by zero.

Binary `0b` literals are unchanged. The concrete interpreter parses them; this slice does not.

`docs/CPP_PORT_PLAN.md` still says `decl` treats an empty initialiser as a missing declaration via `init.empty()`, and that `std::stoll` throws `ParseFail("nud ...")` on a hex literal above `INT64_MAX`. That description is stale.

## Tests

`tests/cpp/test_interval.cpp`

- `interval: `int x =` with an empty initialiser reads as 0; huge literals are not a crash` — empty `int x =` is not the full range; a huge literal does not throw out of `run_interval`.
- `interval: empty initialiser is [0,0]; a literal past int64 is unencoded, not 0` — `[0,0]` in a statement and in `for`; hex, octal `2^63`, and a decimal past `uint64_t` do not become 0; fitting literals still report `1 / 0`; `08` does not become 0. `FAILED` rows are not proofs.
