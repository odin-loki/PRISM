# Vendored source trees

Copies of upstream **source** (not pip/vcpkg, not git submodules, not
release binaries). Each tree is a shallow snapshot with `.git` removed
so it lives in this repository.

Already in tree (build-linked): `xsimd`, `z3`, `pcre2`, `nlohmann`,
`doctest`, `llama.cpp`.

Mined / adapter projects (source for algorithms and local builds):

| Directory | Upstream |
|-----------|----------|
| `esbmc` | https://github.com/esbmc/esbmc |
| `FuSeBMC` | https://github.com/kaled-alshmrany/FuSeBMC-1 |
| `cbmc` | https://github.com/diffblue/cbmc |
| `klee` | https://github.com/klee/klee |
| `AFLplusplus` | https://github.com/AFLplusplus/AFLplusplus |
| `Frama-C` | https://github.com/Frama-C/Frama-C-snapshot |
| `infer` | https://github.com/facebook/infer |
| `codeql` | https://github.com/github/codeql |
| `cppcheck` | https://github.com/danmar/cppcheck |
| `strix` | https://github.com/meyerphi/strix |
| `semgrep` | https://github.com/semgrep/semgrep |
| `coccinelle` | https://github.com/coccinelle/coccinelle |
| `dafny` | https://github.com/dafny-lang/dafny |
| `Fuzz4All` | https://github.com/fuzz4all/fuzz4all |
| `rapidcheck` | https://github.com/emil-e/rapidcheck |

LLVM/clang-tidy and Qt are not copied (full trees are multi-gigabyte
toolchains). Missing PATH binaries still report `NOTRUN`.
