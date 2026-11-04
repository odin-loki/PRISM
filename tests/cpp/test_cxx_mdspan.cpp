// The std::mdspan model (src/prism/pir/models/cxx/mdspan) is a non-owning
// view. Out-of-range operator() is a __prism_check. An in-range call reads
// the caller's element. The destructor does not free that array.
//
// The trace is compiled by this driver, not by CMake. Without clang++ and
// ASan/UBSan the run is NOTRUN. The header text is checked either way.

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

fs::path model_file() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "mdspan"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int count_of(const std::string& s, const std::string& n) {
    int c = 0;
    for (std::size_t p = 0; (p = s.find(n, p)) != std::string::npos; p += n.size()) ++c;
    return c;
}

struct Proc {
    int rc = -1;
    std::string out;
    std::string err;
    bool ran = false;
};

Proc run_proc(const std::vector<std::string>& argv) {
    Proc p;
#ifndef _WIN32
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    int pipe_out[2]{-1, -1};
    int pipe_err[2]{-1, -1};
    if (pipe(pipe_out) != 0 || pipe(pipe_err) != 0) return p;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipe_out[0]);
    posix_spawn_file_actions_adddup2(&actions, pipe_out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_out[1]);
    posix_spawn_file_actions_addclose(&actions, pipe_err[0]);
    posix_spawn_file_actions_adddup2(&actions, pipe_err[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_err[1]);
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0].c_str(), &actions, nullptr, args.data(), environ) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(pipe_out[0]);
        close(pipe_out[1]);
        close(pipe_err[0]);
        close(pipe_err[1]);
        return p;
    }
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_out[1]);
    close(pipe_err[1]);
    auto drain = [](int fd, std::string* dest) {
        char buf[8192];
        ssize_t n = 0;
        while ((n = ::read(fd, buf, sizeof buf)) > 0) dest->append(buf, static_cast<size_t>(n));
        close(fd);
    };
    drain(pipe_out[0], &p.out);
    drain(pipe_err[0], &p.err);
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return p;
    p.ran = true;
    if (WIFEXITED(st)) p.rc = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) p.rc = 128 + WTERMSIG(st);
#else
    prism::qa::RunOpts o;
    o.timeout_s = 120;
    auto r = prism::qa::run_tree(argv, o);
    p.ran = !r.start_failed && !r.timed_out;
    p.rc = r.rc;
    p.out = r.out;
    std::erase(p.out, '\r');  // text-mode stdout on Windows is CRLF
    p.err = r.err;
#endif
    return p;
}

bool run_ok(const std::vector<std::string>& argv) {
    const Proc p = run_proc(argv);
    return p.ran && p.rc == 0;
}

std::string fwd(std::string s) {
    for (auto& c : s)
        if (c == '\\') c = '/';
    return s;
}

#ifdef _WIN32
constexpr const char* kExe = "mdspan_trace.exe";
#else
constexpr const char* kExe = "mdspan_trace";
#endif

const char* kStub =
    "#include <cstdio>\n"
    "#include <cstdlib>\n"
    "extern \"C\" void __prism_check(int ok, const char* cls, const char* msg) {\n"
    "    if (!ok) {\n"
    "        std::fprintf(stderr, \"CHECK %s %s\\n\", cls ? cls : \"\", msg ? msg : \"\");\n"
    "        std::fflush(stderr);\n"
    "        std::abort();\n"
    "    }\n"
    "}\n";

struct MdCtx {
    fs::path tmp;
    std::string cxx;
    bool tried = false;
    bool ok = false;
};

MdCtx& ctx() {
    static MdCtx c;
    if (c.tried) return c;
    c.tried = true;
    c.tmp = fs::temp_directory_path() / "prism-cxxmdspan-doctest";
    fs::remove_all(c.tmp);
    fs::create_directories(c.tmp);
    std::ofstream(c.tmp / "check_stub.cpp") << kStub;
    for (const char* name : {"clang++-18", "clang++"}) {
        if (run_ok({name, "--version"})) {
            c.cxx = name;
            break;
        }
    }
    if (c.cxx.empty()) return c;
    // The model header has to compile, and ASan has to run, or the ownership
    // check below cannot tell a freed pointer from a live one.
    const std::vector<std::string> flags{"-std=c++23", "-O0", "-g", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-w", "-isystem",
                                         (repo() / "src" / "prism" / "pir" / "models" / "cxx").string()};
    auto probe = c.tmp / "probe.cpp";
    std::ofstream(probe) << "#include <mdspan>\n"
                            "#ifndef __GLIBCXX__\n#error the model builds on libstdc++ internals\n#endif\n"
                            "int main() {\n"
                            "  int a[1] = {7};\n"
                            "  std::mdspan<int, std::dextents<std::size_t, 1>> m(a, 1);\n"
                            "  return m(0) == 7 ? 0 : 1;\n"
                            "}\n";
    std::vector<std::string> build{c.cxx};
    build.insert(build.end(), flags.begin(), flags.end());
    build.push_back(probe.string());
    build.push_back((c.tmp / "check_stub.cpp").string());
    build.push_back("-o");
    build.push_back((c.tmp / "probe").string());
    if (!run_ok(build)) return c;
    c.ok = run_ok({(c.tmp / "probe").string()});
    return c;
}

const std::vector<std::string>& flags() {
    static const std::vector<std::string> f{"-std=c++23", "-O0", "-g", "-fsanitize=address,undefined",
                                            "-fno-sanitize-recover=all", "-w"};
    return f;
}

fs::path build_trace(const MdCtx& c) {
    fs::path out = c.tmp / kExe;
    std::vector<std::string> argv{c.cxx};
    argv.insert(argv.end(), flags().begin(), flags().end());
    argv.push_back("-isystem");
    argv.push_back((repo() / "src" / "prism" / "pir" / "models" / "cxx").string());
    argv.push_back((repo() / "tests" / "cxx_models" / "mdspan_trace.cpp").string());
    argv.push_back((c.tmp / "check_stub.cpp").string());
    argv.push_back("-o");
    argv.push_back(out.string());
    REQUIRE(run_ok(argv));
    return out;
}

}  // namespace

TEST_CASE("cxx mdspan model: out-of-range operator() is a check and the view does not own") {
    const fs::path path = model_file();
    REQUIRE(fs::is_regular_file(path));
    CHECK_FALSE(fs::is_directory(path));
    const std::string src = slurp(path);
    CHECK(src.find("#ifndef _GLIBCXX_MDSPAN") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std {") != std::string::npos);
    for (const char* bad : {"#include <span>", "#include <vector>", "#include <memory>"})
        CHECK(src.find(bad) == std::string::npos);
    CHECK(src.find("::operator new") == std::string::npos);
    CHECK(src.find("::operator delete") == std::string::npos);
    CHECK(src.find("delete[]") == std::string::npos);
    CHECK(src.find("submdspan(") == std::string::npos);
    CHECK(src.find("class mdarray") == std::string::npos);
    // The standard template mdspan<T, extents<I, E...>> derives from the rank
    // implementation __prism_mdspan<T, N>: ranks 1 through 11. Rank 12 (and
    // any other rank) stays the incomplete primary.
    CHECK(src.find("template <typename _Tp, typename _Extents") != std::string::npos);
    CHECK(src.find(": public __prism_mdspan<_Tp, sizeof...(_Exts)>") != std::string::npos);
    CHECK(src.find("class mdspan;") != std::string::npos);
    CHECK(src.find("class mdspan {") == std::string::npos);
    CHECK(src.find("class __prism_mdspan<_Tp, 1>") != std::string::npos);
    CHECK(src.find("class __prism_mdspan<_Tp, 2>") != std::string::npos);
    CHECK(src.find("class __prism_mdspan<_Tp, 3>") != std::string::npos);
    CHECK(src.find("class __prism_mdspan<_Tp, 4>") != std::string::npos);
    CHECK(src.find("class __prism_mdspan<_Tp, 5>") != std::string::npos);
    CHECK(src.find("class __prism_mdspan<_Tp, 6>") != std::string::npos);
    CHECK(src.find("class __prism_mdspan<_Tp, 7>") != std::string::npos);
    for (int r = 8; r <= 11; ++r)
        CHECK(src.find("class __prism_mdspan<_Tp, " + std::to_string(r) + ">") != std::string::npos);
    CHECK(src.find("class __prism_mdspan<_Tp, 12>") == std::string::npos);
    CHECK(src.find("class __prism_mdspan<_Tp, 0>") == std::string::npos);
    CHECK(count_of(src, "~__prism_mdspan() = default") == 11);
    // The class name in the header comment is unquoted. The checks are quoted.
    CHECK(count_of(src, "\"PTR-NULL-DEREF\"") == 11);
    CHECK(count_of(src, "\"FUNC-CONTRACT\"") == 11);
    CHECK(src.find("null data pointer with a non-zero extent") != std::string::npos);
    // Rank 2's product is still a check, and the stored size is that product
    // only when it fits. Ranks 3 through 11 store _M_size from __prod after
    // the fit check. None of them stores a wrapped multi-factor product.
    CHECK(src.find("const bool __fits = __e1 == 0 || __e0 <= static_cast<size_type>(-1) / __e1;") !=
          std::string::npos);
    CHECK(src.find("if (__fits) _M_size = __e0 * __e1;") != std::string::npos);
    CHECK(src.find("const bool __zero = __e0 == 0 || __e1 == 0 || __e2 == 0;") != std::string::npos);
    CHECK(src.find("const bool __zero = __e0 == 0 || __e1 == 0 || __e2 == 0 || __e3 == 0;") != std::string::npos);
    CHECK(src.find("const bool __zero = __e0 == 0 || __e1 == 0 || __e2 == 0 || __e3 == 0 || __e4 == 0;") !=
          std::string::npos);
    CHECK(src.find("const bool __zero = __e0 == 0 || __e1 == 0 || __e2 == 0 || __e3 == 0 || __e4 == 0 || __e5 == 0;") !=
          std::string::npos);
    CHECK(src.find("const bool __zero = __e0 == 0 || __e1 == 0 || __e2 == 0 || __e3 == 0 || __e4 == 0 || __e5 == 0 || "
                   "__e6 == 0;") != std::string::npos);
    CHECK(src.find("__e7 == 0;") != std::string::npos);
    CHECK(src.find("__e7 == 0 || __e8 == 0;") != std::string::npos);
    CHECK(src.find("__e7 == 0 || __e8 == 0 || __e9 == 0;") != std::string::npos);
    CHECK(src.find("__e7 == 0 || __e8 == 0 || __e9 == 0 || __e10 == 0;") != std::string::npos);
    CHECK(src.find("__prod = __e0123456789 * __e10;") != std::string::npos);
    CHECK(count_of(src, "if (__fits) _M_size = __prod;") == 9);
    CHECK(src.find("_M_size = __e0 * __e1 * __e2") == std::string::npos);
    CHECK(src.find("_M_size = __e0 * __e1 * __e2 * __e3") == std::string::npos);
    CHECK(src.find("_M_size = __e0 * __e1 * __e2 * __e3 * __e4") == std::string::npos);
    CHECK(src.find("_M_size = __e0 * __e1 * __e2 * __e3 * __e4 * __e5") == std::string::npos);
    CHECK(src.find("_M_size = __e0 * __e1 * __e2 * __e3 * __e4 * __e5 * __e6") == std::string::npos);
    CHECK(src.find("_M_size = __e0 * __e1 * __e2 * __e3 * __e4 * __e5 * __e6 * __e7") == std::string::npos);
    CHECK(count_of(src, "std::mdspan extents product overflows") == 10);
    CHECK(src.find("__i < _M_e0 && __j < _M_e1,") != std::string::npos);
    CHECK(src.find("__i < _M_e0 && __j < _M_e1 && __k < _M_e2,") != std::string::npos);
    CHECK(src.find("__i < _M_e0 && __j < _M_e1 && __k < _M_e2 && __l < _M_e3,") != std::string::npos);
    CHECK(src.find("__i < _M_e0 && __j < _M_e1 && __k < _M_e2 && __l < _M_e3 && __m < _M_e4,") != std::string::npos);
    CHECK(src.find("__i < _M_e0 && __j < _M_e1 && __k < _M_e2 && __l < _M_e3 && __m < _M_e4 && __n < _M_e5,") !=
          std::string::npos);
    CHECK(src.find("__i < _M_e0 && __j < _M_e1 && __k < _M_e2 && __l < _M_e3 && __m < _M_e4 && __n < _M_e5 &&") !=
          std::string::npos);
    CHECK(src.find("__p < _M_e6,") != std::string::npos);
    CHECK(src.find("__p < _M_e6 && __q < _M_e7,") != std::string::npos);
    CHECK(src.find("__p < _M_e6 && __q < _M_e7 && __r < _M_e8,") != std::string::npos);
    CHECK(src.find("__p < _M_e6 && __q < _M_e7 && __r < _M_e8 && __s < _M_e9,") != std::string::npos);
    CHECK(src.find("__p < _M_e6 && __q < _M_e7 && __r < _M_e8 && __s < _M_e9 && __t < _M_e10,") !=
          std::string::npos);
    CHECK(src.find("mdspan(_Tp*, std::size_t, std::size_t, std::size_t) -> mdspan<_Tp, dextents<std::size_t, 3>>;") != std::string::npos);
    CHECK(src.find("mdspan(_Tp*, std::size_t, std::size_t, std::size_t, std::size_t) -> mdspan<_Tp, dextents<std::size_t, 4>>;") !=
          std::string::npos);
    CHECK(src.find("mdspan(_Tp*, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t) -> mdspan<_Tp, dextents<std::size_t, 5>>;") !=
          std::string::npos);
    CHECK(src.find("mdspan(_Tp*, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t) -> "
                   "mdspan<_Tp, dextents<std::size_t, 6>>;") != std::string::npos);
    CHECK(src.find("mdspan(_Tp*, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, "
                   "std::size_t) -> mdspan<_Tp, dextents<std::size_t, 7>>;") != std::string::npos);
    CHECK(src.find("mdspan(_Tp*, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t) -> mdspan<_Tp, dextents<std::size_t, 8>>;") != std::string::npos);
    CHECK(src.find("mdspan(_Tp*, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t) -> mdspan<_Tp, dextents<std::size_t, 9>>;") != std::string::npos);
    CHECK(src.find("mdspan(_Tp*, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t) -> mdspan<_Tp, dextents<std::size_t, 10>>;") != std::string::npos);
    CHECK(src.find("mdspan(_Tp*, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t, std::size_t) -> mdspan<_Tp, dextents<std::size_t, 11>>;") != std::string::npos);
    CHECK(src.find("dextents<std::size_t, 12>") == std::string::npos);

    std::size_t from = 0;
    for (int n = 0; n < 11; ++n) {
        const auto op = src.find("reference operator()", from);
        REQUIRE(op != std::string::npos);
        const auto ret = src.find("return _M_ptr[", op);
        REQUIRE(ret != std::string::npos);
        const auto chk = src.rfind("__prism_check", ret);
        REQUIRE(chk != std::string::npos);
        CHECK(chk > op);
        const std::string body = src.substr(op, ret - op);
        CHECK(body.find("MEM-OOB-READ") != std::string::npos);
        CHECK(body.find("outside the extent") != std::string::npos);
        CHECK(body.find("return _Tp") == std::string::npos);
        CHECK(body.find("return {}") == std::string::npos);
        from = ret + 1;
    }
    CHECK(src.find("reference operator()", from) == std::string::npos);
    // Rank 1 reports the stored extent, not a padded stand-in.
    CHECK(src.find("return _M_ext;") != std::string::npos);
    CHECK(src.find("return _M_e0;") != std::string::npos);
    CHECK(src.find("return _M_e2;") != std::string::npos);
    CHECK(src.find("return _M_e3;") != std::string::npos);
    CHECK(src.find("return _M_e4;") != std::string::npos);
    CHECK(src.find("return _M_e5;") != std::string::npos);
    CHECK(src.find("return _M_e6;") != std::string::npos);
    CHECK(src.find("return _M_e7;") != std::string::npos);
    CHECK(src.find("return _M_e8;") != std::string::npos);
    CHECK(src.find("return _M_e9;") != std::string::npos);
    CHECK(src.find("return _M_e10;") != std::string::npos);
    for (int r = 8; r <= 11; ++r)
        CHECK(src.find("__r < " + std::to_string(r) + ", \"FUNC-CONTRACT\"") != std::string::npos);
    CHECK(src.find("__i * _M_e1 * _M_e2 * _M_e3 + __j * _M_e2 * _M_e3 + __k * _M_e3 + __l") != std::string::npos);
    CHECK(src.find("__i * _M_e1 * _M_e2 * _M_e3 * _M_e4 + __j * _M_e2 * _M_e3 * _M_e4 + __k * _M_e3 * _M_e4 +") !=
          std::string::npos);
    CHECK(src.find("__l * _M_e4 + __m") != std::string::npos);
    CHECK(src.find("__i * _M_e1 * _M_e2 * _M_e3 * _M_e4 * _M_e5 + __j * _M_e2 * _M_e3 * _M_e4 * _M_e5 +") !=
          std::string::npos);
    CHECK(src.find("__k * _M_e3 * _M_e4 * _M_e5 + __l * _M_e4 * _M_e5 + __m * _M_e5 + __n") != std::string::npos);
    CHECK(src.find("__i * _M_e1 * _M_e2 * _M_e3 * _M_e4 * _M_e5 * _M_e6 +") != std::string::npos);
    CHECK(src.find("__j * _M_e2 * _M_e3 * _M_e4 * _M_e5 * _M_e6 +") != std::string::npos);
    CHECK(src.find("__k * _M_e3 * _M_e4 * _M_e5 * _M_e6 + __l * _M_e4 * _M_e5 * _M_e6 +") != std::string::npos);
    CHECK(src.find("__m * _M_e5 * _M_e6 + __n * _M_e6 + __p") != std::string::npos);
    // Rank 8's last terms and rank 11's first and last terms: every stride is
    // the product of the later extents.
    CHECK(src.find("__p * _M_e7 + __q];") != std::string::npos);
    CHECK(src.find("__i * _M_e1 * _M_e2 * _M_e3 * _M_e4 * _M_e5 * _M_e6 * _M_e7 * _M_e8 * _M_e9 * _M_e10 +") !=
          std::string::npos);
    CHECK(src.find("__r * _M_e9 * _M_e10 + __s * _M_e10 + __t];") != std::string::npos);
}

TEST_CASE("cxx mdspan model: in-range call reads the real element and the view does not own") {
    auto& c = ctx();
    if (!c.ok) {
        MESSAGE("NOTRUN: clang++ with ASan/UBSan and libstdc++ not usable here");
        return;
    }
    std::vector<std::string> deps{c.cxx, "-std=c++23", "-M", "-isystem",
                                 (repo() / "src" / "prism" / "pir" / "models" / "cxx").string(),
                                 (repo() / "tests" / "cxx_models" / "mdspan_trace.cpp").string()};
    const Proc listed = run_proc(deps);
    REQUIRE(listed.ran);
    REQUIRE(listed.rc == 0);
    CHECK(fwd(listed.out).find(fwd(model_file().string())) != std::string::npos);

    const fs::path exe = build_trace(c);
    const Proc ok = run_proc({exe.string()});
    REQUIRE(ok.ran);
    REQUIRE(ok.rc == 0);
    // operator()(0) wrote 11 through the view into the caller's array.
    CHECK(ok.out.find("in 11 20 30 40\n") != std::string::npos);
    CHECK(ok.out.find("size 4 4\n") != std::string::npos);
    CHECK(ok.out.find("write 11\n") != std::string::npos);
    CHECK(ok.out.find("alias 1\n") != std::string::npos);
    // The stack array is still live after the view dies, and the heap
    // block is still the one delete[] frees (no double-free under ASan).
    CHECK(ok.out.find("inside 8\n") != std::string::npos);
    CHECK(ok.out.find("after 8\n") != std::string::npos);
    CHECK(ok.out.find("heap 9\n") != std::string::npos);
    CHECK(ok.out.find("kept 9\n") != std::string::npos);
    CHECK(ok.out.find("empty 0 0 1\n") != std::string::npos);
    // Rank 2: extent(0) is 2, extent(1) is 3, (1, 2) is the last real element.
    CHECK(ok.out.find("r2 6 2 3 1 6\n") != std::string::npos);
    // Rank 3: 2*3*4 = 24, (1, 2, 3) is the last real element. A zero extent
    // keeps size() at 0 even when the other two would overflow. All-zero
    // extents may be a null pointer.
    CHECK(ok.out.find("r3 24 2 3 4 1 24\n") != std::string::npos);
    CHECK(ok.out.find("z3 0\n") != std::string::npos);
    CHECK(ok.out.find("e3 0 1\n") != std::string::npos);
    // Rank 4: 2*3*4*5 = 120, (1, 2, 3, 4) is the last real element. A zero
    // extent keeps size() at 0 even when the other three would overflow.
    // All-zero extents may be a null pointer.
    CHECK(ok.out.find("r4 120 2 3 4 5 1 120\n") != std::string::npos);
    CHECK(ok.out.find("z4 0\n") != std::string::npos);
    CHECK(ok.out.find("e4 0 1\n") != std::string::npos);
    // Rank 5: 2*3*4*5*6 = 720, (1, 2, 3, 4, 5) is the last real element. A
    // zero extent keeps size() at 0 even when the other four would overflow.
    // All-zero extents may be a null pointer.
    CHECK(ok.out.find("r5 720 2 3 4 5 6 1 720\n") != std::string::npos);
    CHECK(ok.out.find("z5 0\n") != std::string::npos);
    CHECK(ok.out.find("e5 0 1\n") != std::string::npos);
    // Rank 6: 2*3*4*5*6*7 = 5040, (1, 2, 3, 4, 5, 6) is the last real
    // element. A zero extent keeps size() at 0 even when the other five
    // would overflow. All-zero extents may be a null pointer.
    CHECK(ok.out.find("r6 5040 2 3 4 5 6 7 1 5040\n") != std::string::npos);
    CHECK(ok.out.find("z6 0\n") != std::string::npos);
    CHECK(ok.out.find("e6 0 1\n") != std::string::npos);
    // Rank 7: 2*3*4*5*6*7*8 = 40320, (1, 2, 3, 4, 5, 6, 7) is the last real
    // element. A zero extent keeps size() at 0 even when the other six
    // would overflow. All-zero extents may be a null pointer.
    CHECK(ok.out.find("r7 40320 2 3 4 5 6 7 8 1 40320\n") != std::string::npos);
    CHECK(ok.out.find("z7 0\n") != std::string::npos);
    CHECK(ok.out.find("e7 0 1\n") != std::string::npos);
    // Rank 8: extents (2, 3, 2, 3, 2, 3, 2, 3), size() 1296, the first, last and
    // (1, 0, 1, 0, 1, 0, 1, 0) elements. A zero extent keeps size() at 0 even when
    // the other seven would overflow. All-zero extents may be a null pointer. The
    // deduction guide gives dextents<size_t, 8>, and operator[] is operator().
    CHECK(ok.out.find("r8 8 1296 2 3 2 3 2 3 2 3 1 1296 778\n") != std::string::npos);
    CHECK(ok.out.find("z8 0\n") != std::string::npos);
    CHECK(ok.out.find("e8 0 1\n") != std::string::npos);
    CHECK(ok.out.find("d8 1 778 1\n") != std::string::npos);
    // Rank 9: extents (2, 3, 2, 3, 2, 3, 2, 3, 2), size() 2592, the first, last and
    // (1, 0, 1, 0, 1, 0, 1, 0, 1) elements. A zero extent keeps size() at 0 even when
    // the other eight would overflow. All-zero extents may be a null pointer. The
    // deduction guide gives dextents<size_t, 9>, and operator[] is operator().
    CHECK(ok.out.find("r9 9 2592 2 3 2 3 2 3 2 3 2 1 2592 1556\n") != std::string::npos);
    CHECK(ok.out.find("z9 0\n") != std::string::npos);
    CHECK(ok.out.find("e9 0 1\n") != std::string::npos);
    CHECK(ok.out.find("d9 1 1556 1\n") != std::string::npos);
    // Rank 10: extents (2, 3, 2, 3, 2, 3, 2, 3, 2, 3), size() 7776, the first, last and
    // (1, 0, 1, 0, 1, 0, 1, 0, 1, 0) elements. A zero extent keeps size() at 0 even when
    // the other nine would overflow. All-zero extents may be a null pointer. The
    // deduction guide gives dextents<size_t, 10>, and operator[] is operator().
    CHECK(ok.out.find("r10 10 7776 2 3 2 3 2 3 2 3 2 3 1 7776 4666\n") != std::string::npos);
    CHECK(ok.out.find("z10 0\n") != std::string::npos);
    CHECK(ok.out.find("e10 0 1\n") != std::string::npos);
    CHECK(ok.out.find("d10 1 4666 1\n") != std::string::npos);
    // Rank 11: extents (2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2), size() 15552, the first, last and
    // (1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1) elements. A zero extent keeps size() at 0 even when
    // the other ten would overflow. All-zero extents may be a null pointer. The
    // deduction guide gives dextents<size_t, 11>, and operator[] is operator().
    CHECK(ok.out.find("r11 11 15552 2 3 2 3 2 3 2 3 2 3 2 1 15552 9332\n") != std::string::npos);
    CHECK(ok.out.find("z11 0\n") != std::string::npos);
    CHECK(ok.out.find("e11 0 1\n") != std::string::npos);
    CHECK(ok.out.find("d11 1 9332 1\n") != std::string::npos);

    const Proc oob = run_proc({exe.string(), "oob"});
    REQUIRE(oob.ran);
    CHECK(oob.rc != 0);
    CHECK(oob.err.find("CHECK MEM-OOB-READ") != std::string::npos);
    CHECK(oob.err.find("outside the extent") != std::string::npos);

    const Proc oob2 = run_proc({exe.string(), "oob2"});
    REQUIRE(oob2.ran);
    CHECK(oob2.rc != 0);
    CHECK(oob2.err.find("CHECK MEM-OOB-READ") != std::string::npos);
    CHECK(oob2.err.find("outside the extents") != std::string::npos);

    const Proc nullp = run_proc({exe.string(), "null"});
    REQUIRE(nullp.ran);
    CHECK(nullp.rc != 0);
    CHECK(nullp.err.find("CHECK PTR-NULL-DEREF") != std::string::npos);
    CHECK(nullp.err.find("non-zero extent") != std::string::npos);

    for (const char* mode : {"oob3", "oob3j", "oob3k"}) {
        const Proc past = run_proc({exe.string(), mode});
        REQUIRE(past.ran);
        CHECK(past.rc != 0);
        CHECK(past.err.find("CHECK MEM-OOB-READ") != std::string::npos);
        CHECK(past.err.find("outside the extents") != std::string::npos);
    }

    const Proc null3 = run_proc({exe.string(), "null3"});
    REQUIRE(null3.ran);
    CHECK(null3.rc != 0);
    CHECK(null3.err.find("CHECK PTR-NULL-DEREF") != std::string::npos);
    CHECK(null3.err.find("non-zero extent") != std::string::npos);

    const Proc ovf3 = run_proc({exe.string(), "ovf3"});
    REQUIRE(ovf3.ran);
    CHECK(ovf3.rc != 0);
    CHECK(ovf3.err.find("CHECK MEM-OOB-READ") != std::string::npos);
    CHECK(ovf3.err.find("extents product overflows") != std::string::npos);

    // Rank 2's overflowing product is still a check. size() is not returned.
    const Proc ovf2 = run_proc({exe.string(), "ovf2"});
    REQUIRE(ovf2.ran);
    CHECK(ovf2.rc != 0);
    CHECK(ovf2.err.find("CHECK MEM-OOB-READ") != std::string::npos);
    CHECK(ovf2.err.find("extents product overflows") != std::string::npos);

    for (const char* mode : {"oob4", "oob4j", "oob4k", "oob4l"}) {
        const Proc past = run_proc({exe.string(), mode});
        REQUIRE(past.ran);
        CHECK(past.rc != 0);
        CHECK(past.err.find("CHECK MEM-OOB-READ") != std::string::npos);
        CHECK(past.err.find("outside the extents") != std::string::npos);
    }

    const Proc null4 = run_proc({exe.string(), "null4"});
    REQUIRE(null4.ran);
    CHECK(null4.rc != 0);
    CHECK(null4.err.find("CHECK PTR-NULL-DEREF") != std::string::npos);
    CHECK(null4.err.find("non-zero extent") != std::string::npos);

    const Proc ovf4 = run_proc({exe.string(), "ovf4"});
    REQUIRE(ovf4.ran);
    CHECK(ovf4.rc != 0);
    CHECK(ovf4.err.find("CHECK MEM-OOB-READ") != std::string::npos);
    CHECK(ovf4.err.find("extents product overflows") != std::string::npos);

    for (const char* mode : {"oob5", "oob5j", "oob5k", "oob5l", "oob5m"}) {
        const Proc past = run_proc({exe.string(), mode});
        REQUIRE(past.ran);
        CHECK(past.rc != 0);
        CHECK(past.err.find("CHECK MEM-OOB-READ") != std::string::npos);
        CHECK(past.err.find("outside the extents") != std::string::npos);
    }

    const Proc null5 = run_proc({exe.string(), "null5"});
    REQUIRE(null5.ran);
    CHECK(null5.rc != 0);
    CHECK(null5.err.find("CHECK PTR-NULL-DEREF") != std::string::npos);
    CHECK(null5.err.find("non-zero extent") != std::string::npos);

    const Proc ovf5 = run_proc({exe.string(), "ovf5"});
    REQUIRE(ovf5.ran);
    CHECK(ovf5.rc != 0);
    CHECK(ovf5.err.find("CHECK MEM-OOB-READ") != std::string::npos);
    CHECK(ovf5.err.find("extents product overflows") != std::string::npos);

    for (const char* mode : {"oob6", "oob6j", "oob6k", "oob6l", "oob6m", "oob6n"}) {
        const Proc past = run_proc({exe.string(), mode});
        REQUIRE(past.ran);
        CHECK(past.rc != 0);
        CHECK(past.err.find("CHECK MEM-OOB-READ") != std::string::npos);
        CHECK(past.err.find("outside the extents") != std::string::npos);
    }

    const Proc null6 = run_proc({exe.string(), "null6"});
    REQUIRE(null6.ran);
    CHECK(null6.rc != 0);
    CHECK(null6.err.find("CHECK PTR-NULL-DEREF") != std::string::npos);
    CHECK(null6.err.find("non-zero extent") != std::string::npos);

    const Proc ovf6 = run_proc({exe.string(), "ovf6"});
    REQUIRE(ovf6.ran);
    CHECK(ovf6.rc != 0);
    CHECK(ovf6.err.find("CHECK MEM-OOB-READ") != std::string::npos);
    CHECK(ovf6.err.find("extents product overflows") != std::string::npos);

    for (const char* mode : {"oob7", "oob7j", "oob7k", "oob7l", "oob7m", "oob7n", "oob7p"}) {
        const Proc past = run_proc({exe.string(), mode});
        REQUIRE(past.ran);
        CHECK(past.rc != 0);
        CHECK(past.err.find("CHECK MEM-OOB-READ") != std::string::npos);
        CHECK(past.err.find("outside the extents") != std::string::npos);
    }

    const Proc null7 = run_proc({exe.string(), "null7"});
    REQUIRE(null7.ran);
    CHECK(null7.rc != 0);
    CHECK(null7.err.find("CHECK PTR-NULL-DEREF") != std::string::npos);
    CHECK(null7.err.find("non-zero extent") != std::string::npos);

    const Proc ovf7 = run_proc({exe.string(), "ovf7"});
    REQUIRE(ovf7.ran);
    CHECK(ovf7.rc != 0);
    CHECK(ovf7.err.find("CHECK MEM-OOB-READ") != std::string::npos);
    CHECK(ovf7.err.find("extents product overflows") != std::string::npos);

    // Each of the 8 indices one past its extent, and the last one through
    // operator[]: MEM-OOB-READ before an address is formed.
    for (const char* mode : {"oob8", "oob8j", "oob8k", "oob8l", "oob8m", "oob8n", "oob8p", "oob8q", "oob8b"}) {
        const Proc past = run_proc({exe.string(), mode});
        REQUIRE(past.ran);
        CHECK(past.rc != 0);
        CHECK(past.err.find("CHECK MEM-OOB-READ") != std::string::npos);
        CHECK(past.err.find("outside the extents") != std::string::npos);
    }

    const Proc null8 = run_proc({exe.string(), "null8"});
    REQUIRE(null8.ran);
    CHECK(null8.rc != 0);
    CHECK(null8.err.find("CHECK PTR-NULL-DEREF") != std::string::npos);
    CHECK(null8.err.find("non-zero extent") != std::string::npos);

    const Proc ovf8 = run_proc({exe.string(), "ovf8"});
    REQUIRE(ovf8.ran);
    CHECK(ovf8.rc != 0);
    CHECK(ovf8.err.find("CHECK MEM-OOB-READ") != std::string::npos);
    CHECK(ovf8.err.find("extents product overflows") != std::string::npos);

    // Each of the 9 indices one past its extent, and the last one through
    // operator[]: MEM-OOB-READ before an address is formed.
    for (const char* mode : {"oob9", "oob9j", "oob9k", "oob9l", "oob9m", "oob9n", "oob9p", "oob9q", "oob9r", "oob9b"}) {
        const Proc past = run_proc({exe.string(), mode});
        REQUIRE(past.ran);
        CHECK(past.rc != 0);
        CHECK(past.err.find("CHECK MEM-OOB-READ") != std::string::npos);
        CHECK(past.err.find("outside the extents") != std::string::npos);
    }

    const Proc null9 = run_proc({exe.string(), "null9"});
    REQUIRE(null9.ran);
    CHECK(null9.rc != 0);
    CHECK(null9.err.find("CHECK PTR-NULL-DEREF") != std::string::npos);
    CHECK(null9.err.find("non-zero extent") != std::string::npos);

    const Proc ovf9 = run_proc({exe.string(), "ovf9"});
    REQUIRE(ovf9.ran);
    CHECK(ovf9.rc != 0);
    CHECK(ovf9.err.find("CHECK MEM-OOB-READ") != std::string::npos);
    CHECK(ovf9.err.find("extents product overflows") != std::string::npos);

    // Each of the 10 indices one past its extent, and the last one through
    // operator[]: MEM-OOB-READ before an address is formed.
    for (const char* mode : {"oob10", "oob10j", "oob10k", "oob10l", "oob10m", "oob10n", "oob10p", "oob10q", "oob10r", "oob10s", "oob10b"}) {
        const Proc past = run_proc({exe.string(), mode});
        REQUIRE(past.ran);
        CHECK(past.rc != 0);
        CHECK(past.err.find("CHECK MEM-OOB-READ") != std::string::npos);
        CHECK(past.err.find("outside the extents") != std::string::npos);
    }

    const Proc null10 = run_proc({exe.string(), "null10"});
    REQUIRE(null10.ran);
    CHECK(null10.rc != 0);
    CHECK(null10.err.find("CHECK PTR-NULL-DEREF") != std::string::npos);
    CHECK(null10.err.find("non-zero extent") != std::string::npos);

    const Proc ovf10 = run_proc({exe.string(), "ovf10"});
    REQUIRE(ovf10.ran);
    CHECK(ovf10.rc != 0);
    CHECK(ovf10.err.find("CHECK MEM-OOB-READ") != std::string::npos);
    CHECK(ovf10.err.find("extents product overflows") != std::string::npos);

    // Each of the 11 indices one past its extent, and the last one through
    // operator[]: MEM-OOB-READ before an address is formed.
    for (const char* mode : {"oob11", "oob11j", "oob11k", "oob11l", "oob11m", "oob11n", "oob11p", "oob11q", "oob11r", "oob11s", "oob11t", "oob11b"}) {
        const Proc past = run_proc({exe.string(), mode});
        REQUIRE(past.ran);
        CHECK(past.rc != 0);
        CHECK(past.err.find("CHECK MEM-OOB-READ") != std::string::npos);
        CHECK(past.err.find("outside the extents") != std::string::npos);
    }

    const Proc null11 = run_proc({exe.string(), "null11"});
    REQUIRE(null11.ran);
    CHECK(null11.rc != 0);
    CHECK(null11.err.find("CHECK PTR-NULL-DEREF") != std::string::npos);
    CHECK(null11.err.find("non-zero extent") != std::string::npos);

    const Proc ovf11 = run_proc({exe.string(), "ovf11"});
    REQUIRE(ovf11.ran);
    CHECK(ovf11.rc != 0);
    CHECK(ovf11.err.find("CHECK MEM-OOB-READ") != std::string::npos);
    CHECK(ovf11.err.find("extents product overflows") != std::string::npos);

    // Rank 12 is not declared: dextents<size_t, 12> names the incomplete
    // __prism_mdspan<int, 12>, and twelve size_t extents have no deduction
    // guide. Each unit fails to compile, so pir falls back. The same text with
    // rank 11 compiles, so the failure is the rank and not the spelling.
    auto compiles = [&](const std::string& name, const std::string& body) {
        const fs::path src_path = c.tmp / (name + ".cpp");
        std::ofstream(src_path) << "#include <mdspan>\n#include <cstddef>\nint " << name << "() {\n"
                                << body << "}\n";
        std::vector<std::string> argv{c.cxx};
        argv.insert(argv.end(), flags().begin(), flags().end());
        argv.push_back("-isystem");
        argv.push_back((repo() / "src" / "prism" / "pir" / "models" / "cxx").string());
        argv.push_back(src_path.string());
        argv.push_back("-c");
        argv.push_back("-o");
        argv.push_back((c.tmp / (name + ".o")).string());
        return run_ok(argv);
    };
    auto ones = [](int n) {
        std::string s;
        for (int x = 0; x < n; ++x) s += x ? ", 1" : "1";
        return s;
    };
    auto zeros = [](int n) {
        std::string s;
        for (int x = 0; x < n; ++x) s += x ? ", 0" : "0";
        return s;
    };
    for (int r : {11, 12}) {
        const std::string n = std::to_string(r);
        const std::string dyn = "  int a[1] = {1};\n  std::mdspan<int, std::dextents<std::size_t, " + n + ">> m(a, " +
                                ones(r) + ");\n  return m[" + zeros(r) + "];\n";
        std::string st = "  int a[1] = {1};\n  std::mdspan<int, std::extents<std::size_t";
        for (int x = 0; x < r; ++x) st += ", 1";
        st += ">> m(a, " + ones(r) + ");\n  return m(" + zeros(r) + ");\n";
        std::string ctad = "  int a[1] = {1};\n  std::mdspan m(a";
        for (int x = 0; x < r; ++x) ctad += ", std::size_t(1)";
        ctad += ");\n  return m[" + zeros(r) + "];\n";
        if (r == 11) {
            CHECK(compiles("rank11_dyn", dyn));
            CHECK(compiles("rank11_static", st));
            CHECK(compiles("rank11_ctad", ctad));
        } else {
            CHECK_FALSE(compiles("rank12_dyn", dyn));
            CHECK_FALSE(compiles("rank12_static", st));
            CHECK_FALSE(compiles("rank12_ctad", ctad));
        }
    }

    const fs::path standard_sig = c.tmp / "standard_sig.cpp";
    std::ofstream(standard_sig) << "#include <mdspan>\n"
                                   "struct E {};\n"
                                   "struct L {};\n"
                                   "struct A {};\n"
                                   "int standard_sig() {\n"
                                   "  std::mdspan<int, E, L, A> m;\n"
                                   "  return m.rank();\n"
                                   "}\n";
    std::vector<std::string> sig{c.cxx};
    sig.insert(sig.end(), flags().begin(), flags().end());
    sig.push_back("-isystem");
    sig.push_back((repo() / "src" / "prism" / "pir" / "models" / "cxx").string());
    sig.push_back(standard_sig.string());
    sig.push_back("-c");
    sig.push_back("-o");
    sig.push_back((c.tmp / "standard_sig.o").string());
    CHECK_FALSE(run_ok(sig));
}
