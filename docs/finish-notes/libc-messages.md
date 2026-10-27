# libc check messages

Slice on `136f174bb`. Null checks in the libc models keep class `STR-NULL-ARG`
and the same condition. Only the message text changes, so the name in the
message is the function that contains the check.

`strdup` said `strlen() called with unchecked pointer parameter s`. The check
is in `strdup`. The same `strlen` sentence was copied into other functions
that then call `strlen`.

Messages now:

| Function | Message |
| --- | --- |
| `strcat` | `strcat() called with unchecked pointer parameter d` |
| `strncat` | `strncat() called with unchecked pointer parameter d` |
| `strdup` | `strdup() called with unchecked pointer parameter s` |
| `atoi` | `atoi() called with unchecked pointer parameter s` |
| `atol` | `atol() called with unchecked pointer parameter s` |
| `strtol` | `strtol() called with unchecked pointer parameter s` |
| `getenv` | `getenv() called with unchecked pointer parameter name` |
| `fopen` | `fopen() called with unchecked pointer parameter path` |
| `fopen` | `fopen() called with unchecked pointer parameter mode` |
| `fputs` | `fputs() called with unchecked pointer parameter s` |
| `puts` | `puts() called with unchecked pointer parameter s` |

`strlen` itself has no `__prism_check`. `fclose` and `fwrite` already named
themselves. `prism_file_ok` says `stdio call on a NULL FILE pointer` and does
not name another function. `strtoul` has no check of its own; it calls
`strtol`, whose message names `strtol`.

`tests/cpp/test_libc_messages.cpp` reads `string.cpp` from `PRISM_SOURCE_DIR`
and checks the `strdup` message contains `strdup` and does not contain
`strlen() called`. The class and the `s != 0` condition stay on that call.

No check was removed or weakened. No proof was added.
