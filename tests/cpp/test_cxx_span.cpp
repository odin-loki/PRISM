// The std::span model header (src/prism/pir/models/cxx/span) is a file on the
// include path, same shape as tests/cpp/test_cxx_list.cpp. CMake's prism_tests
// source list is explicit (not a glob): this file is one line in
// _prism_test_sources. The trace is tests/cxx_models/span_trace.cpp.

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

fs::path span_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "span"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct SpanCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

SpanCtx& ctx() {
    static SpanCtx c;
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
    std::ofstream(probe) << "#include <bits/c++config.h>\n#include <span>\n"
                            "int main() { int a[1] = {1}; std::span<int> s(a); return s[0] - 1; }\n";
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
    fs::path out = tmp / (with_models ? "span_trace.model" : "span_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "span_trace.cpp").string());
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

}  // namespace

TEST_CASE("cxx span model: non-owning view and empty front is a precondition") {
    const fs::path header = span_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE(src.find("class span") != std::string::npos);

    // The destructor does not release the viewed memory, and the header
    // never allocates. Visible without running pir.
    CHECK(src.find("~span() noexcept = default") != std::string::npos);
    CHECK(src.find("delete") == std::string::npos);
    CHECK(src.find("free(") == std::string::npos);
    CHECK(src.find("malloc") == std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <array>") == std::string::npos);
    CHECK(src.find("#include <deque>") == std::string::npos);

    const auto front = src.find("front() const noexcept");
    REQUIRE(front != std::string::npos);
    const auto front_body = src.substr(front, src.find("back()", front) - front);
    CHECK(front_body.find("__glibcxx_assert(!this->empty())") != std::string::npos);
    CHECK(front_body.find("return *_M_ptr;") != std::string::npos);
    CHECK(front_body.find("return _Tp") == std::string::npos);
    CHECK(front_body.find("return {}") == std::string::npos);

    const auto back = src.find("back() const noexcept");
    REQUIRE(back != std::string::npos);
    const auto back_body = src.substr(back, src.find("subspan", back) - back);
    CHECK(back_body.find("__glibcxx_assert(!this->empty())") != std::string::npos);

    const auto index = src.find("operator[](size_type __n) const noexcept");
    REQUIRE(index != std::string::npos);
    const auto index_body = src.substr(index, front - index);
    CHECK(index_body.find("__glibcxx_assert(__n < this->size())") != std::string::npos);
    CHECK(index_body.find("return *(_M_ptr + __n);") != std::string::npos);
    CHECK(index_body.find("return _Tp") == std::string::npos);
    CHECK(index_body.find("return {}") == std::string::npos);

    CHECK(src.find("__glibcxx_assert(__offset <= this->size() && __count <= this->size() - __offset)") !=
          std::string::npos);
    CHECK(src.find("return span(_M_ptr + __offset, __count);") != std::string::npos);
}

TEST_CASE("cxx span model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxspan-doctest";
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

TEST_CASE("cxx span model: valid index reads the element and the view does not own") {
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
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "span_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(span_header().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    REQUIRE(b.size() >= 13);
    CHECK(b[0] == "size=4");
    CHECK(b[1] == "empty=0");
    CHECK(b[2] == "at2=30");
    CHECK(b[6] == "kept=5");
    CHECK(b[7] == "last2=2 30 40 1");
    CHECK(b[8] == "last0=0 1");
    CHECK(b[9] == "none_last=0");
    CHECK(b[10] == "bytes=" + std::to_string(static_cast<unsigned long long>(4) * sizeof(int)));
    CHECK(b[11] == "none_bytes=0");
    CHECK(b[12] == "last0_bytes=0");
}

TEST_CASE("cxx span model: last past size is a check and a null nonzero size stays one") {
    const std::string src = slurp(span_header());
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("~span() noexcept = default") != std::string::npos);
    CHECK(src.find("size_bytes, and the template first/subspan") == std::string::npos);
    CHECK(src.find("the template first/subspan") != std::string::npos);

    const auto ctor = src.find("span(pointer __p, size_type __n) noexcept");
    REQUIRE(ctor != std::string::npos);
    const auto ctor_end = src.find("template <size_t _Nm>", ctor);
    REQUIRE(ctor_end != std::string::npos);
    const auto ctor_body = src.substr(ctor, ctor_end - ctor);
    CHECK(ctor_body.find("_M_len(__n)") != std::string::npos);
    CHECK(ctor_body.find("_M_len(0)") == std::string::npos);
    CHECK(ctor_body.find("__p == nullptr && __n != 0") != std::string::npos);
    CHECK(ctor_body.find("__prism_check(0, \"PTR-NULL-DEREF\",") != std::string::npos);

    const auto last = src.find("span last(size_type __count) const noexcept");
    REQUIRE(last != std::string::npos);
    const auto last_end = src.find("struct iterator", last);
    REQUIRE(last_end != std::string::npos);
    const auto body = src.substr(last, last_end - last);
    const auto check = body.find("__prism_check(__count <= this->size(), \"MEM-OOB-READ\",");
    REQUIRE(check != std::string::npos);
    const auto bad = body.find("if (__count > this->size())");
    REQUIRE(bad != std::string::npos);
    const auto bad_ret = body.find("return span(_M_ptr, 0);");
    REQUIRE(bad_ret != std::string::npos);
    const auto good = body.find("return span(_M_ptr + (this->size() - __count), __count);");
    REQUIRE(good != std::string::npos);
    CHECK(check < bad);
    CHECK(bad < bad_ret);
    CHECK(bad_ret < good);
    CHECK(body.find("return *") == std::string::npos);
    CHECK(body.find("operator[]") == std::string::npos);
    CHECK(src.find("__glibcxx_assert(__count <= this->size())") != std::string::npos);
    CHECK(src.find("return span(_M_ptr, __count);") != std::string::npos);
    CHECK(src.find("__glibcxx_assert(__offset <= this->size())") != std::string::npos);
    CHECK(src.find("__glibcxx_assert(__offset <= this->size() && __count <= this->size() - __offset)") !=
          std::string::npos);
}

TEST_CASE("cxx span model: last past size and a null nonzero span abort in the check") {
    auto& c = ctx();
    if (!c.ok) {
        MESSAGE("NOTRUN: clang++ with ASan/UBSan and libstdc++ not usable here");
        return;
    }
    const fs::path src = c.tmp / "span_last_fail.cpp";
    {
        std::ofstream out(src);
        out << "#include <span>\n#include <cstdio>\n#include <cstdlib>\n"
               "extern \"C\" void __prism_check(int ok, const char* cls, const char* msg) {\n"
               "  if (!ok) {\n"
               "    std::fprintf(stderr, \"CHECK %s: %s\\n\", cls, msg);\n"
               "    std::fflush(stderr);\n"
               "    std::abort();\n"
               "  }\n"
               "}\n"
               "int main(int argc, char** argv) {\n"
               "  int a[4] = {10, 20, 30, 40};\n"
               "  std::span<int> s(a);\n"
               "  if (argc > 1 && argv[1][0] == 'n') {\n"
               "    int* p = nullptr;\n"
               "    std::span<int> bad(p, 1);\n"
               "    std::printf(\"size=%zu\\n\", bad.size());\n"
               "    return 0;\n"
               "  }\n"
               "  if (argc > 1 && argv[1][0] == 'z') {\n"
               "    std::span<int> e = s.last(0);\n"
               "    std::printf(\"z=%zu\\n\", e.size());\n"
               "    return 0;\n"
               "  }\n"
               "  std::span<int> t = s.last(5);\n"
               "  std::printf(\"size=%zu\\n\", t.size());\n"
               "  return 0;\n"
               "}\n";
    }
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
#ifdef _WIN32
    const fs::path exe = c.tmp / "span_last_fail.exe";
#else
    const fs::path exe = c.tmp / "span_last_fail";
#endif
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> build{c.cxx};
    build.insert(build.end(), flags.begin(), flags.end());
    build.push_back("-isystem");
    build.push_back(models.string());
    build.push_back(src.string());
    build.push_back("-o");
    build.push_back(exe.string());
    REQUIRE(run_cmd(build));

    std::string err;
    CHECK_FALSE(run_cmd({exe.string()}, nullptr, &err));
    CHECK(err.find("CHECK MEM-OOB-READ") != std::string::npos);
    CHECK(err.find("std::span::last count exceeds size()") != std::string::npos);

    std::string nerr;
    CHECK_FALSE(run_cmd({exe.string(), "n"}, nullptr, &nerr));
    CHECK(nerr.find("CHECK PTR-NULL-DEREF") != std::string::npos);
    CHECK(nerr.find("null pointer with a non-zero size") != std::string::npos);

    std::string zout;
    REQUIRE(run_cmd({exe.string(), "z"}, &zout));
    CHECK(zout.find("z=0\n") != std::string::npos);
}

TEST_CASE("cxx span model: size_bytes is size times the element size") {
    const std::string src = slurp(span_header());
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("size_bytes, and the template first/subspan") == std::string::npos);
    CHECK(src.find("the template first/subspan") != std::string::npos);
    CHECK(src.find("~span() noexcept = default") != std::string::npos);

    const auto fn = src.find("size_bytes() const noexcept");
    REQUIRE(fn != std::string::npos);
    const auto fn_end = src.find("empty() const noexcept", fn);
    REQUIRE(fn_end != std::string::npos);
    const std::string body = src.substr(fn, fn_end - fn);

    const auto zero = body.find("sizeof(element_type) == 0");
    const auto zero_check =
        body.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::span::size_bytes element size is zero\")");
    const auto zero_trap = body.find("__builtin_trap()");
    const auto empty_if = body.find("size() == 0");
    const auto empty_ret = body.find("return 0;", empty_if == std::string::npos ? 0 : empty_if);
    const auto over = body.find("size() > (static_cast<size_type>(-1) / sizeof(element_type))");
    const auto over_check = body.find("__prism_check(0, \"MEM-OOB-READ\",");
    const auto over_msg =
        body.find("std::span::size_bytes size times element size does not fit in size_t");
    REQUIRE(zero_trap != std::string::npos);
    const auto over_trap = body.find("__builtin_trap()", zero_trap + std::string("__builtin_trap()").size());
    const auto prod = body.find("return size() * sizeof(element_type);");
    REQUIRE(zero != std::string::npos);
    REQUIRE(zero_check != std::string::npos);
    REQUIRE(empty_if != std::string::npos);
    REQUIRE(empty_ret != std::string::npos);
    REQUIRE(over != std::string::npos);
    REQUIRE(over_check != std::string::npos);
    REQUIRE(over_msg != std::string::npos);
    REQUIRE(over_trap != std::string::npos);
    REQUIRE(prod != std::string::npos);
    CHECK(zero < zero_check);
    CHECK(zero_check < zero_trap);
    CHECK(zero_trap < empty_if);
    CHECK(empty_if < empty_ret);
    CHECK(empty_ret < over);
    CHECK(over < over_check);
    CHECK(over_check < over_msg);
    CHECK(over_msg < over_trap);
    CHECK(over_trap < prod);
    CHECK(body.substr(zero_check, zero_trap - zero_check).find("return") == std::string::npos);
    CHECK(body.substr(over_check, over_trap - over_check).find("return") == std::string::npos);
    CHECK(body.find("return *") == std::string::npos);
    CHECK(body.find("*_M_ptr") == std::string::npos);
    CHECK(body.find("operator[]") == std::string::npos);
    CHECK(body.find("_M_ptr[") == std::string::npos);
}

TEST_CASE("cxx span model: size_bytes of an empty span is 0 and a product that does not fit traps") {
    auto& c = ctx();
    if (!c.ok) {
        MESSAGE("NOTRUN: clang++ with ASan/UBSan and libstdc++ not usable here");
        return;
    }
    const fs::path src = c.tmp / "span_size_bytes.cpp";
    {
        std::ofstream out(src);
        out << "#include <span>\n#include <cstddef>\n#include <cstdio>\n#include <cstdlib>\n"
               "extern \"C\" void __prism_check(int ok, const char* cls, const char* msg) {\n"
               "  if (!ok) {\n"
               "    std::fprintf(stderr, \"CHECK %s: %s\\n\", cls, msg);\n"
               "    std::fflush(stderr);\n"
               "    std::abort();\n"
               "  }\n"
               "}\n"
               "int main(int argc, char** argv) {\n"
               "  if (argc > 1 && argv[1][0] == 'e') {\n"
               "    int* p = nullptr;\n"
               "    std::span<int> s(p, 0);\n"
               "    std::printf(\"empty_bytes=%zu\\n\", s.size_bytes());\n"
               "    return 0;\n"
               "  }\n"
               "  if (argc > 1 && argv[1][0] == 'f') {\n"
               "    int one = 1;\n"
               "    const std::size_t n = static_cast<std::size_t>(-1) / sizeof(int);\n"
               "    std::span<int> s(&one, n);\n"
               "    std::printf(\"fit=%zu\\n\", s.size_bytes());\n"
               "    return 0;\n"
               "  }\n"
               "  if (argc > 1 && argv[1][0] == 'z') {\n"
               "    struct Z { int a[0]; };\n"
               "    static_assert(sizeof(Z) == 0);\n"
               "    Z z{};\n"
               "    std::span<Z> empty(&z, 0);\n"
               "    std::printf(\"zero_empty=%zu\\n\", empty.size_bytes());\n"
               "    return 0;\n"
               "  }\n"
               "  int one = 1;\n"
               "  const std::size_t n = static_cast<std::size_t>(-1) / sizeof(int) + 1;\n"
               "  std::span<int> s(&one, n);\n"
               "  std::printf(\"wrapped=%zu\\n\", s.size_bytes());\n"
               "  return 0;\n"
               "}\n";
    }
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
#ifdef _WIN32
    const fs::path exe = c.tmp / "span_size_bytes.exe";
#else
    const fs::path exe = c.tmp / "span_size_bytes";
#endif
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> build{c.cxx};
    build.insert(build.end(), flags.begin(), flags.end());
    build.push_back("-isystem");
    build.push_back(models.string());
    build.push_back(src.string());
    build.push_back("-o");
    build.push_back(exe.string());
    REQUIRE(run_cmd(build));

    std::string empty_out;
    REQUIRE(run_cmd({exe.string(), "e"}, &empty_out));
    CHECK(empty_out.find("empty_bytes=0\n") != std::string::npos);

    const auto nfit = static_cast<std::size_t>(-1) / sizeof(int);
    const auto fit_bytes = static_cast<unsigned long long>(nfit) * sizeof(int);
    std::string fit_out;
    REQUIRE(run_cmd({exe.string(), "f"}, &fit_out));
    CHECK(fit_out.find("fit=" + std::to_string(fit_bytes) + "\n") != std::string::npos);
    CHECK(fit_out.find("wrapped=") == std::string::npos);

    std::string out;
    std::string err;
    CHECK_FALSE(run_cmd({exe.string()}, &out, &err));
    CHECK(out.find("wrapped=") == std::string::npos);
    CHECK(err.find("CHECK MEM-OOB-READ") != std::string::npos);
    CHECK(err.find("std::span::size_bytes size times element size does not fit in size_t") != std::string::npos);

    std::string zero_out;
    std::string zero_err;
    CHECK_FALSE(run_cmd({exe.string(), "z"}, &zero_out, &zero_err));
    CHECK(zero_out.find("zero_empty=") == std::string::npos);
    CHECK(zero_err.find("CHECK FUNC-CONTRACT") != std::string::npos);
    CHECK(zero_err.find("std::span::size_bytes element size is zero") != std::string::npos);
}
