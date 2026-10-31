# Coverage: audit of coroutine/format/flat_map/flat_set/generator, modules, and hard limits

Written on prism-cov, code-first; nothing here has been run yet (see
/workspace/PRISM-cov-FINAL_TESTS.md).

## Audit (session 3)
- `<coroutine>`: handles, from_address, equality, noop handle. resume/destroy/
  done on a non-noop handle `__prism_check` + trap: the pir cannot run a
  compiler-built frame, so a model cannot resume it. Programs that resume
  coroutines fall back to libstdc++ -> NEEDS-HARNESS. Gap kept; honest.
- `<generator>` (C++23): **no model file exists** (re-checked session 4: a
  minimal model is not feasible; every member that does work, begin() and
  iterator increment, resumes the coroutine frame, so it could only trap,
  which gives the same NEEDS-HARNESS as the fallback with more code to trust). std::generator is built on
  coroutine frames, so the same limit applies; a model would have to trap on
  the first iteration. Not written: libstdc++ 14's <generator> is used as is,
  and the unit is reported in `extra.cxx_models` as a fallback.
- `<format>`: subset of integer/string replacement fields; unsupported specs
  fail to compile against the model (fallback), never print fake text.
  Session 4 (filled, unverified): `format_to_n` for double/float with `{}`,
  `{:f}`, `{:.Nf}`, `{:e}`, `{:.Ne}`, `{:g}`, `{:+...}` (precision <= 99).
  The digits are NOT computed: characters are havoc and the untruncated size
  is havoc within a per-spec bound (e.g. `{:.Nf}` <= 1+309+1+N), so bounds
  safety is checkable but a property that depends on the digits is not
  provable. chrono::duration `{}`: exact count + [time.format] suffix.
  Ranges of int/long long with `{}`, `{::}`, `{:n}`: exact text.
  Session 5 (written, unverified): `std::format`, `format_to` (any output
  iterator), `formatted_size`, `vformat` with no arguments, returning
  `std::string`. Arguments: bool, char, all integers, float/double, const
  char*, string_view, string. Fields `{}`/`{N}`/`{:spec}`, `{{`/`}}`. Spec:
  fill/align (`<`,`>`,`^`), sign, `#` (0b/0B/0/0x/0X prefix; for floats a
  forced point), `0`, width, `.prec` (strings truncate), `L`, types
  d b B o x X c s and a A e E f F g G. Integer/string/bool/char text is exact
  (width/fill exact too). `L` is exact only for the "C" global locale (the
  default; `std::locale::global` is not modelled). Floating text including
  `a` stays havoc within a per-spec length bound (`a`: <= 22 chars, no
  "0x", as [format.string.std]). Still not modelled: nested `{}`
  width/precision (FUNC-CONTRACT), compile-time format-string checking
  (errors appear as FUNC-CONTRACT at analysis), chrono calendar types,
  user `formatter` specialisations, wide strings.
- `<flat_map>` / `<flat_set>`: covered (finish-notes flat-map*.md, flat-set*.md).
  Session 4 (filled, unverified): `Compare` template parameter (default
  `less<Key>`), `sorted_unique` and container constructors (the
  sorted-unique precondition is CHECKED as FUNC-CONTRACT, not trusted),
  `extract() &&` returning `vector` key/mapped containers sorted by key,
  `replace`, and `contains`/`count`/`at` taking any key when
  `Compare::is_transparent`. Session 5 (written, unverified): iterator /
  const_iterator in key order (pair-of-references value, structured
  bindings work), begin/end/cbegin/cend, `find` (and transparent find),
  `erase(iterator)`; dereferencing or incrementing end() is FUNC-CONTRACT.
  Still no insert ranges or custom key/mapped container types.

## Session 4 cost cuts (unverified)
- unordered_map/set copy: keeps the source bucket count (as libstdc++ does)
  so a copy allocates buckets once instead of rehashing 13/29/59/...; unique
  tables link nodes without the duplicate lookup and reuse the cached hash.
- ostringstream integer output: digits into a stack buffer, no std::to_string
  temporary (one heap block per `<<` less).

## Session 4 C coverage (unverified)
C99 `_Complex`, `[static N]` params, snprintf, `_Pragma`; C11 `alignof`,
aligned_alloc, `*_explicit` atomics + fences, CMPLX; C17 aligned_alloc with any
size and an unsupported alignment (NULL); C23 memccpy, strndup, free_sized,
memalignment, `__VA_OPT__`, `alignas`/`thread_local` keywords,
`__has_c_attribute`. New models: aligned_alloc, free_sized,
free_aligned_sized, memalignment (any power of two; address not modelled),
memccpy, strndup. C coverage is now complete apart from the blocked items
below.

## Session 5: C23 decimal floating point and storage-class compound literals
- `_Decimal32/64/128`: checked clang 18.1.8 and 23.1.4 (the newest in
  /workspace/opt): both reject the types ("GNU decimal type extension not
  supported"), and LLVM IR has no decimal type. GCC lowers them to libgcc
  `__bid_*` calls but PRISM consumes LLVM IR, so GCC is no route. Route
  implemented (unverified): when a C unit fails with that diagnostic, the
  pir stage (stage.cpp `dfp_rewrite`) rewrites DF/DD/DL literal suffixes to
  user-defined literals, wraps the unit in `extern "C"` (C symbol names;
  `#include` lines stay outside), keeps `#line` to the original file, and
  lowers it as C++23 with `models/dfp/prism_dfp.hpp` force-included. The
  shim's classes implement IEEE 754-2008 decimal arithmetic (7/16/34
  digits, emax 96/384/6144) with round-half-even, infinities, NaN, x/0,
  mixed-width promotion, exact literal parsing, and an FP-CAST check on
  out-of-range decimal->integer conversion. Precisely what remains out:
  rounding modes other than FE_DEC_TONEAREST, the `<math.h>` quantum
  functions and `*d32/*d64/*d128` math library, printf/scanf `%H/%D/%DD`,
  the BID/DPD bit layout (memcpy/union punning sees the shim's layout), and
  C units that are not valid C++ (implicit `void*` conversion, K&R
  definitions, `restrict`, VLAs in some forms): those still fail and the
  error says the shim was tried. Subnormal precision loss is approximated.
- C23 storage-class compound literals (N3038, `(static T){..}`,
  `(constexpr T){..}`, `thread_local`, `register`): clang 23.1.4 still
  rejects all of them, so the LLVM bump alone does not fix this. Route
  implemented (unverified): stage.cpp `c23_compound_literal_rewrite`
  turns a block-scope literal into a GNU statement expression over a static
  object (`(*({ static T __prism_clN = {..}; &__prism_clN; }))`, an lvalue
  of type T with static storage), and drops the specifier at file scope.
  constexpr without static becomes static constexpr: same value and
  constness; only address identity across evaluations differs, which
  6.5.2.5 leaves unspecified for const literals. `constexpr` object
  declarations need clang 19+, so the constexpr tasks depend on prism-llvm.

## Modules (`import std;`)
Not supported, and not fixable inside the model directory: the models are
headers placed first on the include path. `import std;` loads a prebuilt
BMI of the real library and never reads an include path, so no model is
used; and PRISM's IR front end (clang 18) needs a matching `std.cppm` BMI
built per configuration. A unit using `import std;` must keep falling back
(NEEDS-HARNESS / ERROR row), not be reported PROVED. Revisit with the clang
19+ bump (pin-and-vendor step): it would need a modelled `std.cppm` that
re-exports the model headers.

## Truly blocked
- `constexpr` C23 objects and `#embed`: need clang >= 19 (see the clang 19 plan).
- A real concurrency proof for condition_variable: the model is single-threaded.
  `wait` returns at once (spurious wakeup, permitted), `wait(l, pred)` assumes
  pred() on return; a forever-false pred is a hang, not a checked path.
- regex under a non-"C" locale: imbue resets the regex per [re.regex.locale]
  but classification stays ASCII; `getloc` is not declared.
