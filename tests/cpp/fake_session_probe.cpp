// Fake session child for run_session doctests on WIN32 (see tests/cpp/test_qa.cpp).
// Mode is argv[1]; the parent checks stdout/stderr/exit code/timeout.
//   echo-out        print "probe-out" to stdout, exit 0
//   echo-err        print "probe-err" to stderr, exit 0
//   exit <code>     exit with <code>
//   echo-env <var>  print the value of <var> + newline; exit 3 if unset
//   echo-cwd        print the current directory + newline
//   copy-stdin      copy stdin to stdout byte for byte
//   sleep <sec>     sleep <sec> seconds (a timeout test kills it), exit 0
// Unknown mode or missing argument: usage on stderr, exit 2.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

static int usage() {
    std::fputs("usage: fake_session_probe echo-out|echo-err|exit N|echo-env VAR|"
               "echo-cwd|copy-stdin|sleep SEC\n",
               stderr);
    return 2;
}

static bool write_all(HANDLE h, const char* p, DWORD n) {
    while (n > 0) {
        DWORD w = 0;
        if (!WriteFile(h, p, n, &w, nullptr) || w == 0) return false;
        p += w;
        n -= w;
    }
    return true;
}

static int copy_stdin() {
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (in == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE) return 1;
    char buf[4096];
    for (;;) {
        DWORD n = 0;
        // Closed pipe / EOF: ReadFile fails (ERROR_BROKEN_PIPE) or returns 0.
        if (!ReadFile(in, buf, sizeof buf, &n, nullptr) || n == 0) break;
        if (!write_all(out, buf, n)) return 1;
    }
    return 0;
}

static int echo_env(const char* var) {
    char buf[32768];
    const DWORD n = GetEnvironmentVariableA(var, buf, sizeof buf);
    if (n == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) return 3;
    if (n >= sizeof buf) return 1;
    std::printf("%s\n", buf);
    return 0;
}

static int echo_cwd() {
    char buf[MAX_PATH * 4];
    const DWORD n = GetCurrentDirectoryA(sizeof buf, buf);
    if (n == 0 || n >= sizeof buf) return 1;
    std::printf("%s\n", buf);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string mode = argv[1];
    const char* arg = argc > 2 ? argv[2] : nullptr;
    if (mode == "echo-out") {
        std::fputs("probe-out\n", stdout);
        return 0;
    }
    if (mode == "echo-err") {
        std::fputs("probe-err\n", stderr);
        return 0;
    }
    if (mode == "exit") return arg ? std::atoi(arg) : usage();
    if (mode == "echo-env") return arg ? echo_env(arg) : usage();
    if (mode == "echo-cwd") return echo_cwd();
    if (mode == "copy-stdin") return copy_stdin();
    if (mode == "sleep") {
        if (!arg) return usage();
        Sleep(static_cast<DWORD>(std::atof(arg) * 1000.0));
        return 0;
    }
    return usage();
}
