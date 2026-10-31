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
// of the same type compare with == and !=. visit, swap, and relational
// comparison of variant stay undeclared. This is not PROVED and not CLEAN.

#include <doctest/doctest.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
#else
#  include "support/proctree.hpp"
#endif

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

struct VarCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

VarCtx& ctx() {
    static VarCtx c;
    return c;
}

bool run_cmd(const std::vector<std::string>& argv, std::string* out = nullptr, std::string* err = nullptr) {
#ifndef _WIN32
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    int pipe_out[2]{-1, -1};
    int pipe_err[2]{-1, -1};
    if (out && pipe(pipe_out) != 0) return false;
    if (err && pipe(pipe_err) != 0) {
        if (out) {
            close(pipe_out[0]);
            close(pipe_out[1]);
        }
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (out) {
        posix_spawn_file_actions_addclose(&actions, pipe_out[0]);
        posix_spawn_file_actions_adddup2(&actions, pipe_out[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_out[1]);
    }
    if (err) {
        posix_spawn_file_actions_addclose(&actions, pipe_err[0]);
        posix_spawn_file_actions_adddup2(&actions, pipe_err[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_err[1]);
    }
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0].c_str(), &actions, nullptr, args.data(), environ) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        if (out) {
            close(pipe_out[0]);
            close(pipe_out[1]);
        }
        if (err) {
            close(pipe_err[0]);
            close(pipe_err[1]);
        }
        return false;
    }
    posix_spawn_file_actions_destroy(&actions);
    if (out) {
        close(pipe_out[1]);
        out->clear();
        char buf[8192];
        ssize_t n = 0;
        while ((n = ::read(pipe_out[0], buf, sizeof buf)) > 0) out->append(buf, static_cast<size_t>(n));
        close(pipe_out[0]);
    }
    if (err) {
        close(pipe_err[1]);
        err->clear();
        char buf[8192];
        ssize_t n = 0;
        while ((n = ::read(pipe_err[0], buf, sizeof buf)) > 0) err->append(buf, static_cast<size_t>(n));
        close(pipe_err[0]);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return false;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
#else
    prism::qa::RunOpts o;
    o.timeout_s = 300;
    auto r = prism::qa::run_tree(argv, o);
    if (out) {
        *out = r.out;
        std::erase(*out, '\r');  // text-mode stdout on Windows is CRLF
    }
    if (err) *err = r.err;
    return !r.start_failed && !r.timed_out && r.rc == 0;
#endif
}

std::string fwd(std::string s) {
    for (auto& c : s)
        if (c == '\\') c = '/';
    return s;
}

#ifdef _WIN32
constexpr const char* kProbeExe = "probe.exe";
#else
constexpr const char* kProbeExe = "probe";
#endif

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    auto probe = tmp / "probe.cpp";
    std::ofstream(probe) << "#include <bits/c++config.h>\n#include <variant>\n"
                            "int main() { std::variant<int, char> v; v.emplace<int>(1); return std::get<int>(v) - 1; }\n";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> build{cxx};
    build.insert(build.end(), flags.begin(), flags.end());
    build.push_back(probe.string());
    build.push_back("-o");
    build.push_back((tmp / kProbeExe).string());
    if (!run_cmd(build)) return false;
    return run_cmd({(tmp / kProbeExe).string()});
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
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    fs::path out = tmp / (with_models ? "variant_trace.model" : "variant_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "variant_trace.cpp").string());
    argv.push_back("-o");
    argv.push_back(out.string());
    REQUIRE(run_cmd(argv));
    return out;
}

std::vector<std::string> run_lines(const fs::path& exe) {
    std::string stdout_text;
    REQUIRE(run_cmd({exe.string()}, &stdout_text));
    std::vector<std::string> lines;
    std::istringstream in(stdout_text);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
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
        for (const char* name : {"clang++-18", "clang++"}) {
            if (run_cmd({name, "--version"})) {
                c.cxx = name;
                break;
            }
        }
        if (!c.cxx.empty()) c.ok = probe_clang_asan(c.cxx, c.tmp);
    }
    if (!c.ok) MESSAGE("NOTRUN: clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx variant model: stored value is the emplaced alternative and inactive get is a check") {
    auto& c = ctx();
    if (!c.ok) return;
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
    for (const char* name : {"visit", "swap", "bad_variant_access", "hash", "in_place"}) {
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
    for (const char* op : {"operator<", "operator>", "operator<=", "operator>="}) {
        CAPTURE(op);
        CHECK(code.find(std::string(op) + "(const variant") == std::string::npos);
        CHECK(code.find(std::string(op) + "(variant") == std::string::npos);
    }
    CHECK(code.find("constexpr bool operator<(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator>(monostate, monostate) noexcept { return false; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator<=(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(code.find("constexpr bool operator>=(monostate, monostate) noexcept { return true; }") != std::string::npos);
    CHECK(code.find("#include <memory>") == std::string::npos);
    CHECK(code.find("#include <compare>") == std::string::npos);
}

TEST_CASE("cxx variant model: visit, swap, and variant comparison fail against the model") {
    auto& c = ctx();
    if (!c.ok) return;
    struct Snip {
        const char* name;
        const char* src;
    };
    const Snip snips[] = {
        {"rel",
         "#include <variant>\n"
         "int use(){ std::variant<int, char> a(1), b(2); return a < b ? 1 : 0; }\n"},
        {"cmp",
         "#include <variant>\n"
         "int use(){ std::variant<int, char> a(1), b(1); return (a <=> b) == 0 ? 1 : 0; }\n"},
        {"swap",
         "#include <variant>\n"
         "int use(){ std::variant<int, char> a(1), b(2); a.swap(b); return std::get<int>(a); }\n"},
        {"visit",
         "#include <variant>\n"
         "int use(){ std::variant<int, char> a(1);\n"
         "  return std::visit([](auto x){ return static_cast<int>(x); }, a); }\n"},
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
    CHECK(helper.find("_M_get") < helper.find("return false;"));

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

    const std::string ne_body = code.substr(ne, code.find("struct variant_size", ne) - ne);
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
    if (!c.ok) return;

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
    if (!c.ok) return;
    const fs::path src = c.tmp / "mono_order.cpp";
    std::ofstream(src) << "#include <variant>\n"
                          "int use(){ std::monostate a, b;\n"
                          "  return (a < b || a > b || !(a <= b) || !(a >= b)) ? 1 : 0; }\n";
    std::string err;
    CHECK(syntax_only(c.cxx, src, false, &err));
    CHECK(syntax_only(c.cxx, src, true, &err));
}
