# Lean slice: `__prism_obj_size`

Branch `finish/lean-hobjsz`, started at `e59a8b3df966e33c78ffbd9ced0a8fc90465b16d`
(`git merge --ff-only` of that commit). The five files of
`origin/cursor/hobjsz-lean-proof-05dd` (`aa645290bc2aa5e2f9c6bff282c9f647400552a6`)
were checked out; `docs/ENGINEERING_BACKLOG.md` and `docs/ROADMAP_STATUS.md`
were not.

`lake` is not on PATH and `~/.elan` has no toolchain, so nothing here was
kernel-checked. The notes below are a static review against Lean 4.34
(`proofs/lean-toolchain`, `BitVec.toNat_sub`).

## Proved (no `sorry`, no `admit`)

`sinstX_sim` for `SInst.hobjsz` (result register and none) no longer ends in
`admit`. `XHeap.lean`, `XLlvm.lean`, `XMemSim.lean`, and `XRefine.lean` contain
no `sorry` and no `admit`.

- `sub64_ge`: `binVal .sub 64` is Nat subtraction when both operands are
  `< 2^64` and the second is `≤` the first. Lean 4.34's `BitVec.toNat_sub` is
  `((2^n - y.toNat) + x.toNat) % 2^n`, so the non-wrapping case is
  `Nat.add_mod_left`, not `omega` on a modulus.
- `hobjszMachine`: the select `hobjszCore` stores, after the destination-width
  modulus, is `hobjszVal` (`0` when the offset is past the object, otherwise
  `size - offset`).
- `hobjszCore_run`: the five temporaries (`obj.size`, mask `2^48-1`, `sub`,
  `ugt`, `select`) store that value and leave cells below `k` unchanged.
- `hobjszVal_lt`: the value fits in 64 bits.
- `sinstX_sim` for `.hobjsz`: operand temps, then `hobjszCore_run`; a
  destination is one `copy`. `Except.ok` is injected before `obtain rfl`, and
  the select temporary's width is `64`.

## Checker expectations

`fixtures/heap.pirl` has 12 `func` records. The previous summary
(`agree-ext=15 outside=2`) counted five `new`/`delete` records D118 removed
and two `realloc` records that were outside before `hobjsz` was in the
checker. The summary kept here is:

`summary agree=0 agree-ext=12 agree-reject=0 outside=0 mismatch=0`

`mismatch` stays 0. `realloc_ok` and `realloc_stale_bad` contain `L hobjsz`
whose C++ PIR is `obj.size`, `and` with `2^48-1`, `sub`, `ugt`, `select`,
`copy`, which is `hobjszCore`. This was not re-run. If those two functions
are still outside, the honest summary is `agree-ext=10 outside=2`.

## Still outside (no new `admit`)

D136 `__prism_havoc_bytes` is still outside. `export_lean.cpp` already writes
`L hhavoc <ptr> <len>` for a 64-bit length. `MemTr::write_havoc` zero-extends
a shorter length, runs the write access checks at alignment 1 guarded by
`n ≠ 0`, allocates `n` bytes of kind `Extern` (7), init 2, alignment 16,
`memcpy`s them onto the destination, and ends the temporary's lifetime.

Making that agree needs a new `SInst.hhavoc`, a `Check.lean` line, `trSInstX`
in `XTranslate.lean`, both matches in `XLazy.lean`, and a simulation the size
of the existing access-check and `memcpy` proofs. The translator files are
outside this slice. Adding the constructor without them does not compile, so
no `admit` was added and the instruction stays outside. `export_lean.cpp` was
not edited: the line format already matches what a `Check.lean` clause would
parse.

`MemKind::File` in `hfree`, `__prism_read_range`, and `__prism_check` are
unchanged.

## Commands for the parent

From a machine with the pinned toolchain:

```bash
cd proofs/refinement && lake build
cd proofs/refinement && ./check.sh
```

`./check.sh` fails on any warning or `sorry`, then runs the axiom audit and
compares fixture summary lines.

Heap fixture only, after `prism` and `prism-qa` are built off OneDrive (WSL
ext4 or `C:\Temp`):

```bash
prism-qa pir-lean-check proofs/refinement/fixtures/heap.pirl --bin <build>/prism
```

The summary line must stay `mismatch=0`. If `realloc_*` is still outside,
set the fixture to `agree-ext=10 outside=2`.

Full corpus, same binary:

```bash
prism-qa pir-lean-check testdata/pir testdata --bin <build>/prism
```
