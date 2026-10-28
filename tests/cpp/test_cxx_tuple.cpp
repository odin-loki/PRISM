// Differential test of the std::tuple model header
// (src/prism/pir/models/cxx/tuple). Same shape as
// tests/cpp/test_cxx_initializer_list.cpp: the trace is compiled twice, once
// against libstdc++ and once with -isystem on the model directory so
// `#include <tuple>` is this header. CMake's prism_tests source list is
// explicit (not a glob).

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
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

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "tuple"; }

struct TupleCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

TupleCtx& ctx() {
    static TupleCtx c;
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
    std::ofstream(probe) << "#include <tuple>\n"
                            "int main() {\n"
                            "  std::tuple<int> t(42);\n"
                            "  return (std::get<0>(t) == 42 && std::tuple_size<std::tuple<>>::value == 0) ? 0 : 1;\n"
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
    fs::path out = tmp / (with_models ? "tuple_trace.model" : "tuple_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "tuple_trace.cpp").string());
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

bool has_line(const std::vector<std::string>& lines, const std::string& want) {
    for (const auto& line : lines)
        if (line == want) return true;
    return false;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A default return used as a stand-in for a missing element: `return {}`,
// `return T{}`, `return T()`. A return of a stored member is not that.
bool default_return_stand_in(const std::string& text) {
    static const std::regex re(
        R"(\breturn\s+(?:\{\s*\}|[A-Za-z_][A-Za-z0-9_]*\s*(?:\{\s*\}|\(\s*\))))");
    return std::regex_search(text, re);
}

bool syntax_ok(const std::string& cxx, const fs::path& src, bool with_models) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    std::vector<std::string> argv{cxx, "-std=c++23", "-fsyntax-only"};
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back(src.string());
    return run_cmd(argv);
}

}  // namespace

TEST_CASE("cxx tuple model: header is a file and has no default return for a missing element") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string text = slurp(header);
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("#ifndef _GLIBCXX_TUPLE") != std::string::npos);
    CHECK(text.find("#pragma GCC system_header") != std::string::npos);
    CHECK(text.find("namespace std") != std::string::npos);
    CHECK(text.find("#include <memory>") == std::string::npos);
    CHECK(text.find("#include <variant>") == std::string::npos);
    CHECK(text.find("#include <vector>") == std::string::npos);
    CHECK(text.find("#include <list>") == std::string::npos);
    CHECK(text.find("#include <deque>") == std::string::npos);
    CHECK(text.find("#include <map>") == std::string::npos);
    CHECK(text.find("#include <string>") == std::string::npos);
    CHECK_FALSE(default_return_stand_in(text));
    CHECK(text.find("return {}") == std::string::npos);
    CHECK(text.find("return T{}") == std::string::npos);
    CHECK(text.find("return T()") == std::string::npos);
}

TEST_CASE("cxx tuple model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxtuple-doctest";
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

TEST_CASE("cxx tuple model: get<0> is the stored int and empty tuple_size is 0") {
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
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "tuple_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(model_header().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    CHECK(has_line(b, "get0=42"));
    CHECK(has_line(b, "set0=5"));
    CHECK(has_line(b, "cget0=7"));
    CHECK(has_line(b, "rget0=42"));
    CHECK(has_line(b, "get0b=3"));
    CHECK(has_line(b, "get1=9"));
    CHECK(has_line(b, "bind=3,9"));
    CHECK(has_line(b, "empty=0"));
    CHECK(has_line(b, "size1=1"));
    CHECK(has_line(b, "size2=2"));
    CHECK(has_line(b, "zget=0"));

    const fs::path bad = c.tmp / "bad_index.cpp";
    std::ofstream(bad) << "#include <tuple>\n"
                          "int bad_index() {\n"
                          "  std::tuple<int> t(1);\n"
                          "  return std::get<1>(t);\n"
                          "}\n";
    CHECK_FALSE(syntax_ok(c.cxx, bad, true));

    const fs::path empty = c.tmp / "empty_get.cpp";
    std::ofstream(empty) << "#include <tuple>\n"
                            "int empty_get() {\n"
                            "  std::tuple<> t;\n"
                            "  return std::get<0>(t);\n"
                            "}\n";
    CHECK_FALSE(syntax_ok(c.cxx, empty, true));
}

TEST_CASE("cxx tuple model: get<2> of a 3-tuple is the third stored value") {
    const std::string text = slurp(model_header());
    CHECK(text.find("struct _Prism_tuple_get<2, tuple<_T0, _T1, _T2>>") != std::string::npos);
    CHECK(text.find("return __t._M_2;") != std::string::npos);
    CHECK(text.find("integral_constant<size_t, 3>") != std::string::npos);
    CHECK(text.find("struct tuple_element<2, tuple<_T0, _T1, _T2>>") != std::string::npos);
    CHECK(text.find("using type = _T2;") != std::string::npos);
    CHECK_FALSE(default_return_stand_in(text));
    CHECK(text.find("return {}") == std::string::npos);
    CHECK(text.find("return T{}") == std::string::npos);
    CHECK(text.find("return T()") == std::string::npos);

    auto& c = ctx();
    if (!c.ok) return;

    const fs::path src = c.tmp / "tuple3_get2.cpp";
    std::ofstream(src) << "#include <cstdio>\n"
                          "#include <tuple>\n"
                          "int main() {\n"
                          "  const int third = 17;\n"
                          "  std::tuple<int, int, int> t(4, 5, third);\n"
                          "  const int got = std::get<2>(t);\n"
                          "  std::printf(\"%d\\n\", got);\n"
                          "  return got == third ? 0 : 1;\n"
                          "}\n";
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    fs::path out = c.tmp / "tuple3_get2";
#ifdef _WIN32
    out = c.tmp / "tuple3_get2.exe";
#endif
    std::vector<std::string> argv{c.cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    argv.push_back("-isystem");
    argv.push_back(models.string());
    argv.push_back(src.string());
    argv.push_back("-o");
    argv.push_back(out.string());
    REQUIRE(run_cmd(argv));
    CHECK(has_line(run_lines(out), "17"));

    const fs::path bad = c.tmp / "tuple3_get3.cpp";
    std::ofstream(bad) << "#include <tuple>\n"
                          "int bad_index() {\n"
                          "  std::tuple<int, int, int> t(1, 2, 3);\n"
                          "  return std::get<3>(t);\n"
                          "}\n";
    CHECK_FALSE(syntax_ok(c.cxx, bad, true));

    const fs::path five = c.tmp / "tuple5.cpp";
    std::ofstream(five) << "#include <tuple>\n"
                           "int five() {\n"
                           "  std::tuple<int, int, int, int, int> t(1, 2, 3, 4, 5);\n"
                           "  return std::get<0>(t);\n"
                           "}\n";
    CHECK_FALSE(syntax_ok(c.cxx, five, true));
}

TEST_CASE("cxx tuple model: get<3> of a 4-tuple is the fourth stored value") {
    const std::string text = slurp(model_header());
    CHECK(text.find("struct _Prism_tuple_get<3, tuple<_T0, _T1, _T2, _T3>>") != std::string::npos);
    CHECK(text.find("return __t._M_3;") != std::string::npos);
    CHECK(text.find("integral_constant<size_t, 4>") != std::string::npos);
    CHECK(text.find("struct tuple_element<3, tuple<_T0, _T1, _T2, _T3>>") != std::string::npos);
    CHECK(text.find("using type = _T3;") != std::string::npos);
    CHECK_FALSE(default_return_stand_in(text));
    CHECK(text.find("return {}") == std::string::npos);
    CHECK(text.find("return T{}") == std::string::npos);
    CHECK(text.find("return T()") == std::string::npos);

    auto& c = ctx();
    if (!c.ok) return;

    const fs::path src = c.tmp / "tuple4_get3.cpp";
    std::ofstream(src) << "#include <cstdio>\n"
                          "#include <tuple>\n"
                          "int main() {\n"
                          "  const int fourth = 17;\n"
                          "  std::tuple<int, int, int, int> t(4, 5, 6, fourth);\n"
                          "  const int got = std::get<3>(t);\n"
                          "  std::printf(\"%d\\n\", got);\n"
                          "  return got == fourth ? 0 : 1;\n"
                          "}\n";
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    fs::path out = c.tmp / "tuple4_get3";
#ifdef _WIN32
    out = c.tmp / "tuple4_get3.exe";
#endif
    std::vector<std::string> argv{c.cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    argv.push_back("-isystem");
    argv.push_back(models.string());
    argv.push_back(src.string());
    argv.push_back("-o");
    argv.push_back(out.string());
    REQUIRE(run_cmd(argv));
    CHECK(has_line(run_lines(out), "17"));

    const fs::path bad = c.tmp / "tuple4_get4.cpp";
    std::ofstream(bad) << "#include <tuple>\n"
                          "int bad_index() {\n"
                          "  std::tuple<int, int, int, int> t(1, 2, 3, 4);\n"
                          "  return std::get<4>(t);\n"
                          "}\n";
    CHECK_FALSE(syntax_ok(c.cxx, bad, true));

    const fs::path five = c.tmp / "tuple5_omit.cpp";
    std::ofstream(five) << "#include <tuple>\n"
                           "int five() {\n"
                           "  std::tuple<int, int, int, int, int> t(1, 2, 3, 4, 5);\n"
                           "  return std::get<0>(t);\n"
                           "}\n";
    CHECK_FALSE(syntax_ok(c.cxx, five, true));
}
