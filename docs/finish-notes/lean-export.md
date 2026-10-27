# Lean export lines

Slice on `finish/lean-export-line`, started at `123a667c8`. The C++ Lean export in `src/prism/pir/export_lean.cpp` already emits `L hhavoc` for `__prism_havoc_bytes` and the six-field `L memcpy` line. The emitter was left as it is.

## `L hhavoc`

`model_line` writes a call of `__prism_havoc_bytes` when it has two operands, the first is a `ptr`, and the length is 64 bits:

```
L hhavoc <dst> <len>
```

`dst` is a pointer operand (`%name`, or `%@global` for an entry global). `len` is an operand (`%name` or `#bits`). That is `MemTr::write_havoc` in `translate.cpp`. `PrismRefine/XLlvm.lean` has no `hhavoc` instruction. `parseXLlvm` in `Check.lean` has no arm for the token, so the line is `bad line` and `checkX` reports `outside`.

## `L memcpy`

`llvm.memcpy` and `llvm.memmove` are six tokens after `L`: destination, source, length, the integer width of the length, and the memmove flag (`1` for `llvm.memmove`, `0` for `llvm.memcpy`).

```
L memcpy <dst> <src> <len> <width> <move>
```

`Check.lean` parses that shape and fills `selfOk` with true:

```
| ["memcpy", d, sp, n, lw, mv] =>
    .memcpy (← opnd? d) (← opnd? sp) (← opnd? n) (← nat? lw) (mv == "1") true
```

`translate.cpp` calls `memcpy_` with `self_ok` true for both intrinsics. `proofs/refinement/fixtures/intrinsics.pirl` uses the same line:

```
L memcpy %a %@__const.copy_ok.a #16 64 0
L memcpy %arraydecay %arraydecay1 #16 64 0
L memcpy %p %a %n 64 1
```

The last line is `llvm.memmove`. `tests/cpp/test_main.cpp` already expects `L memcpy %p %a %n 64 1`.

`__prism_memcpy` keeps a seventh field, `selfOk`, written as `0`:

```
L memcpy <dst> <src> <len> <width> <move> 0
```

The longer parser arm reads that flag with `(so == "1")`. `translate.cpp` calls `memcpy_` for `__prism_memcpy` with the default `self_ok` false (the fourth operand is only the memmove flag). `proofs/refinement/fixtures/heap.pirl` has `L memcpy %call9 %p %cond 64 0 0`. The six-field arm would set `selfOk` true, so the `0` stays.

## Left unchanged

`export_lean.cpp` was not edited. `proofs/`, `stage.cpp`, and tests were not edited.
