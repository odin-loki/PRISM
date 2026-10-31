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
  lowers it as C++23 with `models/cxx/prism_dfp.hpp` force-included. The
  shim's classes implement IEEE 754-2008 decimal arithmetic (7/16/34
  digits, emax 96/384/6144) with round-half-even, infinities, NaN, x/0,
  mixed-width promotion, exact literal parsing, and an FP-CAST check on
  out-of-range decimal->integer conversion. Session 6 closed the items
  session 5 left out (written, compile-checked; host smoke run listed in
  FINAL_TESTS):
  - BID encoding: objects are the 4/8/16-byte IEEE 754 BID interchange
    bits (both coefficient forms, canonical inf/NaN, non-canonical
    coefficient reads as 0), so memcpy/union views and sizeof are real
    (host check: 0.1DD+0.2DD -> 0x31a0000000000003, 1DF -> 0x32800001).
  - Rounding modes: all five FE_DEC_* modes, fe_dec_getround/setround;
    every rounding step drops excess digits at once with a sticky digit
    (session 5's digit-by-digit rounding double-rounded, e.g. 149 -> 2;
    fixed). `#pragma STDC FENV_DEC_ROUND m` becomes a scoped mode in a
    block, a static initialiser at file scope.
  - `<math.h>`: the d32/d64/d128 functions listed in the shim header; the
    exact ones (rounding, quantum functions, sqrt, fmod, classification)
    are exact; transcendental ones go through long double.
  - printf/scanf `%H/%D/%DD` (e f g a, flags, width, precision, `*`):
    printf/fprintf/sprintf/snprintf/scanf/fscanf/sscanf calls in the unit
    become typed variadic templates; other conversions go to the C library.
  - C that is not C++: C++-only keywords used as identifiers are renamed,
    `restrict/_Bool/_Noreturn/_Alignas/_Alignof/_Thread_local` are mapped,
    and the implicit conversions C allows (void* -> T*, int -> enum) are
    made explicit from clang's own diagnostics (source ranges), up to 8
    rounds (stage.cpp `dfp_cxx_fixups`).
  - The shim and prism_fpfmt.h are embedded (CMake PRISM_PIR_MODELS) and
    written to the per-run model directory, which is now created for C
    runs too, so installed builds have them.
  Session 7 (written; header host-smoked, stage.cpp compile-checked):
  - Signed zero: decimal zeros keep their sign (IEEE 754 6.3): -0, x*-0,
    0/-x, x/-inf, exact x + -x (= -0 only under FE_DEC_DOWNWARD), rounding
    to zero, BID sign bit, signbit/copysign/fabs, printf '-0'.
  - printf %H/%D/%DD rounding of dropped digits honours the current
    FE_DEC_* mode (prism_fpfmt::round_to takes a mode and sign; directed
    modes step a nonzero tail, also below the first kept place).
  - The file-scope `#pragma STDC FENV_DEC_ROUND` is lexical: every
    function body after it (to the next file-scope pragma or the end of
    the unit) opens with a scoped mode; nothing before it is affected.
    FE_DEC_DYNAMIC at file scope ends the region. Remaining nuance:
    file-scope decimal initialisers after the pragma are evaluated by the
    shim at startup in the dynamic mode (default nearest), not the
    pragma's mode.
  - Fixed: the shim did not compile against glibc's <math.h> macros
    (signbit/isnan/... macros bypassed the decimal overloads); the macros
    are now #undef'd after <math.h>.
  Still open after session 7 (not yet written; next session):
  transcendental functions are not correctly rounded for _Decimal128
  (long double has ~19 digits; a correctly rounded 34-digit libm is a
  project of its own, as libdfp shows); v*printf/v*scanf with a va_list
  holding decimal arguments are not rewritten (the va_list erases the
  type); C constructs with no C++ spelling at all (K&R definitions,
  `[static N]` parameters in some positions, designated-initializer
  reordering, VLA typedefs) still fail and the error says the shim was
  tried; the file-scope pragma is dynamic (affects the whole program from
  startup) where C23 makes it lexical [closed in session 7, above].
  Session 8 (written; header and stage.cpp compile-checked only):
  - CLOSED (unverified): file-scope decimal initialisers after a
    file-scope FENV_DEC_ROUND use the pragma's mode: each top-level
    declaration after the pragma is bracketed by
    __PRISM_DFP_FILE_INIT_BEGIN(mode)/__PRISM_DFP_FILE_INIT_END static
    objects (declaration-order dynamic initialisation in one unit).
  - CLOSED (unverified): stage.cpp's file-scope brace counter and the
    compound-literal initialiser scan skip string/char literals and
    // and /* */ comments (block comments across lines).
  - CLOSED (unverified), item 5: vprintf/vfprintf/vsprintf/vsnprintf are
    rewritten to __prism_dfp_v*; the shim recovers each argument's type
    from the format (conversion + length modifier, %H/%D/%DD as decimal
    classes passed through `...`). v*scanf is still not rewritten.
  Still open after session 8: item 1 (exact sprintf bytes for symbolic
  doubles), item 4 (correctly rounded _Decimal128 transcendentals via a
  wider type and precision-raising loop), item 6 (K&R definitions,
  designated-initializer reordering, [static N]/VLA forms via a split C++
  file), item 8 (unwind-limit guards on the tiny-double exact-text loops),
  v*scanf with decimal pointers.
  Session 9 (written; header and stage.cpp compile-checked only):
  - CLOSED (unverified): v*scanf. vscanf/vfscanf/vsscanf are rewritten to
    __prism_dfp_v*scanf; one pointer per non-suppressed conversion is
    taken from the va_list (scansets skipped), typed decimal for
    %H/%D/%DD, and handed to the existing scanner.
  - CLOSED (unverified), item 8: prism_fpfmt exact() scales by 5^13 / 2^31
    per step, so the tiny-double loop is at most 83+1 iterations (was 1074)
    and the large-double loop 32+1 (was 971); the digit loop was already
    <= 89 steps of /1e9.
  Still open after session 9 (not written): item 1, item 4, item 6.
  Item 4 plan: the decw coefficient is __int128 (<= 38 digits), too narrow
  for a Ziv loop; it needs a base-1e9 limb fixed-point type, exp/log by
  argument reduction + Taylor series at W digits, W = 40, 60, 100, ...
  until rounding r-err and r+err to 34 digits agree; the other functions
  derive from exp/log/sin/cos with their own exact cases.
  Session 10 (written; compile-checked only):
  - CLOSED (unverified), item 6: when the shim fails, stage.cpp
    `dfp_split` cuts the unit at file scope (string/comment aware, K&R
    heads recognised): function definitions using a decimal type, literal
    or FENV_DEC (or calling such a function, to a fixpoint) go to
    <unit>.split.cpp lowered against the shim; the rest stays C (K&R,
    reordered designators, [static N], VLAs compile as C). Both sides get
    types and prototypes; non-decimal objects are defined on the C side
    and declared extern on the C++ side; the two IR modules are joined
    with llvm-link (next to clang). Limits: a file-scope static object
    used on both sides is duplicated; a multi-declarator initialised
    declaration becomes extern for its first declarator only; a decimal
    function whose own body needs a C-only construct still fails.
  - CLOSED (unverified), item 4: models/cxx/prism_dfp_mp.hpp, a
    base-1e9 multi-limb decimal float (no C++ library headers) and Ziv's
    loop (W = P+30, doubling to a cap; digits after P+1 minus a 12-digit
    margin and cancellation losses must be neither all 0 nor all 9;
    result rounded via a sticky digit by _S_make in the current FE_DEC
    mode). Correctly rounded for all three formats: exp, exp2, exp10,
    expm1, log, log2, log10, log1p, sin, cos, tan (reduction with
    exponent-sized pi), asin, acos, atan, atan2, sinh, cosh, tanh, cbrt,
    hypot, pow (finite x > 0). Exact cases are taken as exact at the cap
    (exp10 and log10 of powers of ten are exact up front); tiny arguments
    round x (or 1) nudged by the sign of the first dropped term.
    Still long double: erf, erfc, tgamma, lgamma; pow with x <= 0 or
    non-finite arguments (exact special values); tanh of huge |x| returns
    +-1 as exact (it is just inside 1: wrong under FE_DEC_TOWARDZERO /
    the mode toward zero).
  - CLOSED (unverified), item 1: sprintf/snprintf whose literal format has
    exactly one conversion, a non-literal double (%f/%e/%g/%a, any case,
    flags, width, precision; not '*' or the ' flag), is lowered to the new
    libc model __prism_fmt1d (models/libc/fmtdouble.cpp), which runs the
    prism_fpfmt algorithm on the symbolic value and writes the exact bytes
    and return value (snprintf truncation included). The model is linked
    whenever a printf-family function is declared; prism_fpfmt.h is
    written next to the libc models when they are lowered.
  Still open after session 10: the item-4 remainders above (erf, erfc,
  tgamma, lgamma on long double; tanh of huge |x| under toward-zero
  modes); item 1 with two or more conversions alongside a symbolic
  double, '*' width/precision, or %Lf (those keep the bounded havoc).
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

## Exact binary floating-point text (session 6)
`models/cxx/prism_fpfmt.h`: the full decimal expansion of the double
(bignum, no libc), half-even rounding, exact shortest round-trip decided
against the two rounding-interval midpoints, exact hex. Used by
std::format/format_to/format_to_n/formatted_size (all float types and
specs; `{}` and `{:e}`/`{:f}` without precision follow
[format.string.std]), by the decimal shim's printf, and by the
translator's printf planner (libc_format.cpp: a literal double argument
gets its exact text length). Host check: 20000 random doubles x 17 styles
against glibc snprintf and libstdc++ to_chars, 0 mismatches. Limit: the
translator's sprintf/snprintf model still writes havoc bytes bounded by
the planned length (the length is exact for literal doubles; non-literal
doubles keep the per-conversion bound). Making printf output bytes exact
for symbolic doubles needs the translator to run this algorithm
symbolically, which the bound-only printf model does not do.

## Items owned by prism-llvm
`import std;`, `<generator>`, the condition_variable concurrency check
and regex locales were handed to the prism-llvm branch (LLVM 23.1.4 bump,
started 31 Oct) and are not re-done here to avoid conflicting work; the
sections below describe the state on prism-cov until that branch merges.

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
- `constexpr` C23 objects and `#embed`: need clang >= 19; resolved by the prism-llvm bump (tasks written, compile with clang 23.1.4).
- A real concurrency proof for condition_variable: the model is single-threaded.
  `wait` returns at once (spurious wakeup, permitted), `wait(l, pred)` assumes
  pred() on return; a forever-false pred is a hang, not a checked path.
- regex under a non-"C" locale: imbue resets the regex per [re.regex.locale]
  but classification stays ASCII; `getloc` is not declared.
