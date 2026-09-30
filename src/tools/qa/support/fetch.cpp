#include "fetch.hpp"

#include "proctree.hpp"

#include "prism/ai.hpp"
#include "prism/regex.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

#include <unistd.h>

namespace prism::qa {

const std::string JULIET_URL =
    "https://samate.nist.gov/SARD/downloads/test-suites/2017-10-01-juliet-test-suite-for-c-cplusplus-v1-3.zip";
const std::string JULIET_SHA256 = "ada9d7e1c323d283446df3f55bdee0d00bda1fed786785fe98764d58688f38eb";
const long long JULIET_SIZE = 152957342;
const std::vector<std::string> JULIET_CWES = {"CWE190", "CWE191", "CWE369", "CWE476", "CWE680"};

namespace {

// Only data types whose overflow is undefined behaviour: unsigned wrap and
// char/short arithmetic (done in int) are not UB, so Juliet's CWE190/191
// variants on those types are not `false` tasks for a UB checker.
const std::map<std::string, std::string> JULIET_TYPE = {
    {"CWE190", R"(__(int|int64_t)_)"},
    {"CWE191", R"(__(int|int64_t)_)"},
    {"CWE369", R"(__int_)"},
    {"CWE476", R"(__(int|long|int64_t|char|struct)_\d\d)"},
    {"CWE680", R"(__)"},
};
const std::map<std::string, std::string> JULIET_PROPERTY = {
    {"CWE190", "no-overflow"}, {"CWE191", "no-overflow"}, {"CWE369", "no-div0"},
    {"CWE476", "no-null-deref"}, {"CWE680", "memsafety"},
};

void write_file(const fs::path& p, const std::string& text) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o << text;
    if (!o) throw std::runtime_error("cannot write " + p.string());
}

std::vector<std::string> split_lines(const std::string& s) {
    // str.splitlines() for \n, \r\n and \r
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\n' || c == '\r') {
            out.push_back(cur);
            cur.clear();
            if (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ++i;
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string strip(const std::string& s) {
    std::size_t a = 0, b = s.size();
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; };
    while (a < b && ws(s[a])) ++a;
    while (b > a && ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string re_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) out += '\\';
        out += c;
    }
    return out;
}

std::vector<fs::path> sorted_rglob(const fs::path& root, const std::string& name_or_ext, bool by_ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const auto& p = it->path();
        if (by_ext ? p.extension() == name_or_ext : p.filename() == name_or_ext) out.push_back(p);
    }
    auto parts = [](const fs::path& p) {
        std::vector<std::string> v;
        for (const auto& c : p) v.push_back(c.string());
        return v;
    };
    std::sort(out.begin(), out.end(), [&](const fs::path& a, const fs::path& b) { return parts(a) < parts(b); });
    return out;
}

RunResult must_run(const std::vector<std::string>& argv, double timeout, const fs::path& cwd = {}) {
    RunOpts o;
    o.timeout_s = timeout;
    o.cwd = cwd;
    auto r = run_tree(argv, o);
    if (r.start_failed) throw std::runtime_error(argv[0] + ": NOTRUN (" + r.err + ")");
    if (r.timed_out) throw std::runtime_error(argv[0] + ": timed out");
    return r;
}

}  // namespace

std::string latin1_to_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::optional<std::string> juliet_task(const std::string& base, const std::string& text, const std::string& cwe) {
    static const Regex fn_re(R"((?m)^(?:static\s+)?void\s+(\w+)\s*\(\s*(?:void)?\s*\)\s*$\s*\{)");
    std::vector<std::pair<std::string, bool>> expected;
    auto set = [&](const std::string& k, bool v) {
        for (auto& [kk, vv] : expected)
            if (kk == k) {
                vv = v;
                return;
            }
        expected.emplace_back(k, v);
    };
    for (const auto& m : fn_re.finditer(text)) {
        std::string name = m.group(1);
        auto ends = [&](const std::string& suf) {
            return name.size() >= suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0;
        };
        if (ends("_bad")) set(name, false);
        else if (name.rfind("good", 0) == 0 || ends("_good")) set(name, true);
    }
    if (expected.empty()) return std::nullopt;
    std::string y = "format_version: 1\ninput_files: " + base + "\nlanguage: C\nproperty: " + JULIET_PROPERTY.at(cwe) +
                    "\norigin: juliet-1.3\nexpected:\n";
    for (const auto& [k, v] : expected) y += "  " + k + ": " + (v ? "true" : "false") + "\n";
    return y;
}

fs::path fetch_juliet(const fs::path& dest, const std::vector<std::string>& flows, const JulietSource& src) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    fs::path zpath = dest / "juliet-c-cplusplus-v1.3.zip";
    auto size_of = [&](const fs::path& p) -> long long {
        std::error_code e;
        auto s = fs::file_size(p, e);
        return e ? -1 : static_cast<long long>(s);
    };
    if (!fs::exists(zpath, ec) || size_of(zpath) != src.size) {
        std::cerr << "downloading " << src.url << "\n";
        if (which("curl").empty()) throw std::runtime_error("curl: NOTRUN (not on PATH; needed to download Juliet)");
        fs::path tmp = zpath;
        tmp.replace_extension(".part");
        // NIST answers 403 to some default User-Agents.
        auto r = must_run({"curl", "-fsSL", "--retry", "3", "-A", "prism-conformance/1 (curl-compatible)", "-o",
                           tmp.string(), src.url},
                          3600);
        if (r.rc != 0) throw std::runtime_error("download failed: " + py_tail(utf8_clean(r.err), 400));
        fs::rename(tmp, zpath, ec);
        if (ec) throw std::runtime_error("cannot move " + tmp.string() + ": " + ec.message());
    }
    std::string h = prism::ai::sha256_file(zpath);
    if (h != src.sha256) throw std::runtime_error("Juliet archive hash mismatch: " + h + " != " + src.sha256);
    if (which("unzip").empty()) throw std::runtime_error("unzip: NOTRUN (not on PATH; needed to extract Juliet)");
    fs::path tasks_root = dest / "juliet";
    static const Regex name_re(R"(^(CWE\d+)_\w+?_(\d\d)\.c\z)");
    auto listing = must_run({"unzip", "-Z1", zpath.string()}, 600);
    if (listing.rc != 0) throw std::runtime_error("unzip -Z1 failed: " + py_tail(utf8_clean(listing.err), 400));
    std::vector<std::string> support, wanted;
    for (const auto& name : split_lines(listing.out)) {
        std::vector<std::string> parts;
        std::size_t a = 0;
        for (;;) {
            std::size_t b = name.find('/', a);
            parts.push_back(name.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        const bool is_dir = !name.empty() && name.back() == '/';
        if (parts.size() < 3 || parts[0] != "C") continue;
        if (parts[1] == "testcasesupport" && !is_dir) {
            support.push_back(name);
            continue;
        }
        if (parts[1] != "testcases" || is_dir) continue;
        const std::string& base = parts.back();
        auto m = name_re.search_match(base);
        if (!m) continue;
        std::string cwe = m->group(1), flow = m->group(2);
        if (std::find(JULIET_CWES.begin(), JULIET_CWES.end(), cwe) == JULIET_CWES.end()) continue;
        if (std::find(flows.begin(), flows.end(), flow) == flows.end()) continue;
        if (!Regex(JULIET_TYPE.at(cwe)).search(base)) continue;
        wanted.push_back(name);
    }
    auto extract = [&](const std::vector<std::string>& names, const fs::path& into) {
        for (std::size_t i = 0; i < names.size(); i += 200) {
            std::vector<std::string> argv = {"unzip", "-o", "-qq", zpath.string()};
            for (std::size_t k = i; k < std::min(names.size(), i + 200); ++k) argv.push_back(names[k]);
            argv.insert(argv.end(), {"-d", into.string()});
            auto r = must_run(argv, 1800);
            if (r.rc != 0) throw std::runtime_error("unzip failed: " + py_tail(utf8_clean(r.err), 400));
        }
    };
    extract(support, dest);
    std::string tmpl = (dest / ".extract-XXXXXX").string();
    if (!::mkdtemp(tmpl.data())) throw std::runtime_error("mkdtemp failed under " + dest.string());
    fs::path tmp = tmpl;
    int written = 0;
    try {
        extract(wanted, tmp);
        for (const auto& name : wanted) {
            std::string base = fs::path(name).filename().string();
            std::string cwe = name_re.search_match(base)->group(1);
            std::string text = latin1_to_utf8(read_text(tmp / name));
            auto y = juliet_task(base, text, cwe);
            if (!y) continue;
            fs::path out_dir = tasks_root / cwe;
            fs::create_directories(out_dir, ec);
            write_file(out_dir / base, text);
            write_file(out_dir / (base.substr(0, base.size() - 2) + ".yml"), *y);
            ++written;
        }
    } catch (...) {
        fs::remove_all(tmp, ec);
        throw;
    }
    fs::remove_all(tmp, ec);
    std::cerr << "juliet: " << written << " task files under " << tasks_root.string() << "\n";
    return tasks_root;
}

// ---------------------------------------------------------------------------- ESBMC

const std::string ESBMC_REPO = "https://github.com/esbmc/esbmc.git";
const std::string ESBMC_COMMIT = "653926f91580d8d67858d42db814f09d9a9fa257";
const std::vector<std::string> ESBMC_DIRS = {"regression/esbmc-cpp",   "regression/esbmc-cpp11",
                                             "regression/esbmc-cpp14", "regression/esbmc-cpp17",
                                             "regression/esbmc-cpp20", "regression/esbmc-cpp23"};
// Options that change what "VERIFICATION SUCCESSFUL/FAILED" speaks about.
const std::vector<std::string> ESBMC_SKIP_OPTIONS = {
    "--no-assertions", "--no-bounds-check", "--no-pointer-check", "--no-div-by-zero-check",
    "--no-align-check", "--no-pointer-relation-check", "--memory-leak-check", "--overflow-check",
    "--unsigned-overflow-check", "--ub-shift-check", "--nan-check", "--data-races-check",
    "--deadlock-check", "--function", "--cheri", "--struct-fields-check", "--is-instr-modelling",
    // a reachable `ERROR:` label is a violation: PRISM checks no labels
    "--error-label"};
// Labels that contradict the C++ standard (and a native sanitizer run of the
// deterministic program): skipped at conversion, each with the reason.
const std::vector<std::pair<std::string, std::string>> ESBMC_DISPUTED = {
    {"esbmc-cpp/cpp/github_6199_fail",
     "std::string(nullptr, 0): [nullptr, nullptr + 0) is a valid empty range ([string.cons]); ESBMC's string model "
     "checks null first"},
    {"esbmc-cpp/cpp/github_6588_multidim_fail",
     "new int[2][3]() value-initialises to zero ([expr.new], [dcl.init]); the assert holds"},
};
// Curated subset (tests/conformance/esbmc-cpp): categories and files whose
// licence is not ESBMC's own Apache-2.0 are left out (CBMC's BSD-4-clause
// tests, GCC testsuite files, Qt, textbook listings).
const std::set<std::string> ESBMC_CURATE_SKIP_CATEGORIES = {"cpp03_cbmc", "cpp03_gcc_template_tests", "cpp03_qt",
                                                            "cpp03_esbmc_systemc"};
const std::string ESBMC_FOREIGN_TEXT =
    R"((?i)copyright|\(c\)|deitel|pearson|fig\.\s*\d|llbmc|gnu general|licen[cs]e|\bgcc\b|\bdg-)";

namespace {
const std::set<std::string> ESBMC_SOURCE_SUFFIXES = {".c",   ".cc", ".cpp", ".cxx", ".c++", ".h",
                                                     ".hh",  ".hpp", ".hxx", ".inc", ".tcc"};
// FAILED verdicts that come from a bound of ESBMC's C++ operational model
// (its fixed-capacity string/stream models), not from the program.
const char* const ESBMC_MODEL_FAILURE = R"((?i)capacity exceed|forgotten memory|memory leak)";
// Programs whose single run is not their only behaviour.
const char* const ESBMC_NONDET =
    R"(\bnondet_|__VERIFIER_nondet|\brand\s*\(|\bcin\b|\bscanf\b|\bgetchar\b|\bfgets\b|\bargv\b|\btime\s*\(|\bstd::random_device|\bthread\b|pthread_)";

std::string option_skip(const std::string& opts) {
    for (const auto& flag : ESBMC_SKIP_OPTIONS)
        if (Regex(R"((?:^|\s))" + re_escape(flag) + R"((?:[\s=]|$))").search(opts))
            return "option " + flag + " changes the checked property set";
    return "";
}
}  // namespace

std::pair<std::optional<EsbmcTask>, std::string> esbmc_convert(const fs::path& test_dir, const fs::path& regression,
                                                               bool thorough) {
    auto desc = split_lines(utf8_clean(read_text(test_dir / "test.desc")));
    auto lstrip = [](const std::string& s) {
        std::size_t a = 0;
        while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        return s.substr(a);
    };
    if (desc.size() < 3 || lstrip(desc[0]).rfind("<", 0) == 0)
        return {std::nullopt, "test.desc not in the CORE/THOROUGH line format"};
    std::string level = strip(desc[0]);
    std::string up = test_dir.lexically_relative(regression).generic_string();
    for (const auto& [d, _] : ESBMC_DISPUTED)
        if (d == up) return {std::nullopt, "label contradicts the C++ standard (ESBMC_DISPUTED)"};
    if (level == "KNOWNBUG") return {std::nullopt, "KNOWNBUG (ESBMC's own label is not trusted upstream)"};
    if (level != "CORE" && level != "THOROUGH") return {std::nullopt, "test level '" + level + "'"};
    if (level == "THOROUGH" && !thorough) return {std::nullopt, "THOROUGH (slow tier; --esbmc-thorough)"};
    std::string main_file = strip(desc[1]);
    std::vector<fs::path> sources;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(test_dir, ec))
        if (e.is_regular_file() && ESBMC_SOURCE_SUFFIXES.count(lower(e.path().extension().string())))
            sources.push_back(e.path());
    std::string msuf = lower(fs::path(main_file).extension().string());
    if (main_file.empty() || !(msuf == ".cpp" || msuf == ".cc" || msuf == ".cxx" || msuf == ".c++"))
        return {std::nullopt, "input is not a single C++ file"};
    if (sources.size() != 1 || sources[0].filename().string() != main_file)
        return {std::nullopt, "several source/header files (PRISM tasks are single files)"};
    std::string opts = strip(desc[2]);
    if (auto why = option_skip(opts); !why.empty()) return {std::nullopt, why};
    static const Regex verdict_re(R"(^\s*\^?\s*VERIFICATION (SUCCESSFUL|FAILED)\s*\$?\s*\z)");
    std::set<std::string> verdicts;
    std::string patterns;
    for (std::size_t i = 3; i < desc.size(); ++i) {
        if (auto m = verdict_re.search_match(desc[i])) verdicts.insert(m->group(1));
        if (i > 3) patterns += "\n";
        patterns += desc[i];
    }
    if (verdicts.size() != 1) return {std::nullopt, "no single VERIFICATION SUCCESSFUL/FAILED line"};
    bool expected = *verdicts.begin() == "SUCCESSFUL";
    if (!expected && Regex(ESBMC_MODEL_FAILURE).search(patterns))
        return {std::nullopt, "failure is a limit of ESBMC's operational model, not of the program"};
    std::string text = utf8_clean(read_text(sources[0]));
    if (text.find("__ESBMC") != std::string::npos) return {std::nullopt, "uses ESBMC intrinsics"};
    EsbmcTask t;
    static const Regex std_re(R"(--std[= ]\s*(\S+))");
    if (auto m = std_re.search_match(opts)) {
        t.std = m->group(1);
        for (std::size_t p; (p = t.std.find("gnu++")) != std::string::npos;) t.std.replace(p, 5, "c++");
    }
    static const Regex unwind_re(R"(--unwind\s+(\d+))");
    auto unwind = unwind_re.search_match(opts);
    t.source = sources[0];
    t.text = text;
    t.upstream = up;
    t.level = level;
    t.options = opts;
    t.expected = expected;
    // a SUCCESSFUL under --no-unwinding-assertions only speaks up to the bound
    t.label_bound = expected && unwind && opts.find("--no-unwinding-assertions") != std::string::npos
                        ? std::atoi(unwind->group(1).c_str())
                        : 0;
    t.deterministic = !Regex(ESBMC_NONDET).search(text);
    for (std::size_t i = 3; i < desc.size() && t.reason.size() < 3; ++i) {
        std::string ln = strip(desc[i]);
        if (!ln.empty() && desc[i].find("VERIFICATION") == std::string::npos) t.reason.push_back(ln);
    }
    return {t, ""};
}

std::pair<std::string, std::string> esbmc_task_name(const std::string& upstream) {
    std::vector<std::string> parts;
    for (const auto& p : fs::path(upstream)) parts.push_back(p.string());
    auto join_from = [&](std::size_t k) {
        std::string s;
        for (std::size_t i = k; i < parts.size(); ++i) s += (i > k ? "__" : "") + parts[i];
        return s;
    };
    if (!parts.empty() && parts[0] == "esbmc-cpp" && parts.size() > 2) {
        std::string cat = parts[1];
        for (auto& c : cat)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) c = '_';
        return {"cpp03_" + cat, join_from(2)};
    }
    std::string cat = parts.empty() ? "" : parts[0];
    if (auto p = cat.find("esbmc-"); p != std::string::npos) cat.erase(p, 6);
    return {cat, join_from(1)};
}

std::string esbmc_task_yaml(const EsbmcTask& t, const std::string& src_name) {
    std::string y = "format_version: 1\ninput_files: " + src_name + "\nlanguage: C++\nproperty: esbmc-cpp\nupstream: esbmc@" +
                    ESBMC_COMMIT.substr(0, 12) + " regression/" + t.upstream + "\nesbmc_options: " +
                    ojson(t.options).dump(-1, ' ', true) + "\n";
    if (!t.std.empty()) y += "std: " + t.std + "\n";
    if (t.label_bound) y += "label_bound: " + std::to_string(t.label_bound) + "\n";
    y += std::string("deterministic: ") + (t.deterministic ? "true" : "false") + "\n";
    y += std::string("expected:\n  main: ") + (t.expected ? "true" : "false") + "\n";
    return y;
}

fs::path fetch_esbmc(const fs::path& dest, bool thorough) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    fs::path repo = dest / "esbmc-src";
    if (which("git").empty()) throw std::runtime_error("git: NOTRUN (not on PATH)");
    auto git = [&](std::vector<std::string> a) {
        std::vector<std::string> argv = {"git", "-C", repo.string()};
        argv.insert(argv.end(), a.begin(), a.end());
        auto r = must_run(argv, 1800);
        if (r.rc != 0) {
            std::string what;
            for (const auto& x : a) what += (what.empty() ? "" : " ") + x;
            throw std::runtime_error("git " + what + ": " + py_tail(strip(utf8_clean(r.err)), 400));
        }
        return r.out;
    };
    if (!fs::exists(repo / ".git", ec)) {
        fs::create_directories(repo, ec);
        git({"init", "-q"});
        git({"remote", "add", "origin", ESBMC_REPO});
    }
    RunOpts o;
    o.timeout_s = 60;
    std::string head = strip(run_tree({"git", "-C", repo.string(), "rev-parse", "HEAD"}, o).out);
    std::vector<std::string> sparse = {"sparse-checkout", "set"};
    sparse.insert(sparse.end(), ESBMC_DIRS.begin(), ESBMC_DIRS.end());
    if (head != ESBMC_COMMIT) {
        std::cerr << "fetching " << ESBMC_REPO << " @ " << ESBMC_COMMIT << "\n";
        git({"fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", ESBMC_COMMIT});
        git(sparse);
        git({"checkout", "-q", "--detach", "FETCH_HEAD"});
    } else {
        git(sparse);
    }
    head = strip(git({"rev-parse", "HEAD"}));
    if (head != ESBMC_COMMIT) throw std::runtime_error("ESBMC checkout is at " + head + ", pinned " + ESBMC_COMMIT);
    fs::path regression = repo / "regression";
    fs::path tasks_root = dest / "esbmc-cpp";
    if (fs::exists(tasks_root, ec)) fs::remove_all(tasks_root, ec);
    std::map<std::string, int> skipped;
    std::vector<std::string> skip_order;
    int written = 0;
    for (const auto& d : ESBMC_DIRS) {
        for (const auto& desc : sorted_rglob(repo / d, "test.desc", false)) {
            auto [t, why] = esbmc_convert(desc.parent_path(), regression, thorough);
            if (!t) {
                if (!skipped.count(why)) skip_order.push_back(why);
                skipped[why]++;
                continue;
            }
            auto [cat, stem] = esbmc_task_name(t->upstream);
            fs::path out = tasks_root / cat;
            fs::create_directories(out, ec);
            std::string src_name = stem + ".cpp";
            write_file(out / src_name, t->text);
            write_file(out / (stem + ".yml"), esbmc_task_yaml(*t, src_name));
            ++written;
        }
    }
    std::cerr << "esbmc-cpp: " << written << " task files under " << tasks_root.string() << "\n";
    std::stable_sort(skip_order.begin(), skip_order.end(),
                     [&](const std::string& a, const std::string& b) { return skipped[a] > skipped[b]; });
    for (const auto& why : skip_order) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%5d", skipped[why]);
        std::cerr << "  skipped " << buf << ": " << why << "\n";
    }
    return tasks_root;
}

int curate_esbmc(const fs::path& fetched, const fs::path& dest, int per_verdict, int per_category, const fs::path& work) {
    struct Cand {
        std::string key;
        Task t;
    };
    std::vector<Cand> cands;
    const Regex foreign(ESBMC_FOREIGN_TEXT);
    for (const auto& yml : rglob_yml(fetched)) {
        auto t = load_task(yml, fetched.parent_path());
        if (!t || !t->deterministic || ESBMC_CURATE_SKIP_CATEGORIES.count(t->category)) continue;
        if (foreign.search(utf8_clean(read_text(t->source)))) continue;
        std::string up;
        for (const auto& ln : split_lines(read_text(yml)))
            if (ln.rfind("upstream:", 0) == 0) {
                // ln.split(" ", 2)[-1]
                std::size_t a = ln.find(' ');
                std::size_t b = a == std::string::npos ? a : ln.find(' ', a + 1);
                up = b != std::string::npos ? ln.substr(b + 1) : a != std::string::npos ? ln.substr(a + 1) : ln;
                break;
            }
        cands.push_back({prism::ai::sha256_hex(up), std::move(*t)});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.key < b.key; });
    std::map<bool, int> kept = {{true, 0}, {false, 0}};
    std::map<std::pair<std::string, bool>, int> per_cat;
    std::error_code ec;
    if (fs::exists(dest, ec))
        for (const auto& sub : fs::directory_iterator(dest, ec))
            if (sub.is_directory()) fs::remove_all(sub.path(), ec);  // task directories; LICENSE/NOTICE files stay
    for (const auto& c : cands) {
        const Task& t = c.t;
        bool exp = t.expected_of("main");
        if (kept[exp] >= per_verdict || per_cat[{t.category, exp}] >= per_category) continue;
        Exec res = run_program(t, work / "curate" / path_key(t.ident));
        if (res.outcome != (exp ? "clean" : "ub")) continue;
        fs::path out = dest / t.category;
        fs::create_directories(out, ec);
        fs::copy_file(t.source, out / t.source.filename(), fs::copy_options::overwrite_existing, ec);
        fs::copy_file(t.yml, out / t.yml.filename(), fs::copy_options::overwrite_existing, ec);
        kept[exp]++;
        per_cat[{t.category, exp}]++;
        if (kept[true] >= per_verdict && kept[false] >= per_verdict) break;
    }
    std::cerr << "esbmc-cpp subset: " << kept[true] << " true, " << kept[false] << " false under " << dest.string()
              << "\n";
    return kept[true] + kept[false];
}

}  // namespace prism::qa
