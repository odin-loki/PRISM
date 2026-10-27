# string null arguments

Slice `finish/string-null`, base `d036e09c7c29d24e411c1375a227a69189d819b1`.

`src/prism/pir/models/libc/string.cpp` names a null pointer argument with `__prism_check` and class `STR-NULL-ARG`. The message is the function that contains the check, then the parameter name: `strcpy() called with unchecked pointer parameter d`, and the same shape for the other pointer (`s`).

Each pointer parameter of `strlen`, `strcpy`, `strncpy`, `strcmp`, `strncmp`, `strchr`, `strrchr`, and `strdup` is checked before that function loads or stores through it. `strdup` already checked `s`; its message text is the same sentence.

`strcat` and `strncat` keep their existing messages: `strcat() called with unchecked pointer parameter d` and `strncat() called with unchecked pointer parameter d`. `tests/cpp/test_libc_messages.cpp` is unchanged.

`strnlen` with `n == 0` returns 0. That path does not read `s` and does not require `s` to be non-null. When `n > 0`, `s` is checked before the first read.

`memcpy`, `memmove`, `memset`, `memcmp`, and `memchr` with a length of 0 do not read, do not write, and do not synthesize a byte. `memcpy`, `memmove`, and `memset` return the destination. `memcmp` returns 0. `memchr` returns a null pointer. A length greater than 0 checks each pointer before the copy or the byte loop. A null destination and a positive length is that check. The function does not return the null pointer in place of the check.

A null argument is not hidden by returning an empty string or 0. The zero-length results above are the C results for a length of 0.

`stdlib.cpp`, `stdio.cpp`, and the `string.h` shim are unchanged.

## Tests

`tests/cpp/test_string_null.cpp` reads `string.cpp` as text from `PRISM_SOURCE_DIR`, the same slurp as `tests/cpp/test_libc_messages.cpp`. It checks that `strcpy`'s message contains `strcpy()`, that `memcpy` contains a `__prism_check`, and that the `strcat` message read from the file is still `strcat() called with unchecked pointer parameter d` (that text contains `strcat()`).

`CMakeLists.txt` lists `tests/cpp/test_string_null.cpp` as one new line in `_prism_test_sources`.

`prism_tests` was not executed. This slice does not configure or build.
