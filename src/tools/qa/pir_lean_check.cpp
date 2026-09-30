// pir-lean-check: the C++ LLVM->PIR translator against the proved Lean
// translator (roadmap 8.2 "LLVM IR to PIR translation", 2.4 translation
// validation).
//
// Runs PRISM's pir stage over each tree with PRISM_PIR_LEAN_EXPORT set, so
// every translated function is written as an (LLVM fragment, PIR) pair
// (src/prism/pir/export_lean.cpp), then runs pir_lean_check
// (proofs/refinement/CheckMain.lean) on the pairs. For every function in the
// modelled LLVM fragment the checker demands that the C++ PIR be exactly the
// output of the Lean translator the refinement theorems are proved about
// (proofs/refinement/PrismRefine/Sound.lean: pir_sound and friends), so for
// those functions the theorems hold of the PIR PRISM actually verified. The
// checker stays Lean: it is that proved translator.
//
// Exit 1 when any function is a MISMATCH, 2 when a tool is missing or no
// tree exported a pair (NOTRUN, never a silent pass).

#include "qa.hpp"

#include "../../prism/proc.hpp"
#include "prism/ai_proof.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace prism::qa {

namespace {
const char* const KINDS[] = {"agree", "agree-ext", "agree-reject", "outside", "MISMATCH"};

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    std::size_t a = 0;
    for (;;) {
        auto b = line.find('\t', a);
        out.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

std::vector<std::string> words(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

std::vector<fs::path> pirl_files(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".pirl") out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace

CheckerRows parse_checker_output(const std::string& out) {
    CheckerRows r;
    std::istringstream in(out);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto cols = split_tabs(line);
        if (cols.size() < 3) continue;
        if (std::find(std::begin(KINDS), std::end(KINDS), cols[0]) == std::end(KINDS)) continue;
        ++r.counts[cols[0]];
        r.rows.push_back(std::move(cols));
    }
    return r;
}

std::string lean_total_line(const std::map<std::string, int>& total) {
    auto get = [&](const char* k) {
        auto it = total.find(k);
        return it == total.end() ? 0 : it->second;
    };
    const int in_fragment = get("agree") + get("agree-ext") + get("MISMATCH");
    return "total: agree=" + std::to_string(get("agree")) + " agree-ext=" + std::to_string(get("agree-ext")) +
           " agree-reject=" + std::to_string(get("agree-reject")) + " outside=" + std::to_string(get("outside")) +
           " mismatch=" + std::to_string(get("MISMATCH")) +
           " (functions in the proved fragment: " + std::to_string(in_fragment) + ")";
}

std::optional<fs::path> find_pir_lean_checker(const std::string& explicit_path, const fs::path& repo) {
    std::error_code ec;
    if (!explicit_path.empty()) {
        if (fs::is_regular_file(explicit_path, ec)) return fs::absolute(explicit_path);
        return std::nullopt;
    }
    const auto project = repo / "proofs" / "refinement";
    const auto exe = project / ".lake" / "build" / "bin" / "pir_lean_check";
    if (fs::is_regular_file(exe, ec)) return exe;
    auto lake = ai::find_lake();
    if (!lake) return std::nullopt;
    detail::SessionOptions so;
    so.cwd = project;
    auto r = detail::run_session({lake->string(), "build", "pir_lean_check"}, so);
    if (r.rc == 0 && fs::is_regular_file(exe, ec)) return exe;
    return std::nullopt;
}

int pir_lean_check_main(const Args& args) {
    const auto repo = repo_root();
    std::vector<fs::path> trees;
    std::string bin, checker_arg;
    fs::path json_out, keep;
    int jobs = static_cast<int>(std::max(2u, std::thread::hardware_concurrency()));
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "--bin") bin = next();
        else if (a == "--checker") checker_arg = next();
        else if (a == "--jobs" || a == "-j") jobs = std::max(1, std::atoi(next().c_str()));
        else if (a == "--json") json_out = next();
        else if (a == "--keep") keep = next();
        else if (a == "-h" || a == "--help") {
            std::cout << "prism-qa pir-lean-check [TREE...] [--bin PRISM] [--checker EXE] [--jobs N]\n"
                         "                        [--json OUT.json] [--keep DIR]\n"
                         "C++ LLVM->PIR translator vs the proved Lean translator (proofs/refinement).\n"
                         "Default trees: tests/pir and testdata. Exit 1 on a MISMATCH, 2 NOTRUN.\n";
            return 0;
        } else if (!a.starts_with("-")) trees.emplace_back(a);
        else {
            std::cerr << "pir-lean-check: unknown option " << a << "\n";
            return 2;
        }
    }
    auto exe = find_prism(bin, repo);
    if (!exe) {
        std::cout << "pir_lean_check: NOTRUN: no PRISM binary" << (bin.empty() ? "" : " at " + bin)
                  << " (build it or pass --bin)\n";
        return 2;
    }
    auto checker = find_pir_lean_checker(checker_arg, repo);
    if (!checker) {
        std::cout << "pir_lean_check: NOTRUN: pir_lean_check not built and lake not found "
                     "(cd proofs/refinement && lake build)\n";
        return 2;
    }
    if (trees.empty()) trees = {repo / "tests" / "pir", repo / "testdata"};

    nlohmann::json summary = nlohmann::json::object();
    std::map<std::string, int> total;
    std::vector<std::vector<std::string>> mismatches;
    int exported_trees = 0;
    TempDir tmp("prism-pir-lean-");
    for (const auto& tree : trees) {
        const auto out = tmp.path / tree.filename();
        const auto pairs = out / "pairs";
        detail::SessionOptions so;
        so.cwd = repo;
        so.env = {{"PRISM_PIR_LEAN_EXPORT", pairs.string()}};
        auto run = detail::run_session({exe->string(), fs::absolute(tree).string(), "--no-llm", "--stage",
                                        "inventory,classify,pir", "--out", (out / "prism-out").string(), "--jobs",
                                        std::to_string(jobs)},
                                       so);
        const auto files = pirl_files(pairs);
        if (files.empty()) {
            const auto log = run.out + run.err;
            std::cout << tree.string() << ": no pairs exported (pir stage did not run?)\n"
                      << log.substr(log.size() > 2000 ? log.size() - 2000 : 0) << "\n";
            summary[tree.string()] = {{"error", "no pairs exported"}};
            continue;
        }
        ++exported_trees;
        std::vector<std::string> argv{checker->string()};
        for (auto& f : files) argv.push_back(f.string());
        auto r = detail::run_session(argv);
        auto parsed = parse_checker_output(r.out);
        // outside reasons: the first two words of the reason column, most common first
        std::vector<std::pair<std::string, int>> reasons;
        for (const auto& row : parsed.rows) {
            if (row[0] == "MISMATCH") mismatches.push_back(row);
            if (row[0] != "outside" || row.size() <= 3) continue;
            auto w = words(row[3]);
            std::string key = w.empty() ? "" : w[0];
            if (w.size() > 1) key += " " + w[1];
            auto it = std::find_if(reasons.begin(), reasons.end(), [&](auto& p) { return p.first == key; });
            if (it == reasons.end()) reasons.emplace_back(key, 1);
            else ++it->second;
        }
        std::stable_sort(reasons.begin(), reasons.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if (reasons.size() > 12) reasons.resize(12);
        nlohmann::json rj = nlohmann::json::object();
        for (auto& [k, n] : reasons) rj[k] = n;
        nlohmann::json cj = nlohmann::json::object();
        for (auto& [k, n] : parsed.counts) {
            cj[k] = n;
            total[k] += n;
        }
        summary[tree.string()] = {{"counts", cj}, {"outside_reasons", rj}};
        std::cout << tree.string() << ": ";
        bool first = true;
        for (auto* k : KINDS) {
            std::cout << (first ? "" : ", ") << k << "=" << parsed.counts[k];
            first = false;
        }
        std::cout << "\n";
        if (!keep.empty()) {
            const auto dst = keep / tree.filename();
            std::error_code ec;
            fs::create_directories(dst, ec);
            for (auto& f : files) fs::copy_file(f, dst / f.filename(), fs::copy_options::overwrite_existing, ec);
        }
    }
    for (const auto& row : mismatches) {
        std::cout << "MISMATCH";
        for (std::size_t i = 1; i < row.size(); ++i) std::cout << " " << row[i];
        std::cout << "\n";
    }
    std::cout << lean_total_line(total) << "\n";
    if (!json_out.empty()) {
        nlohmann::json tj = nlohmann::json::object();
        for (auto& [k, n] : total) tj[k] = n;
        std::ofstream(json_out, std::ios::binary) << nlohmann::json{{"trees", summary}, {"total", tj}}.dump(2) << "\n";
    }
    if (total["MISMATCH"]) return 1;
    if (exported_trees == 0) {
        std::cout << "pir_lean_check: NOTRUN: no tree exported a pair; nothing was checked\n";
        return 2;
    }
    return 0;
}

}  // namespace prism::qa
