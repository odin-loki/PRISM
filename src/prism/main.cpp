#include "prism/ai_proof.hpp"
#include "prism/ai_assist.hpp"
#include "prism/cli.hpp"
#include "prism/config.hpp"
#include "prism/pipeline.hpp"
#include "prism/pir.hpp"
#include "prism/svcomp.hpp"
#include "proc.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
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
    // Ctrl-C / SIGTERM (a scorer's timeout) also kills the solvers, checkers
    // and compilers PRISM started: they run in process groups of their own
    detail::install_child_cleanup();
    // Roadmap 9.2: `prism prove FILE.lean THEOREM` (Lean proof search).
    if (argc > 1 && std::string_view(argv[1]) == "prove") return ai::prove_main(argc - 1, argv + 1);
    // Report-level subcommands (roadmap 9.3 / 9.4): they read report.json and
    // never change a verdict.
    if (argc > 1) {
        const std::string sub = argv[1];
        if (sub == "regress") return ai::regress_main(argc - 2, argv + 2);
        if (sub == "ask") return ai::ask_main(argc - 2, argv + 2);
        if (sub == "draft") return ai::draft_main(argc - 2, argv + 2);
        // Roadmap 6.3: PRISM as an SV-COMP verifier (docs/SVCOMP.md).
        if (sub == "svcomp") return svcomp::svcomp_main(argc - 2, argv + 2);
        if (sub == "triage") {
            auto parsed = parse_triage_cli(std::span<const char* const>(argv + 2, argc - 2));
            if (!parsed) {
                std::cerr << "prism triage: error: " << parsed.error() << "\n";
                return 2;
            }
            std::filesystem::path rp = parsed->report_dir;
            ai::TriageOptions topt;
            if (parsed->threshold >= 0) topt.threshold = parsed->threshold;
            topt.use_embedder = parsed->use_embedder;
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
    // --gui: prism_gui parses its own flags (e.g. --smoke-screenshot); hand
    // every argument over before the scan parser rejects the ones it does not know.
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--gui") == 0) return launch_gui(argc, argv);

    auto parsed = parse_cli(std::span<const char* const>(argv + 1, argc - 1));
    if (!parsed) {
        std::cerr << "prism: error: " << parsed.error() << "\n(prism --help lists the options)\n";
        return 2;
    }
    if (parsed->help) {
        std::cout << cli_usage();
        return 0;
    }
    if (parsed->version) {
        std::cout << "prism " << PRISM_VERSION << " (C++ engine)\n";
        return 0;
    }
    if (parsed->list_stages) {
        for (auto* p = STAGE_ORDER; *p; ++p) std::cout << *p << "\n";
        return 0;
    }
    auto& cfg = parsed->cfg;
    const auto& fail_on = parsed->fail_on;
    const auto& pir_vcs_src = parsed->pir_vcs_src;
    const auto& solve_smt2 = parsed->solve_smt2;
    const bool z3_only = parsed->z3_only;

    if (!pir_vcs_src.empty()) {
        std::cout << prism::pir::unit_vcs_json(std::filesystem::absolute(pir_vcs_src), cfg,
                                               std::filesystem::absolute(cfg.out))
                  << "\n";
        return 0;
    }
    if (!solve_smt2.empty()) {
        std::ifstream in(solve_smt2, std::ios::binary);
        if (!in) {
            std::cerr << "cannot read " << solve_smt2 << "\n";
            return 2;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        std::cout << prism::pir::solve_smt2_json(ss.str(), cfg, z3_only) << "\n";
        return 0;
    }

    cfg.root = std::filesystem::absolute(parsed->path);
    cfg.out = std::filesystem::absolute(cfg.out);
    // A mistyped PATH is not an empty, clean tree (Laws 1 and 7).
    if (std::error_code ec; !std::filesystem::exists(cfg.root, ec)) {
        std::cerr << "prism: error: PATH does not exist: " << cfg.root.string() << "\n";
        return 2;
    }

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
