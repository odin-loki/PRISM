// Stand-in engine for the prism-qa doctests (tests/cpp/test_qa_conformance.cpp).
//
//   fake_prism STATUS [prism args...]   claims STATUS for every function of
//       the source (the first prism argument): --list-stages prints
//       inventory/classify/bmc/harness; otherwise OUT/report.json gets one bmc
//       finding per function (cls INT-SIGNED-OVF, no counterexample).
//   fake_prism HANG PIDFILE [...]       starts a "solver" (sleep) in a process
//       group of its own, as the solver runner does, writes its pid to
//       PIDFILE and waits: a scorer timeout must kill both.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

std::string json_str(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::vector<std::string> args(argv + 2, argv + argc);
    const std::string status = argv[1];
    if (status == "HANG") {
        if (args.empty()) return 2;
        pid_t p = ::fork();
        if (p == 0) {
            ::setpgid(0, 0);
            ::execlp("sleep", "sleep", "300", static_cast<char*>(nullptr));
            ::_exit(127);
        }
        std::ofstream(args[0]) << p;
        ::sleep(300);
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
