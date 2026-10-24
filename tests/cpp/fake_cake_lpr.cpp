// Fake cake_lpr for the certified-mode "checker out of memory" doctest on WIN32
// (tests/cpp/test_certified.cpp). The POSIX case writes a `#!/bin/sh` script;
// here the test copies this PE to <dir>/cake_lpr.exe and runs it through
// prism::solver::check_lrat. It does what that script does:
//   - writes its arguments, space separated, to "<its own path>.args"
//     (the test checks that --CML_HEAP_SIZE=<mb> reaches the checker);
//   - prints "CakeML heap space exhausted." and exits 1.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    char self[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameA(nullptr, self, sizeof self);
    if (n == 0 || n >= sizeof self) return 3;
    const std::string argf = std::string(self) + ".args";
    FILE* f = std::fopen(argf.c_str(), "wb");
    if (!f) return 3;
    for (int i = 1; i < argc; ++i) std::fprintf(f, "%s%s", i > 1 ? " " : "", argv[i]);
    std::fputs("\n", f);
    std::fclose(f);
    std::puts("CakeML heap space exhausted.");
    std::fflush(stdout);
    return 1;
}
