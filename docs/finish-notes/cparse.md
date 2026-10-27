# cparse parameters

`split_params` canonicalises each parameter, then takes the name as the last
word of that same spelling. `const`, `volatile`, `restrict`, `register`,
`__restrict` and `__restrict__` are dropped as whole words (so `constant_t`
stays a word). The cut does not use an index from a copy that inserted
spaces around `*`.

| source | type | name |
|---|---|---|
| `const int *p` | `int *` | `p` |
| `int *const q` | `int *` | `q` |
| `volatile unsigned long n` | `unsigned long` | `n` |
| `const int *out` | `int *` | `out` |
| `int *restrict out2` | `int *` | `out2` |
| `register int r` | `int` | `r` |

`kind_of` classifies a parameter as `POINTER` when the type contains `*` or
`[` before it drops those qualifier words. A const, volatile, restrict or
register scalar stays `SCALAR`, including `const` and `int` separated by a
tab. A pointer such as `int *const p` or `const int *p` stays `POINTER`.

Doctest cases in `tests/cpp/test_cparse.cpp`:

- `cparse: qualified parameters keep type and name`
- `cparse: const and volatile scalars stay scalar`
