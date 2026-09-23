# Supply chain

Roadmap Part 1: every byte PRISM compiles or runs is pinned, hashed and
licence-checked, and a release can be rebuilt bit for bit.

## What is in the repository

`third_party/` holds only the six libraries PRISM links, each an unmodified
upstream copy pinned in `third_party/MANIFEST.toml`:

| Component | Version | Upstream commit | Licence |
|---|---|---|---|
| Z3 | 4.13.4 | `6f24123f0c9d` (z3-4.13.4) | MIT |
| PCRE2 | 10.44 | `6ae58beca071` (pcre2-10.44) | BSD-3-Clause WITH PCRE2-exception |
| xsimd | 13.2.0 | `1f8dd9c8e162` (13.2.0) | BSD-3-Clause |
| nlohmann/json | 3.11.3 | `9cca280a4d0c` (v3.11.3) | MIT |
| doctest | 2.4.11 | `ae7a13539fb7` (v2.4.11) | MIT |
| llama.cpp | b6875 | `3eb2be1ca5f3` (b6875) | MIT |

The fifteen mined source trees (AFLplusplus, Frama-C, FuSeBMC, Fuzz4All,
cbmc, coccinelle, codeql, cppcheck, dafny, esbmc, infer, klee, rapidcheck,
semgrep, strix; about 170,000 files) were deleted. They were never built;
`docs/MINED.md` records what PRISM re-implemented from each, and
`[[mined]]` rows in the manifest keep their URLs and licences. The tracked
tree is now about 140 MB of source on disk (llama.cpp ~90 MB, Z3 ~33 MB,
PCRE2 ~16 MB); the `.git` directory only shrinks after the history rewrite
below.

## MANIFEST.toml

One `[[component]]` per dependency:

- `kind = "linked"`: compiled into PRISM from `path`. Records `tag`,
  `commit`, `archive_sha256`, `tree_sha256` (digest of the in-tree copy),
  `tree_version_*` (the version marker in the source) and `drift` (the few
  files where the in-tree copy differs from upstream: line endings and files
  the original vendoring dropped).
- `kind = "external"`: a tool PRISM runs as a separate process (ESBMC, CBMC,
  KLEE, cppcheck, Frama-C, Infer, Semgrep, Coccinelle, AFL++, Dafny, Strix,
  CaDiCaL, Kissat, cake_lpr). Pinned to an upstream release tag's commit.
- `kind = "system"`: host tools (clang, clang-tidy, gcc, bwrap, the polyglot
  linters). Not pinned; findings still record which binary ran (below).

`archive_sha256` is the SHA-256 of the uncompressed tar that
`git archive --format=tar --prefix=<name>-<commit>/ <commit>` produces after
`git fetch --depth 1 <url> <commit>`. The fetch itself is content-addressed
by the commit id; the SHA-256 is a second, collision-resistant check over the
exact bytes PRISM builds from. (GitHub's `.tar.gz` endpoints were not
reachable from the pinning machine, and their gzip bytes are not a stable
contract.)

### How the pins were resolved

`third_party/vendor.log` (now removed; its content is summarised in the
manifest header) recorded clone URLs and timestamps from 2026-09-19 but no
commit hashes, because the trees were shallow clones with `.git` removed.

- Linked libraries: read the version from the in-tree source, resolve the
  matching tag with `git ls-remote`, fetch the tag's tree and compare it file
  by file with the in-tree copy. All six match except the files listed under
  `drift`. llama.cpp carries no release number, so its commit was found by
  comparing the in-tree tree against every upstream commit since 2025-09-01;
  `3eb2be1` (tag `b6875`) is the only exact match.
- External tools: the mined copies were development heads (for example
  cppcheck 2.21.99, KLEE 3.3-pre), which cannot be reproduced. Each is pinned
  to the upstream release at or just below that version.

## scripts/fetch_deps.py

Standard library plus the `git` CLI. Fails closed: any commit, hash or version
mismatch exits non-zero (3 for a hash mismatch) and installs nothing.

```
python scripts/fetch_deps.py --list
python scripts/fetch_deps.py --linked              # offline: version marker + tree digest
python scripts/fetch_deps.py --linked --refetch    # + fetch each pinned archive, verify sha256, diff vs tree
python scripts/fetch_deps.py --tool cadical --tool kissat --tool cake_lpr
python scripts/fetch_deps.py --tool esbmc --no-build
```

`--tool NAME` fetches the pinned commit, verifies `archive_sha256`, and
installs into `~/.prism/tools/<name>/<commit>/` (override the root with
`PRISM_TOOLS_DIR`): `src/` (verified source), `bin/` (executables) and
`PRISM-TOOL.json`. Everything is staged in a temporary sibling directory and
renamed into place only after every step passed. Build recipes exist for
CaDiCaL and Kissat (`./configure && make`) and cake_lpr (compile the shipped,
CakeML-verified `cake_lpr.S` with its C FFI shim). For the other tools the
script verifies and unpacks the source, prints the build hint from the
manifest, and tells you where to put the binary.

Checked on 2026-09-23: `--tool cadical --tool kissat --tool cake_lpr` built
CaDiCaL 3.0.1 and Kissat 4.0.4, and
`cadical --lrat u.cnf u.lrat && cake_lpr u.cnf u.lrat` printed
`s VERIFIED UNSAT`.

## How PRISM finds and records tools

Search order in both engines (`prism/config.py`, `src/prism/config.cpp`):

1. `--tool NAME=PATH` / `Config.tools`
2. `~/.prism/tools/<component>/<pinned commit>/bin/<exe>`: only the commit
   the manifest pins. The Python engine reads `MANIFEST.toml` with `tomllib`.
   The C++ engine reads it at CMake configure time into the generated
   `prism/manifest_pins.hpp`, so a binary trusts the builds its own manifest
   named.
3. `PATH`

A missing tool is `NOTRUN` with the hint
`python scripts/fetch_deps.py --tool <component> (pinned in third_party/MANIFEST.toml)`.
Law 9: a tools directory (or, in the Python engine, a manifest) inside the
scanned tree is ignored unless `--allow-exec` is given.

Every finding an external tool produced carries `extra["tool_sha"]`: the
manifest commit when the binary lives under
`~/.prism/tools/<name>/<commit>/`, otherwise
`path:<absolute path>;sha256:<SHA-256 of the binary>` (cached per path, size
and mtime). This is in place for cppcheck, ESBMC, Dafny and every optional
tool (KLEE, AFL++, Frama-C, Infer, clang-tidy, CBMC, Strix, Semgrep, spatch)
in both engines.

## Licence firewall

`scripts/licence_check.py` (run in CI and in the Docker build) fails when a
`linked` component carries a copyleft SPDX id: GPL, LGPL, AGPL, SSPL, EUPL,
OSL and similar, and also weak copyleft (MPL, EPL, CDDL) unless the component
is added to `ALLOW_WEAK_COPYLEFT` after review. It also fails on a missing or
NOASSERTION licence, or a missing licence file. External and system tools may
carry any licence because PRISM only runs them as separate, unmodified
processes.

The CodeQL adapter was removed from both engines: the CodeQL engine is under
the GitHub CodeQL Terms, which restrict commercial use. The MIT repository
only contains queries.

Before any release, verify these licences against current upstream: AFL++
(the licence file in the mined copy said AGPL-3.0-or-later, while older
releases were Apache-2.0) and Strix (AGPL v3 in tree). `LICENSE` (PRISM's own
draft source-available evaluation licence) and this table must be reviewed
by an Australian IP lawyer before the first sale.

## SBOM

`python scripts/sbom.py --version vX.Y.Z -o prism.cdx.json` writes a
CycloneDX 1.5 JSON SBOM from the manifest, using only the standard library.
Linked libraries are `library`/`required`, external and system tools are
`application`/`optional`. Each entry carries its purl
(`pkg:github/<owner>/<repo>@<commit>`), SHA-256 and licence, plus `prism:*`
properties (commit, tag, tree digest, drift). The output is deterministic: the
serial number is a UUIDv5 of the manifest and version, and the timestamp is
`SOURCE_DATE_EPOCH` when set. CI uploads it as the `prism-sbom` artifact on
every push.

## Reproducible, signed releases

`Dockerfile`:

- `ubuntu:24.04` pinned by index digest
  (`sha256:008173c2…`, resolved 2026-09-23; re-resolve with
  `docker buildx imagetools inspect ubuntu:24.04`).
- apt pointed at `snapshot.ubuntu.com/ubuntu/<UBUNTU_SNAPSHOT>` so
  `clang-18`, `lld-18` and `llvm-18` resolve to the same packages every time.
  The compiler is used by absolute path.
- `SOURCE_DATE_EPOCH` (build arg, the tagged commit's time),
  `-ffile-prefix-map=/src=.`, `-ffile-prefix-map=/build=build`,
  `-Wl,--build-id=sha1`, and deterministic `llvm-ar`.
- `licence_check.py` and `fetch_deps.py --linked` run before compiling;
  `prism_tests` runs after.
- The `artefacts` stage exports `prism`, `libprism_native.so`,
  `prism.cdx.json` and `SHA256SUMS`.

`.github/workflows/release.yml` runs on tags `v*`. It builds twice from
scratch with `--no-cache` and fails if the two `SHA256SUMS` differ. It then
signs each artefact keylessly with Sigstore (`cosign sign-blob --bundle`,
GitHub OIDC with `id-token: write`), verifies the signatures, and attaches
everything to a GitHub release.

### Checking a release (buyer or auditor)

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
python3 scripts/fetch_deps.py --linked --refetch   # in-tree libs == pinned upstream archives
python3 scripts/licence_check.py
```

Known limits: the Docker build was written without a Docker daemon available
and has not been run yet. The first tagged release is the first real
reproducibility check; if the two builds differ, `diffoscope` on the pair
shows where. Z3's build may embed its own version string, but not a date.
Rebuilds are expected to match only on x86-64 with AVX2, the configured
target (`-mavx2`).

## History rewrite (owner action)

Deleting the trees does not shrink `.git`: the mined files are still in the
history, about 200 MB packed. `scripts/rewrite_history.sh` removes the fifteen
paths from every commit with `git filter-repo --invert-paths`. Run it on a
fresh `git clone --mirror`. It asks you to type `I UNDERSTAND` and never
pushes. The owner decides on the one force-push
(`git push --force --mirror origin`), after which every collaborator must
re-clone. Commit ids change, so do this before anything cites them.
