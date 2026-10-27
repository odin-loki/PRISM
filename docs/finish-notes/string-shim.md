# string.h shim

Slice `finish/string-shim`, base `62ef0572d9dbd522c23d60142ddf7082ccf2c511`.

`src/prism/pir/models/libc/shim/string.h` is the Windows LP64 `<string.h>` name header. It declares `memset`, `memcpy`, and `strlen`. A one-line comment states that this header is declarations only.

Operational models stay in `src/prism/pir/models/libc/string.cpp`. `memcpy` calls `__prism_memcpy` with overlap disallowed. `memset` calls `__prism_memset`. `strlen` walks its argument. Those bodies are the checks PIR runs. `string.cpp` is unchanged in this slice; another change already renamed its check messages.

A function body in the header would define the symbol in the scanned translation unit. A body that returns the destination pointer, without the checks in `string.cpp`, would hide null and overlap defects. No body was added.

`pthread.h`, `assert.h`, and `stdatomic.h` are unchanged.

## Tests

`tests/cpp/test_string_shim.cpp` reads the shim as text from `PRISM_SOURCE_DIR`. It requires these declarations:

- `void* memset(void*, int, size_t);`
- `void* memcpy(void*, const void*, size_t);`
- `size_t strlen(const char*);`

The header text has no `{`, so it has no brace function body. The header text does not contain `PROVED`.

`CMakeLists.txt` gained one line in `_prism_test_sources`: `tests/cpp/test_string_shim.cpp`. The test target lists sources and does not glob.

`prism_tests` was not executed. This slice does not configure or build.
