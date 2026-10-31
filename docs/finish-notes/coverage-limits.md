# Coverage: audit of coroutine/format/flat_map/flat_set/generator, modules, and hard limits

Written on prism-cov, code-first; nothing here has been run yet (see
/workspace/PRISM-cov-FINAL_TESTS.md).

## Audit (session 3)
- `<coroutine>`: handles, from_address, equality, noop handle. resume/destroy/
  done on a non-noop handle `__prism_check` + trap: the pir cannot run a
  compiler-built frame, so a model cannot resume it. Programs that resume
  coroutines fall back to libstdc++ -> NEEDS-HARNESS. Gap kept; honest.
- `<generator>` (C++23): **modelled on prism-llvm** (models/cxx/generator), on a
  rewritten `<coroutine>` model whose resume/destroy/done/promise use the
  clang coroutine builtins (coro-split lowers them to calls the pir follows).
  `ranges::elements_of` and allocator generators still fall back. Old note: std::generator is built on
  coroutine frames, so the same limit applies; a model would have to trap on
  the first iteration. Not written: libstdc++ 14's <generator> is used as is,
  and the unit is reported in `extra.cxx_models` as a fallback.
- `<format>`: subset of integer/string replacement fields; unsupported specs
  fail to compile against the model (fallback), never print fake text.
  Remaining gaps: floating-point specs (`{:.3f}`, `{:e}`), chrono, range
  formatting (C++23 `{::}`), `format_to_n`. Not filled this session.
- `<flat_map>` / `<flat_set>`: covered (finish-notes flat-map*.md, flat-set*.md).
  Remaining gaps: `sorted_unique` constructors, `extract`/`replace`,
  heterogeneous lookup. Not filled this session.

## Modules (`import std;`)
**prism-llvm:** models/cxx/std.cppm is a modelled `std` module (global module
fragment includes the model headers; `export using` the modelled names).
`source_imports_std` and `build_cxx_std_module` (libc_models.cpp) detect the
import and precompile it; lower_to_ir must add `-fmodule-file=std=<pcm>` -
that one call is in stage.cpp and is left for the cohesion merge. Old note:
Not supported, and not fixable inside the model directory: the models are
headers placed first on the include path. `import std;` loads a prebuilt
BMI of the real library and never reads an include path, so no model is
used; and PRISM's IR front end (clang 18) needs a matching `std.cppm` BMI
built per configuration. A unit using `import std;` must keep falling back
(NEEDS-HARNESS / ERROR row), not be reported PROVED. Revisit with the clang
19+ bump (pin-and-vendor step): it would need a modelled `std.cppm` that
re-exports the model headers.

## Truly blocked
- ~~`constexpr` C23 objects and `#embed`~~: unblocked on prism-llvm (LLVM 23.1.4 pin); tasks
  c23_constexpr_* and c23_embed_* added (not yet run).
- **prism-llvm:** condition_variable is now checked over thread interleavings
  (bits/prism_sched.h, new <thread> model, mutex lock/lock_guard/unique_lock):
  threads queue at construction and run at solver-chosen sync points; a wait
  nothing can satisfy is CXX-DEADLOCK. Bounded: run-to-completion bodies, 8
  threads, depth 4 (past a bound = NEEDS-HARNESS). Old note:
- A real concurrency proof for condition_variable: the model is single-threaded.
  `wait` returns at once (spurious wakeup, permitted), `wait(l, pred)` assumes
  pred() on return; a forever-false pred is a hang, not a checked path.
- **prism-llvm:** regex imbue now takes \d \w \s from the locale's ctype<char>
  facet, keeps the locale (getloc, previous locale, assign). [[:class:]], icase,
  collation still unmodelled. Old note:
- regex under a non-"C" locale: imbue resets the regex per [re.regex.locale]
  but classification stays ASCII; `getloc` is not declared.
