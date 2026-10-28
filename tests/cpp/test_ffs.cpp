// ffs, ffsl, and ffsll models. Reads the model and the contract harness as text.
// Does not compile them. docs/finish-notes/ffs.md docs/finish-notes/ffsll.md

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string s{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    std::size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    return s.substr(a);
}

// Statements inside `sig`, comments and blank lines dropped.
std::vector<std::string> statements(const std::string& src, const std::string& sig) {
    const auto at = src.find(sig);
    if (at == std::string::npos) return {};
    const auto open = at + sig.size() - 1;
    if (open >= src.size() || src[open] != '{') return {};
    int depth = 0;
    std::string interior;
    for (std::size_t i = open; i < src.size(); ++i) {
        if (src[i] == '{') {
            ++depth;
            if (depth == 1) continue;
        } else if (src[i] == '}') {
            --depth;
            if (depth == 0) break;
        }
        if (depth >= 1) interior.push_back(src[i]);
    }
    std::vector<std::string> lines;
    std::string cur;
    auto flush = [&] {
        auto t = trim(cur);
        cur.clear();
        if (t.empty() || t[0] == '/' || t[0] == '*') return;
        lines.push_back(std::move(t));
    };
    for (char c : interior) {
        if (c == '\n') flush();
        else cur.push_back(c);
    }
    flush();
    return lines;
}

std::string function_body(std::string_view src, std::string_view sig) {
    const auto at = src.find(sig);
    if (at == std::string_view::npos) return {};
    const auto open = src.find('{', at);
    if (open == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = open; i < src.size(); ++i) {
        if (src[i] == '{') {
            ++depth;
        } else if (src[i] == '}') {
            --depth;
            if (depth == 0) return std::string(src.substr(open, i - open + 1));
        }
    }
    return {};
}

std::set<std::string> defined_models(const std::string& src) {
    std::set<std::string> names;
    std::istringstream in(src);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.rfind("int ", 0) != 0) continue;
        const auto paren = line.find('(');
        const auto brace = line.find('{');
        if (paren == std::string::npos || brace == std::string::npos || brace < paren) continue;
        const auto semi = line.find(';');
        if (semi != std::string::npos && semi < brace) continue;
        names.insert(trim(line.substr(4, paren - 4)));
    }
    return names;
}

std::size_t count_of(std::string_view s, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = 0; (at = s.find(needle, at)) != std::string_view::npos; at += needle.size()) ++n;
    return n;
}

std::string strip_hash_comment(std::string s) {
    bool quote = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') quote = !quote;
        if (!quote && s[i] == '#' && (i == 0 || s[i - 1] == ' ')) return trim(s.substr(0, i));
    }
    return trim(s);
}

}  // namespace

TEST_CASE("ffs model: zero is zero and the least set bit is one plus its index") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "strings_compat.cpp");
    REQUIRE(!src.empty());
    CHECK(src.find("The result is one plus the index of the least set bit, and it is never a wrapped negative.") !=
          std::string::npos);
    CHECK(src.find("ffsl follows the same rule on long.") != std::string::npos);
    CHECK(src.find("ffsll(0) is 0. ffsll(1) is 1. ffsll(2) is 2. ffsll(8) is 4.") != std::string::npos);
    CHECK(src.find("The argument is converted to unsigned long long before the scan. Do not negate it.") !=
          std::string::npos);
    CHECK(src.find("A value with only the high bit set returns 64.") != std::string::npos);
    CHECK(src.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(count_of(src, "PROVED") == 1);
    CHECK(count_of(src, "CLEAN") == 1);
    CHECK(defined_models(src) == std::set<std::string>{"ffs", "ffsl", "ffsll"});
    for (const char* banned : {"strcasecmp", "locale"}) CHECK(src.find(banned) == std::string::npos);

    const auto ffs_body = function_body(src, "int ffs(int i) {");
    const auto ffsl_body = function_body(src, "int ffsl(long i) {");
    const auto ffsll_body = function_body(src, "int ffsll(long long i) {");
    REQUIRE(!ffs_body.empty());
    REQUIRE(!ffsl_body.empty());
    REQUIRE(!ffsll_body.empty());
    CHECK(ffs_body.find('-') == std::string::npos);
    CHECK(ffsl_body.find('-') == std::string::npos);
    CHECK(ffsll_body.find('-') == std::string::npos);
    CHECK(ffs_body.find("never a wrapped negative") != std::string::npos);
    CHECK(ffsl_body.find("never a wrapped negative") != std::string::npos);
    CHECK(ffsll_body.find("never a wrapped negative") != std::string::npos);
    CHECK(ffsll_body.find("unsigned long long") != std::string::npos);

    CHECK(statements(src, "int ffs(int i) {") == std::vector<std::string>{
                                                     "unsigned int x = (unsigned int)i;",
                                                     "if (x == 0u) return 0;",
                                                     "int bit = 0;",
                                                     "while (bit < 63 && (x & 1u) == 0u) {",
                                                     "x >>= 1;",
                                                     "bit += 1;",
                                                     "}",
                                                     "return bit + 1;",
                                                 });
    CHECK(statements(src, "int ffsl(long i) {") == std::vector<std::string>{
                                                       "unsigned long x = (unsigned long)i;",
                                                       "if (x == 0ul) return 0;",
                                                       "int bit = 0;",
                                                       "while (bit < 63 && (x & 1ul) == 0ul) {",
                                                       "x >>= 1;",
                                                       "bit += 1;",
                                                       "}",
                                                       "return bit + 1;",
                                                   });
    CHECK(statements(src, "int ffsll(long long i) {") == std::vector<std::string>{
                                                             "unsigned long long x = (unsigned long long)i;",
                                                             "if (x == 0ull) return 0;",
                                                             "int bit = 0;",
                                                             "while (bit < 63 && (x & 1ull) == 0ull) {",
                                                             "x >>= 1;",
                                                             "bit += 1;",
                                                             "}",
                                                             "return bit + 1;",
                                                         });
}

TEST_CASE("ffs contracts: harness includes the model and calls ffs, ffsl, and ffsll") {
    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "strings_compat_contracts.cpp");
    REQUIRE(!cpp.empty());
    CHECK(cpp.find("#include \"../../../src/prism/pir/models/libc/strings_compat.cpp\"") != std::string::npos);
    CHECK(cpp.find("#include \"harness.h\"") != std::string::npos);
    CHECK(cpp.find("assert(ffs(0) == 0);") != std::string::npos);
    CHECK(cpp.find("assert(ffs(1) == 1);") != std::string::npos);
    CHECK(cpp.find("assert(ffs(2) == 2);") != std::string::npos);
    CHECK(cpp.find("assert(ffs(8) == 4);") != std::string::npos);
    CHECK(cpp.find("assert(ffs(0) == 1);") != std::string::npos);
    CHECK(cpp.find("assert(ffsll(0LL) == 0);") != std::string::npos);
    CHECK(cpp.find("assert(ffsll(1LL) == 1);") != std::string::npos);
    CHECK(cpp.find("assert(ffsll(2LL) == 2);") != std::string::npos);
    CHECK(cpp.find("assert(ffsll(8LL) == 4);") != std::string::npos);
    CHECK(cpp.find("assert(ffsll(0LL) == 1);") != std::string::npos);
    CHECK(cpp.find("ffsl(") != std::string::npos);
    CHECK(cpp.find("ffsll(") != std::string::npos);
    CHECK(cpp.find("ffs(") != std::string::npos);
    CHECK(cpp.find("__assert_fail") != std::string::npos);
    for (const char* banned : {"strcasecmp", "locale"}) CHECK(cpp.find(banned) == std::string::npos);
}

TEST_CASE("ffs contracts: yml has a true function, a false function, and expect_class") {
    const auto yml = slurp(repo() / "testdata" / "conformance" / "libc-models" / "strings_compat_contracts.yml");
    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "strings_compat_contracts.cpp");
    REQUIRE(!yml.empty());
    REQUIRE(!cpp.empty());
    CHECK(yml.find("format_version: 1") != std::string::npos);
    CHECK(yml.find("input_files: strings_compat_contracts.cpp") != std::string::npos);
    CHECK(yml.find("language: C") != std::string::npos);
    CHECK(yml.find("property: libc-contract") != std::string::npos);

    enum class Sec { None, Expected, Class };
    Sec sec = Sec::None;
    std::map<std::string, bool> expected;
    std::map<std::string, std::string> classes;
    std::istringstream in(yml);
    std::string line;
    while (std::getline(in, line)) {
        line = strip_hash_comment(line);
        if (line.empty()) continue;
        if (line == "expected:") {
            sec = Sec::Expected;
            continue;
        }
        if (line.rfind("expect_class:", 0) == 0) {
            sec = Sec::Class;
            continue;
        }
        if (sec == Sec::None) continue;
        const auto colon = line.find(':');
        REQUIRE(colon != std::string::npos);
        const auto key = trim(line.substr(0, colon));
        auto val = trim(line.substr(colon + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
        if (sec == Sec::Expected) {
            REQUIRE((val == "true" || val == "false"));
            expected[key] = val == "true";
        } else {
            classes[key] = val;
        }
    }

    bool any_true = false;
    std::set<std::string> false_fns;
    for (const auto& [name, exp] : expected) {
        CAPTURE(name);
        CHECK(cpp.find("int " + name + "(") != std::string::npos);
        const bool suffix = name.size() >= 5 && name.compare(name.size() - 5, 5, "_true") == 0;
        CHECK(suffix == exp);
        if (exp) any_true = true;
        else false_fns.insert(name);
    }
    CHECK(any_true);
    CHECK(!false_fns.empty());

    std::set<std::string> classed;
    for (const auto& [name, cls] : classes) {
        CAPTURE(name);
        classed.insert(name);
        CHECK(cls == "FUNC-CONTRACT");
    }
    CHECK(classed == false_fns);
}
