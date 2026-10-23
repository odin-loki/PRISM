// Stand-in engine for the prism-qa doctests (tests/cpp/test_qa_conformance.cpp).
//
//   fake_prism STATUS [prism args...]   claims STATUS for every function of
//       the source (the first prism argument): --list-stages prints
//       inventory/classify/bmc/harness; otherwise OUT/report.json gets one bmc
//       finding per function (cls INT-SIGNED-OVF, no counterexample).
//   fake_prism HANG PIDFILE [...]       starts a "solver" (sleep) in a process
//       group of its own, as the solver runner does, writes its pid to
//       PIDFILE and waits: a scorer timeout must kill both.
//
// POSIX: the test wraps it in a /bin/sh script that execs it with fixed
// leading arguments. WIN32: no script (a .cmd would need cmd.exe quoting), so
// the test copies this executable to NAME.exe and writes NAME.exe.lead, one
// leading argument per line (the same sidecar idea as fake_cc.exe.fakebody);
// the "solver" is this executable run as "--sleep-child".

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::string json_str(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

#ifdef _WIN32
std::string self_path() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).string();
}

// NAME.exe.lead: one argument per line (a trailing CR is dropped).
std::vector<std::string> read_lead(const std::string& exe) {
    std::vector<std::string> lead;
    std::ifstream in(exe + ".lead", std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lead.push_back(line);
    }
    return lead;
}

// The "solver": a process of its own that only waits to be killed.
int spawn_solver(const std::string& pidfile) {
    const std::string exe = self_path();
    if (exe.empty()) return 2;
    std::string cl = "\"" + exe + "\" --sleep-child";
    std::vector<char> buf(cl.begin(), cl.end());
    buf.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // not inheriting handles: the solver must not hold the scorer's pipes
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return 127;
    std::ofstream(pidfile) << pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> all(argv + 1, argv + argc);
#ifdef _WIN32
    if (!all.empty() && all[0] == "--sleep-child") {
        Sleep(300 * 1000);
        return 0;
    }
    {
        std::vector<std::string> lead = read_lead(self_path());
        lead.insert(lead.end(), all.begin(), all.end());
        all = std::move(lead);
    }
#endif
    if (all.empty()) return 2;
    std::vector<std::string> args(all.begin() + 1, all.end());
    const std::string status = all[0];
    if (status == "HANG") {
        if (args.empty()) return 2;
#ifdef _WIN32
        if (const int rc = spawn_solver(args[0])) return rc;
        Sleep(300 * 1000);
#else
        pid_t p = ::fork();
        if (p == 0) {
            ::setpgid(0, 0);
            ::execlp("sleep", "sleep", "300", static_cast<char*>(nullptr));
            ::_exit(127);
        }
        std::ofstream(args[0]) << p;
        ::sleep(300);
#endif
        return 0;
    }
    for (const auto& a : args)
        if (a == "--list-stages") {
            std::cout << "inventory\nclassify\nbmc\nharness\n";
            return 0;
        }
    if (args.empty()) return 2;
    std::string out;
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--out") out = args[i + 1];
    if (out.empty()) return 2;
    std::ifstream in(args[0]);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    if (ec) return 2;
    static const std::regex fn_re(R"(^[\w ]*?\b(\w+)\([^)]*\)\s*\{)", std::regex::ECMAScript | std::regex::multiline);
    std::string findings;
    for (auto it = std::sregex_iterator(src.begin(), src.end(), fn_re); it != std::sregex_iterator(); ++it) {
        if (!findings.empty()) findings += ", ";
        findings += "{\"stage\": \"bmc\", \"status\": " + json_str(status) + ", \"function\": " + json_str((*it)[1]) +
                    ", \"cls\": \"INT-SIGNED-OVF\", \"message\": \"fake\", \"counterexample\": \"\"}";
    }
    std::ofstream(out + "/report.json") << "{\"stages\": [{\"name\": \"bmc\", \"status\": \"ok\", \"findings\": ["
                                        << findings << "]}]}";
    return 0;
}
