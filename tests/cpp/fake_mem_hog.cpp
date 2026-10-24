// Memory hog for the certified-mode "checker killed past its resident-memory
// cap" doctest on WIN32 (tests/cpp/test_certified.cpp). The POSIX case runs a
// `#!/bin/sh` script that execs python3 and touches 400 MB; here the test
// copies this PE to <dir>/lrat-check.exe and runs it through
// prism::solver::check_lrat with a 100 MB cap. It commits and touches 400 MB
// (so the pages are resident, not just reserved), then sleeps 20 s: only the
// watchdog ends it early. A finished run prints "c VERIFIED" so a missed kill
// would show up as a verdict, not pass quietly.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>

int main() {
    const SIZE_T bytes = static_cast<SIZE_T>(400) << 20;
    auto* p = static_cast<volatile char*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!p) return 3;
    for (SIZE_T i = 0; i < bytes; i += 4096) p[i] = 1;
    Sleep(20000);
    std::puts("c VERIFIED");
    std::fflush(stdout);
    return 0;
}
