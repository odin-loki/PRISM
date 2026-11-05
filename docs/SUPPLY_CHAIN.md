# Supply chain

Every dependency PRISM compiles, links or runs is pinned and hashed. Linked
code is licence-checked. A release can be rebuilt bit for bit.

## Dependencies

| Dependency | Kind | Pin | Where |
|---|---|---|---|
| Z3 4.13.4, PCRE2 10.44, nlohmann/json 3.11.3, doctest 2.4.11 | linked, vendored | commit + archive and tree SHA-256 | `third_party/` and `third_party/MANIFEST.toml` |
| FORGE (odin-loki/FORGE) | linked, mandatory, not vendored | branch port/cipin (landing): full commit SHA in `cmake/FORGE_PIN`. On main (991c4eafa): unpinned, tracks `main` | `FORGE_SOURCE_DIR`, else fetched by `cmake/forge.cmake` |
| LLVM/clang 23.1.4 | toolchain (compiler, `pir` front end, libclang) | tag + SHA-256 per platform | `third_party/llvm/PIN.toml`, fetched by `scripts/fetch_llvm.py` |
| CMake 3.31.6, Qt 6.8.3 qtbase, CUDA 12.6, Lean 4 v4.34.0, Linux sysroot, lint tools | toolchain | version + SHA-256 | `third_party/TOOLCHAIN_PINS.toml`, fetched by `scripts/fetch_toolchain.py NAME` |
| ESBMC, CBMC, KLEE, cppcheck, Frama-C, Infer, Semgrep, Coccinelle, AFL++, Dafny, Strix, CaDiCaL, Kissat, Bitwuzla, cake_lpr, lean4export, nanoda | external processes | commit + archive SHA-256 | `MANIFEST.toml` `kind = "external"`, built by `prism-deps tool NAME` |
| clang-tidy, gcc, bwrap, the polyglot linters | host tools | not pinned in the manifest; findings record the binary that ran | `MANIFEST.toml` `kind = "system"` |
| Lean libraries | proofs | `lean-toolchain` and `lake-manifest.json` revisions | `proofs/` |
| Docker base image | build image | `ubuntu:24.04` by digest | `Dockerfile` |
| Language model | data, never committed | a FORGE `.fpk` file the user provides | [AI.md](AI.md) |

A download whose SHA-256 differs from its pin is refused.

The only tracked Python outside `third_party/` is the CI bootstrap
(`scripts/fetch_llvm.py`, `scripts/fetch_toolchain.py`, which run before any
build exists) and the BenchExec plugin `tools/svcomp/prism.py`.
`tests/cpp/test_deps.cpp` ("deps: tracked .py files match the phase-5
allow-list") fails on any other tracked `.py`.

## FORGE

FORGE is the platform layer (logging, processes, thread pool, settings) and
the language-model engine. It is linked and mandatory but not vendored.

Branch port/cipin (landing) pins it to one full commit SHA in
`cmake/FORGE_PIN`, the single source of truth:

- `cmake/forge.cmake` reads the file. When `FORGE_SOURCE_DIR` is empty it
  fetches that commit with `git fetch --depth 1` into
  `build/_deps/forge-src` and checks that `HEAD` is the pin. With
  `-DFORGE_SOURCE_DIR=PATH` it builds that checkout and warns when its `HEAD`
  is not the pin.
- Every workflow that builds PRISM reads the file and checks out FORGE at
  that SHA.

Bumping the pin is a one-line reviewed change:

```
scripts/bump_forge_pin.sh          # the current FORGE main SHA
scripts/bump_forge_pin.sh <sha>    # or an exact commit
git commit cmake/FORGE_PIN
```

## MANIFEST.toml

One `[[component]]` per dependency:

- `kind = "linked"`: compiled into PRISM from `path`. Records `tag`,
  `commit`, `archive_sha256`, `tree_sha256` (digest of the in-tree copy),
  `tree_version_*` (the version marker in the source) and `drift` (files
  where the in-tree copy differs from upstream).
- `kind = "external"`: a tool PRISM runs as a separate process, pinned to an
  upstream release commit.
- `kind = "system"`: host tools. Not pinned; findings still record which
  binary ran.
- `[[mined]]`: the source trees PRISM's techniques came from. The trees are
  deleted; the rows keep their URLs and licences ([MINED.md](MINED.md)).

`archive_sha256` is the SHA-256 of the uncompressed tar that
`git archive --format=tar --prefix=<name>-<commit>/ <commit>` produces after
`git fetch --depth 1 <url> <commit>`.

## prism-deps

`prism-deps` (`src/tools/prism_deps/`) is a standard-library-only C++ tool.
It uses the `git` CLI for fetches. It fails closed: any commit, hash or
version mismatch exits non-zero (3 for a hash mismatch) and installs
nothing.

```
build/prism-deps list
build/prism-deps linked              # offline: version marker + tree digest
build/prism-deps linked --refetch    # also fetch each pinned archive and compare
build/prism-deps tool cadical kissat cake_lpr
build/prism-deps licence-check
build/prism-deps sbom --version vX.Y.Z -o prism.cdx.json
```

`tool NAME` fetches the pinned commit, checks `archive_sha256` and installs
into `~/.prism/tools/<name>/<commit>/` (root: `PRISM_TOOLS_DIR`), with
`src/`, `bin/` and `PRISM-TOOL.json`. It stages everything in a temporary
directory and renames it into place only when every step passed. CaDiCaL,
Kissat and cake_lpr have build recipes. For the other tools it unpacks the
checked source and prints the build hint from the manifest.

## How PRISM finds and records tools

Search order (`src/prism/config.cpp`):

1. `--tool NAME=PATH`.
2. `~/.prism/tools/<component>/<pinned commit>/bin/<exe>`, only for the
   commit the manifest pins. CMake writes the pins into the generated header
   `prism/manifest_pins.hpp`, so a binary trusts only the builds its own
   manifest names.
3. `PATH`.

A missing tool is `NOTRUN` with the hint
`prism-deps tool <component> (pinned in third_party/MANIFEST.toml)`. A
tools directory inside the scanned tree is ignored unless `--allow-exec` is
given (Law 9).

Every finding an external tool produced carries `extra["tool_sha"]`: the
manifest commit for a binary under `~/.prism/tools/<name>/<commit>/`, else
`path:<absolute path>;sha256:<SHA-256 of the binary>`.

## Licence firewall

`prism-deps licence-check` runs in CI and in the Docker build. It fails when
a `linked` component carries a copyleft SPDX id (GPL, LGPL, AGPL, SSPL,
EUPL, OSL and the like), or a weak copyleft one (MPL, EPL, CDDL) not listed
in `ALLOW_WEAK_COPYLEFT`. It also fails on a missing or `NOASSERTION`
licence or a missing licence file. External and system tools may carry any
licence, because PRISM only runs them as separate, unmodified processes.

There is no CodeQL adapter: the CodeQL engine's terms restrict commercial
use.

PRISM's own licence is the GNU Affero General Public License, version 3
(`LICENSE`). Open before a release: check the AFL++ and Strix licences
against current upstream, and a legal review of the third-party table.

## SBOM

`prism-deps sbom` writes a CycloneDX 1.5 JSON SBOM from the manifest.
Linked libraries are `library`/`required`; external and system tools are
`application`/`optional`. Each entry has its purl
(`pkg:github/<owner>/<repo>@<commit>`), SHA-256 and licence. The output is
deterministic: the serial number is a UUIDv5 of the manifest and version,
and the timestamp is `SOURCE_DATE_EPOCH` when set. CI uploads it as the
`prism-sbom` artifact on every push.

## Reproducible, signed releases

The `Dockerfile`:

- starts from `ubuntu:24.04` pinned by digest, with apt pointed at a
  snapshot date;
- fetches LLVM 23.1.4 with `scripts/fetch_llvm.py` and builds with it by
  absolute path;
- sets `SOURCE_DATE_EPOCH`, `-ffile-prefix-map`, `-Wl,--build-id=sha1` and
  uses the deterministic `llvm-ar`;
- runs `prism-deps licence-check` and `prism-deps linked` before compiling
  and `prism_tests` after;
- exports `prism`, `prism.cdx.json` and `SHA256SUMS` from its `artefacts`
  stage.

`.github/workflows/release.yml` runs on `v*` tags. It builds twice from
scratch with `--no-cache` and fails if the two `SHA256SUMS` differ. It signs
each artefact keylessly with Sigstore (`cosign sign-blob --bundle`, GitHub
OIDC), verifies the signatures and attaches everything to a GitHub release.
No `v*` tag has been pushed yet, so the workflow has not run.

Rebuilds are expected to match only on x86-64 with AVX2 (`-mavx2`).

### Checking a release

```
# 1. signatures: the certificate must name this repository's release workflow
cosign verify-blob --bundle prism.sigstore.json \
  --certificate-identity https://github.com/<owner>/PRISM/.github/workflows/release.yml@refs/tags/vX.Y.Z \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com prism
sha256sum -c SHA256SUMS

# 2. rebuild bit for bit
git checkout vX.Y.Z
docker build --no-cache --build-arg SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) \
  --build-arg PRISM_VERSION=vX.Y.Z --output type=local,dest=rebuild .
diff rebuild/SHA256SUMS SHA256SUMS        # must be empty

# 3. dependencies
build/prism-deps linked --refetch
build/prism-deps licence-check
```

If two builds differ, run `diffoscope` on the pair.

## History rewrite (owner action)

The deleted mined trees are still in git history. `scripts/rewrite_history.sh`
removes them from every commit with `git filter-repo --invert-paths`. Run it
on a fresh `git clone --mirror`. It asks you to type `I UNDERSTAND` and
never pushes. The force-push is the owner's decision, and afterwards every
collaborator must re-clone.
