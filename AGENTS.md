# Agents: read CLAUDE.md

This project is **PRISM**. It was once called Helix; that name is retired and
must not appear anywhere outside `third_party/`. The rules, laws, layout and
build commands are in [CLAUDE.md](CLAUDE.md).

The C++ coding standard is [docs/CXX_STANDARD.md](docs/CXX_STANDARD.md).

## Where to edit and where to build

| Location | Use for |
|---|---|
| A OneDrive or other synced Desktop clone | editing, git, docs. Never build here |
| A clone on WSL ext4, e.g. `~/PRISM` | Linux `cmake`, `ninja`, `prism_tests` |
| `C:\Temp\prism-build-*` or another non-synced path | Windows builds ([docs/MSVC_PORT.md](docs/MSVC_PORT.md)) |

Do not run `cmake`, `ninja` or `prism_tests` on OneDrive paths or through
`/mnt/c/...` (DrvFS). Sync is slow, the build is slow, and Windows links
flake (`LNK1104`).

Keep a full git checkout on ext4, with `third_party/` and `.git/`
(`tests/cpp/test_deps.cpp` needs both). Get it with `git clone` or
`git pull`, or copy edits from the Windows clone with
`scripts/wsl_sync_from_windows.sh`. Do not `rsync --delete` from `/mnt/c`
without keeping `.git`.

Build with the pinned LLVM 23.1.4 as in [CLAUDE.md](CLAUDE.md#build-and-test).
System clang 18 cannot build FORGE.
