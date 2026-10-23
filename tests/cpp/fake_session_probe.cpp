// Fake session child for run_session / run_tree doctests on WIN32 (see
// tests/cpp/test_qa.cpp, tests/cpp/test_qa_conformance.cpp).
// Mode is argv[1]; the parent checks stdout/stderr/exit code/timeout.
//   echo-out        print "probe-out" to stdout, exit 0
//   echo-err        print "probe-err" to stderr, exit 0
//   exit <code>     exit with <code>
//   echo-env <var>  print the value of <var> + newline; exit 3 if unset
//   echo-cwd        print the current directory + newline
//   copy-stdin      copy stdin to stdout byte for byte
//   sleep <sec>     sleep <sec> seconds (a timeout test kills it), exit 0
//   spawn-sleep <sec> <pidfile>
//                   start a grandchild "sleep <sec>", write its pid to <pidfile>,
//                   then sleep <sec> too: a timeout must end both (Job Object)
//   spawn-bg <sec>  start a grandchild "sleep <sec>" that holds none of our
//                   handles, print its pid, exit 0 at once: the session ends it
//   alloc <mb>      commit <mb> MB; exit 0 when it succeeds, 4 when refused
//                   (a memory cap on the job)
// Unknown mode or missing argument: usage on stderr, exit 2.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int usage() {
    std::fputs("usage: fake_session_probe echo-out|echo-err|exit N|echo-env VAR|"
               "echo-cwd|copy-stdin|sleep SEC|spawn-sleep SEC PIDFILE|spawn-bg SEC|alloc MB\n",
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

// A grandchild running this probe as "sleep <sec>". It inherits no handle, so
// it never holds the parent's stdout/stderr pipes open. Returns its pid or 0.
static DWORD spawn_sleeper(const char* sec) {
    char self[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameA(nullptr, self, sizeof self);
    if (n == 0 || n >= sizeof self) return 0;
    std::string cl = std::string("\"") + self + "\" sleep " + sec;
    cl.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return 0;
    const DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pid;
}

static int spawn_sleep(const char* sec, const char* pidfile) {
    const DWORD pid = spawn_sleeper(sec);
    if (pid == 0) return 127;
    // written whole, then renamed: the parent never reads half a pid
    const std::string tmp = std::string(pidfile) + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return 1;
    std::fprintf(f, "%lu", static_cast<unsigned long>(pid));
    std::fclose(f);
    if (!MoveFileExA(tmp.c_str(), pidfile, MOVEFILE_REPLACE_EXISTING)) return 1;
    Sleep(static_cast<DWORD>(std::atof(sec) * 1000.0));
    return 0;
}

static int spawn_bg(const char* sec) {
    const DWORD pid = spawn_sleeper(sec);
    if (pid == 0) return 127;
    std::printf("%lu\n", static_cast<unsigned long>(pid));
    return 0;
}

static int alloc_mb(const char* mb) {
    const SIZE_T bytes = static_cast<SIZE_T>(std::atof(mb) * 1024.0 * 1024.0);
    void* p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return 4;
    std::memset(p, 1, bytes);
    VirtualFree(p, 0, MEM_RELEASE);
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
    if (mode == "spawn-sleep") return (arg && argc > 3) ? spawn_sleep(arg, argv[3]) : usage();
    if (mode == "spawn-bg") return arg ? spawn_bg(arg) : usage();
    if (mode == "alloc") return arg ? alloc_mb(arg) : usage();
    return usage();
}
