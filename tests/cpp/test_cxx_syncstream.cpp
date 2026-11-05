// The std::syncstream model header (src/prism/pir/models/cxx/syncstream) is a
// file on the include path. CMake's prism_tests source list is explicit (not
// a glob): this file is one line in _prism_test_sources. The trace is
// tests/cxx_models/syncstream_trace.cpp.
//
// A write at the size is a __prism_check. A read at the size is a
// __prism_check and does not load the pointer. sungetc at offset 0 is a
// __prism_check and does not decrement or load before the buffer. sputn
// with a negative count is a __prism_check then __builtin_trap and does
// not write. A count of 0 returns 0 and does not read. A positive count
// stores only into the remaining buffer. in_avail() returns the bytes still
// left at the offset without writing or loading. The header does not define
// emit.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

namespace cxxh = prism_test::cxx;
using cxxh::fwd;
using cxxh::repo;
using cxxh::run_cmd;

fs::path syncstream_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "syncstream"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool is_ident(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

// Body of the first function whose text contains `sig`, including braces.
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

// `name` as its own identifier, then '('. A comment that only says emit does not count.
bool defines_call(std::string_view src, std::string_view name) {
    for (std::size_t i = 0; i + name.size() <= src.size(); ++i) {
        if (src.compare(i, name.size(), name) != 0) continue;
        if (i > 0 && is_ident(src[i - 1])) continue;
        std::size_t j = i + name.size();
        if (j < src.size() && is_ident(src[j])) continue;
        while (j < src.size() && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n' || src[j] == '\r')) ++j;
        if (j < src.size() && src[j] == '(') return true;
    }
    return false;
}

struct SyncCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

SyncCtx& ctx() {
    static SyncCtx c;
    return c;
}

const std::vector<std::string> kFlags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-pthread",
                                      "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-O1", "-w"};

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <syncstream>\n"
                                  "int main() { return 0; }\n",
                                  kFlags);
}

// The trace is built against the models only.
fs::path build_prog(const std::string& cxx, const fs::path& tmp) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("syncstream_trace.cpp"), "syncstream_trace", kFlags, true);
}

std::vector<std::string> run_lines(const fs::path& exe, const char* arg = nullptr) {
    return arg != nullptr ? cxxh::run_lines(exe, {arg}) : cxxh::run_lines(exe);
}

bool is_check(const fs::path& exe, const char* arg, const char* cls, const char* msg, const char* banned) {
    std::string out, err;
    if (run_cmd({exe.string(), arg}, &out, &err)) return false;
    const std::string both = out + err;
    if (both.find(std::string("CHECK ") + cls + ":") == std::string::npos) return false;
    if (both.find(msg) == std::string::npos) return false;
    if (out.find(banned) != std::string::npos) return false;
    return true;
}

bool has_line(const std::vector<std::string>& lines, const std::string& want) {
    for (const auto& line : lines)
        if (line == want) return true;
    return false;
}

// One in-range sgetc prints `need`, then the next call is a check and does not print `banned`.
bool read_then_check(const fs::path& exe, const char* arg, const char* need, const char* banned, const char* cls,
                     const char* msg) {
    std::string out, err;
    if (run_cmd({exe.string(), arg}, &out, &err)) return false;
    if (out.find(need) == std::string::npos) return false;
    if (out.find(banned) != std::string::npos) return false;
    const std::string both = out + err;
    if (both.find(std::string("CHECK ") + cls + ":") == std::string::npos) return false;
    if (both.find(msg) == std::string::npos) return false;
    return true;
}

// One failing check is reported, the process still exits 0, `line` is printed,
// and `absent` is not in stdout or stderr.
bool one_check_then_line(const fs::path& exe, const char* arg, const char* line, const char* cls, const char* msg,
                         const char* absent) {
    std::string out, err;
    if (!run_cmd({exe.string(), arg}, &out, &err)) return false;
    if (out.find(line) == std::string::npos) return false;
    const std::string both = out + err;
    if (both.find(std::string("CHECK ") + cls + ":") == std::string::npos) return false;
    if (both.find(msg) == std::string::npos) return false;
    if (both.find(absent) != std::string::npos) return false;
    return true;
}

// The constructor check runs, then sgetc's check, and the process still prints `line`.
bool checks_then_line(const fs::path& exe, const char* arg, const char* line, const char* first, const char* second) {
    std::string out, err;
    if (!run_cmd({exe.string(), arg}, &out, &err)) return false;
    if (out.find(line) == std::string::npos) return false;
    const auto a = err.find(first);
    const auto b = err.find(second);
    if (a == std::string::npos || b == std::string::npos) return false;
    return a < b;
}

}  // namespace

TEST_CASE("cxx syncstream model: a write or a read at the size is a check, a step back at offset 0 is a check, "
          "sputn stores only the remaining bytes, and emit is not defined") {
    const fs::path header = syncstream_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());

    CHECK(src.find("_GLIBCXX_SYNCSTREAM") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std") != std::string::npos);
    CHECK(src.find("not a concurrency proof") != std::string::npos);
    CHECK(src.find("fails to compile and pir falls back") != std::string::npos);
    CHECK(src.find("_PRISM_SYNCSTREAM_MODEL") != std::string::npos);
    CHECK(src.find("#include <cstddef>") != std::string::npos);
    CHECK(src.find("#include <span>") == std::string::npos);
    CHECK(src.find("#include <sstream>") == std::string::npos);
    CHECK(src.find("#include <string>") == std::string::npos);
    CHECK(src.find("#include <string_view>") == std::string::npos);
    CHECK(src.find("#include <ostream>") == std::string::npos);
    CHECK(src.find("#include <spanstream>") == std::string::npos);
    CHECK(src.find("#include <mutex>") == std::string::npos);
    CHECK(src.find("malloc") == std::string::npos);
    CHECK(src.find("free(") == std::string::npos);
    CHECK(src.find("printf") == std::string::npos);
    CHECK(src.find("std::cout") == std::string::npos);
    CHECK(src.find("char* _M_buf;") != std::string::npos);
    CHECK(src.find("size_t _M_size;") != std::string::npos);
    CHECK(src.find("size_t _M_off;") != std::string::npos);
    CHECK(src.find("~basic_syncbuf() = default;") != std::string::npos);

    const auto ctor = src.find("explicit basic_syncbuf(");
    REQUIRE(ctor != std::string::npos);
    const auto sputc_at = src.find("void sputc(", ctor);
    REQUIRE(sputc_at != std::string::npos);
    const auto sputn_at = src.find("ptrdiff_t sputn(", sputc_at);
    REQUIRE(sputn_at != std::string::npos);
    const auto sgetc_at = src.find("int sgetc(", sputn_at);
    REQUIRE(sgetc_at != std::string::npos);
    const std::string ctor_body = src.substr(ctor, sputc_at - ctor);
    CHECK(ctor_body.find("__prism_check((__buf != nullptr || __n == 0) ? 1 : 0, \"FUNC-CONTRACT\",") !=
          std::string::npos);
    CHECK(ctor_body.find("basic_syncbuf constructed with a null buffer and a non-zero size") != std::string::npos);
    CHECK(ctor_body.find("if (!(__buf != nullptr || __n == 0)) _M_size = 0;") != std::string::npos);

    const std::string sputc = src.substr(sputc_at, sputn_at - sputc_at);
    CHECK(sputc.find("const int __ok = (_M_off < _M_size) ? 1 : 0;") != std::string::npos);
    CHECK(sputc.find("__prism_check(__ok, \"MEM-OOB-WRITE\", \"basic_syncbuf::sputc would write past the caller "
                     "buffer\")") != std::string::npos);
    const auto check_at = sputc.find("__prism_check");
    const auto write_at = sputc.find("_M_buf[_M_off] = __c;");
    const auto adv_at = sputc.find("++_M_off;");
    REQUIRE(check_at != std::string::npos);
    REQUIRE(write_at != std::string::npos);
    REQUIRE(adv_at != std::string::npos);
    CHECK(check_at < write_at);
    CHECK(write_at < adv_at);
    CHECK(sputc.find("if (__ok) {") != std::string::npos);
    CHECK(sputc.find("_M_off =") == std::string::npos);
    CHECK(sputc.find("++_M_off;", adv_at + 1) == std::string::npos);

    const std::string sputn = src.substr(sputn_at, sgetc_at - sputn_at);
    CHECK(sputn.find("__prism_check((__n >= 0) ? 1 : 0, \"FUNC-CONTRACT\",") != std::string::npos);
    CHECK(sputn.find("basic_syncbuf::sputn count is negative") != std::string::npos);
    const auto ncheck = sputn.find("__prism_check");
    const std::string trap_line = "if (__n < 0) __builtin_trap();";
    const auto ntrap = sputn.find(trap_line);
    const auto nzero = sputn.find("if (__n == 0) return 0;");
    const auto nroom = sputn.find("if (_M_off >= _M_size) return 0;");
    const auto nbound = sputn.find("while (__stored < __n && _M_off < _M_size)");
    const std::string store_line = "_M_buf[_M_off] = __s[__stored];";
    const auto nwrite = sputn.find(store_line);
    const auto nret = sputn.find("return __stored;");
    REQUIRE(ncheck != std::string::npos);
    REQUIRE(ntrap != std::string::npos);
    REQUIRE(nzero != std::string::npos);
    REQUIRE(nroom != std::string::npos);
    REQUIRE(nbound != std::string::npos);
    REQUIRE(nwrite != std::string::npos);
    REQUIRE(nret != std::string::npos);
    CHECK(ncheck < ntrap);
    CHECK(ntrap < nzero);
    CHECK(nzero < nroom);
    const auto nnull = sputn.find("__prism_check(__s != nullptr ? 1 : 0, \"PTR-NULL-DEREF\",");
    const std::string null_trap = "if (__s == nullptr) __builtin_trap();";
    const auto nnull_trap = sputn.find(null_trap);
    REQUIRE(nnull != std::string::npos);
    REQUIRE(nnull_trap != std::string::npos);
    CHECK(nroom < nnull);
    CHECK(nnull < nnull_trap);
    CHECK(nnull_trap < nbound);
    CHECK(nbound < nwrite);
    CHECK(nwrite < nret);
    CHECK(sputn.find("__s[", nwrite + store_line.size()) == std::string::npos);
    CHECK(sputn.find("_M_buf[", nwrite + store_line.size()) == std::string::npos);
    CHECK(sputn.find("__builtin_trap()", nnull_trap + null_trap.size()) == std::string::npos);
    CHECK(sputn.find("return __n;") == std::string::npos);
    CHECK(sputn.find("++_M_off;", sputn.find("++_M_off;") + 1) == std::string::npos);

    const auto sungetc_at = src.find("int sungetc(", sgetc_at);
    REQUIRE(sungetc_at != std::string::npos);
    const std::string sgetc = src.substr(sgetc_at, sungetc_at - sgetc_at);
    CHECK(sgetc.find("const int __inv = (_M_off < _M_size) ? 1 : 0;") != std::string::npos);
    CHECK(sgetc.find("__prism_check(__inv, \"MEM-OOB-READ\", \"basic_syncbuf::sgetc would read past the caller "
                     "buffer\")") != std::string::npos);
    const auto gcheck = sgetc.find("__prism_check");
    const auto gread = sgetc.find("static_cast<unsigned char>(_M_buf[_M_off])");
    const auto gadv = sgetc.find("++_M_off;");
    const auto gret = sgetc.find("return static_cast<int>(__ch);");
    const auto geof = sgetc.find("return -1;");
    REQUIRE(gcheck != std::string::npos);
    REQUIRE(gread != std::string::npos);
    REQUIRE(gadv != std::string::npos);
    REQUIRE(gret != std::string::npos);
    REQUIRE(geof != std::string::npos);
    CHECK(gcheck < gread);
    CHECK(gread < gadv);
    CHECK(gadv < gret);
    CHECK(gret < geof);
    CHECK(sgetc.find("if (__inv) {") != std::string::npos);
    CHECK(sgetc.find("_M_buf[", sgetc.find("_M_buf[") + 1) == std::string::npos);
    CHECK(sgetc.find("return -1;", geof + 1) == std::string::npos);
    CHECK(sgetc.find("_M_off =") == std::string::npos);
    CHECK(sgetc.find("++_M_off;", gadv + 1) == std::string::npos);

    const auto avail_at = src.find("size_t in_avail(", sungetc_at);
    REQUIRE(avail_at != std::string::npos);
    const auto priv = src.find("private:", avail_at);
    REQUIRE(priv != std::string::npos);
    const std::string sungetc = src.substr(sungetc_at, avail_at - sungetc_at);
    CHECK(sungetc.find("const int __back = (_M_off > 0) ? 1 : 0;") != std::string::npos);
    CHECK(sungetc.find("__prism_check(__back, \"MEM-OOB-READ\", \"basic_syncbuf::sungetc would read before the caller "
                       "buffer\")") != std::string::npos);
    const auto ucheck = sungetc.find("__prism_check");
    const auto udec = sungetc.find("--_M_off;");
    const auto uread = sungetc.find("static_cast<unsigned char>(_M_buf[_M_off])");
    const auto uret = sungetc.find("return static_cast<int>(__ch);");
    const auto ueof = sungetc.find("return -1;");
    REQUIRE(ucheck != std::string::npos);
    REQUIRE(udec != std::string::npos);
    REQUIRE(uread != std::string::npos);
    REQUIRE(uret != std::string::npos);
    REQUIRE(ueof != std::string::npos);
    CHECK(ucheck < udec);
    CHECK(udec < uread);
    CHECK(uread < uret);
    CHECK(uret < ueof);
    CHECK(sungetc.find("if (__back) {") != std::string::npos);
    CHECK(sungetc.find("_M_buf[", sungetc.find("_M_buf[") + 1) == std::string::npos);
    CHECK(sungetc.find("return -1;", ueof + 1) == std::string::npos);
    CHECK(sungetc.find("_M_off =") == std::string::npos);
    CHECK(sungetc.find("--_M_off;", udec + 1) == std::string::npos);
    CHECK(sungetc.find("++_M_off") == std::string::npos);

    CHECK(defines_call(src, "sputc"));
    CHECK(defines_call(src, "sputn"));
    CHECK(defines_call(src, "sgetc"));
    CHECK(defines_call(src, "sungetc"));
    CHECK_FALSE(defines_call(src, "emit"));
    CHECK(src.find("emit(") == std::string::npos);
    CHECK(src.find("class syncstream") == std::string::npos);
    CHECK(src.find("osyncstream") == std::string::npos);
}

TEST_CASE("cxx syncstream model: in_avail returns bytes left at the offset and does not load") {
    const fs::path header = syncstream_header();
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("in_avail is not PROVED and not CLEAN") != std::string::npos);
    CHECK(src.find("#include <span>") == std::string::npos);
    CHECK(src.find("#include <sstream>") == std::string::npos);
    CHECK(src.find("#include <string>") == std::string::npos);
    CHECK(src.find("#include <string_view>") == std::string::npos);
    CHECK(src.find("__prism_check((__buf != nullptr || __n == 0) ? 1 : 0, \"FUNC-CONTRACT\",") != std::string::npos);
    CHECK(src.find("if (!(__buf != nullptr || __n == 0)) _M_size = 0;") != std::string::npos);
    CHECK(src.find("const int __ok = (_M_off < _M_size) ? 1 : 0;") != std::string::npos);
    CHECK(src.find("__prism_check(__s != nullptr ? 1 : 0, \"PTR-NULL-DEREF\",") != std::string::npos);
    CHECK(src.find("if (__s == nullptr) __builtin_trap();") != std::string::npos);

    const auto at = src.find("size_t in_avail() const noexcept");
    REQUIRE(at != std::string::npos);
    CHECK(src.find("size_t in_avail() const noexcept", at + 1) == std::string::npos);
    const auto sungetc_at = src.find("int sungetc(");
    const auto priv = src.find("\nprivate:");
    REQUIRE(sungetc_at != std::string::npos);
    REQUIRE(priv != std::string::npos);
    CHECK(sungetc_at < at);
    CHECK(at < priv);

    const auto avail = body_of(src, "size_t in_avail(");
    REQUIRE(!avail.empty());
    CHECK(avail.find("return _M_off < _M_size ? _M_size - _M_off : 0;") != std::string::npos);
    CHECK(avail.find("_M_buf") == std::string::npos);
    CHECK(avail.find("[") == std::string::npos);
    CHECK(avail.find("=") == std::string::npos);
    CHECK(avail.find("++") == std::string::npos);
    CHECK(avail.find("--") == std::string::npos);
    CHECK(avail.find("__prism_check") == std::string::npos);
    CHECK(avail.find("__builtin_trap") == std::string::npos);
    CHECK(avail.find("sputc") == std::string::npos);
    CHECK(avail.find("sgetc") == std::string::npos);
    CHECK(avail.find("sungetc") == std::string::npos);
    CHECK(avail.find("sputn") == std::string::npos);

    const auto sputc = body_of(src, "void sputc(");
    const auto sputn = body_of(src, "ptrdiff_t sputn(");
    const auto sgetc = body_of(src, "int sgetc(");
    const auto sungetc = body_of(src, "int sungetc(");
    const auto ctor = body_of(src, "explicit basic_syncbuf(");
    REQUIRE(!sputc.empty());
    REQUIRE(!sputn.empty());
    REQUIRE(!sgetc.empty());
    REQUIRE(!sungetc.empty());
    REQUIRE(!ctor.empty());
    CHECK(sputc.find("in_avail") == std::string::npos);
    CHECK(sputn.find("in_avail") == std::string::npos);
    CHECK(sgetc.find("in_avail") == std::string::npos);
    CHECK(sungetc.find("in_avail") == std::string::npos);
    CHECK(ctor.find("in_avail") == std::string::npos);
    CHECK(sputc.find("const int __ok = (_M_off < _M_size) ? 1 : 0;") != std::string::npos);
    CHECK(ctor.find("if (!(__buf != nullptr || __n == 0)) _M_size = 0;") != std::string::npos);
    CHECK(sputn.find("if (__s == nullptr) __builtin_trap();") != std::string::npos);
    CHECK(src.find("~basic_syncbuf() = default;") != std::string::npos);
    CHECK(defines_call(src, "in_avail"));
}

TEST_CASE("cxx syncstream model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxsyncstream-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx syncstream model: one stored byte stays in the caller buffer") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-pthread",
                                         "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-O1",
                                         "-w"};
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags.begin(), flags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "syncstream_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(syncstream_header().string())) != std::string::npos);

    auto mod = build_prog(c.cxx, c.tmp);
    auto lines = run_lines(mod);
    CHECK(has_line(lines, "byte 65"));
    CHECK(has_line(lines, "kept 65"));
    CHECK(is_check(mod, "full", "MEM-OOB-WRITE", "basic_syncbuf::sputc would write past the caller buffer", "wrote"));
    CHECK(is_check(mod, "wrap", "MEM-OOB-WRITE", "basic_syncbuf::sputc would write past the caller buffer", "wrapped"));
    CHECK(is_check(mod, "null", "FUNC-CONTRACT", "basic_syncbuf constructed with a null buffer and a non-zero size",
                   "constructed"));
    auto got = run_lines(mod, "get");
    CHECK(has_line(got, "got 0"));
    CHECK(has_line(got, "got 255"));
    CHECK(has_line(got, "got 65"));
    CHECK(is_check(mod, "empty", "MEM-OOB-READ", "basic_syncbuf::sgetc would read past the caller buffer", "got"));
    CHECK(is_check(mod, "null0", "MEM-OOB-READ", "basic_syncbuf::sgetc would read past the caller buffer", "read"));
    CHECK(read_then_check(mod, "past", "once 65", "past", "MEM-OOB-READ",
                          "basic_syncbuf::sgetc would read past the caller buffer"));
    CHECK(checks_then_line(mod, "forced", "forced -1",
                           "CHECK FUNC-CONTRACT: basic_syncbuf constructed with a null buffer and a non-zero size",
                           "CHECK MEM-OOB-READ: basic_syncbuf::sgetc would read past the caller buffer"));
    auto back = run_lines(mod, "back");
    CHECK(has_line(back, "back 65"));
    CHECK(has_line(back, "back 255"));
    CHECK(has_line(back, "back 0"));
    CHECK(is_check(mod, "before", "MEM-OOB-READ", "basic_syncbuf::sungetc would read before the caller buffer",
                   "before"));
    CHECK(is_check(mod, "unnull0", "MEM-OOB-READ", "basic_syncbuf::sungetc would read before the caller buffer",
                   "unread"));
    CHECK(read_then_check(mod, "step", "step 65", "under", "MEM-OOB-READ",
                          "basic_syncbuf::sungetc would read before the caller buffer"));
    CHECK(checks_then_line(mod, "unforced", "unforced -1",
                           "CHECK FUNC-CONTRACT: basic_syncbuf constructed with a null buffer and a non-zero size",
                           "CHECK MEM-OOB-READ: basic_syncbuf::sungetc would read before the caller buffer"));
    CHECK(one_check_then_line(mod, "stay", "stayed -1 65", "MEM-OOB-READ",
                                  "basic_syncbuf::sungetc would read before the caller buffer", "MEM-OOB-WRITE"));
    auto put = run_lines(mod, "put");
    CHECK(has_line(put, "put 3"));
    CHECK(has_line(put, "keptn 65 98 67 0"));
    auto zero = run_lines(mod, "zero");
    CHECK(has_line(zero, "zero 0 90"));
    auto room = run_lines(mod, "room");
    CHECK(has_line(room, "room 2 65 66"));
    auto noroom = run_lines(mod, "noroom");
    CHECK(has_line(noroom, "noroom 0 65"));
    auto emptyn = run_lines(mod, "emptyn");
    CHECK(has_line(emptyn, "emptyn 0 90"));
    auto rest = run_lines(mod, "rest");
    CHECK(has_line(rest, "rest 3 65 66 67 68"));
    auto avail = run_lines(mod, "avail");
    CHECK(has_line(avail, "avail 4 87 88 89 90"));
    auto availmid = run_lines(mod, "availmid");
    CHECK(has_line(availmid, "availmid 3 65 88 89 90"));
    auto avail0 = run_lines(mod, "avail0");
    CHECK(has_line(avail0, "avail0 0"));
    auto availend = run_lines(mod, "availend");
    CHECK(has_line(availend, "availend 0 65"));
    auto availat = run_lines(mod, "availat");
    CHECK(has_line(availat, "availat 0 81"));
    CHECK(is_check(mod, "neg", "FUNC-CONTRACT", "basic_syncbuf::sputn count is negative", "stored"));
    CHECK(is_check(mod, "neggo", "FUNC-CONTRACT", "basic_syncbuf::sputn count is negative", "stored"));
    CHECK(read_then_check(mod, "after", "filled 2", "extra", "MEM-OOB-WRITE",
                          "basic_syncbuf::sputc would write past the caller buffer"));
    CHECK(one_check_then_line(mod, "putforced", "putforced 0", "FUNC-CONTRACT",
                                  "basic_syncbuf constructed with a null buffer and a non-zero size", "MEM-OOB-WRITE"));
}
