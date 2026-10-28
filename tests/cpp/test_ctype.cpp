// C locale ASCII ctype model. Reads the model and the contract harness as
// text. Does not compile them.

#include <doctest/doctest.h>

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
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    std::size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    return s.substr(a);
}

// Statements inside `int name(int c) { ... }`, comments and blank lines dropped.
std::vector<std::string> statements(const std::string& src, const std::string& name) {
    const std::string sig = "int " + name + "(int c) {";
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

std::set<std::string> defined_models(const std::string& src) {
    std::set<std::string> names;
    std::istringstream in(src);
    std::string line;
    const std::string suffix = "(int c) {";
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.rfind("int ", 0) != 0) continue;
        if (line.size() <= 4 + suffix.size()) continue;
        if (line.compare(line.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        names.insert(line.substr(4, line.size() - 4 - suffix.size()));
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

TEST_CASE("ctype model: C locale ASCII ranges, EOF, and no extra class") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "ctype.cpp");
    REQUIRE(!src.empty());
    CHECK(src.find("This is not a proof of a locale other than the C locale, and it is not PROVED and not CLEAN.") !=
          std::string::npos);
    CHECK(count_of(src, "PROVED") == 1);
    CHECK(count_of(src, "CLEAN") == 1);
    CHECK(src.find("#define EOF (-1)") != std::string::npos);

    const std::set<std::string> want = {"isdigit", "isspace", "isalpha", "isalnum",
                                        "islower", "isupper", "tolower", "toupper"};
    CHECK(defined_models(src) == want);
    for (const char* banned : {"ispunct", "isxdigit", "isblank", "iscntrl", "isprint", "isgraph", "isascii", "toascii"})
        CHECK(src.find(banned) == std::string::npos);

    CHECK(statements(src, "isdigit") == std::vector<std::string>{
                                            "if (c == EOF) return 0;",
                                            "if (c >= '0' && c <= '9') return 1;",
                                            "return 0;",
                                        });
    CHECK(statements(src, "isspace") == std::vector<std::string>{
                                            "if (c == EOF) return 0;",
                                            "if (c == ' ' || c == '\\t' || c == '\\n' || c == '\\v' || c == '\\f' || c == '\\r') return 1;",
                                            "return 0;",
                                        });
    CHECK(statements(src, "isalpha") == std::vector<std::string>{
                                            "if (c == EOF) return 0;",
                                            "if (c >= 'A' && c <= 'Z') return 1;",
                                            "if (c >= 'a' && c <= 'z') return 1;",
                                            "return 0;",
                                        });
    CHECK(statements(src, "isalnum") == std::vector<std::string>{
                                            "if (c == EOF) return 0;",
                                            "if (isdigit(c) != 0) return 1;",
                                            "if (isalpha(c) != 0) return 1;",
                                            "return 0;",
                                        });
    CHECK(statements(src, "islower") == std::vector<std::string>{
                                            "if (c == EOF) return 0;",
                                            "if (c >= 'a' && c <= 'z') return 1;",
                                            "return 0;",
                                        });
    CHECK(statements(src, "isupper") == std::vector<std::string>{
                                            "if (c == EOF) return 0;",
                                            "if (c >= 'A' && c <= 'Z') return 1;",
                                            "return 0;",
                                        });
    CHECK(statements(src, "tolower") == std::vector<std::string>{
                                            "if (c == EOF) return EOF;",
                                            "if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';",
                                            "return c;",
                                        });
    CHECK(statements(src, "toupper") == std::vector<std::string>{
                                            "if (c == EOF) return EOF;",
                                            "if (c >= 'a' && c <= 'z') return c - 'a' + 'A';",
                                            "return c;",
                                        });
}

TEST_CASE("ctype contracts: harness includes the model and calls every function") {
    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "ctype_contracts.cpp");
    REQUIRE(!cpp.empty());
    CHECK(cpp.find("#include \"../../../src/prism/pir/models/libc/ctype.cpp\"") != std::string::npos);
    CHECK(cpp.find("isdigit('a') != 0") != std::string::npos);
    CHECK(cpp.find("tolower('A') == 'a'") != std::string::npos);
    CHECK(cpp.find("toupper('a') == 'A'") != std::string::npos);
    CHECK(cpp.find("tolower('0') == '0'") != std::string::npos);
    CHECK(cpp.find("toupper('0') == '0'") != std::string::npos);
    for (const char* name : {"isdigit", "isspace", "isalpha", "isalnum", "islower", "isupper", "tolower", "toupper"}) {
        CAPTURE(name);
        CHECK(cpp.find(std::string(name) + "(") != std::string::npos);
    }
}

TEST_CASE("ctype contracts: yml has a true function, a false function, and expect_class") {
    const auto yml = slurp(repo() / "testdata" / "conformance" / "libc-models" / "ctype_contracts.yml");
    const auto cpp = slurp(repo() / "testdata" / "conformance" / "libc-models" / "ctype_contracts.cpp");
    REQUIRE(!yml.empty());
    REQUIRE(!cpp.empty());
    CHECK(yml.find("format_version: 1") != std::string::npos);
    CHECK(yml.find("input_files: ctype_contracts.cpp") != std::string::npos);
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
