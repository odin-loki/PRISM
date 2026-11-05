// Differential test of the std::variant model header
// (src/prism/pir/models/cxx/variant). Same shape as tests/cpp/test_cxx_list.cpp.
// CMake's prism_tests source list is explicit (not a glob):
// docs/finish-notes/variant.md has the line that links this file.
//
// The stored alternative is the one that was emplaced. std::get_if on an
// inactive alternative is nullptr. std::get on an inactive alternative is a
// __prism_check, and the model does not print a fake T.
//
// std::monostate is empty. Two monostate objects compare equal: == is true,
// != is false, < and > are false, and <= and >= are true. Two variant objects
// of the same type compare with ==, !=, <, >, <=, and >=. operator> is
// operator< with the arguments swapped. operator<= is !(b < a), the negation
// of that operator>. operator>= is !(a < b), using the existing operator<.
// visit of lvalue variants is modelled. swap and operator<=> stay undeclared.
// This is not PROVED and not CLEAN.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

namespace cxxh = prism_test::cxx;
using cxxh::fwd;
using cxxh::repo;
using cxxh::run_cmd;
using cxxh::run_lines;

struct VarCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

VarCtx& ctx() {
    static VarCtx c;
    return c;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <variant>\n"
                                  "int main() { std::variant<int, char> v; v.emplace<int>(1); return std::get<int>(v) - 1; }\n",
                                  cxxh::asan_flags());
}

fs::path build_text(const std::string& cxx, const fs::path& tmp, const std::string& name, const std::string& src,
                    bool with_models) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const fs::path in = tmp / (name + ".cpp");
    std::ofstream(in) << src;
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    fs::path out = tmp / (name + (with_models ? ".model" : ".lib"));
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back(in.string());
    argv.push_back("-o");
    argv.push_back(out.string());
    REQUIRE(run_cmd(argv));
    return out;
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("variant_trace.cpp"), "variant_trace", cxxh::asan_flags(), with_models);
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Comments name the omitted APIs. A declaration is what remains.
std::string strip_comments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            i += 2;
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            if (i + 1 < s.size()) i += 2;
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

bool has_ident(const std::string& s, const char* id) {
    const std::string name(id);
    for (std::size_t i = 0; (i = s.find(name, i)) != std::string::npos; i += name.size()) {
        const bool left = i == 0 || !(std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_');
        const std::size_t j = i + name.size();
        const bool right = j >= s.size() || !(std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_');
        if (left && right) return true;
    }
    return false;
}

bool syntax_only(const std::string& cxx, const fs::path& src, bool with_models, std::string* err) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    std::vector<std::string> argv{cxx, "-std=c++23", "-fsyntax-only", "-w"};
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back(src.string());
    return run_cmd(argv, nullptr, err);
}

bool inactive_get_is_check(const fs::path& exe, const char* mode) {
    std::string out, err;
    if (run_cmd({exe.string(), mode}, &out, &err)) return false;
    const std::string both = out + err;
    if (both.find("CHECK CXX-VARIANT-GET:") == std::string::npos) return false;
    if (both.find("inactive variant alternative") == std::string::npos) return false;
    if (out.find("fake=") != std::string::npos) return false;
    return true;
}

}  // namespace

TEST_CASE("cxx variant model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxvariant-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx variant model: stored value is the emplaced alternative and inactive get is a check") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags.begin(), flags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "variant_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd((models / "variant").string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a.size() > 5);
    CHECK(a == b);
    bool saw_stored = false;
    bool saw_null = false;
    bool saw_ptr = false;
    bool saw_mono = false;
    bool saw_mono_var = false;
    for (const auto& line : b) {
        if (line.find("stored=42") != std::string::npos) saw_stored = true;
        if (line.find("inactive_null=1") != std::string::npos) saw_null = true;
        if (line.find("active_ptr=42") != std::string::npos) saw_ptr = true;
        if (line == "mono eq=1 ne=0 lt=0 gt=0 le=1 ge=1") saw_mono = true;
        if (line == "mono_var index=0 holds=1 geteq=1 getne=0 valueless=0") saw_mono_var = true;
        CHECK(line.find("stored=0") == std::string::npos);
        CHECK(line != "mono eq=0 ne=1 lt=0 gt=0 le=1 ge=1");
        CHECK(line != "mono eq=0 ne=0 lt=0 gt=0 le=1 ge=1");
        CHECK(line != "mono eq=1 ne=1 lt=0 gt=0 le=1 ge=1");
        CHECK(line != "mono eq=1 ne=0 lt=1 gt=1 le=0 ge=0");
        CHECK(line.find("geteq=0") == std::string::npos);
        CHECK(line.find("getne=1") == std::string::npos);
        CHECK(line.find("valueless=1") == std::string::npos);
    }
    CHECK(saw_stored);
    CHECK(saw_null);
    CHECK(saw_ptr);
    CHECK(saw_mono);
    CHECK(saw_mono_var);

    CHECK(inactive_get_is_check(mod, "bad"));
    CHECK(inactive_get_is_check(mod, "badi"));
}

TEST_CASE("cxx variant model: monostate is empty and equal, omitted names are comments") {
    const fs::path header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "variant";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());

    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <compare>") == std::string::npos);
    CHECK(src.find("struct monostate {};") != std::string::npos);
    CHECK(src.find("constexpr bool operator==(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(src.find("constexpr bool operator!=(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(src.find("constexpr bool operator<(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(src.find("constexpr bool operator>(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(src.find("constexpr bool operator<=(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(src.find("constexpr bool operator>=(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(src.find("constexpr bool valueless_by_exception() const noexcept { return false; }") != std::string::npos);
    CHECK(src.find("\"CXX-VARIANT-GET\"") != std::string::npos);
    CHECK(src.find("std::get on an inactive variant alternative") != std::string::npos);
    CHECK(src.find("return __v.template _M_get<_I>();") != std::string::npos);
    CHECK(src.find("return T{}") == std::string::npos);
    CHECK(src.find("return _T{}") == std::string::npos);
    CHECK(src.find("::operator new") == std::string::npos);
    CHECK(src.find("malloc") == std::string::npos);

    // The comment still names the omitted APIs. A declaration would survive stripping.
    CHECK(src.find("comparison of variant") != std::string::npos);
    CHECK(src.find("This is not PROVED and not CLEAN.") != std::string::npos);
    const std::string code = strip_comments(src);
    CHECK(has_ident(code, "monostate"));
    CHECK(has_ident(code, "visit"));  // modelled (one or more lvalue variants)
    for (const char* name : {"swap", "bad_variant_access", "hash", "in_place"}) {
        CAPTURE(name);
        CHECK_FALSE(has_ident(code, name));
    }
    CHECK(code.find("operator<=>") == std::string::npos);
    CHECK(code.find("constexpr bool operator==(const variant<_Types...>& __a, const variant<_Types...>& __b)") !=
          std::string::npos);
    CHECK(code.find("constexpr bool operator!=(const variant<_Types...>& __a, const variant<_Types...>& __b)") !=
          std::string::npos);
    CHECK(code.find("operator==(variant") == std::string::npos);
    CHECK(code.find("operator!=(variant") == std::string::npos);
    CHECK(code.find("constexpr bool operator<(const variant<_Types...>& __a, const variant<_Types...>& __b)") !=
          std::string::npos);
    int variant_lt = 0;
    for (std::size_t i = 0; (i = code.find("operator<(const variant", i)) != std::string::npos; i += 1)
        ++variant_lt;
    CHECK(variant_lt == 1);
    int variant_gt = 0;
    for (std::size_t i = 0; (i = code.find("operator>(const variant", i)) != std::string::npos; i += 1)
        ++variant_gt;
    CHECK(variant_gt == 1);
    CHECK(code.find("constexpr bool operator>(const variant<_Types...>& __a, const variant<_Types...>& __b)") !=
          std::string::npos);
    int variant_le = 0;
    for (std::size_t i = 0; (i = code.find("operator<=(const variant", i)) != std::string::npos; i += 1)
        ++variant_le;
    CHECK(variant_le == 1);
    CHECK(code.find("constexpr bool operator<=(const variant<_Types...>& __a, const variant<_Types...>& __b)") !=
          std::string::npos);
    CHECK(code.find("return !(__b < __a);") != std::string::npos);
    int variant_ge = 0;
    for (std::size_t i = 0; (i = code.find("operator>=(const variant", i)) != std::string::npos; i += 1)
        ++variant_ge;
    CHECK(variant_ge == 1);
    CHECK(code.find("constexpr bool operator>=(const variant<_Types...>& __a, const variant<_Types...>& __b)") !=
          std::string::npos);
    CHECK(code.find("return !(__a < __b);") != std::string::npos);
    CHECK(code.find("operator>=(variant") == std::string::npos);
    CHECK(code.find("constexpr bool operator<(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator>(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator<=(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator>=(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(code.find("#include <memory>") == std::string::npos);
    CHECK(code.find("#include <compare>") == std::string::npos);
}

TEST_CASE("cxx variant model: swap and operator<=> fail against the model") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    struct Snip {
        const char* name;
        const char* src;
    };
    const Snip held[] = {
        {"lt",
         "#include <variant>\n"
         "int use(){ std::variant<int, char> a(1), b(2); return a < b ? 1 : 0; }\n"},
        {"gt",
         "#include <variant>\n"
         "int use(){ std::variant<int, char> a(1), b(2); return a > b ? 1 : 0; }\n"},
        {"le",
         "#include <variant>\n"
         "int use(){ std::variant<int, char> a(1), b(2); return a <= b ? 1 : 0; }\n"},
        {"ge",
         "#include <variant>\n"
         "int use(){ std::variant<int, char> a(1), b(2); return a >= b ? 1 : 0; }\n"},
    };
    for (const auto& snip : held) {
        CAPTURE(snip.name);
        const fs::path src = c.tmp / (std::string("hold_") + snip.name + ".cpp");
        std::ofstream(src) << snip.src;
        std::string err;
        CHECK(syntax_only(c.cxx, src, false, &err));
        CHECK(syntax_only(c.cxx, src, true, &err));
    }
    const Snip snips[] = {
        {"cmp",
         "#include <variant>\n"
         "int use(){ std::variant<int, char> a(1), b(1); return (a <=> b) == 0 ? 1 : 0; }\n"},
        {"swap",
         "#include <variant>\n"
         "int use(){ std::variant<int, char> a(1), b(2); a.swap(b); return std::get<int>(a); }\n"},
        {"bad",
         "#include <variant>\n"
         "int use(){ std::bad_variant_access ex; (void)ex; return 0; }\n"},
        {"hash",
         "#include <variant>\n"
         "int use(){ return static_cast<int>(std::hash<std::monostate>{}(std::monostate{})); }\n"},
        {"mono_cmp",
         "#include <variant>\n"
         "int use(){ std::monostate a, b; return (a <=> b) == 0 ? 1 : 0; }\n"},
    };
    for (const auto& snip : snips) {
        CAPTURE(snip.name);
        const fs::path src = c.tmp / (std::string("omit_") + snip.name + ".cpp");
        std::ofstream(src) << snip.src;
        std::string err;
        CHECK(syntax_only(c.cxx, src, false, &err));
        CHECK_FALSE(syntax_only(c.cxx, src, true, &err));
    }
}

TEST_CASE("cxx variant model: equality reads only the active alternative") {
    const fs::path header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "variant";
    const std::string src = slurp(header);
    const std::string code = strip_comments(src);
    const auto at = code.find("constexpr bool _Prism_var_eq_at(");
    const auto eq = code.find("constexpr bool operator==(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto ne = code.find("constexpr bool operator!=(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    REQUIRE(at != std::string::npos);
    REQUIRE(eq != std::string::npos);
    REQUIRE(ne != std::string::npos);
    REQUIRE(at < eq);
    REQUIRE(eq < ne);

    const std::string helper = code.substr(at, eq - at);
    CHECK(helper.find("if (__a.index() == _I && __b.index() == _I)") != std::string::npos);
    CHECK(helper.find("return __a.template _M_get<_I>() == __b.template _M_get<_I>();") != std::string::npos);
    CHECK(helper.find("return false;") != std::string::npos);
    CHECK(helper.find("return true") == std::string::npos);
    CHECK(helper.find("{}") == std::string::npos);
    CHECK(prism_test::appears_before(helper, "_M_get", "return false;"));

    const std::string eq_body = code.substr(eq, ne - eq);
    const auto both = eq_body.find("if (__a.valueless_by_exception() && __b.valueless_by_exception())");
    const auto either = eq_body.find("if (__a.valueless_by_exception() || __b.valueless_by_exception())");
    const auto diff = eq_body.find("if (__a.index() != __b.index())");
    const auto yes = eq_body.find("return true;");
    const auto read = eq_body.find("_Prism_var_eq_at<0>(__a, __b)");
    REQUIRE(both != std::string::npos);
    REQUIRE(either != std::string::npos);
    REQUIRE(diff != std::string::npos);
    REQUIRE(yes != std::string::npos);
    REQUIRE(read != std::string::npos);
    CHECK(both < yes);
    CHECK(yes < either);
    CHECK(either < diff);
    CHECK(diff < read);
    CHECK(eq_body.find("_M_get") == std::string::npos);
    CHECK(eq_body.find("_M_ptr") == std::string::npos);
    CHECK(eq_body.find("_M_storage") == std::string::npos);
    CHECK(eq_body.find("_M_first") == std::string::npos);
    CHECK(eq_body.find("T{}") == std::string::npos);
    CHECK(eq_body.find("_T{}") == std::string::npos);
    CHECK(eq_body.find("return true;") != std::string::npos);
    int variant_eq = 0;
    for (std::size_t i = 0; (i = code.find("operator==(const variant", i)) != std::string::npos; i += 1)
        ++variant_eq;
    CHECK(variant_eq == 1);

    const auto lt_at = code.find("constexpr bool _Prism_var_lt_at(");
    REQUIRE(lt_at != std::string::npos);
    REQUIRE(ne < lt_at);
    const std::string ne_body = code.substr(ne, lt_at - ne);
    CHECK(ne_body.find("return !(__a == __b);") != std::string::npos);
    CHECK(ne_body.find("_M_get") == std::string::npos);
    CHECK(ne_body.find("return true") == std::string::npos);
    CHECK(ne_body.find("T{}") == std::string::npos);

    CHECK(code.find("constexpr bool operator==(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator!=(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(src.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(code.find("PROVED") == std::string::npos);
    CHECK(code.find("CLEAN") == std::string::npos);
}

TEST_CASE("cxx variant model: same-type equality matches libstdc++ and different types do not compile") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);

    const char* diff_eq =
        "#include <variant>\n"
        "int use(){ std::variant<int> a(1); std::variant<char> b('a'); return a == b ? 1 : 0; }\n";
    const char* diff_ne =
        "#include <variant>\n"
        "int use(){ std::variant<int> a(1); std::variant<long> b(1L); return a != b ? 1 : 0; }\n";
    for (const char* text : {diff_eq, diff_ne}) {
        const fs::path src = c.tmp / "variant_diff_type.cpp";
        std::ofstream(src) << text;
        std::string err;
        CHECK_FALSE(syntax_only(c.cxx, src, false, &err));
        CHECK_FALSE(syntax_only(c.cxx, src, true, &err));
    }

    const char* same =
        "#include <variant>\n"
        "struct NoDef {\n"
        "  int v;\n"
        "  explicit NoDef(int x) : v(x) {}\n"
        "  bool operator==(const NoDef& o) const { return v == o.v; }\n"
        "};\n"
        "int use(){\n"
        "  std::variant<NoDef, char> a(NoDef(1));\n"
        "  std::variant<NoDef, char> b(NoDef(1));\n"
        "  std::variant<NoDef, char> d('x');\n"
        "  return (a == b && !(a != b) && !(a == d) && (a != d)) ? 1 : 0;\n"
        "}\n";
    const fs::path same_src = c.tmp / "variant_same_eq.cpp";
    std::ofstream(same_src) << same;
    std::string err;
    CHECK(syntax_only(c.cxx, same_src, false, &err));
    CHECK(syntax_only(c.cxx, same_src, true, &err));

    const char* prog =
        "#include <cstdio>\n"
        "#include <variant>\n"
        "struct NoDef {\n"
        "  int v;\n"
        "  explicit NoDef(int x) : v(x) {}\n"
        "  NoDef(const NoDef& o) : v(o.v) {}\n"
        "  bool operator==(const NoDef& o) const {\n"
        "    std::printf(\"call %d %d\\n\", v, o.v);\n"
        "    return v == o.v;\n"
        "  }\n"
        "};\n"
        "int main() {\n"
        "  std::variant<int, char> ia(1), ib(1), ic(2), id('A');\n"
        "  std::printf(\"ieq %d %d %d %d %d %d\\n\",\n"
        "              static_cast<int>(ia == ia), static_cast<int>(ia == ib), static_cast<int>(ia == ic),\n"
        "              static_cast<int>(ia == id), static_cast<int>(ia != ib), static_cast<int>(ia != id));\n"
        "  std::variant<int, int> da, db, dc;\n"
        "  da.emplace<0>(1);\n"
        "  db.emplace<1>(1);\n"
        "  dc.emplace<0>(1);\n"
        "  std::printf(\"dup %d %d %d %d\\n\", static_cast<int>(da == db), static_cast<int>(da == dc),\n"
        "              static_cast<int>(da != db), static_cast<int>(da != dc));\n"
        "  std::variant<std::monostate, int> m1, m2, m3(4);\n"
        "  std::printf(\"meq %d %d %d %d\\n\", static_cast<int>(m1 == m2), static_cast<int>(m1 == m3),\n"
        "              static_cast<int>(m1 != m2), static_cast<int>(m1 != m3));\n"
        "  std::variant<NoDef, char> a(NoDef(3)), b(NoDef(3)), cv(NoDef(4)), d('Z');\n"
        "  const int same = a == b;\n"
        "  const int diffv = a == cv;\n"
        "  const int diffi = a == d;\n"
        "  std::printf(\"nd %d %d %d\\n\", same, diffv, diffi);\n"
        "  return 0;\n"
        "}\n";
    auto lib = build_text(c.cxx, c.tmp, "variant_eq", prog, false);
    auto mod = build_text(c.cxx, c.tmp, "variant_eq", prog, true);
    const std::vector<std::string> expect{"ieq 1 1 0 0 0 1", "dup 0 1 1 0", "meq 1 0 0 1",
                                          "call 3 3",        "call 3 4",    "nd 1 0 0"};
    CHECK(run_lines(lib) == expect);
    CHECK(run_lines(mod) == expect);
}

TEST_CASE("cxx variant model: monostate ordering compiles against the model and libstdc++") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    const fs::path src = c.tmp / "mono_order.cpp";
    std::ofstream(src) << "#include <variant>\n"
                          "int use(){ std::monostate a, b;\n"
                          "  return (a < b || a > b || !(a <= b) || !(a >= b)) ? 1 : 0; }\n";
    std::string err;
    CHECK(syntax_only(c.cxx, src, false, &err));
    CHECK(syntax_only(c.cxx, src, true, &err));
}

TEST_CASE("cxx variant model: operator< returns before it reads storage") {
    const fs::path header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "variant";
    const std::string src = slurp(header);
    const std::string code = strip_comments(src);
    const auto at = code.find("constexpr bool _Prism_var_lt_at(");
    const auto lt = code.find("constexpr bool operator<(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto gt = code.find("constexpr bool operator>(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto ne = code.find("constexpr bool operator!=(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto size = code.find("struct variant_size");
    REQUIRE(at != std::string::npos);
    REQUIRE(lt != std::string::npos);
    REQUIRE(gt != std::string::npos);
    REQUIRE(ne != std::string::npos);
    REQUIRE(size != std::string::npos);
    REQUIRE(ne < at);
    REQUIRE(at < lt);
    REQUIRE(lt < gt);
    REQUIRE(gt < size);

    const std::string helper = code.substr(at, lt - at);
    CHECK(helper.find("if (__a.index() == _I && __b.index() == _I)") != std::string::npos);
    CHECK(helper.find("return __a.template _M_get<_I>() < __b.template _M_get<_I>();") != std::string::npos);
    CHECK(helper.find("return false;") != std::string::npos);
    CHECK(helper.find("return true") == std::string::npos);
    CHECK(helper.find("{}") == std::string::npos);
    CHECK(prism_test::appears_before(helper, "_M_get", "return false;"));
    CHECK(helper.find("_M_get<_I>() ==") == std::string::npos);

    const std::string lt_body = code.substr(lt, gt - lt);
    const auto both = lt_body.find("if (__a.valueless_by_exception() && __b.valueless_by_exception())");
    const auto left = lt_body.find("if (__a.valueless_by_exception())");
    const auto right = lt_body.find("if (__b.valueless_by_exception())");
    const auto diff = lt_body.find("if (__a.index() != __b.index())");
    const auto cmp = lt_body.find("return __a.index() < __b.index();");
    const auto read = lt_body.find("_Prism_var_lt_at<0>(__a, __b)");
    REQUIRE(both != std::string::npos);
    REQUIRE(left != std::string::npos);
    REQUIRE(right != std::string::npos);
    REQUIRE(diff != std::string::npos);
    REQUIRE(cmp != std::string::npos);
    REQUIRE(read != std::string::npos);
    const auto both_ret = lt_body.find("return false;", both);
    const auto left_ret = lt_body.find("return true;", left);
    const auto right_ret = lt_body.find("return false;", right);
    REQUIRE(both_ret != std::string::npos);
    REQUIRE(left_ret != std::string::npos);
    REQUIRE(right_ret != std::string::npos);
    CHECK(both < both_ret);
    CHECK(both_ret < left);
    CHECK(left < left_ret);
    CHECK(left_ret < right);
    CHECK(right < right_ret);
    CHECK(right_ret < diff);
    CHECK(diff < cmp);
    CHECK(cmp < read);
    CHECK(lt_body.find("_M_get") == std::string::npos);
    CHECK(lt_body.find("_M_ptr") == std::string::npos);
    CHECK(lt_body.find("_M_storage") == std::string::npos);
    CHECK(lt_body.find("_M_first") == std::string::npos);
    CHECK(lt_body.find("T{}") == std::string::npos);
    CHECK(lt_body.find("_T{}") == std::string::npos);
    CHECK(lt_body.find("__prism_check") == std::string::npos);
    int variant_lt = 0;
    for (std::size_t i = 0; (i = code.find("operator<(const variant", i)) != std::string::npos; i += 1)
        ++variant_lt;
    CHECK(variant_lt == 1);
    CHECK(code.find("constexpr bool operator<(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator==(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator!=(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator>(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator<=(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator>=(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(code.find("return __a.template _M_get<_I>() == __b.template _M_get<_I>();") != std::string::npos);
    CHECK(code.find("return !(__a == __b);") != std::string::npos);
    CHECK(src.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(code.find("PROVED") == std::string::npos);
    CHECK(code.find("CLEAN") == std::string::npos);
    CHECK(code.find("operator<=>") == std::string::npos);
    CHECK(code.find("#include <memory>") == std::string::npos);
    CHECK(code.find("#include <compare>") == std::string::npos);
    CHECK(has_ident(code, "visit"));  // modelled (one or more lvalue variants)
    for (const char* name : {"swap", "bad_variant_access", "hash", "in_place"}) {
        CAPTURE(name);
        CHECK_FALSE(has_ident(code, name));
    }
}

TEST_CASE("cxx variant model: same-type operator< matches libstdc++ and different types do not compile") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);

    const char* diff_lt =
        "#include <variant>\n"
        "int use(){ std::variant<int> a(1); std::variant<char> b('a'); return a < b ? 1 : 0; }\n";
    const char* no_lt =
        "#include <variant>\n"
        "struct NoLt { int v; explicit NoLt(int x) : v(x) {} };\n"
        "int use(){ std::variant<NoLt, char> a(NoLt(1)); std::variant<NoLt, char> b(NoLt(2));\n"
        "  return a < b ? 1 : 0; }\n";
    for (const char* text : {diff_lt, no_lt}) {
        const fs::path src = c.tmp / "variant_diff_lt.cpp";
        std::ofstream(src) << text;
        std::string err;
        CHECK_FALSE(syntax_only(c.cxx, src, false, &err));
        CHECK_FALSE(syntax_only(c.cxx, src, true, &err));
    }

    const char* prog =
        "#include <cstdio>\n"
        "#include <variant>\n"
        "struct Ord {\n"
        "  int v;\n"
        "  explicit Ord(int x) : v(x) {}\n"
        "  bool operator<(const Ord& o) const {\n"
        "    std::printf(\"lt %d %d\\n\", v, o.v);\n"
        "    return v < o.v;\n"
        "  }\n"
        "};\n"
        "int main() {\n"
        "  std::variant<int, char> ia(1), ib(1), ic(2), id('A'), ie('B');\n"
        "  const std::variant<int, char> ca(1), cb(2);\n"
        "  std::printf(\"ilt %d %d %d %d %d %d %d %d %d\\n\",\n"
        "              static_cast<int>(ia < ia), static_cast<int>(ia < ib), static_cast<int>(ia < ic),\n"
        "              static_cast<int>(ic < ia), static_cast<int>(ia < id), static_cast<int>(id < ia),\n"
        "              static_cast<int>(id < ie), static_cast<int>(ie < id), static_cast<int>(ca < cb));\n"
        "  std::variant<int, int> a0, b1, c0, d1;\n"
        "  a0.emplace<0>(1);\n"
        "  b1.emplace<1>(1);\n"
        "  c0.emplace<0>(2);\n"
        "  d1.emplace<1>(0);\n"
        "  std::printf(\"dup %d %d %d %d %d %d\\n\", static_cast<int>(a0 < b1), static_cast<int>(b1 < a0),\n"
        "              static_cast<int>(a0 < c0), static_cast<int>(c0 < a0), static_cast<int>(b1 < d1),\n"
        "              static_cast<int>(d1 < b1));\n"
        "  std::variant<std::monostate, int> m1, m2, m3(4), m4(5);\n"
        "  std::printf(\"mlt %d %d %d %d %d %d\\n\", static_cast<int>(m1 < m2), static_cast<int>(m2 < m1),\n"
        "              static_cast<int>(m1 < m3), static_cast<int>(m3 < m1), static_cast<int>(m3 < m4),\n"
        "              static_cast<int>(m4 < m3));\n"
        "  std::variant<Ord, char> oa(Ord(3)), ob(Ord(3)), oc(Ord(4)), od('Z');\n"
        "  const int ab = oa < ob;\n"
        "  const int bc = ob < oc;\n"
        "  const int ca2 = oc < oa;\n"
        "  const int ad = oa < od;\n"
        "  const int da = od < oa;\n"
        "  std::printf(\"ord %d %d %d %d %d\\n\", ab, bc, ca2, ad, da);\n"
        "  std::variant<int, Ord> x(1), y(2), z(Ord(9));\n"
        "  const int xy = x < y;\n"
        "  const int xz = x < z;\n"
        "  const int zx = z < x;\n"
        "  const int zz = z < z;\n"
        "  std::printf(\"mix %d %d %d %d\\n\", xy, xz, zx, zz);\n"
        "  return 0;\n"
        "}\n";
    auto lib = build_text(c.cxx, c.tmp, "variant_lt", prog, false);
    auto mod = build_text(c.cxx, c.tmp, "variant_lt", prog, true);
    const std::vector<std::string> expect{"ilt 0 0 1 0 1 0 1 0 1",
                                          "dup 1 0 1 0 0 1",
                                          "mlt 0 0 1 0 1 0",
                                          "lt 3 3",
                                          "lt 3 4",
                                          "lt 4 3",
                                          "ord 0 1 0 1 0",
                                          "lt 9 9",
                                          "mix 1 1 0 0"};
    CHECK(run_lines(lib) == expect);
    CHECK(run_lines(mod) == expect);
}

TEST_CASE("cxx variant model: operator> is operator< with the arguments swapped") {
    const fs::path header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "variant";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    const std::string code = strip_comments(src);
    const auto lt = code.find("constexpr bool operator<(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto gt = code.find("constexpr bool operator>(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto le = code.find("constexpr bool operator<=(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto size = code.find("struct variant_size");
    REQUIRE(lt != std::string::npos);
    REQUIRE(gt != std::string::npos);
    REQUIRE(le != std::string::npos);
    REQUIRE(size != std::string::npos);
    REQUIRE(lt < gt);
    REQUIRE(gt < le);
    REQUIRE(le < size);

    const std::string lt_body = code.substr(lt, gt - lt);
    const auto both = lt_body.find("if (__a.valueless_by_exception() && __b.valueless_by_exception())");
    const auto both_ret = lt_body.find("return false;", both);
    const auto read = lt_body.find("_Prism_var_lt_at<0>(__a, __b)");
    REQUIRE(both != std::string::npos);
    REQUIRE(both_ret != std::string::npos);
    REQUIRE(read != std::string::npos);
    CHECK(both < both_ret);
    CHECK(both_ret < read);
    CHECK(lt_body.find("_M_get") == std::string::npos);
    CHECK(lt_body.find("_M_storage") == std::string::npos);

    const std::string gt_body = code.substr(gt, le - gt);
    CHECK(gt_body.find("return __b < __a;") != std::string::npos);
    CHECK(gt_body.find("__a < __b") == std::string::npos);
    CHECK(gt_body.find("__x < __y") != std::string::npos);
    CHECK(gt_body.find("__x > __y") == std::string::npos);
    CHECK(gt_body.find("return true") == std::string::npos);
    CHECK(gt_body.find("return false") == std::string::npos);
    CHECK(gt_body.find("_M_get") == std::string::npos);
    CHECK(gt_body.find("_M_ptr") == std::string::npos);
    CHECK(gt_body.find("_M_storage") == std::string::npos);
    CHECK(gt_body.find("_M_first") == std::string::npos);
    CHECK(gt_body.find("valueless_by_exception") == std::string::npos);
    CHECK(gt_body.find("index(") == std::string::npos);
    CHECK(gt_body.find("get_if") == std::string::npos);
    CHECK(gt_body.find("holds_alternative") == std::string::npos);
    CHECK(gt_body.find("__prism_check") == std::string::npos);
    CHECK(gt_body.find("T{}") == std::string::npos);
    CHECK(gt_body.find("_T{}") == std::string::npos);
    int variant_gt = 0;
    for (std::size_t i = 0; (i = code.find("operator>(const variant", i)) != std::string::npos; i += 1)
        ++variant_gt;
    CHECK(variant_gt == 1);
    int variant_lt = 0;
    for (std::size_t i = 0; (i = code.find("operator<(const variant", i)) != std::string::npos; i += 1)
        ++variant_lt;
    CHECK(variant_lt == 1);
    CHECK(code.find("return !(__a == __b);") != std::string::npos);
    CHECK(code.find("return __a.template _M_get<_I>() == __b.template _M_get<_I>();") != std::string::npos);
    CHECK(code.find("return __a.template _M_get<_I>() < __b.template _M_get<_I>();") != std::string::npos);
    CHECK(code.find("constexpr bool operator==(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator!=(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator<(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator>(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator<=(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator>=(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(src.find("std::get on an inactive variant alternative") != std::string::npos);
    CHECK(src.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(code.find("PROVED") == std::string::npos);
    CHECK(code.find("CLEAN") == std::string::npos);
    CHECK(code.find("operator<=>") == std::string::npos);
    CHECK(code.find("constexpr bool operator>=(const variant<_Types...>& __a, const variant<_Types...>& __b)") !=
          std::string::npos);
    CHECK(code.find("return !(__a < __b);") != std::string::npos);
    CHECK(code.find("operator>=(variant") == std::string::npos);
    CHECK(code.find("return !(__b < __a);") != std::string::npos);
    CHECK(has_ident(code, "visit"));  // modelled (one or more lvalue variants)
    for (const char* name : {"swap", "bad_variant_access", "hash", "in_place"}) {
        CAPTURE(name);
        CHECK_FALSE(has_ident(code, name));
    }
}

TEST_CASE("cxx variant model: same-type operator> matches libstdc++ and different types do not compile") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);

    const char* diff_gt =
        "#include <variant>\n"
        "int use(){ std::variant<int> a(1); std::variant<char> b('a'); return a > b ? 1 : 0; }\n";
    const char* no_lt =
        "#include <variant>\n"
        "struct NoLt { int v; explicit NoLt(int x) : v(x) {} };\n"
        "int use(){ std::variant<NoLt, char> a(NoLt(1)); std::variant<NoLt, char> b(NoLt(2));\n"
        "  return a > b ? 1 : 0; }\n";
    for (const char* text : {diff_gt, no_lt}) {
        const fs::path src = c.tmp / "variant_diff_gt.cpp";
        std::ofstream(src) << text;
        std::string err;
        CHECK_FALSE(syntax_only(c.cxx, src, false, &err));
        CHECK_FALSE(syntax_only(c.cxx, src, true, &err));
    }

    const char* only_gt =
        "#include <variant>\n"
        "struct OnlyGt {\n"
        "  int v;\n"
        "  explicit OnlyGt(int x) : v(x) {}\n"
        "  bool operator>(const OnlyGt& o) const { return v > o.v; }\n"
        "};\n"
        "int use(){ std::variant<OnlyGt, char> a(OnlyGt(1)); std::variant<OnlyGt, char> b(OnlyGt(2));\n"
        "  return a > b ? 1 : 0; }\n";
    const fs::path only_src = c.tmp / "variant_only_gt.cpp";
    std::ofstream(only_src) << only_gt;
    std::string err;
    CHECK(syntax_only(c.cxx, only_src, false, &err));
    CHECK_FALSE(syntax_only(c.cxx, only_src, true, &err));

    const char* prog =
        "#include <cstdio>\n"
        "#include <variant>\n"
        "int main() {\n"
        "  std::variant<int, char> ia(1), ib(1), ic(2), id('A'), ie('B');\n"
        "  const std::variant<int, char> ca(1), cb(2);\n"
        "  std::printf(\"igt %d %d %d %d %d %d %d %d %d\\n\",\n"
        "              static_cast<int>(ia > ia), static_cast<int>(ia > ib), static_cast<int>(ia > ic),\n"
        "              static_cast<int>(ic > ia), static_cast<int>(ia > id), static_cast<int>(id > ia),\n"
        "              static_cast<int>(id > ie), static_cast<int>(ie > id), static_cast<int>(ca > cb));\n"
        "  std::variant<int, int> a0, b1, c0, d1;\n"
        "  a0.emplace<0>(1);\n"
        "  b1.emplace<1>(1);\n"
        "  c0.emplace<0>(2);\n"
        "  d1.emplace<1>(0);\n"
        "  std::printf(\"dup %d %d %d %d %d %d\\n\", static_cast<int>(a0 > b1), static_cast<int>(b1 > a0),\n"
        "              static_cast<int>(a0 > c0), static_cast<int>(c0 > a0), static_cast<int>(b1 > d1),\n"
        "              static_cast<int>(d1 > b1));\n"
        "  std::variant<std::monostate, int> m1, m2, m3(4), m4(5);\n"
        "  std::printf(\"mgt %d %d %d %d %d %d\\n\", static_cast<int>(m1 > m2), static_cast<int>(m2 > m1),\n"
        "              static_cast<int>(m1 > m3), static_cast<int>(m3 > m1), static_cast<int>(m3 > m4),\n"
        "              static_cast<int>(m4 > m3));\n"
        "  return 0;\n"
        "}\n";
    auto lib = build_text(c.cxx, c.tmp, "variant_gt", prog, false);
    auto mod = build_text(c.cxx, c.tmp, "variant_gt", prog, true);
    const std::vector<std::string> expect{"igt 0 0 0 1 0 1 0 1 0", "dup 0 1 0 1 1 0", "mgt 0 0 0 1 0 1"};
    CHECK(run_lines(lib) == expect);
    CHECK(run_lines(mod) == expect);
}

TEST_CASE("cxx variant model: operator> calls operator< and does not read an inactive alternative") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);

    const char* prog =
        "#include <cstdio>\n"
        "#include <variant>\n"
        "struct Ord {\n"
        "  int v;\n"
        "  explicit Ord(int x) : v(x) {}\n"
        "  bool operator<(const Ord& o) const {\n"
        "    std::printf(\"lt %d %d\\n\", v, o.v);\n"
        "    return v < o.v;\n"
        "  }\n"
        "  bool operator>(const Ord& o) const {\n"
        "    std::printf(\"gt %d %d\\n\", v, o.v);\n"
        "    return v > o.v;\n"
        "  }\n"
        "};\n"
        "int main() {\n"
        "  std::variant<Ord, char> oa(Ord(3)), ob(Ord(3)), oc(Ord(4)), od('Z');\n"
        "  const int ab = oa > ob;\n"
        "  const int bc = ob > oc;\n"
        "  const int ca2 = oc > oa;\n"
        "  const int ad = oa > od;\n"
        "  const int da = od > oa;\n"
        "  std::printf(\"ord %d %d %d %d %d\\n\", ab, bc, ca2, ad, da);\n"
        "  std::variant<int, Ord> x(1), y(2), z(Ord(9));\n"
        "  const int xy = x > y;\n"
        "  const int xz = x > z;\n"
        "  const int zx = z > x;\n"
        "  const int zz = z > z;\n"
        "  std::printf(\"mix %d %d %d %d\\n\", xy, xz, zx, zz);\n"
        "  return 0;\n"
        "}\n";
    const fs::path src = c.tmp / "variant_gt_ord.cpp";
    std::ofstream(src) << prog;
    std::string err;
    CHECK(syntax_only(c.cxx, src, false, &err));
    CHECK(syntax_only(c.cxx, src, true, &err));
    auto mod = build_text(c.cxx, c.tmp, "variant_gt_ord", prog, true);
    const std::vector<std::string> expect{"lt 3 3", "lt 4 3", "lt 3 4", "ord 0 0 1 0 1", "lt 9 9", "mix 0 0 1 0"};
    const auto lines = run_lines(mod);
    CHECK(lines == expect);
    for (const auto& line : lines) CHECK(line.find("gt ") == std::string::npos);
}

TEST_CASE("cxx variant model: operator<= is the negation of operator< with the arguments swapped") {
    const fs::path header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "variant";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    const std::string code = strip_comments(src);
    const auto lt = code.find("constexpr bool operator<(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto gt = code.find("constexpr bool operator>(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto le = code.find("constexpr bool operator<=(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto ge = code.find("constexpr bool operator>=(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto size = code.find("struct variant_size");
    REQUIRE(lt != std::string::npos);
    REQUIRE(gt != std::string::npos);
    REQUIRE(le != std::string::npos);
    REQUIRE(ge != std::string::npos);
    REQUIRE(size != std::string::npos);
    REQUIRE(lt < gt);
    REQUIRE(gt < le);
    REQUIRE(le < ge);
    REQUIRE(ge < size);

    const std::string lt_body = code.substr(lt, gt - lt);
    const auto both = lt_body.find("if (__a.valueless_by_exception() && __b.valueless_by_exception())");
    const auto both_ret = lt_body.find("return false;", both);
    const auto read = lt_body.find("_Prism_var_lt_at<0>(__a, __b)");
    REQUIRE(both != std::string::npos);
    REQUIRE(both_ret != std::string::npos);
    REQUIRE(read != std::string::npos);
    CHECK(both < both_ret);
    CHECK(both_ret < read);
    CHECK(lt_body.find("_M_get") == std::string::npos);

    const std::string gt_body = code.substr(gt, le - gt);
    CHECK(gt_body.find("return __b < __a;") != std::string::npos);
    CHECK(gt_body.find("return !(__b < __a);") == std::string::npos);

    const std::string le_body = code.substr(le, ge - le);
    CHECK(le_body.find("return !(__b < __a);") != std::string::npos);
    CHECK(le_body.find("return __b < __a;") == std::string::npos);
    CHECK(le_body.find("__a < __b") == std::string::npos);
    CHECK(le_body.find("__a > __b") == std::string::npos);
    CHECK(le_body.find("__x < __y") != std::string::npos);
    CHECK(le_body.find("__x <= __y") == std::string::npos);
    CHECK(le_body.find("__x > __y") == std::string::npos);
    CHECK(le_body.find("return true") == std::string::npos);
    CHECK(le_body.find("return false") == std::string::npos);
    CHECK(le_body.find("_Prism_var_lt_at") == std::string::npos);
    CHECK(le_body.find("_M_get") == std::string::npos);
    CHECK(le_body.find("_M_ptr") == std::string::npos);
    CHECK(le_body.find("_M_storage") == std::string::npos);
    CHECK(le_body.find("_M_first") == std::string::npos);
    CHECK(le_body.find("valueless_by_exception") == std::string::npos);
    CHECK(le_body.find("index(") == std::string::npos);
    CHECK(le_body.find("get_if") == std::string::npos);
    CHECK(le_body.find("holds_alternative") == std::string::npos);
    CHECK(le_body.find("__prism_check") == std::string::npos);
    CHECK(le_body.find("T{}") == std::string::npos);
    CHECK(le_body.find("_T{}") == std::string::npos);
    CHECK(le_body.find("monostate{}") == std::string::npos);
    CHECK(le_body.find("monostate{") == std::string::npos);
    int variant_le = 0;
    for (std::size_t i = 0; (i = code.find("operator<=(const variant", i)) != std::string::npos; i += 1)
        ++variant_le;
    CHECK(variant_le == 1);
    int variant_gt = 0;
    for (std::size_t i = 0; (i = code.find("operator>(const variant", i)) != std::string::npos; i += 1)
        ++variant_gt;
    CHECK(variant_gt == 1);
    int variant_lt = 0;
    for (std::size_t i = 0; (i = code.find("operator<(const variant", i)) != std::string::npos; i += 1)
        ++variant_lt;
    CHECK(variant_lt == 1);
    int swapped_lt = 0;
    for (std::size_t i = 0; (i = code.find("return __b < __a;", i)) != std::string::npos; i += 1)
        ++swapped_lt;
    CHECK(swapped_lt == 1);
    int neg_lt = 0;
    for (std::size_t i = 0; (i = code.find("return !(__b < __a);", i)) != std::string::npos; i += 1)
        ++neg_lt;
    CHECK(neg_lt == 1);
    CHECK(code.find("return !(__a == __b);") != std::string::npos);
    CHECK(code.find("return __a.template _M_get<_I>() == __b.template _M_get<_I>();") != std::string::npos);
    CHECK(code.find("return __a.template _M_get<_I>() < __b.template _M_get<_I>();") != std::string::npos);
    CHECK(code.find("constexpr bool operator==(const variant<_Types...>& __a, const variant<_Types...>& __b)") !=
          std::string::npos);
    CHECK(code.find("constexpr bool operator!=(const variant<_Types...>& __a, const variant<_Types...>& __b)") !=
          std::string::npos);
    CHECK(code.find("constexpr bool operator==(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator!=(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator<(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator>(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator<=(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator>=(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(src.find("std::get on an inactive variant alternative") != std::string::npos);
    CHECK(src.find("comparison of variant") != std::string::npos);
    CHECK(src.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(code.find("PROVED") == std::string::npos);
    CHECK(code.find("CLEAN") == std::string::npos);
    CHECK(code.find("operator<=>") == std::string::npos);
    CHECK(code.find("constexpr bool operator>=(const variant<_Types...>& __a, const variant<_Types...>& __b)") !=
          std::string::npos);
    CHECK(code.find("return !(__a < __b);") != std::string::npos);
    CHECK(code.find("operator>=(variant") == std::string::npos);
    CHECK(code.find("#include <memory>") == std::string::npos);
    CHECK(code.find("#include <compare>") == std::string::npos);
    CHECK(has_ident(code, "visit"));  // modelled (one or more lvalue variants)
    for (const char* name : {"swap", "bad_variant_access", "hash", "in_place"}) {
        CAPTURE(name);
        CHECK_FALSE(has_ident(code, name));
    }
}

TEST_CASE("cxx variant model: same-type operator<= matches libstdc++ and different types do not compile") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);

    const char* diff_le =
        "#include <variant>\n"
        "int use(){ std::variant<int> a(1); std::variant<char> b('a'); return a <= b ? 1 : 0; }\n";
    const char* no_lt =
        "#include <variant>\n"
        "struct NoLt { int v; explicit NoLt(int x) : v(x) {} };\n"
        "int use(){ std::variant<NoLt, char> a(NoLt(1)); std::variant<NoLt, char> b(NoLt(2));\n"
        "  return a <= b ? 1 : 0; }\n";
    for (const char* text : {diff_le, no_lt}) {
        const fs::path src = c.tmp / "variant_diff_le.cpp";
        std::ofstream(src) << text;
        std::string err;
        CHECK_FALSE(syntax_only(c.cxx, src, false, &err));
        CHECK_FALSE(syntax_only(c.cxx, src, true, &err));
    }

    const char* only_le =
        "#include <variant>\n"
        "struct OnlyLe {\n"
        "  int v;\n"
        "  explicit OnlyLe(int x) : v(x) {}\n"
        "  bool operator<=(const OnlyLe& o) const { return v <= o.v; }\n"
        "};\n"
        "int use(){ std::variant<OnlyLe, char> a(OnlyLe(1)); std::variant<OnlyLe, char> b(OnlyLe(2));\n"
        "  return a <= b ? 1 : 0; }\n";
    const fs::path only_le_src = c.tmp / "variant_only_le.cpp";
    std::ofstream(only_le_src) << only_le;
    std::string err;
    CHECK(syntax_only(c.cxx, only_le_src, false, &err));
    CHECK_FALSE(syntax_only(c.cxx, only_le_src, true, &err));

    const char* only_lt =
        "#include <variant>\n"
        "struct OnlyLt {\n"
        "  int v;\n"
        "  explicit OnlyLt(int x) : v(x) {}\n"
        "  bool operator<(const OnlyLt& o) const { return v < o.v; }\n"
        "};\n"
        "int use(){ std::variant<OnlyLt, char> a(OnlyLt(1)); std::variant<OnlyLt, char> b(OnlyLt(2));\n"
        "  return a <= b ? 1 : 0; }\n";
    const fs::path only_lt_src = c.tmp / "variant_only_lt.cpp";
    std::ofstream(only_lt_src) << only_lt;
    CHECK_FALSE(syntax_only(c.cxx, only_lt_src, false, &err));
    CHECK(syntax_only(c.cxx, only_lt_src, true, &err));

    const char* prog =
        "#include <cstdio>\n"
        "#include <variant>\n"
        "int main() {\n"
        "  std::variant<int, char> ia(1), ib(1), ic(2), id('A'), ie('B');\n"
        "  const std::variant<int, char> ca(1), cb(2);\n"
        "  std::printf(\"ile %d %d %d %d %d %d %d %d %d\\n\",\n"
        "              static_cast<int>(ia <= ia), static_cast<int>(ia <= ib), static_cast<int>(ia <= ic),\n"
        "              static_cast<int>(ic <= ia), static_cast<int>(ia <= id), static_cast<int>(id <= ia),\n"
        "              static_cast<int>(id <= ie), static_cast<int>(ie <= id), static_cast<int>(ca <= cb));\n"
        "  std::variant<int, int> a0, b1, c0, d1, e0;\n"
        "  a0.emplace<0>(1);\n"
        "  b1.emplace<1>(1);\n"
        "  c0.emplace<0>(2);\n"
        "  d1.emplace<1>(0);\n"
        "  e0.emplace<0>(1);\n"
        "  std::printf(\"dup %d %d %d %d %d %d %d\\n\", static_cast<int>(a0 <= b1), static_cast<int>(b1 <= a0),\n"
        "              static_cast<int>(a0 <= c0), static_cast<int>(c0 <= a0), static_cast<int>(b1 <= d1),\n"
        "              static_cast<int>(d1 <= b1), static_cast<int>(a0 <= e0));\n"
        "  std::variant<std::monostate, int> m1, m2, m3(4), m4(5);\n"
        "  std::printf(\"mle %d %d %d %d %d %d\\n\", static_cast<int>(m1 <= m2), static_cast<int>(m2 <= m1),\n"
        "              static_cast<int>(m1 <= m3), static_cast<int>(m3 <= m1), static_cast<int>(m3 <= m4),\n"
        "              static_cast<int>(m4 <= m3));\n"
        "  return 0;\n"
        "}\n";
    auto lib = build_text(c.cxx, c.tmp, "variant_le", prog, false);
    auto mod = build_text(c.cxx, c.tmp, "variant_le", prog, true);
    const std::vector<std::string> expect{"ile 1 1 1 0 1 0 1 0 1", "dup 1 0 1 0 0 1 1", "mle 1 1 1 0 1 0"};
    CHECK(run_lines(lib) == expect);
    CHECK(run_lines(mod) == expect);
}

TEST_CASE("cxx variant model: operator<= calls operator< and does not read an inactive alternative") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);

    const char* prog =
        "#include <cstdio>\n"
        "#include <variant>\n"
        "struct Ord {\n"
        "  int v;\n"
        "  explicit Ord(int x) : v(x) {}\n"
        "  bool operator<(const Ord& o) const {\n"
        "    std::printf(\"lt %d %d\\n\", v, o.v);\n"
        "    return v < o.v;\n"
        "  }\n"
        "  bool operator<=(const Ord& o) const {\n"
        "    std::printf(\"le %d %d\\n\", v, o.v);\n"
        "    return v <= o.v;\n"
        "  }\n"
        "  bool operator>(const Ord& o) const {\n"
        "    std::printf(\"gt %d %d\\n\", v, o.v);\n"
        "    return v > o.v;\n"
        "  }\n"
        "};\n"
        "int main() {\n"
        "  std::variant<Ord, char> oa(Ord(3)), ob(Ord(3)), oc(Ord(4)), od('Z');\n"
        "  const int ab = oa <= ob;\n"
        "  const int bc = ob <= oc;\n"
        "  const int ca2 = oc <= oa;\n"
        "  const int ad = oa <= od;\n"
        "  const int da = od <= oa;\n"
        "  std::printf(\"ord %d %d %d %d %d\\n\", ab, bc, ca2, ad, da);\n"
        "  std::variant<int, Ord> x(1), y(2), z(Ord(9));\n"
        "  const int xy = x <= y;\n"
        "  const int xz = x <= z;\n"
        "  const int zx = z <= x;\n"
        "  const int zz = z <= z;\n"
        "  std::printf(\"mix %d %d %d %d\\n\", xy, xz, zx, zz);\n"
        "  return 0;\n"
        "}\n";
    const fs::path src = c.tmp / "variant_le_ord.cpp";
    std::ofstream(src) << prog;
    std::string err;
    CHECK(syntax_only(c.cxx, src, false, &err));
    CHECK(syntax_only(c.cxx, src, true, &err));
    auto mod = build_text(c.cxx, c.tmp, "variant_le_ord", prog, true);
    const std::vector<std::string> expect{"lt 3 3", "lt 4 3", "lt 3 4", "ord 1 1 0 1 0", "lt 9 9", "mix 1 1 0 1"};
    const auto lines = run_lines(mod);
    CHECK(lines == expect);
    for (const auto& line : lines) {
        CHECK(line.find("le ") == std::string::npos);
        CHECK(line.find("gt ") == std::string::npos);
    }
}

TEST_CASE("cxx variant model: operator>= is the negation of operator<") {
    const fs::path header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "variant";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    const std::string code = strip_comments(src);
    const auto lt = code.find("constexpr bool operator<(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto gt = code.find("constexpr bool operator>(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto le = code.find("constexpr bool operator<=(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto ge = code.find("constexpr bool operator>=(const variant<_Types...>& __a, const variant<_Types...>& __b)");
    const auto size = code.find("struct variant_size");
    REQUIRE(lt != std::string::npos);
    REQUIRE(gt != std::string::npos);
    REQUIRE(le != std::string::npos);
    REQUIRE(ge != std::string::npos);
    REQUIRE(size != std::string::npos);
    REQUIRE(lt < gt);
    REQUIRE(gt < le);
    REQUIRE(le < ge);
    REQUIRE(ge < size);

    const std::string lt_body = code.substr(lt, gt - lt);
    const auto both = lt_body.find("if (__a.valueless_by_exception() && __b.valueless_by_exception())");
    const auto both_ret = lt_body.find("return false;", both);
    const auto read = lt_body.find("_Prism_var_lt_at<0>(__a, __b)");
    REQUIRE(both != std::string::npos);
    REQUIRE(both_ret != std::string::npos);
    REQUIRE(read != std::string::npos);
    CHECK(both < both_ret);
    CHECK(both_ret < read);
    CHECK(lt_body.find("_M_get") == std::string::npos);

    const std::string ge_body = code.substr(ge, size - ge);
    CHECK(ge_body.find("return !(__a < __b);") != std::string::npos);
    CHECK(ge_body.find("return __a < __b;") == std::string::npos);
    CHECK(ge_body.find("return !(__b < __a);") == std::string::npos);
    CHECK(ge_body.find("return __b < __a;") == std::string::npos);
    CHECK(ge_body.find("__a > __b") == std::string::npos);
    CHECK(ge_body.find("__b < __a") == std::string::npos);
    CHECK(ge_body.find("__x < __y") != std::string::npos);
    CHECK(ge_body.find("__x >= __y") == std::string::npos);
    CHECK(ge_body.find("__x > __y") == std::string::npos);
    CHECK(ge_body.find("__x <= __y") == std::string::npos);
    CHECK(ge_body.find("return true") == std::string::npos);
    CHECK(ge_body.find("return false") == std::string::npos);
    CHECK(ge_body.find("_Prism_var_lt_at") == std::string::npos);
    CHECK(ge_body.find("_M_get") == std::string::npos);
    CHECK(ge_body.find("_M_ptr") == std::string::npos);
    CHECK(ge_body.find("_M_storage") == std::string::npos);
    CHECK(ge_body.find("_M_first") == std::string::npos);
    CHECK(ge_body.find("valueless_by_exception") == std::string::npos);
    CHECK(ge_body.find("index(") == std::string::npos);
    CHECK(ge_body.find("get_if") == std::string::npos);
    CHECK(ge_body.find("holds_alternative") == std::string::npos);
    CHECK(ge_body.find("__prism_check") == std::string::npos);
    CHECK(ge_body.find("T{}") == std::string::npos);
    CHECK(ge_body.find("_T{}") == std::string::npos);
    CHECK(ge_body.find("monostate{}") == std::string::npos);
    CHECK(ge_body.find("monostate{") == std::string::npos);
    int variant_ge = 0;
    for (std::size_t i = 0; (i = code.find("operator>=(const variant", i)) != std::string::npos; i += 1)
        ++variant_ge;
    CHECK(variant_ge == 1);
    int variant_le = 0;
    for (std::size_t i = 0; (i = code.find("operator<=(const variant", i)) != std::string::npos; i += 1)
        ++variant_le;
    CHECK(variant_le == 1);
    int variant_gt = 0;
    for (std::size_t i = 0; (i = code.find("operator>(const variant", i)) != std::string::npos; i += 1)
        ++variant_gt;
    CHECK(variant_gt == 1);
    int variant_lt = 0;
    for (std::size_t i = 0; (i = code.find("operator<(const variant", i)) != std::string::npos; i += 1)
        ++variant_lt;
    CHECK(variant_lt == 1);
    int neg_same = 0;
    for (std::size_t i = 0; (i = code.find("return !(__a < __b);", i)) != std::string::npos; i += 1)
        ++neg_same;
    CHECK(neg_same == 1);
    int neg_swap = 0;
    for (std::size_t i = 0; (i = code.find("return !(__b < __a);", i)) != std::string::npos; i += 1)
        ++neg_swap;
    CHECK(neg_swap == 1);
    int swapped_lt = 0;
    for (std::size_t i = 0; (i = code.find("return __b < __a;", i)) != std::string::npos; i += 1)
        ++swapped_lt;
    CHECK(swapped_lt == 1);
    CHECK(code.find("return !(__a == __b);") != std::string::npos);
    CHECK(code.find("return __a.template _M_get<_I>() == __b.template _M_get<_I>();") != std::string::npos);
    CHECK(code.find("return __a.template _M_get<_I>() < __b.template _M_get<_I>();") != std::string::npos);
    CHECK(code.find("constexpr bool operator==(const variant<_Types...>& __a, const variant<_Types...>& __b)") !=
          std::string::npos);
    CHECK(code.find("constexpr bool operator!=(const variant<_Types...>& __a, const variant<_Types...>& __b)") !=
          std::string::npos);
    CHECK(code.find("constexpr bool operator==(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator!=(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator<(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator>(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator<=(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator>=(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(src.find("std::get on an inactive variant alternative") != std::string::npos);
    CHECK(src.find("comparison of variant") != std::string::npos);
    CHECK(src.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(code.find("PROVED") == std::string::npos);
    CHECK(code.find("CLEAN") == std::string::npos);
    CHECK(code.find("operator<=>") == std::string::npos);
    CHECK(code.find("operator>=(variant") == std::string::npos);
    CHECK(code.find("#include <memory>") == std::string::npos);
    CHECK(code.find("#include <compare>") == std::string::npos);
    CHECK(has_ident(code, "visit"));  // modelled (one or more lvalue variants)
    for (const char* name : {"swap", "bad_variant_access", "hash", "in_place"}) {
        CAPTURE(name);
        CHECK_FALSE(has_ident(code, name));
    }
}

TEST_CASE("cxx variant model: same-type operator>= matches libstdc++ and different types do not compile") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);

    const char* diff_ge =
        "#include <variant>\n"
        "int use(){ std::variant<int> a(1); std::variant<char> b('a'); return a >= b ? 1 : 0; }\n";
    const char* no_lt =
        "#include <variant>\n"
        "struct NoLt { int v; explicit NoLt(int x) : v(x) {} };\n"
        "int use(){ std::variant<NoLt, char> a(NoLt(1)); std::variant<NoLt, char> b(NoLt(2));\n"
        "  return a >= b ? 1 : 0; }\n";
    for (const char* text : {diff_ge, no_lt}) {
        const fs::path src = c.tmp / "variant_diff_ge.cpp";
        std::ofstream(src) << text;
        std::string err;
        CHECK_FALSE(syntax_only(c.cxx, src, false, &err));
        CHECK_FALSE(syntax_only(c.cxx, src, true, &err));
    }

    const char* only_ge =
        "#include <variant>\n"
        "struct OnlyGe {\n"
        "  int v;\n"
        "  explicit OnlyGe(int x) : v(x) {}\n"
        "  bool operator>=(const OnlyGe& o) const { return v >= o.v; }\n"
        "};\n"
        "int use(){ std::variant<OnlyGe, char> a(OnlyGe(1)); std::variant<OnlyGe, char> b(OnlyGe(2));\n"
        "  return a >= b ? 1 : 0; }\n";
    const fs::path only_ge_src = c.tmp / "variant_only_ge.cpp";
    std::ofstream(only_ge_src) << only_ge;
    std::string err;
    CHECK(syntax_only(c.cxx, only_ge_src, false, &err));
    CHECK_FALSE(syntax_only(c.cxx, only_ge_src, true, &err));

    const char* only_lt =
        "#include <variant>\n"
        "struct OnlyLt {\n"
        "  int v;\n"
        "  explicit OnlyLt(int x) : v(x) {}\n"
        "  bool operator<(const OnlyLt& o) const { return v < o.v; }\n"
        "};\n"
        "int use(){ std::variant<OnlyLt, char> a(OnlyLt(1)); std::variant<OnlyLt, char> b(OnlyLt(2));\n"
        "  return a >= b ? 1 : 0; }\n";
    const fs::path only_lt_src = c.tmp / "variant_only_lt_ge.cpp";
    std::ofstream(only_lt_src) << only_lt;
    CHECK_FALSE(syntax_only(c.cxx, only_lt_src, false, &err));
    CHECK(syntax_only(c.cxx, only_lt_src, true, &err));

    const char* prog =
        "#include <cstdio>\n"
        "#include <variant>\n"
        "int main() {\n"
        "  std::variant<int, char> ia(1), ib(1), ic(2), id('A'), ie('B');\n"
        "  const std::variant<int, char> ca(1), cb(2);\n"
        "  std::printf(\"ige %d %d %d %d %d %d %d %d %d\\n\",\n"
        "              static_cast<int>(ia >= ia), static_cast<int>(ia >= ib), static_cast<int>(ia >= ic),\n"
        "              static_cast<int>(ic >= ia), static_cast<int>(ia >= id), static_cast<int>(id >= ia),\n"
        "              static_cast<int>(id >= ie), static_cast<int>(ie >= id), static_cast<int>(ca >= cb));\n"
        "  std::variant<int, int> a0, b1, c0, d1, e0;\n"
        "  a0.emplace<0>(1);\n"
        "  b1.emplace<1>(1);\n"
        "  c0.emplace<0>(2);\n"
        "  d1.emplace<1>(0);\n"
        "  e0.emplace<0>(1);\n"
        "  std::printf(\"dup %d %d %d %d %d %d %d\\n\", static_cast<int>(a0 >= b1), static_cast<int>(b1 >= a0),\n"
        "              static_cast<int>(a0 >= c0), static_cast<int>(c0 >= a0), static_cast<int>(b1 >= d1),\n"
        "              static_cast<int>(d1 >= b1), static_cast<int>(a0 >= e0));\n"
        "  std::variant<std::monostate, int> m1, m2, m3(4), m4(5);\n"
        "  std::printf(\"mge %d %d %d %d %d %d\\n\", static_cast<int>(m1 >= m2), static_cast<int>(m2 >= m1),\n"
        "              static_cast<int>(m1 >= m3), static_cast<int>(m3 >= m1), static_cast<int>(m3 >= m4),\n"
        "              static_cast<int>(m4 >= m3));\n"
        "  return 0;\n"
        "}\n";
    auto lib = build_text(c.cxx, c.tmp, "variant_ge", prog, false);
    auto mod = build_text(c.cxx, c.tmp, "variant_ge", prog, true);
    const std::vector<std::string> expect{"ige 1 1 0 1 0 1 0 1 0", "dup 0 1 0 1 1 0 1", "mge 1 1 0 1 0 1"};
    CHECK(run_lines(lib) == expect);
    CHECK(run_lines(mod) == expect);
}

TEST_CASE("cxx variant model: operator>= calls operator< and does not read an inactive alternative") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);

    const char* prog =
        "#include <cstdio>\n"
        "#include <variant>\n"
        "struct Ord {\n"
        "  int v;\n"
        "  explicit Ord(int x) : v(x) {}\n"
        "  bool operator<(const Ord& o) const {\n"
        "    std::printf(\"lt %d %d\\n\", v, o.v);\n"
        "    return v < o.v;\n"
        "  }\n"
        "  bool operator>=(const Ord& o) const {\n"
        "    std::printf(\"ge %d %d\\n\", v, o.v);\n"
        "    return v >= o.v;\n"
        "  }\n"
        "  bool operator>(const Ord& o) const {\n"
        "    std::printf(\"gt %d %d\\n\", v, o.v);\n"
        "    return v > o.v;\n"
        "  }\n"
        "};\n"
        "int main() {\n"
        "  std::variant<Ord, char> oa(Ord(3)), ob(Ord(3)), oc(Ord(4)), od('Z');\n"
        "  const int ab = oa >= ob;\n"
        "  const int bc = ob >= oc;\n"
        "  const int ca2 = oc >= oa;\n"
        "  const int ad = oa >= od;\n"
        "  const int da = od >= oa;\n"
        "  std::printf(\"ord %d %d %d %d %d\\n\", ab, bc, ca2, ad, da);\n"
        "  std::variant<int, Ord> x(1), y(2), z(Ord(9));\n"
        "  const int xy = x >= y;\n"
        "  const int xz = x >= z;\n"
        "  const int zx = z >= x;\n"
        "  const int zz = z >= z;\n"
        "  std::printf(\"mix %d %d %d %d\\n\", xy, xz, zx, zz);\n"
        "  return 0;\n"
        "}\n";
    const fs::path src = c.tmp / "variant_ge_ord.cpp";
    std::ofstream(src) << prog;
    std::string err;
    CHECK(syntax_only(c.cxx, src, false, &err));
    CHECK(syntax_only(c.cxx, src, true, &err));
    auto mod = build_text(c.cxx, c.tmp, "variant_ge_ord", prog, true);
    const std::vector<std::string> expect{"lt 3 3", "lt 3 4", "lt 4 3", "ord 1 0 1 0 1", "lt 9 9", "mix 0 0 1 1"};
    const auto lines = run_lines(mod);
    CHECK(lines == expect);
    for (const auto& line : lines) {
        CHECK(line.find("ge ") == std::string::npos);
        CHECK(line.find("gt ") == std::string::npos);
    }
}
