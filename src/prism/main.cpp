#include "prism/ai_proof.hpp"
#include "prism/ai_assist.hpp"
#include "prism/config.hpp"
#include "prism/pipeline.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  ifdef ERROR
#    undef ERROR
#  endif
#else
#  include <cerrno>
#  include <unistd.h>
#endif

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, ',')) {
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

static bool file_is_exe(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec) && !ec;
}

static std::filesystem::path gui_name() {
#ifdef _WIN32
    return "prism_gui.exe";
#else
    return "prism_gui";
#endif
}

static void add_dir(std::vector<std::filesystem::path>& dirs, std::filesystem::path d) {
    if (d.empty()) return;
    std::error_code ec;
    auto can = std::filesystem::weakly_canonical(d, ec);
    if (!ec) d = can;
    for (const auto& x : dirs)
        if (x == d) return;
    dirs.push_back(std::move(d));
}

static std::vector<std::filesystem::path> self_dirs(const char* argv0) {
    std::vector<std::filesystem::path> dirs;
#ifdef _WIN32
    std::vector<char> buf(32768);
    DWORD n = GetModuleFileNameA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n && n < buf.size()) add_dir(dirs, std::filesystem::path(buf.data()).parent_path());
#else
    char buf[4096]{};
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        add_dir(dirs, std::filesystem::path(buf).parent_path());
    }
#endif
    if (argv0 && *argv0) {
        std::error_code ec;
        auto abs = std::filesystem::absolute(argv0, ec);
        if (!ec) add_dir(dirs, abs.parent_path());
    }
    return dirs;
}

static std::filesystem::path search_path_gui() {
    const auto name = gui_name();
#ifdef _WIN32
    char found[32768]{};
    DWORD n = SearchPathA(nullptr, "prism_gui.exe", nullptr,
                          static_cast<DWORD>(sizeof found), found, nullptr);
    if (n && n < sizeof found) return found;
#else
    const char* path = std::getenv("PATH");
    if (!path) return {};
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        auto cand = std::filesystem::path(dir) / name;
        if (file_is_exe(cand)) return cand;
    }
#endif
    return {};
}

static std::filesystem::path find_prism_gui(const char* argv0) {
    const auto name = gui_name();
    for (const auto& dir : self_dirs(argv0)) {
        auto cand = dir / name;
        if (file_is_exe(cand)) return cand;
    }
    return search_path_gui();
}

static int notrun_missing_gui() {
    std::cout << "NOTRUN gui: prism_gui not found — not a clean window\n";
    std::cout << "  install: build prism_gui with WSL clang++ Qt6 Widgets (never MinGW)\n";
    return 0;
}

static int error_spawn_gui(const std::string& detail) {
    std::cout << "ERROR gui: failed to spawn prism_gui — not a clean window\n";
    if (!detail.empty()) std::cout << "  " << detail << "\n";
    return 2;
}

#ifdef _WIN32
static std::wstring utf8_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static std::string quote_win(const std::string& a) {
    if (a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string o = "\"";
    for (char c : a) {
        if (c == '"') o += "\\\"";
        else o += c;
    }
    o += '"';
    return o;
}
#endif

static int spawn_prism_gui(const std::filesystem::path& gui, int argc, char** argv) {
    std::vector<std::string> args;
    args.push_back(gui.string());
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gui") == 0) continue;
        args.push_back(argv[i]);
    }

#ifdef _WIN32
    std::string cl;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) cl += ' ';
        cl += quote_win(args[i]);
    }
    auto wapp = utf8_wide(args[0]);
    auto wcl = utf8_wide(cl);
    std::vector<wchar_t> buf(wcl.begin(), wcl.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(wapp.c_str(), buf.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        return error_spawn_gui("CreateProcess error " + std::to_string(GetLastError()));
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    std::vector<char*> cargv;
    cargv.reserve(args.size() + 1);
    for (auto& a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    ::execv(args[0].c_str(), cargv.data());
    return error_spawn_gui(std::string("execv: ") + std::strerror(errno));
#endif
}

static int launch_gui(int argc, char** argv) {
    auto gui = find_prism_gui(argc > 0 ? argv[0] : nullptr);
    if (gui.empty()) return notrun_missing_gui();
    return spawn_prism_gui(gui, argc, argv);
}

int main(int argc, char** argv) {
    using namespace prism;
    // Roadmap 9.2: `prism prove FILE.lean THEOREM` (Lean proof search).
    if (argc > 1 && std::string_view(argv[1]) == "prove") return ai::prove_main(argc - 1, argv + 1);
    // Report-level subcommands (roadmap 9.3 / 9.4): they read report.json and
    // never change a verdict.
    if (argc > 1) {
        const std::string sub = argv[1];
        if (sub == "regress") return ai::regress_main(argc - 2, argv + 2);
        if (sub == "ask") return ai::ask_main(argc - 2, argv + 2);
        if (sub == "draft") return ai::draft_main(argc - 2, argv + 2);
        if (sub == "triage") {
            std::filesystem::path rp = "prism-out";
            ai::TriageOptions topt;
            for (int i = 2; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--threshold" && i + 1 < argc) topt.threshold = std::stod(argv[++i]);
                else if (a == "--no-embed") topt.use_embedder = false;
                else rp = a;
            }
            if (std::filesystem::is_regular_file(rp)) rp = rp.parent_path();
            auto rep = RunReport::load(rp / "report.json");
            if (!rep) {
                std::cerr << "ERROR triage: cannot read " << (rp / "report.json").string() << "\n";
                return 2;
            }
            auto t = ai::triage(*rep, topt);
            std::ofstream(rp / "triage.json", std::ios::binary) << ai::triage_json(t);
            std::cout << ai::triage_markdown(t, *rep);
            return 0;
        }
    }
    auto cfg = default_config();
    std::string path = "testdata";
    std::string fail_on = "never";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 < argc) return argv[++i];
            return {};
        };
        if (a == "--no-llm") cfg.llm = false;
        else if (a == "--gui") cfg.gui = true;
        else if (a == "--resume") cfg.resume = true;
        else if (a == "--allow-exec") cfg.allow_exec = true;
        else if (a == "--strict-aliasing") cfg.strict_aliasing = true;
        else if (a == "--pir-drafts") cfg.pir_drafts = true;
        else if (a == "--fp-checks") cfg.fp_checks = true;
        else if (a == "--version" || a == "-V") {
            std::cout << "prism " << PRISM_VERSION << " (C++ engine)\n";
            return 0;
        } else if (a == "--list-stages") {
            for (auto* p = STAGE_ORDER; *p; ++p) std::cout << *p << "\n";
            return 0;
        } else if (a == "--out") cfg.out = next();
        else if (a == "--pbsd") cfg.pbsd_root = std::filesystem::absolute(next());
        else if (a == "--stage") cfg.stages = split_csv(next());
        else if (a == "--skip") cfg.skip = split_csv(next());
        else if (a == "--unwind") cfg.unwind = std::stoi(next());
        else if (a == "--fuzz-budget") cfg.fuzz_budget = std::stod(next());
        else if (a == "--fuzz-iters") cfg.fuzz_iters = std::stoi(next());
        else if (a == "--repair-rounds") cfg.repair_rounds = std::stoi(next());
        else if (a == "--jobs" || a == "-j") cfg.jobs = std::stoi(next());
        else if (a == "--requirements") cfg.requirements.push_back(std::filesystem::absolute(next()));
        else if (a == "--contracts-approved") cfg.contracts_approved = std::filesystem::absolute(next());
        else if (a == "--fail-on") {
            fail_on = next();
            if (fail_on != "never" && fail_on != "defect" && fail_on != "gap") {
                std::cerr << "--fail-on expects never|defect|gap\n";
                return 2;
            }
        }
        else if (a == "--tool") {
            auto spec = next();
            auto eq = spec.find('=');
            if (eq != std::string::npos && eq > 0 && eq + 1 < spec.size())
                cfg.tools[spec.substr(0, eq)] = spec.substr(eq + 1);
        }
        else if (a == "-h" || a == "--help") {
            std::cout <<
                "prism PATH [--gui] [--no-llm] [--jobs N] [--tool NAME=PATH] [--pbsd PATH]\n"
                "           [--stage a,b] [--skip a,b] [--out DIR] [--resume] [--unwind N]\n"
                "           [--fuzz-budget N] [--fuzz-iters N] [--repair-rounds N]\n"
                "           [--list-stages] [--version] [--fail-on never|defect|gap] [--allow-exec]\n"
                "           [--requirements PATH] [--contracts-approved PATH]\n"
                "           [--strict-aliasing] [--pir-drafts] [--fp-checks]\n"
                "prism prove FILE.lean THEOREM [--write] [--allow-exec] (Lean proof search; prove --help)\n"
                "PRISM = Performance, Regression, Integration and Security Module\n"
                "Checks any codebase (PATH: file or directory, any language): deep C/C++\n"
                "analysis (lints, compiler warnings, BMC, fuzzing, contracts) plus every other\n"
                "language through the polyglot stage (syntax, linters, type checkers) and a\n"
                "secrets/conflict-marker scan of every text file. What could not be checked\n"
                "is reported as NOTRUN, never as clean.\n"
                "Adapter search: --tool, then ~/.prism/tools/<name>/<commit>/bin (fetch_deps), then PATH.\n"
                "--resume reuses ok/NOTRUN stages from --out/stages.jsonl (report.json fallback).\n"
                "--fail-on: exit 1 on defect (FAILED/CRASH/SANFAIL, except findings with\n"
                "  extra.severity warning/note/style) or gap (defect, or anything\n"
                "  NOTRUN/ERROR/TIMEOUT). A crashed stage is exit 2.\n"
                "--allow-exec: run code from the scanned tree (sanitizer/fuzz/diff harnesses,\n"
                "  perl -c, cargo clippy, eslint, LLM programs) in a sandbox; only on code you\n"
                "  trust. Without it those steps are NOTRUN (Law 9).\n"
                "--strict-aliasing: the pir stage also checks effective types (C11 6.5p7);\n"
                "  off by default because real code often breaks strict aliasing on purpose.\n"
                "--pir-drafts: pir takes pointer sizes from the template harness draft when no\n"
                "  requires clause gives them (PROVED-ASSUMING at best; off: NEEDS-HARNESS).\n"
                "--fp-checks: pir also reports floating-point division by zero, invalid\n"
                "  operations (NaN) and overflow to infinity (defined by IEEE/Annex F).\n"
                "--pbsd PATH: ParanoidBSD tree for the pbsd stage (else PRISM_PBSD; no default).\n"
                "  Importing its modules also needs --allow-exec.\n"
                "--requirements PATH: markdown/text requirement documents (file or directory,\n"
                "  repeatable); the review stage drafts contracts traced to their sentences.\n"
                "--contracts-approved PATH: approvals of drafted contracts (default\n"
                "  <root>/contracts.approved.json); only approved clauses give PROVED-ASSUMING.\n"
                "Writes report.json, report.md and report.sarif (SARIF 2.1.0) under --out, plus\n"
                "triage.json (root-cause clusters; ordering only, never a status change).\n"
                "Subcommands over a finished report (see docs/AI.md):\n"
                "  prism regress [--report OUT/report.json] [--write-tests DIR] [--run --allow-exec]\n"
                "  prism ask \"<question>\" [--report OUT/report.json] [--json] [--no-llm]\n"
                "  prism draft [--report OUT/report.json] [--kind report|assurance]\n"
                "  prism triage [OUT] [--threshold T] [--no-embed]\n";
            return 0;
        } else if (!a.starts_with("-")) {
            path = a;
        }
    }

    if (cfg.gui) return launch_gui(argc, argv);

    cfg.root = std::filesystem::absolute(path);
    cfg.out = std::filesystem::absolute(cfg.out);

    auto report = run_pipeline(cfg);
    std::cout << "confidence " << report.confidence
              << "  (vis " << report.visibility << " x ans " << report.answer
              << " x res " << report.resolution << ")\n";
    std::cout << "report " << (cfg.out / "report.md").string() << "  (sarif "
              << (cfg.out / "report.sarif").string() << ")\n";
    for (auto& s : report.stages) {
        if (s.status == "NOTRUN") {
            static bool hdr = false;
            if (!hdr) {
                std::cout << "NOTRUN:\n";
                hdr = true;
            }
            std::cout << "  " << s.name << ": " << s.detail << "  " << s.install << "\n";
        }
    }
    std::vector<const Finding*> failed;
    for (auto& s : report.stages)
        for (auto& f : s.findings)
            if (f.status == "FAILED" || f.status == "CRASH") failed.push_back(&f);
    std::sort(failed.begin(), failed.end(), [](auto* a, auto* b) {
        int ka = a->status == "CRASH" ? 0 : 1;
        int kb = b->status == "CRASH" ? 0 : 1;
        if (ka != kb) return ka < kb;
        if (a->stage != b->stage) return a->stage < b->stage;
        return a->file < b->file;
    });
    for (std::size_t i = 0; i < failed.size() && i < 30; ++i) {
        auto* f = failed[i];
        std::cout << "  " << f->status << " " << f->stage << " " << f->file << ":"
                  << (f->line ? *f->line : 0) << " "
                  << (f->function ? *f->function : "") << " " << f->message << "\n";
    }
    return exit_code(report, fail_on);
}
