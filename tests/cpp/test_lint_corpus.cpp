// The planted-bug lint corpus: every testdata/ plant with the rule it must
// raise, the functions it must name and the twins that must stay silent.
// The table (lint_corpus_rows.inc) holds one row per (file, rule set)
// assertion; a row whose file holds '*' is a glob over testdata/ whose
// matches must all stay silent, minus names containing a `skip` substring.

#include <doctest/doctest.h>

#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/regex.hpp"
#include "prism/stages.hpp"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;

fs::path corpus_root() {
    return fs::path(__FILE__).parent_path().parent_path().parent_path() / "testdata";
}

enum class Expect { Hits, None, Any };

struct LintRow {
    const char* test;
    const char* file;
    std::vector<std::string_view> classes;
    const char* function;  // nullptr: any function
    Expect expect;
    std::vector<std::string_view> named;      // must be among the hit functions
    std::vector<std::string_view> not_named;  // must not be among them
    std::vector<std::string_view> skip;       // glob rows: skip names containing these
};

const std::vector<LintRow>& rows() {
    static const std::vector<LintRow> r = {
#include "lint_corpus_rows.inc"
    };
    return r;
}

// run_lints once per file: the table asks about ~700 files many times.
const std::vector<prism::Finding>& lint(const fs::path& p) {
    static std::map<fs::path, std::vector<prism::Finding>> cache;
    auto it = cache.find(p);
    if (it == cache.end()) it = cache.emplace(p, prism::run_lints({p}, corpus_root())).first;
    return it->second;
}

std::vector<prism::Finding> select(const fs::path& p, const std::vector<std::string_view>& cls,
                                   const char* function) {
    std::vector<prism::Finding> out;
    for (const auto& f : lint(p)) {
        if (std::find(cls.begin(), cls.end(), f.cls) == cls.end()) continue;
        if (function && f.function.value_or("") != function) continue;
        out.push_back(f);
    }
    return out;
}

bool wild_match(std::string_view pat, std::string_view s) {
    if (pat.empty()) return s.empty();
    if (pat.front() == '*') {
        for (std::size_t i = 0; i <= s.size(); ++i)
            if (wild_match(pat.substr(1), s.substr(i))) return true;
        return false;
    }
    return !s.empty() && pat.front() == s.front() && wild_match(pat.substr(1), s.substr(1));
}

std::set<std::string> fn_names(const std::vector<prism::Finding>& hits) {
    std::set<std::string> n;
    for (const auto& f : hits) n.insert(f.function.value_or(""));
    return n;
}

std::string join(const std::vector<std::string_view>& v) {
    std::string s;
    for (auto x : v) s += std::string(x) + " ";
    return s;
}

}  // namespace

TEST_CASE("lint corpus: every plant fires, every twin is silent") {
    const auto root = corpus_root();
    REQUIRE(fs::is_directory(root));
    CHECK(rows().size() > 2000);
    for (const auto& r : rows()) {
        INFO("test_" << r.test << " file " << r.file << " classes " << join(r.classes));
        std::string_view file = r.file;
        if (file.find('*') != std::string_view::npos) {
            REQUIRE(r.expect == Expect::None);
            for (const auto& e : fs::directory_iterator(root)) {
                const std::string name = e.path().filename().string();
                if (!wild_match(file, name)) continue;
                if (std::any_of(r.skip.begin(), r.skip.end(),
                                [&](auto s) { return name.find(s) != std::string::npos; }))
                    continue;
                INFO("glob match " << name);
                CHECK(select(e.path(), r.classes, r.function).empty());
            }
            continue;
        }
        const fs::path p = root / r.file;
        REQUIRE(fs::exists(p));
        const auto hits = select(p, r.classes, r.function);
        if (r.expect == Expect::Hits) CHECK_FALSE(hits.empty());
        if (r.expect == Expect::None) CHECK(hits.empty());
        const auto names = fn_names(hits);
        for (auto n : r.named) {
            INFO("expected function " << n);
            CHECK(names.count(std::string(n)) == 1);
        }
        for (auto n : r.not_named) {
            INFO("forbidden function " << n);
            CHECK(names.count(std::string(n)) == 0);
        }
    }
}

TEST_CASE("lint corpus: MEM-UAF is FAILED, never a proof") {
    const auto root = corpus_root();
    const auto hits = select(root / "uaf.c", {"MEM-UAF"}, nullptr);
    REQUIRE_FALSE(hits.empty());
    const auto names = fn_names(hits);
    CHECK(names.count("uaf_bad") == 1);
    CHECK(names.count("checked_use") == 0);
    for (const auto& f : hits) {
        CHECK(f.status == prism::laws::FAILED);
        CHECK(f.status != prism::laws::PROVED);
    }
}

TEST_CASE("lint corpus: API-THRD-JOIN strength, status and wording") {
    const auto root = corpus_root();
    const auto hits = select(root / "thrd_join_api.c", {"API-THRD-JOIN"}, nullptr);
    REQUIRE_FALSE(hits.empty());
    const auto names = fn_names(hits);
    CHECK(names.count("thrd_join_bad") == 1);
    CHECK(names.count("thrd_detach_bad") == 1);
    CHECK(names.count("thrd_join_ok") == 0);
    CHECK(hits[0].strength == prism::laws::STRENGTH_FINDS);
    CHECK_FALSE(prism::laws::is_proof(hits[0].status));
    for (const auto& h : hits) {
        std::string m = h.message;
        std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return std::tolower(c); });
        CHECK(m.find("thrd") != std::string::npos);
        CHECK(m.find("pthread") == std::string::npos);
    }
    const auto unenc = select(root / "thrd_unenc.c", {"API-THRD-JOIN"}, nullptr);
    REQUIRE_FALSE(unenc.empty());
    for (const auto& f : unenc) {
        CHECK(f.status == prism::laws::FAILED);
        CHECK_FALSE(prism::laws::is_proof(f.status));
    }
    const auto un = fn_names(unenc);
    CHECK(un.count("thrd_join_unenc_bad") == 1);
    CHECK(un.count("thrd_detach_unenc_bad") == 1);
    for (const char* stem : {"abs_ok", "pthread_join_api", "thr_api", "iso_thread_race"}) {
        INFO(stem);
        CHECK(select(root / (std::string(stem) + ".c"), {"API-THRD-JOIN"}, nullptr).empty());
    }
}

TEST_CASE("lint corpus: CXX-COPY-ASSIGN-PTR spares a self-assign guard") {
    const auto root = corpus_root();
    const auto hits = select(root / "copy_assign.cpp", {"CXX-COPY-ASSIGN-PTR"}, nullptr);
    REQUIRE_FALSE(hits.empty());
    CHECK(fn_names(hits).count("copy_assign_bad") == 1);
    CHECK(fn_names(hits).count("copy_assign_ok") == 0);
    CHECK(fn_names(select(root / "self_assign.cpp", {"CXX-COPY-ASSIGN-PTR"}, nullptr)).count("assign_ok") == 0);
    CHECK(select(root / "abs_ok.c", {"CXX-COPY-ASSIGN-PTR"}, nullptr).empty());
}

TEST_CASE("lint corpus: CXX-THROW-SPEC and the parse of throw_spec.cpp") {
    const auto root = corpus_root();
    const auto hits = select(root / "throw_spec.cpp", {"CXX-THROW-SPEC"}, nullptr);
    REQUIRE_FALSE(hits.empty());
    const auto names = fn_names(hits);
    CHECK(names.count("throw_spec_bad") == 1);
    CHECK(names.count("throw_spec_dynamic_bad") == 1);
    CHECK(names.count("throw_spec_ok") == 0);
    CHECK(names.count("throw_stmt_ok") == 0);
    std::map<std::string, prism::FunctionInfo> by;
    for (auto& f : prism::extract_functions(root / "throw_spec.cpp", "throw_spec.cpp")) by.emplace(f.name, f);
    REQUIRE(by.count("throw_spec_ok") == 1);
    REQUIRE(by.count("throw_spec_bad") == 1);
    CHECK(by.at("throw_spec_bad").kind == "VOID");
    CHECK(by.at("throw_spec_ok").kind == "VOID");
    CHECK(select(root / "throw_spec.cpp", {"CXX-THROW-DESTRUCTOR"}, nullptr).empty());
    for (const char* f : {"abs_ok.c", "throw_dtor.cpp", "exception_leak.cpp"}) {
        INFO(f);
        CHECK(select(root / f, {"CXX-THROW-SPEC"}, nullptr).empty());
    }
}

namespace {
prism::FunctionInfo corpus_fn(const char* name) {
    for (const auto& e : fs::directory_iterator(corpus_root())) {
        if (e.path().extension() != ".c") continue;
        for (auto& f : prism::extract_functions(e.path(), e.path().filename().string()))
            if (f.name == name) return f;
    }
    FAIL("no function " << name << " in testdata/*.c");
    return {};
}
}  // namespace

TEST_CASE("lint corpus: array decay and alloca need a pointer harness") {
    for (const char* n : {"arr_esc_bad", "esc_bad", "arr_esc_plus0", "arr_esc_plusi", "arr_esc_plus_rhs",
                          "arr_esc_paren", "esc_paren_addr", "alloca_bad", "alloca_ok"}) {
        INFO(n);
        CHECK(prism::body_needs_pointer_harness(corpus_fn(n).body));
    }
    // `return buf[0]` is a scalar element, not array decay.
    for (const char* n : {"write_slot", "caller"}) {
        INFO(n);
        CHECK_FALSE(prism::body_needs_pointer_harness(corpus_fn(n).body));
    }
    // alloca itself is the missing stack-frame model, even without char *.
    CHECK(prism::body_needs_pointer_harness("alloca(n);"));
    CHECK(prism::body_needs_pointer_harness("__builtin_alloca(16);"));
    CHECK_FALSE(prism::body_needs_pointer_harness("return n + 1;"));
}

// ---------------------------------------------------------------- fast paths
// strip_comments_keep_lines and match_brace must equal the plain character
// loops they replaced; findings must not depend on container order; interval
// must stay silent on syntax it does not encode.
namespace {
std::string ref_strip(std::string_view text, bool blank_strings) {
    std::string out;
    std::size_t i = 0;
    const std::size_t n = text.size();
    bool in_block = false;
    auto starts = [&](std::size_t at, std::string_view s) { return text.substr(at, s.size()) == s; };
    while (i < n) {
        if (in_block) {
            if (starts(i, "*/")) {
                in_block = false;
                i += 2;
            } else {
                out += text[i] == '\n' ? '\n' : ' ';
                ++i;
            }
            continue;
        }
        if (starts(i, "/*")) {
            in_block = true;
            i += 2;
            continue;
        }
        if (starts(i, "//")) {
            while (i < n && text[i] != '\n') {
                out += ' ';
                ++i;
            }
            continue;
        }
        const char c = text[i];
        if (c == '\'') {
            out += c;
            ++i;
            while (i < n && text[i] != '\'') {
                if (text[i] == '\\') {
                    out += text[i++];
                    if (i < n) out += text[i++];
                    continue;
                }
                out += text[i++];
            }
            if (i < n) out += text[i++];
            continue;
        }
        if (c == '"') {
            out += c;
            ++i;
            while (i < n && text[i] != '"') {
                if (text[i] == '\\') {
                    if (blank_strings) {
                        out += ' ';
                        ++i;
                        if (i < n) {
                            out += ' ';
                            ++i;
                        }
                    } else {
                        out += text[i++];
                        if (i < n) out += text[i++];
                    }
                    continue;
                }
                if (blank_strings) out += text[i] == '\n' ? '\n' : ' ';
                else out += text[i];
                ++i;
            }
            if (i < n) out += text[i++];
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// A negative start is not an index into the text: no match.
int ref_match_brace(std::string_view text, int open_idx) {
    if (open_idx < 0) return -1;
    int depth = 0;
    for (int i = open_idx; i < static_cast<int>(text.size()); ++i) {
        if (text[static_cast<std::size_t>(i)] == '{') ++depth;
        else if (text[static_cast<std::size_t>(i)] == '}' && --depth == 0) return i;
    }
    return -1;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}  // namespace

TEST_CASE("lint fastpath: strip_comments_keep_lines equals the reference loop on fuzz") {
    std::mt19937 rng(7);
    const std::string_view alphabet = "/*'\"\\\n{}ab ;";
    int bad = 0;
    for (int k = 0; k < 20000; ++k) {
        std::string s(static_cast<std::size_t>(rng() % 25), ' ');
        for (auto& c : s) c = alphabet[rng() % alphabet.size()];
        for (bool blank : {true, false}) {
            const auto got = prism::strip_comments_keep_lines(s, blank);
            const auto want = ref_strip(s, blank);
            if (got != want && bad++ < 5) {
                INFO("input [" << s << "] blank " << blank);
                CHECK(got == want);
            }
        }
    }
    CHECK(bad == 0);
}

TEST_CASE("lint fastpath: strip_comments_keep_lines equals the reference loop on testdata") {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(corpus_root()))
        if (wild_match("*.c*", e.path().filename().string())) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.size() > 400) files.resize(400);
    REQUIRE(files.size() > 100);
    for (const auto& p : files) {
        const std::string text = read_file(p);
        for (bool blank : {true, false}) {
            INFO(p.filename().string() << " blank " << blank);
            CHECK(prism::strip_comments_keep_lines(text, blank) == ref_strip(text, blank));
        }
    }
}

TEST_CASE("lint fastpath: strip_comments_keep_lines edge cases") {
    // Delimiters vanish, the interior keeps its newlines.
    CHECK(prism::strip_comments_keep_lines("a/*x\ny*/b") == "a \n b");
    CHECK(prism::strip_comments_keep_lines("s = \"a\\\"b\";") == "s = \"    \";");
    // Escaped newline in a string is two spaces.
    CHECK(prism::strip_comments_keep_lines("\"\\\nx\"") == "\"   \"");
    CHECK(prism::strip_comments_keep_lines("c = '\"'; // q") == "c = '\"';     ");
    CHECK(prism::strip_comments_keep_lines("x /* open") == "x      ");
}

TEST_CASE("lint fastpath: match_brace equals the reference loop") {
    std::mt19937 rng(11);
    const std::string_view alphabet = "{}a";
    int bad = 0;
    for (int k = 0; k < 5000; ++k) {
        std::string s(static_cast<std::size_t>(rng() % 17), ' ');
        for (auto& c : s) c = alphabet[rng() % alphabet.size()];
        const int n = static_cast<int>(s.size());
        for (int i = -n - 1; i < n + 2; ++i) {
            if (prism::match_brace(s, i) != ref_match_brace(s, i) && bad++ < 5) {
                INFO("input [" << s << "] at " << i);
                CHECK(prism::match_brace(s, i) == ref_match_brace(s, i));
            }
        }
    }
    CHECK(bad == 0);
}

TEST_CASE("lint fastpath: findings name globals and optionals in sorted order") {
    const fs::path tmp = fs::temp_directory_path() / ("prism_lint_order_" + std::to_string(::getpid()));
    fs::create_directories(tmp);
    {
        std::ofstream(tmp / "multi.cpp") << "#include <optional>\n"
                                            "int both(void) {\n"
                                            "    std::optional<int> zeta;\n"
                                            "    std::optional<int> alpha;\n"
                                            "    return *zeta + *alpha;\n"
                                            "}\n";
        std::ofstream(tmp / "globals.c")
            << "#include <pthread.h>\n"
               "int alpha;\nint beta;\nint gamma_;\nint delta;\n"
               "void *t1(void *a) { alpha += 1; beta += 1; gamma_ += 1; delta += 1; return a; }\n"
               "void *t2(void *a) { alpha += 1; beta += 1; gamma_ += 1; delta += 1; return a; }\n"
               "int start(void) { pthread_t t; return pthread_create(&t, 0, t1, 0); }\n";
    }
    std::vector<fs::path> ps;
    for (const auto& e : fs::directory_iterator(tmp)) ps.push_back(e.path());
    std::sort(ps.begin(), ps.end());
    auto run_once = [&] {
        auto found = prism::run_lints(ps, tmp, 1);
        std::vector<prism::FunctionInfo> fns;
        for (const auto& p : ps)
            for (auto& f : prism::extract_functions(p, p.string())) fns.push_back(f);
        for (auto& f : prism::run_thread(fns)) found.push_back(f);
        std::string out;
        for (const auto& f : found)
            out += f.cls + "|" + std::to_string(f.line.value_or(0)) + "|" + f.message + "\n";
        return out;
    };
    const std::string out = run_once();
    std::error_code ec;
    CHECK(run_once() == out);
    fs::remove_all(tmp, ec);
    INFO(out);
    CHECK(out.find("alpha dereferenced without has_value()") != std::string::npos);
    auto names_after = [&](const char* tail) {
        std::vector<std::string> v;
        prism::Regex re(std::string("global '(\\w+)' ") + tail);
        for (const auto& m : re.finditer(out)) v.push_back(m.group(1));
        return v;
    };
    // One finding per (writer, global): t1's four, then t2's four.
    const std::vector<std::string> want = {"alpha", "beta", "delta", "gamma_",
                                           "alpha", "beta", "delta", "gamma_"};
    CHECK(names_after("incremented") == want);
    const auto race = names_after("written from");
    CHECK(std::is_sorted(race.begin(), race.end()));
    CHECK(race.size() == 4);
}

TEST_CASE("lint fastpath: interval stays silent on va_arg it does not encode") {
    // The engine alone alarms on `x + n`; va_arg is unencoded, so no finding.
    for (const char* name : {"vaarg_unenc_bad", "vaarg_unenc_ok"}) {
        INFO(name);
        std::vector<prism::FunctionInfo> fns;
        for (auto& f : prism::extract_functions(corpus_root() / "vaarg_unenc.c", "vaarg_unenc.c"))
            if (f.name == name) fns.push_back(f);
        REQUIRE(fns.size() == 1);
        CHECK(prism::run_interval(fns).empty());
    }
}
