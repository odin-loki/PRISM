// prism_fuzz_corpus: the seed corpus for fuzzing PRISM's own input handling
// (roadmap 6.2; docs/FUZZ_SELF.md). Standard library only.
//
//     prism_fuzz_corpus OUT_DIR [--prism BIN] [--limit N] [--repo DIR]
//
// Writes, from files already in the repository:
//
//   OUT_DIR/c/         C and C++ sources (testdata/, tests/conformance/prism/),
//                      at most 64 KB each, at most N (default 200)
//   OUT_DIR/ll/        LLVM IR text: the C seeds compiled the way the pir stage
//                      does (clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone,
//                      then opt -passes=mem2reg,...) when clang/opt are present
//   OUT_DIR/report/    report.json / stages.jsonl / functions.json of a PRISM
//                      run on testdata/ (--prism BIN, else `prism` next to this
//                      tool or on PATH), plus hand-written edge cases
//   OUT_DIR/toml/      third_party/MANIFEST.toml and proofs/lakefile.toml
//   OUT_DIR/libfuzzer/ the same seeds with the selector byte the libFuzzer
//                      target tests/fuzz/fuzz_cparse.cpp expects (0 C, 1 IR, 2 JSON)
//
// It only compiles seeds, never runs them; the PRISM run uses --no-llm and no
// --allow-exec, so nothing from the tree is executed. A missing clang, opt or
// prism binary is printed as NOTRUN for that part, never skipped quietly.
// Seed file names are the first 16 hex digits of a 64-bit FNV-1a hash.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef PRISM_FUZZ_CORPUS_NO_MAIN
#define PRISM_FUZZ_CORPUS_INLINE
#else
#define PRISM_FUZZ_CORPUS_INLINE inline
#endif

namespace prism_fuzz_corpus {
namespace fs = std::filesystem;

inline constexpr std::uintmax_t kMaxSeed = 64 * 1024;
inline constexpr std::size_t kMaxReport = 256 * 1024;

// Hand-written JSON edge cases for the report / journal readers.
inline const std::vector<std::string>& edge_json() {
    static const std::vector<std::string> v = {
        "", "{}", "[]", "null", "0", "\"\"", R"({"stages": null})", R"({"stages": [null]})",
        R"({"stages": [{"findings": [null]}]})", R"({"functions": [1]})", R"({"stages": "x"})",
        R"({"started": "x", "visibility": [], "functions": [{"params": [1]}]})",
        R"({"stages": [{"name": 1, "status": {}, "elapsed": "NaN"}]})", "[1,2,3]\n{}\n",
        R"({"stages": [{"findings": [{"line": "12", "extra": []}]}]})"};
    return v;
}

PRISM_FUZZ_CORPUS_INLINE std::string seed_name(std::string_view data) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : data) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

PRISM_FUZZ_CORPUS_INLINE std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

PRISM_FUZZ_CORPUS_INLINE void put(const fs::path& dir, std::string_view data, std::string_view ext) {
    fs::create_directories(dir);
    std::ofstream(dir / (seed_name(data) + std::string(ext)), std::ios::binary)
        .write(data.data(), static_cast<std::streamsize>(data.size()));
}

// Source seeds: files under each root with one of the extensions, at most
// kMaxSeed bytes, in path order, the first `limit` of them.
PRISM_FUZZ_CORPUS_INLINE std::vector<fs::path> seeds(
    const std::vector<std::pair<fs::path, std::vector<std::string>>>& roots, std::size_t limit) {
    std::vector<fs::path> out;
    for (const auto& [root, exts] : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        std::vector<fs::path> found;
        for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            const auto ext = it->path().extension().string();
            if (std::find(exts.begin(), exts.end(), ext) == exts.end()) continue;
            if (it->file_size(e2) > kMaxSeed || e2) continue;
            found.push_back(it->path());
        }
        std::sort(found.begin(), found.end());
        out.insert(out.end(), found.begin(), found.end());
    }
    if (out.size() > limit) out.resize(limit);
    return out;
}

PRISM_FUZZ_CORPUS_INLINE std::string shell_quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

// Runs argv with stdout and stderr discarded; true on exit status 0.
PRISM_FUZZ_CORPUS_INLINE bool run(const std::vector<std::string>& argv) {
    std::string cmd;
    for (const auto& a : argv) cmd += (cmd.empty() ? "" : " ") + shell_quote(a);
    cmd += " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

PRISM_FUZZ_CORPUS_INLINE std::optional<fs::path> which(const std::vector<std::string>& names) {
    const char* path = std::getenv("PATH");
    if (!path) return std::nullopt;
    for (const auto& n : names) {
        std::string_view rest(path);
        while (true) {
            auto c = rest.find(':');
            fs::path cand = fs::path(std::string(rest.substr(0, c))) / n;
            std::error_code ec;
            if (!cand.parent_path().empty() && fs::is_regular_file(cand, ec) &&
                (fs::status(cand, ec).permissions() & fs::perms::owner_exec) != fs::perms::none)
                return cand;
            if (c == std::string_view::npos) break;
            rest.remove_prefix(c + 1);
        }
    }
    return std::nullopt;
}

struct Options {
    fs::path out;
    fs::path repo;
    std::optional<fs::path> prism;
    std::size_t limit = 200;
};

PRISM_FUZZ_CORPUS_INLINE int make_corpus(const Options& o, const fs::path& self_dir, std::ostream& log) {
    const auto& repo = o.repo;
    auto csrc = seeds({{repo / "testdata", {".c", ".cpp", ".h"}},
                       {repo / "tests" / "conformance" / "prism", {".c", ".cpp"}}},
                      o.limit);
    if (csrc.empty()) {
        log << "prism_fuzz_corpus: no C/C++ seeds under " << repo.string() << " (wrong --repo?)\n";
        return 1;
    }
    for (const auto& p : csrc)
        if (auto d = read_file(p)) put(o.out / "c", *d, p.extension().string());

    auto clang = which({"clang"});
    auto opt = which({"opt", "opt-18"});
    if (!clang) log << "ll seeds: NOTRUN (clang not found on PATH)\n";
    else if (!opt) log << "ll seeds: opt not found on PATH (NOTRUN for the optimised IR; clang IR only)\n";
    if (clang) {
        const auto tmp = o.out / ".tmp";
        fs::create_directories(tmp);
        std::size_t n = 0;
        for (const auto& p : csrc) {
            if (n >= 60) break;
            ++n;
            if (p.extension() != ".c") continue;
            const auto ll = tmp / "x.ll", lo = tmp / "y.ll";
            if (!run({clang->string(), "-S", "-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone", "-g", "-w",
                      p.string(), "-o", ll.string()}))
                continue;
            if (auto d = read_file(ll)) put(o.out / "ll", *d, ".ll");
            if (opt && run({opt->string(), "-S", "-passes=mem2reg,lowerswitch,loop-simplify,lcssa,instnamer",
                            ll.string(), "-o", lo.string()}))
                if (auto d = read_file(lo)) put(o.out / "ll", *d, ".ll");
        }
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }

    std::optional<fs::path> prism = o.prism;
    if (!prism) {
        std::error_code ec;
        if (fs::is_regular_file(self_dir / "prism", ec)) prism = self_dir / "prism";
        else prism = which({"prism"});
    }
    if (!prism) {
        log << "report seeds from a PRISM run: NOTRUN (no prism binary; pass --prism BIN)\n";
    } else {
        const auto run_out = o.out / ".prism-run";
        std::error_code ec;
        fs::remove_all(run_out, ec);
        run({prism->string(), (repo / "testdata").string(), "--no-llm", "--stage", "inventory,classify,lints,bmc",
             "--out", run_out.string()});
        bool any = false;
        for (const char* name : {"report.json", "stages.jsonl", "functions.json"}) {
            if (auto d = read_file(run_out / name)) {
                put(o.out / "report", std::string_view(*d).substr(0, kMaxReport), fs::path(name).extension().string());
                any = true;
            }
        }
        if (!any) log << "report seeds from a PRISM run: ERROR (" << prism->string() << " wrote no report)\n";
        fs::remove_all(run_out, ec);
    }
    for (const auto& e : edge_json()) put(o.out / "report", e, ".json");

    for (const auto& t : {repo / "third_party" / "MANIFEST.toml", repo / "proofs" / "lakefile.toml"})
        if (auto d = read_file(t)) put(o.out / "toml", *d, ".toml");

    const std::vector<std::pair<std::string, char>> sel = {{"c", '\0'}, {"ll", '\1'}, {"report", '\2'}};
    for (const auto& [kind, prefix] : sel) {
        std::error_code ec;
        if (!fs::is_directory(o.out / kind, ec)) continue;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(o.out / kind, ec)) files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files)
            if (auto d = read_file(f)) put(o.out / "libfuzzer", std::string(1, prefix) + *d, "");
    }
    std::string line;
    for (const char* k : {"c", "ll", "report", "toml", "libfuzzer"}) {
        std::size_t n = 0;
        std::error_code ec;
        if (fs::is_directory(o.out / k, ec))
            for ([[maybe_unused]] const auto& e : fs::directory_iterator(o.out / k, ec)) ++n;
        line += (line.empty() ? "" : " ") + std::string(k) + "=" + std::to_string(n);
    }
    log << line << "\n";
    return 0;
}

}  // namespace prism_fuzz_corpus

#ifndef PRISM_FUZZ_CORPUS_NO_MAIN
#ifndef PRISM_REPO_DIR
#define PRISM_REPO_DIR "."
#endif
int main(int argc, char** argv) {
    namespace fz = prism_fuzz_corpus;
    namespace fs = std::filesystem;
    fz::Options o;
    o.repo = PRISM_REPO_DIR;
    if (const char* e = std::getenv("PRISM_BIN"); e && *e) o.prism = e;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--prism" && i + 1 < argc) o.prism = argv[++i];
        else if (a == "--limit" && i + 1 < argc) o.limit = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--repo" && i + 1 < argc) o.repo = argv[++i];
        else if (a.rfind("--", 0) == 0 || !o.out.empty()) {
            std::cerr << "usage: prism_fuzz_corpus OUT_DIR [--prism BIN] [--limit N] [--repo DIR]\n";
            return 2;
        } else o.out = a;
    }
    if (o.out.empty()) {
        std::cerr << "usage: prism_fuzz_corpus OUT_DIR [--prism BIN] [--limit N] [--repo DIR]\n";
        return 2;
    }
    std::error_code ec;
    auto self = fs::weakly_canonical(fs::path(argv[0]), ec).parent_path();
    return fz::make_corpus(o, self, std::cout);
}
#endif
