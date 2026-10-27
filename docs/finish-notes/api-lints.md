# API lints slice

HEAD `e59a8b3df`. Compared `docs/MINED.md` API bullets and `src/prism/taxonomy.cpp` `API-*` rows with `src/prism/checkers_api.cpp`.

No new rule ids. Every taxonomy API class that belongs in this file already has a checker (`lint_discarded` / `api_assign_unguarded` / a dedicated function). `API-GETS`, `API-STRTOK-REENTRANT`, `API-TMPNAM`, `API-MKTEMP`, `API-MKSTEMP`, `API-UMASK`, `API-SIGNAL`, `API-SYSTEM`, `API-GETENV-NULL`, `API-STRDUP-NULL`, `API-CHROOT`, `API-IGNORED-ERROR`, `API-SCANF-UNCHECKED`, `API-PRECONDITION`, and `API-POPEN` stay in `checkers_core.cpp`.

Preferred names already covered:

| call | rule |
|---|---|
| `mkfifo` / `mknod` | `API-MKFIFO` |
| `dup` / `dup2` / `dup3` | `API-DUP` |
| `pipe` / `pipe2` | `API-PIPE` |
| `bind` | `API-BIND` |
| `listen` | `API-LISTEN` |
| `connect` | `API-CONNECT` |
| `getsockopt` / `setsockopt` | `API-GETSOCKOPT` |
| `unlink` / `remove` | `API-UNLINK` |
| `renameat` / `renameat2` | `API-RENAMEAT` / `API-RENAMEAT2` |
| `mmap` without `MAP_FAILED` | `API-MMAP` |

Skipped because the id is not in the taxonomy (not invented here): `rename()`, `fclose`, `closedir`, `fflush`, `setvbuf`.
