// The <ranges> model is a partial stand-in: size, ssize, and empty. An array
// size is N. An lvalue with a const size() member returns that size().
// ssize converts that value to ptrdiff_t, and a value that does not fit is
// a FUNC-CONTRACT check then a trap, not a wrapped negative ptrdiff_t.
// empty of T(&)[N] is true only when N == 0, and that body does not load
// an element. N == 0 is a second overload because T(&)[0] is rejected.
// An lvalue with a const empty() member returns that empty().
// A pointer has no overload, so the call fails to compile.
//
// tests/cxx_models/ranges_trace.cpp is compiled twice when clang++ is
// usable: once against libstdc++ and once with this header first.

#include <doctest/doctest.h>

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

fs::path header_path() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "ranges"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string body_of(std::string_view src, std::string_view sig) {
    const auto at = src.find(sig);
    if (at == std::string_view::npos) return {};
    const auto brace = src.find('{', at);
    if (brace == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = brace; i < src.size(); ++i) {
        if (src[i] == '{')
            ++depth;
        else if (src[i] == '}') {
            if (--depth == 0) return std::string(src.substr(brace, i - brace + 1));
        }
    }
    return {};
}

std::size_t count_of(const std::string& s, const std::string& n) {
    std::size_t c = 0;
    std::size_t i = 0;
    while ((i = s.find(n, i)) != std::string::npos) {
        ++c;
        i += n.size();
    }
    return c;
}

struct RangesCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

RangesCtx& ctx() {
    static RangesCtx c;
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
    if (out) *out = r.out;
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
    std::ofstream(probe) << "#include <ranges>\n"
                            "int main() {\n"
                            "  int a[4] = {};\n"
                            "  return std::ranges::size(a) == 4 && std::ranges::ssize(a) == 4 ? 0 : 1;\n"
                            "}\n";
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

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    fs::path out = tmp / (with_models ? "ranges_trace.model" : "ranges_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "ranges_trace.cpp").string());
    argv.push_back("-o");
    argv.push_back(out.string());
    REQUIRE(run_cmd(argv));
    return out;
}

bool compile_snippet(const std::string& cxx, const fs::path& tmp, const std::string& name, const std::string& src,
                     bool with_models) {
    const fs::path in = tmp / (name + ".cpp");
    std::ofstream(in) << src;
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    std::vector<std::string> argv{cxx, "-std=c++23", "-c", "-Wall", "-Wextra", "-Wpedantic", "-Werror"};
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back(in.string());
    argv.push_back("-o");
    argv.push_back((tmp / (name + ".o")).string());
    return run_cmd(argv);
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

bool has_line(const std::vector<std::string>& lines, const std::string& want) {
    for (const auto& line : lines)
        if (line == want) return true;
    return false;
}

}  // namespace

TEST_CASE("cxx ranges model: size is N or a const size(), ssize traps when it does not fit") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("_GLIBCXX_RANGES") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("extern \"C\" void __prism_check(int, const char*, const char*);") != std::string::npos);
    CHECK(count_of(src, "extern \"C\" void __prism_check(int, const char*, const char*);") == 1);
    CHECK(src.find("__prism_check(int, const char*, const char*) {") == std::string::npos);
    CHECK(src.find("This header is not a proof that a range is sorted or that a pointer has a length.") !=
          std::string::npos);
    CHECK(src.find("constexpr std::size_t size(_Tp (&)[_Nm]) noexcept") != std::string::npos);
    CHECK(src.find("return _Nm;") != std::string::npos);
    CHECK(src.find("size(_Tp& __t)") != std::string::npos);
    CHECK(src.find("ssize(_Tp& __t)") != std::string::npos);
    CHECK(src.find("return static_cast<const _Tp&>(__t).size();") != std::string::npos);
    CHECK(src.find("return static_cast<std::ptrdiff_t>(__n);") != std::string::npos);
    CHECK(src.find("_Tp&&") == std::string::npos);
    CHECK(src.find("size(_Tp*") == std::string::npos);
    CHECK(src.find("ssize(_Tp*") == std::string::npos);
    CHECK(src.find("_Tp*") == std::string::npos);
    for (const char* inc : {"memory", "vector", "string", "iterator", "algorithm", "span"}) {
        CAPTURE(inc);
        CHECK(src.find(std::string("#include <") + inc + ">") == std::string::npos);
    }
    for (const char* name : {"begin(", "end(", "sort(", "find(", "view_interface", "namespace views"}) {
        CAPTURE(name);
        CHECK(src.find(name) == std::string::npos);
    }

    const auto trap = body_of(src, "void _Prism_ssize_does_not_fit()");
    REQUIRE_FALSE(trap.empty());
    const auto check = trap.find("__prism_check(0, \"FUNC-CONTRACT\", \"ranges::ssize size does not fit in ptrdiff_t\")");
    const auto builtin = trap.find("__builtin_trap()");
    REQUIRE(check != std::string::npos);
    REQUIRE(builtin != std::string::npos);
    CHECK(check < builtin);
    CHECK(trap.find("return") == std::string::npos);
    CHECK(count_of(src, "__prism_check(0, \"FUNC-CONTRACT\", \"ranges::ssize size does not fit in ptrdiff_t\")") == 1);

    const auto conv = body_of(src, "constexpr std::ptrdiff_t _Prism_as_ptrdiff(_Sz __n) noexcept");
    REQUIRE_FALSE(conv.empty());
    const auto call = conv.find("_Prism_ssize_does_not_fit()");
    const auto ret = conv.find("return static_cast<std::ptrdiff_t>(__n);");
    REQUIRE(call != std::string::npos);
    REQUIRE(ret != std::string::npos);
    CHECK(call < ret);
    CHECK(count_of(conv, "return ") == 1);
    CHECK(conv.find("return -") == std::string::npos);

    const auto arr_empty = body_of(src, "constexpr bool empty(_Tp (&)[_Nm]) noexcept");
    REQUIRE_FALSE(arr_empty.empty());
    CHECK(arr_empty.find("return _Nm == 0;") != std::string::npos);
    CHECK(arr_empty.find('*') == std::string::npos);
    CHECK(arr_empty.find('[') == std::string::npos);
    CHECK(arr_empty.find('.') == std::string::npos);
    CHECK(arr_empty.find("__t") == std::string::npos);

    const auto zero_empty = body_of(src, "constexpr bool empty(_Arr&) noexcept");
    REQUIRE_FALSE(zero_empty.empty());
    CHECK(zero_empty.find("return true;") != std::string::npos);
    CHECK(zero_empty.find('*') == std::string::npos);
    CHECK(zero_empty.find('[') == std::string::npos);
    CHECK(zero_empty.find('.') == std::string::npos);
    CHECK(zero_empty.find("__t") == std::string::npos);
    CHECK(count_of(src, "requires(sizeof(_Arr) == 0)") == 1);

    const auto mem_empty = body_of(src, "empty(_Tp& __t)");
    REQUIRE_FALSE(mem_empty.empty());
    CHECK(mem_empty.find("return static_cast<const _Tp&>(__t).empty();") != std::string::npos);
    CHECK(mem_empty.find('*') == std::string::npos);
    CHECK(mem_empty.find('[') == std::string::npos);
    CHECK(count_of(src, "constexpr bool empty(_Tp (&)[_Nm]) noexcept") == 1);
    CHECK(count_of(src, "constexpr bool empty(_Arr&) noexcept") == 1);
    CHECK(count_of(src, "empty(_Tp& __t)") == 1);
    CHECK(src.find("empty(_Tp*") == std::string::npos);
}

TEST_CASE("cxx ranges model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxranges-doctest";
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

TEST_CASE("cxx ranges model: array and member size match libstdc++, a pointer does not compile") {
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
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "ranges_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(header_path().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    CHECK(has_line(b, "arr 4"));
    CHECK(has_line(b, "sarr 4"));
    CHECK(has_line(b, "box 7"));
    CHECK(has_line(b, "sbox 7"));
    CHECK(has_line(b, "cbox 3"));
    CHECK(has_line(b, "scbox 3"));
    CHECK(has_line(b, "earr 0"));
    CHECK(has_line(b, "zempty 1"));
    CHECK(has_line(b, "cearr 0"));
    CHECK(has_line(b, "inner 0"));
    CHECK(has_line(b, "outer 1"));
    CHECK(has_line(b, "eflag 1"));
    CHECK(has_line(b, "enflag 0"));
    CHECK(has_line(b, "ecflag 1"));
    CHECK(has_line(b, "eprefer 1"));
    CHECK(has_line(b, "eptrs 0"));

    std::string out, err;
    CHECK_FALSE(run_cmd({mod.string(), "huge"}, &out, &err));
    const std::string both = out + err;
    CHECK(both.find("CHECK FUNC-CONTRACT:") != std::string::npos);
    CHECK(both.find("ranges::ssize size does not fit in ptrdiff_t") != std::string::npos);
    CHECK(out.find("wrapped=") == std::string::npos);

    const char* ok_src =
        "#include <ranges>\n"
        "struct Box { int n; constexpr int size() const { return n; } };\n"
        "struct Flag { bool e; constexpr bool empty() const { return e; } };\n"
        "struct Prefer {\n"
        "  constexpr bool empty() const { return true; }\n"
        "  constexpr int size() const { return 4; }\n"
        "};\n"
        "int main() {\n"
        "  int a[4] = {};\n"
        "  const int ca[4] = {};\n"
        "  Box b; b.n = 7;\n"
        "  const Box k{3};\n"
        "  Flag t; t.e = true;\n"
        "  Flag u; u.e = false;\n"
        "  const Flag cf{true};\n"
        "  Prefer pr;\n"
        "  if (std::ranges::size(a) != 4 || std::ranges::ssize(a) != 4) return 1;\n"
        "  if (std::ranges::size(b) != 7 || std::ranges::ssize(b) != 7) return 1;\n"
        "  if (std::ranges::size(k) != 3 || std::ranges::ssize(k) != 3) return 1;\n"
        "  if (std::ranges::empty(a) || std::ranges::empty(ca)) return 1;\n"
        "  if (!std::ranges::empty(t) || std::ranges::empty(u) || !std::ranges::empty(cf)) return 1;\n"
        "  if (!std::ranges::empty(pr) || std::ranges::size(pr) != 4) return 1;\n"
        "  return 0;\n"
        "}\n";
    CHECK(compile_snippet(c.cxx, c.tmp, "ranges_ok", ok_src, true));

    const char* ptr_src =
        "#include <ranges>\n"
        "#include <cstddef>\n"
        "std::size_t f(int* p) { return std::ranges::size(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_ptr", ptr_src, true));

    // A pointer to an array is not an array. size must not give it a length.
    const char* pbound_src =
        "#include <ranges>\n"
        "#include <cstddef>\n"
        "std::size_t f(int (*p)[4]) { return std::ranges::size(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_pbound", pbound_src, true));

    const char* rval_src =
        "#include <ranges>\n"
        "struct Box { int size() const { return 1; } };\n"
        "int f() { return static_cast<int>(std::ranges::size(Box{})); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_rval", rval_src, true));

    const char* mut_src =
        "#include <ranges>\n"
        "struct Mut { int size() { return 1; } };\n"
        "int f() { Mut m; return static_cast<int>(std::ranges::size(m)); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_mut", mut_src, true));

    const char* begin_src =
        "#include <ranges>\n"
        "int* f(int (&a)[4]) { return std::ranges::begin(a); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_begin", begin_src, true));

    const char* end_src =
        "#include <ranges>\n"
        "int* f(int (&a)[4]) { return std::ranges::end(a); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_end", end_src, true));

    const char* sort_src =
        "#include <ranges>\n"
        "void f(int* b, int* e) { std::ranges::sort(b, e); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_sort", sort_src, true));

    const char* find_src =
        "#include <ranges>\n"
        "int* f(int* b, int* e, int v) { return std::ranges::find(b, e, v); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_find", find_src, true));

    const char* empty_ptr_src =
        "#include <ranges>\n"
        "bool f(int* p) { return std::ranges::empty(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_empty_ptr", empty_ptr_src, true));

    const char* empty_pbound_src =
        "#include <ranges>\n"
        "bool f(int (*p)[4]) { return std::ranges::empty(p); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_empty_pbound", empty_pbound_src, true));

    const char* empty_rval_src =
        "#include <ranges>\n"
        "struct Flag { bool empty() const { return true; } };\n"
        "bool f() { return std::ranges::empty(Flag{}); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_empty_rval", empty_rval_src, true));

    const char* empty_mut_src =
        "#include <ranges>\n"
        "struct Mut { bool empty() { return true; } };\n"
        "bool f() { Mut m; return std::ranges::empty(m); }\n";
    CHECK_FALSE(compile_snippet(c.cxx, c.tmp, "ranges_empty_mut", empty_mut_src, true));
}
