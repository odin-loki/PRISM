# Agents: read CLAUDE.md

This project is **PRISM**. It was once called Helix; that name is retired and
must not appear anywhere outside `third_party/`. Full rules, layout, laws and
build/test commands are in [CLAUDE.md](CLAUDE.md).

C++ style target: memory-safe **ISO C++23**, strict safety and performance for
`src/prism/` et al. — [docs/CXX_STANDARD.md](docs/CXX_STANDARD.md) (enforcement
still in progress).

## Where to edit vs where to build

| Location | Use for |
|----------|---------|
| `C:\Users\...\OneDrive\Desktop\PRISM` (or any sync’d Desktop tree) | **Edit** in the IDE, git, docs |
| `~/PRISM` on WSL **ext4** | **Linux** `cmake` / `ninja` / `prism_tests` (`build-wsl/`) |
| `C:\Temp\prism-build-*` (or other non-OneDrive path) | **Windows** MSVC / Z3 builds per [docs/MSVC_PORT.md](docs/MSVC_PORT.md) |

Do **not** run `cmake`, `ninja`, or full `prism_tests` on **OneDrive** paths
(including `build-wsl/` next to the Desktop clone). Sync is slow, ninja is
slow, and link steps flake (`LNK1104` on Windows). Agents must not default to
`/mnt/c/Users/.../PRISM` for builds.

## WSL (agent / local builds)

Do **not** compile through **`/mnt/c/...`** (DrvFS). It emulates Windows
storage and makes `cmake`/`ninja`/`prism_tests` slow and flaky.

Keep a **full git checkout** on ext4, e.g. `~/PRISM` (must include
`third_party/` and `.git/` per `MANIFEST.toml`). After editing on OneDrive,
sync into ext4:

```bash
scripts/wsl_sync_from_windows.sh    # from repo root on Windows, in WSL
cd ~/PRISM && scripts/wsl_build.sh
```

Or `git clone` / `git pull` in `~/PRISM` if the remote is up to date. Do not
`rsync --delete` from `/mnt/c` without preserving `.git` (breaks
`tests/cpp/test_deps.cpp`). Windows MSVC builds stay on `C:\Temp\...` per
[docs/MSVC_PORT.md](docs/MSVC_PORT.md).
