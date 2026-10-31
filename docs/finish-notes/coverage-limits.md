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
  Ranges of int/long long with `{}`, `{::}`, `{:n}`: exact text. Still not
  modelled: std::format / format_to (std::string results), width/fill/align,
  `#`, `a`, `L`, chrono time points and calendar types, nested element specs.
- `<flat_map>` / `<flat_set>`: covered (finish-notes flat-map*.md, flat-set*.md).
  Session 4 (filled, unverified): `Compare` template parameter (default
  `less<Key>`), `sorted_unique` and container constructors (the
  sorted-unique precondition is CHECKED as FUNC-CONTRACT, not trusted),
  `extract() &&` returning `vector` key/mapped containers sorted by key,
  `replace`, and `contains`/`count`/`at` taking any key when
  `Compare::is_transparent`. Still no iterators, find, insert ranges, or
  custom key/mapped container types.

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
below and `_Decimal*` (clang has no decimal floating point) and C23
`static`/`constexpr` storage in compound literals (clang 19+).

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
