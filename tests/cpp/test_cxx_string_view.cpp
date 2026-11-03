// Differential test of the std::string_view model header
// (src/prism/pir/models/cxx/string_view). Same shape as tests/cpp/test_cxx_list.cpp.
// The view does not own its characters, front() on an empty view is a check,
// and operator[] in range is the real character. starts_with and ends_with
// compare at most size() characters. compare(basic_string_view) returns
// -1, 0, or 1 from unsigned char order. substr past size() is a check, and a
// count that would pass size() stops at size(). find and rfind return the
// first or last index of a basic_string_view needle, or npos. An empty
// needle does not read. CMake's prism_tests source list is explicit (not a
// glob).

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
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

struct ViewCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

ViewCtx& ctx() {
    static ViewCtx c;
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
    std::ofstream(probe) << "#include <bits/c++config.h>\n#include <string_view>\n"
                            "int main() { std::string_view v(\"ab\", 2); return v[0] - 'a'; }\n";
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
    fs::path out = tmp / (with_models ? "string_view_trace.model" : "string_view_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "string_view_trace.cpp").string());
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

bool line_has(const std::vector<std::string>& lines, const char* needle) {
    for (const auto& line : lines)
        if (line.find(needle) != std::string::npos) return true;
    return false;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Drop // comments. The header has no block comments.
std::string strip_line_comments(std::string_view src) {
    std::string out;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
            if (i < src.size()) out.push_back('\n');
            continue;
        }
        out.push_back(src[i]);
    }
    return out;
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

std::size_t count_occ(std::string_view src, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = 0; (at = src.find(needle, at)) != std::string_view::npos; at += needle.size()) ++n;
    return n;
}

}  // namespace

TEST_CASE("cxx string_view model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxstringview-doctest";
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

TEST_CASE("cxx string_view model: view does not own, index is the character, empty front is a check") {
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
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "string_view_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd((models / "string_view").string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    // operator[] in range is the real character ('q' is buf[1], 'v' is "view"[0]).
    CHECK(line_has(b, "idx1=q"));
    CHECK(line_has(b, "idx0=v"));
    // The view does not own: data() is the caller's buffer, and that buffer
    // is still those characters after the view's destructor.
    CHECK(line_has(b, "data_eq=1"));
    CHECK(line_has(b, "alive=pqrs"));
    // Empty needle matches, including an empty view. A longer needle is false.
    // A one-character needle is the first or last character. exact_long and
    // suf_long share a prefix or a suffix and are still false.
    CHECK(line_has(b, "sw_empty=1 sw_both=1 sw_pq=1 sw_px=0 sw_long=0 sw_none=0 sw_self=1"));
    CHECK(line_has(b, "ew_empty=1 ew_both=1 ew_rs=1 ew_qr=0 ew_long=0 ew_none=0 ew_self=1"));
    CHECK(line_has(b, "ch_sw=1 0 ch_ew=1 0 ch_empty=0 0"));
    CHECK(line_has(b, "exact_long=0 0 suf_long=0"));
    // substr(0, 0) is empty and shares the caller's pointer. npos and a
    // one-argument substr stop at size(). A wrapped count does too. A
    // position of size() is an empty view at the end, not a view past it.
    CHECK(line_has(b, "sub0=0 1 1"));
    CHECK(line_has(b, "submid=2 qr 1"));
    CHECK(line_has(b, "subnpos=3 3 1"));
    CHECK(line_has(b, "subover=2 rs 1"));
    CHECK(line_has(b, "subend=0 1 1"));
    CHECK(line_has(b, "subz=0 1 1"));
    CHECK(line_has(b, "suball=4 1 1"));
    CHECK(line_has(b, "subempty=0 1 1 0 1"));
    CHECK(line_has(b, "subalive=pqrs"));
    // Signs, so a libstdc++ magnitude other than -1 or 1 still matches.
    // Empty against empty is 0. A high byte is greater than 'a'. A proper
    // prefix is less. Zero-length views, including one past the end, are 0.
    CHECK(line_has(b, "cmp_empty=0 cmp_empty_a=-1 cmp_a_empty=1 cmp_aa=0 cmp_ab=-1 cmp_ba=1"));
    CHECK(line_has(b, "cmp_prefix=-1 cmp_longer=1 cmp_mid=-1 cmp_pref3=-1 cmp_high=1 cmp_zz=0 cmp_end=0"));
    // First hit at or after pos, last hit at or before pos. npos is a miss.
    // An empty needle does not read. A non-empty needle in an empty view
    // does not read the null pointer. pos past size() on find does not read.
    CHECK(line_has(b, "find_hit=0 2 2 npos"));
    CHECK(line_has(b, "rfind_hit=2 2 0 0"));
    CHECK(line_has(b, "find_ab=0 2 npos"));
    CHECK(line_has(b, "rfind_ab=2 0"));
    CHECK(line_has(b, "find_ba=1 3"));
    CHECK(line_has(b, "find_aaa=0 1"));
    CHECK(line_has(b, "find_pref=1 1"));
    CHECK(line_has(b, "find_tail=2 npos 2 npos"));
    CHECK(line_has(b, "find_miss=npos npos"));
    CHECK(line_has(b, "find_hi=1 npos 1"));
    CHECK(line_has(b, "find_empty=0 5 npos 0 3 5 5"));
    CHECK(line_has(b, "find_past=npos npos npos npos"));
    CHECK(line_has(b, "find_null=npos 0 npos npos 0 0"));
    CHECK(line_has(b, "find_long=npos npos"));
    CHECK(line_has(b, "find_endgap=0 2 npos 1 2"));
    CHECK(line_has(b, "find_self=0 0"));
    CHECK(line_has(b, "find_alive=ababa"));

    for (const fs::path& exe : {lib, mod}) {
        std::string err;
        CHECK_FALSE(run_cmd({exe.string(), "front"}, nullptr, &err));
        if (exe == mod) {
            CHECK(err.find("FUNC-CONTRACT") != std::string::npos);
            CHECK(err.find("front()") != std::string::npos);
        }
        err.clear();
        CHECK_FALSE(run_cmd({exe.string(), "substr"}, nullptr, &err));
        if (exe == mod) {
            CHECK(err.find("MEM-OOB-READ") != std::string::npos);
            CHECK(err.find("substr") != std::string::npos);
        }
    }
}

TEST_CASE("cxx string_view model: starts_with and ends_with stay inside the view") {
    const fs::path header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "string_view";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    CHECK(src.find("#include <string>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("substr, starts_with") == std::string::npos);
    CHECK(src.find("compare, find, and every other") == std::string::npos);
    CHECK(src.find("find, and every other member that is not") == std::string::npos);
    CHECK(src.find("contains, and every other member that is not") != std::string::npos);
    CHECK(src.find("compare, find, substr, and every other") == std::string::npos);
    // Pointer plus count stores the pointer and the count. It stays noexcept
    // and does not check null. The view still does not own its characters.
    CHECK(src.find("constexpr basic_string_view() noexcept : _M_len{0}, _M_str{nullptr} {}") != std::string::npos);
    CHECK(src.find("constexpr basic_string_view(const _CharT* __s, size_type __n) noexcept : _M_len{__n}, "
                   "_M_str{__s} {}") != std::string::npos);
    CHECK(src.find("constexpr ~basic_string_view() = default;") != std::string::npos);

    const std::string code = strip_line_comments(src);
    CHECK(count_occ(code, "compare(") == 1);
    CHECK(count_occ(code, "find(") == 2);
    CHECK(count_occ(code, "rfind(") == 1);
    CHECK(count_occ(code, "substr(") == 1);
    CHECK(count_occ(code, "starts_with(") == 2);
    CHECK(count_occ(code, "ends_with(") == 2);
    CHECK(code.find("new ") == std::string::npos);
    CHECK(code.find("delete") == std::string::npos);

    const std::string sw = body_of(code, "bool starts_with(basic_string_view __x) const noexcept");
    const std::string swc = body_of(code, "bool starts_with(_CharT __c) const noexcept");
    const std::string ew = body_of(code, "bool ends_with(basic_string_view __x) const noexcept");
    const std::string ewc = body_of(code, "bool ends_with(_CharT __c) const noexcept");
    REQUIRE_FALSE(sw.empty());
    REQUIRE_FALSE(swc.empty());
    REQUIRE_FALSE(ew.empty());
    REQUIRE_FALSE(ewc.empty());

    const std::string bound = "const size_type __n = __x._M_len < _M_len ? __x._M_len : _M_len;";
    CHECK(sw.find(bound) != std::string::npos);
    CHECK(ew.find(bound) != std::string::npos);
    // A compared length of 0 does not read _M_str. The loop bound is the
    // overlap, at most size(), not the raw needle length.
    REQUIRE(sw.find("_M_str") != std::string::npos);
    REQUIRE(ew.find("_M_str") != std::string::npos);
    CHECK(sw.find("if (__n != 0)") < sw.find("_M_str"));
    CHECK(ew.find("if (__n != 0)") < ew.find("_M_str"));
    CHECK(sw.find("for (size_type __i = 0; __i < __n; ++__i)") != std::string::npos);
    CHECK(ew.find("for (size_type __i = 0; __i < __n; ++__i)") != std::string::npos);
    CHECK(sw.find("__i < __x._M_len") == std::string::npos);
    CHECK(ew.find("__i < __x._M_len") == std::string::npos);
    CHECK(sw.find("return __x._M_len <= _M_len;") != std::string::npos);
    CHECK(ew.find("return __x._M_len <= _M_len;") != std::string::npos);
    CHECK(ew.find("_M_len - __n") != std::string::npos);
    CHECK(ew.find("__x._M_len - __n") != std::string::npos);
    CHECK(sw.find("__prism_check") == std::string::npos);
    CHECK(ew.find("__prism_check") == std::string::npos);

    REQUIRE(swc.find("_M_str") != std::string::npos);
    REQUIRE(ewc.find("_M_str") != std::string::npos);
    CHECK(swc.find("if (_M_len == 0) return false;") < swc.find("_M_str"));
    CHECK(ewc.find("if (_M_len == 0) return false;") < ewc.find("_M_str"));
    CHECK(swc.find("_M_str[0]") != std::string::npos);
    CHECK(ewc.find("_M_str[_M_len - 1]") != std::string::npos);
    CHECK(swc.find("__prism_check") == std::string::npos);
    CHECK(ewc.find("__prism_check") == std::string::npos);

    const std::string sub = body_of(code, "substr(size_type __pos = 0, size_type __count = npos) const");
    REQUIRE_FALSE(sub.empty());
    const auto check = sub.find(
        "__prism_check(__pos <= _M_len, \"MEM-OOB-READ\", \"std::string_view::substr position past size()\")");
    const auto trap = sub.find("__builtin_trap()");
    const auto ret = sub.find("return");
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(ret != std::string::npos);
    CHECK(check < trap);
    CHECK(trap < ret);
    CHECK(sub.substr(check, trap - check).find("return") == std::string::npos);
    CHECK(sub.find("const size_type __room = _M_len - __pos;") != std::string::npos);
    CHECK(sub.find("const size_type __n = __count < __room ? __count : __room;") != std::string::npos);
    CHECK(sub.find("if (_M_str == nullptr)") != std::string::npos);
    CHECK(sub.find("return basic_string_view(nullptr, __n);") != std::string::npos);
    CHECK(sub.find("return basic_string_view(_M_str + __pos, __n);") != std::string::npos);
    CHECK(sub.find("_M_str[") == std::string::npos);
    CHECK(sub.find("__pos + __count") == std::string::npos);
    CHECK(sub.find("__pos +") == std::string::npos);
    CHECK(sub.find("new ") == std::string::npos);
    CHECK(sub.find("delete") == std::string::npos);
    CHECK(code.find("static constexpr size_type npos = static_cast<size_type>(-1);") != std::string::npos);

    const std::string cmp = body_of(code, "int compare(basic_string_view __x) const noexcept");
    REQUIRE_FALSE(cmp.empty());
    const auto empty_eq = cmp.find("if (_M_len == 0 && __x._M_len == 0) return 0;");
    REQUIRE(empty_eq != std::string::npos);
    REQUIRE(cmp.find("_M_str") != std::string::npos);
    CHECK(empty_eq < cmp.find("_M_str"));
    CHECK(cmp.find("if (__n != 0)") < cmp.find("_M_str"));
    CHECK(cmp.find("const size_type __n = __x._M_len < _M_len ? __x._M_len : _M_len;") != std::string::npos);
    CHECK(cmp.find("static_cast<unsigned char>(_M_str[__i])") != std::string::npos);
    CHECK(cmp.find("static_cast<unsigned char>(__x._M_str[__i])") != std::string::npos);
    CHECK(cmp.find("for (size_type __i = 0; __i < __n; ++__i)") != std::string::npos);
    CHECK(cmp.find("if (__a < __b) return -1;") != std::string::npos);
    CHECK(cmp.find("if (__a > __b) return 1;") != std::string::npos);
    CHECK(cmp.find("if (_M_len < __x._M_len) return -1;") != std::string::npos);
    CHECK(cmp.find("if (_M_len > __x._M_len) return 1;") != std::string::npos);
    CHECK(cmp.find("__prism_check") == std::string::npos);
    CHECK(cmp.find("new ") == std::string::npos);
    CHECK(cmp.find("delete") == std::string::npos);
    CHECK(cmp.find("locale") == std::string::npos);

    const std::string fd = body_of(code, "size_type find(basic_string_view __x, size_type __pos = 0) const noexcept");
    const std::string rd =
        body_of(code, "size_type rfind(basic_string_view __x, size_type __pos = npos) const noexcept");
    REQUIRE_FALSE(fd.empty());
    REQUIRE_FALSE(rd.empty());
    const auto fd_empty = fd.find("if (__x._M_len == 0)");
    const auto fd_load = fd.find("_M_str");
    REQUIRE(fd_empty != std::string::npos);
    REQUIRE(fd_load != std::string::npos);
    CHECK(fd_empty < fd_load);
    CHECK(fd.find("if (__pos > _M_len) return npos;") < fd.find("return __pos;"));
    CHECK(fd.find("return __pos;") < fd_load);
    const auto fd_none = fd.find("if (_M_len == 0 || __pos > _M_len || __x._M_len > _M_len) return npos;");
    REQUIRE(fd_none != std::string::npos);
    CHECK(fd_none < fd_load);
    CHECK(fd.find("const size_type __last = _M_len - __x._M_len;") < fd_load);
    CHECK(fd.find("if (__pos > __last) return npos;") < fd_load);
    CHECK(fd.find("if (!_Traits::eq(_M_str[__k], __x._M_str[__j])) break;") != std::string::npos);
    CHECK(fd.find("++__j;") < fd.find("if (__j == __x._M_len) return __i;"));
    CHECK(fd.find("if (__j == __x._M_len) return __i;") < fd.find("++__k;"));
    CHECK(fd.find("if (__i == __last) return npos;") != std::string::npos);
    CHECK(fd.find("++__i;") != std::string::npos);
    CHECK(fd.find("__pos +") == std::string::npos);
    CHECK(fd.find("__i +") == std::string::npos);
    CHECK(fd.find("__k +") == std::string::npos);
    CHECK(fd.find("_M_str +") == std::string::npos);
    CHECK(fd.find("_M_str[_M_len") == std::string::npos);
    CHECK(fd.find("static_cast<unsigned char>") == std::string::npos);
    CHECK(fd.find("__prism_check") == std::string::npos);
    CHECK(fd.find("new ") == std::string::npos);
    CHECK(fd.find("delete") == std::string::npos);
    CHECK(fd.find("locale") == std::string::npos);

    const auto rd_empty = rd.find("if (__x._M_len == 0) return __pos < _M_len ? __pos : _M_len;");
    const auto rd_load = rd.find("_M_str");
    REQUIRE(rd_empty != std::string::npos);
    REQUIRE(rd_load != std::string::npos);
    CHECK(rd_empty < rd_load);
    const auto rd_none = rd.find("if (_M_len == 0 || __x._M_len > _M_len) return npos;");
    REQUIRE(rd_none != std::string::npos);
    CHECK(rd_none < rd_load);
    CHECK(rd.find("const size_type __last = _M_len - __x._M_len;") < rd_load);
    CHECK(rd.find("__pos < __last ? __pos : __last") != std::string::npos);
    CHECK(rd.find("if (!_Traits::eq(_M_str[__k], __x._M_str[__j])) break;") != std::string::npos);
    CHECK(rd.find("++__j;") < rd.find("if (__j == __x._M_len) return __i;"));
    CHECK(rd.find("if (__j == __x._M_len) return __i;") < rd.find("++__k;"));
    CHECK(rd.find("if (__i == 0) return npos;") != std::string::npos);
    CHECK(rd.find("--__i;") != std::string::npos);
    CHECK(rd.find("__pos +") == std::string::npos);
    CHECK(rd.find("__i +") == std::string::npos);
    CHECK(rd.find("__k +") == std::string::npos);
    CHECK(rd.find("_M_str +") == std::string::npos);
    CHECK(rd.find("_M_str[_M_len") == std::string::npos);
    CHECK(rd.find("static_cast<unsigned char>") == std::string::npos);
    CHECK(rd.find("__prism_check") == std::string::npos);
    CHECK(rd.find("new ") == std::string::npos);
    CHECK(rd.find("delete") == std::string::npos);
    CHECK(rd.find("locale") == std::string::npos);
    CHECK(count_occ(src, "This is not PROVED and not CLEAN.") == 2);

    const std::string trace = slurp(repo() / "tests" / "cxx_models" / "string_view_trace.cpp");
    CHECK(trace.find(".starts_with(") != std::string::npos);
    CHECK(trace.find(".ends_with(") != std::string::npos);
    CHECK(trace.find(".substr(") != std::string::npos);
    CHECK(trace.find(".compare(") != std::string::npos);
    CHECK(trace.find("v.find(") != std::string::npos);
    CHECK(trace.find("v.rfind(") != std::string::npos);
    CHECK(trace.find("v.substr(0, 0)") != std::string::npos);
    CHECK(trace.find("empty.substr(0, 0)") != std::string::npos);
}
