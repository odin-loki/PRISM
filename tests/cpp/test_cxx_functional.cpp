// Differential test of the std::function model header
// (src/prism/pir/models/cxx/functional). Same shape as tests/cpp/test_cxx_optional.cpp.
// CMake's prism_tests source list is explicit (not a glob):
// docs/finish-notes/functional.md has the line that links this file.
//
// A stored function pointer is the one operator() calls. operator() on an
// empty std::function is a __prism_check, and the model does not print a
// made-up return value. std::invoke of a function pointer calls that pointer.
// A null function pointer is a __prism_check and does not print a return value.
// std::not_fn of a function pointer calls that pointer and negates the bool.
// A null function pointer is a __prism_check and does not print true or false.
// std::identity returns its argument. std::equal_to, std::not_equal_to,
// std::less, and std::greater are function objects: a == b, !(a == b),
// a < b, and b < a. They do not invent a third value.
// std::plus, std::minus, std::multiplies, std::divides, std::modulus, and
// std::negate are function objects for one type argument. A signed plus,
// minus, or multiplies that does not fit is INT-SIGNED-OVF then a trap,
// and that path does not print a wrapped result. An unsigned one of those
// three that does not fit is FUNC-CONTRACT then a trap. A zero divides or
// modulus is FUNC-CONTRACT then a trap and does not print a quotient.
// Signed divides of the minimum by -1 is that same class then a trap.
// negate of the most negative signed value is INT-SIGNED-OVF then a trap.
// std::logical_and, std::logical_or, and std::logical_not are function
// objects for one type argument. Each returns that operator. A missing
// operator fails the template. There is no stand-in boolean.
// std::bit_and, std::bit_or, and std::bit_xor are function objects for one
// type argument. Each returns that operator. A type that does not provide
// the operator fails the template. There is no stand-in integer and no
// void specialization.
// std::bit_not is a function object for one type argument. It returns the
// ~ of its argument. A type that does not provide the operator fails the
// template. There is no stand-in integer and no void specialization.

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

struct FnCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

FnCtx& ctx() {
    static FnCtx c;
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
    std::ofstream(probe) << "#include <bits/c++config.h>\n#include <functional>\n"
                            "int id(int x) { return x; }\n"
                            "int main() { std::function<int(int)> f = id; return f(1) - 1; }\n";
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
    fs::path out = tmp / (with_models ? "functional_trace.model" : "functional_trace.lib");
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (with_models) {
        argv.push_back("-isystem");
        argv.push_back(models.string());
    }
    argv.push_back((repo() / "tests" / "cxx_models" / "functional_trace.cpp").string());
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

bool empty_call_is_check(const fs::path& exe) {
    std::string out, err;
    if (run_cmd({exe.string(), "empty"}, &out, &err)) return false;
    const std::string both = out + err;
    if (both.find("CHECK CXX-STD-FUNCTION-NULL:") == std::string::npos) return false;
    if (both.find("bad_function_call") == std::string::npos) return false;
    if (out.find("fake=") != std::string::npos) return false;
    return true;
}

bool null_invoke_is_check(const fs::path& exe) {
    std::string out, err;
    if (run_cmd({exe.string(), "nullfn"}, &out, &err)) return false;
    const std::string both = out + err;
    if (both.find("CHECK FUNC-CONTRACT:") == std::string::npos) return false;
    if (both.find("null function pointer") == std::string::npos) return false;
    if (both.find("bad_function_call") != std::string::npos) return false;
    if (out.find("fake=") != std::string::npos) return false;
    return true;
}

bool null_not_fn_is_check(const fs::path& exe) {
    std::string out, err;
    if (run_cmd({exe.string(), "nullnot"}, &out, &err)) return false;
    const std::string both = out + err;
    if (both.find("CHECK FUNC-CONTRACT:") == std::string::npos) return false;
    if (both.find("std::not_fn called with a null function pointer") == std::string::npos) return false;
    if (both.find("bad_function_call") != std::string::npos) return false;
    if (out.find("fake=") != std::string::npos) return false;
    return true;
}

bool arith_trap(const fs::path& exe, const char* arg, const char* cls, const char* msg) {
    std::string out, err;
    if (run_cmd({exe.string(), arg}, &out, &err)) return false;
    const std::string both = out + err;
    if (both.find(std::string("CHECK ") + cls + ":") == std::string::npos) return false;
    if (both.find(msg) == std::string::npos) return false;
    if (out.find("fake=") != std::string::npos) return false;
    if (both.find("runtime error") != std::string::npos) return false;
    return true;
}

std::size_t count_occ(const std::string& s, const std::string& n) {
    std::size_t c = 0;
    for (std::size_t at = 0; (at = s.find(n, at)) != std::string::npos; at += n.size()) ++c;
    return c;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string strip_line_comments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
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

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "functional"; }

}  // namespace

TEST_CASE("cxx functional model: invoke of a null function pointer is a check") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string text = slurp(header);
    REQUIRE_FALSE(text.empty());
    const std::string code = strip_line_comments(text);
    CHECK(text.find("#include <memory>") == std::string::npos);
    CHECK(text.find("#include <type_traits>") == std::string::npos);
    CHECK(text.find("This header does not include memory or type_traits.") != std::string::npos);
    const auto inc1 = code.find("#include");
    const auto inc2 = inc1 == std::string::npos ? std::string::npos : code.find("#include", inc1 + 1);
    const auto inc3 = inc2 == std::string::npos ? std::string::npos : code.find("#include", inc2 + 1);
    CHECK(inc1 != std::string::npos);
    CHECK(inc2 != std::string::npos);
    CHECK(inc3 == std::string::npos);
    CHECK(code.find("#include <bits/stl_function.h>") != std::string::npos);
    CHECK(code.find("#include <bits/ranges_cmp.h>") != std::string::npos);

    const auto call = text.find("_Res operator()(_ArgTypes... __args) const");
    const auto fn_check = text.find("__prism_check(0, \"CXX-STD-FUNCTION-NULL\"", call);
    const auto fn_dead = text.find("__builtin_unreachable()", fn_check);
    const auto fn_ret = text.find("return _M_fn(__args...);", fn_dead);
    REQUIRE(call != std::string::npos);
    REQUIRE(fn_check != std::string::npos);
    REQUIRE(fn_dead != std::string::npos);
    REQUIRE(fn_ret != std::string::npos);
    CHECK(text.find("bad_function_call", fn_check) < fn_dead);
    CHECK(text.substr(fn_check, fn_dead - fn_check).find("return") == std::string::npos);
    CHECK(text.substr(fn_check, fn_dead - fn_check).find("_M_fn(") == std::string::npos);
    CHECK(text.find("function(function&& __o) noexcept : _M_fn(__o._M_fn) { __o._M_fn = nullptr; }") !=
          std::string::npos);
    CHECK(text.find("__o._M_fn = nullptr;") != std::string::npos);

    const auto inv = text.find("_Res invoke(_Res (*__f)(_Params...), _Args&&... __args)");
    const auto inv_nx = text.find("_Res invoke(_Res (*__f)(_Params...) noexcept, _Args&&... __args)", inv);
    REQUIRE(inv != std::string::npos);
    REQUIRE(inv_nx != std::string::npos);
    CHECK(inv < inv_nx);
    for (const auto at : {inv, inv_nx}) {
        const auto next = text.find("\ntemplate", at + 1);
        const auto end = next == std::string::npos ? text.size() : next;
        const std::string body = text.substr(at, end - at);
        const auto check = body.find("__prism_check(0, \"FUNC-CONTRACT\"");
        const auto dead = body.find("__builtin_unreachable()");
        const auto ret = body.find("return __f(static_cast<_Args&&>(__args)...);");
        REQUIRE(check != std::string::npos);
        REQUIRE(dead != std::string::npos);
        REQUIRE(ret != std::string::npos);
        CHECK(check < dead);
        CHECK(dead < ret);
        CHECK(body.find("null function pointer") != std::string::npos);
        CHECK(body.substr(check, dead - check).find("return") == std::string::npos);
        CHECK(body.substr(check, dead - check).find("__f(") == std::string::npos);
        CHECK(body.find("return {}") == std::string::npos);
        CHECK(body.find("return _Res{}") == std::string::npos);
        CHECK(body.find("return _Res()") == std::string::npos);
        CHECK(body.find("return 0") == std::string::npos);
    }

    const auto neg = text.find("bool operator()(_Args&&... __args) const");
    REQUIRE(neg != std::string::npos);
    const auto neg_check = text.find("__prism_check(0, \"FUNC-CONTRACT\"", neg);
    const auto neg_dead = text.find("__builtin_unreachable()", neg_check);
    const auto neg_ret =
        text.find("return !static_cast<bool>(_M_fn(static_cast<_Args&&>(__args)...));", neg_dead);
    REQUIRE(neg_check != std::string::npos);
    REQUIRE(neg_dead != std::string::npos);
    REQUIRE(neg_ret != std::string::npos);
    CHECK(neg_check < neg_dead);
    CHECK(neg_dead < neg_ret);
    CHECK(text.find("std::not_fn called with a null function pointer", neg_check) < neg_dead);
    const std::string neg_null = text.substr(neg_check, neg_dead - neg_check);
    CHECK(neg_null.find("return") == std::string::npos);
    CHECK(neg_null.find("true") == std::string::npos);
    CHECK(neg_null.find("false") == std::string::npos);
    CHECK(neg_null.find("_M_fn(") == std::string::npos);
    const auto nfn = text.find("_Not_fn<_Res (*)(_Params...)> not_fn(_Res (*__f)(_Params...))");
    const auto nfn_nx =
        text.find("_Not_fn<_Res (*)(_Params...) noexcept> not_fn(_Res (*__f)(_Params...) noexcept)");
    REQUIRE(nfn != std::string::npos);
    REQUIRE(nfn_nx != std::string::npos);
    CHECK(nfn < nfn_nx);
    const auto req = text.rfind("requires (_Res_bool<_Res>::value)", nfn);
    const auto req_nx = text.rfind("requires (_Res_bool<_Res>::value)", nfn_nx);
    REQUIRE(req != std::string::npos);
    REQUIRE(req_nx != std::string::npos);
    CHECK(req < nfn);
    CHECK(req_nx < nfn_nx);
    CHECK(req != req_nx);
    CHECK(text.find("struct _Res_bool<void>") != std::string::npos);
    CHECK(code.find("not_fn") != std::string::npos);
    for (const char* name : {"bind", "mem_fn", "reference_wrapper", "bind_front", "cref", "ref("})
        CHECK(code.find(name) == std::string::npos);

    CHECK(text.find("This is not PROVED and not CLEAN.") != std::string::npos);
    const auto proved_at = text.find("PROVED");
    const auto clean_at = text.find("CLEAN");
    REQUIRE(proved_at != std::string::npos);
    REQUIRE(clean_at != std::string::npos);
    CHECK(text.find("PROVED", proved_at + 6) == std::string::npos);
    CHECK(text.find("CLEAN", clean_at + 5) == std::string::npos);
    CHECK(text.find("They are not function pointers.") != std::string::npos);
    CHECK(text.find("does not copy through a default T{}.") != std::string::npos);
    CHECK(text.find("They do not invent a third value.") != std::string::npos);
    CHECK(text.find("fails the template rather than") != std::string::npos);
    CHECK(text.find("returning a stand-in.") != std::string::npos);

    const auto id = text.find("struct identity {");
    const auto id_ret = text.find("return static_cast<_Tp&&>(__t);", id);
    const auto id_end = text.find("#endif", id);
    REQUIRE(id != std::string::npos);
    REQUIRE(id_ret != std::string::npos);
    REQUIRE(id_end != std::string::npos);
    CHECK(id_ret < id_end);
    const std::string id_body = text.substr(id, id_end - id);
    CHECK(id_body.find("return static_cast<_Tp&&>(__t);") != std::string::npos);
    CHECK(id_body.find("{}") == std::string::npos);
    CHECK(id_body.find("_Tp{}") == std::string::npos);
    CHECK(id_body.find("T{}") == std::string::npos);
    CHECK(code.find("struct identity") != std::string::npos);
    for (const char* name : {"struct equal_to", "struct not_equal_to", "struct less", "struct greater"})
        CHECK(code.find(name) != std::string::npos);
    for (const char* ptr : {"(*identity)", "(*equal_to)", "(*not_equal_to)", "(*less)", "(*greater)"})
        CHECK(code.find(ptr) == std::string::npos);
    CHECK(text.find("return __a == __b;") != std::string::npos);
    CHECK(text.find("return !(__a == __b);") != std::string::npos);
    CHECK(text.find("return __a < __b;") != std::string::npos);
    CHECK(text.find("return __b < __a;") != std::string::npos);
    CHECK(text.find("return __a > __b") == std::string::npos);
    CHECK(text.find("return __a != __b") == std::string::npos);
    const auto cmp = text.find("struct equal_to;");
    REQUIRE(cmp != std::string::npos);
    const std::string cmp_body = text.substr(cmp);
    for (const char* stand_in : {"return true", "return false", "return 0", "return {}", "return _Tp{}"})
        CHECK(cmp_body.find(stand_in) == std::string::npos);

    for (const char* name : {"struct plus", "struct minus", "struct multiplies", "struct divides", "struct modulus",
                             "struct negate"}) {
        CHECK(count_occ(code, name) == 1);
        const std::string id = std::string(name).substr(sizeof("struct ") - 1);
        CHECK(code.find("(*" + id + ")") == std::string::npos);
    }
    for (const char* hole : {"struct plus<void>", "struct minus<void>", "struct multiplies<void>",
                             "struct divides<void>", "struct modulus<void>", "struct negate<void>"})
        CHECK(code.find(hole) == std::string::npos);
    CHECK(text.find("for one type argument.") != std::string::npos);
    CHECK(text.find("There is no transparent call") != std::string::npos);

    for (const char* name : {"struct logical_and", "struct logical_or", "struct logical_not"}) {
        CHECK(count_occ(code, name) == 1);
        const std::string id = std::string(name).substr(sizeof("struct ") - 1);
        CHECK(code.find("(*" + id + ")") == std::string::npos);
    }
    for (const char* hole : {"struct logical_and<void>", "struct logical_or<void>", "struct logical_not<void>"})
        CHECK(code.find(hole) == std::string::npos);
    CHECK(text.find("return __a && __b;") != std::string::npos);
    CHECK(text.find("return __a || __b;") != std::string::npos);
    CHECK(text.find("return !__x;") != std::string::npos);
    CHECK(count_occ(text, "constexpr decltype(auto) operator()(const _Tp& __a, const _Tp& __b) const") == 5);
    CHECK(count_occ(text, "constexpr decltype(auto) operator()(const _Tp& __x) const") == 2);
    CHECK(text.find("requires requires { __a && __b; }") != std::string::npos);
    CHECK(text.find("requires requires { __a || __b; }") != std::string::npos);
    CHECK(text.find("requires requires { !__x; }") != std::string::npos);
    CHECK(text.find("The result is that operator.") != std::string::npos);
    CHECK(text.find("A missing operator fails the template.") != std::string::npos);
    CHECK(text.find("There is no stand-in boolean") != std::string::npos);
    CHECK(code.find("#define logical_and _Prism_libstd_logical_and") != std::string::npos);
    CHECK(code.find("#define logical_or _Prism_libstd_logical_or") != std::string::npos);
    CHECK(code.find("#define logical_not _Prism_libstd_logical_not") != std::string::npos);
    CHECK(code.find("#undef logical_and") != std::string::npos);
    CHECK(code.find("#undef logical_or") != std::string::npos);
    CHECK(code.find("#undef logical_not") != std::string::npos);
    CHECK(text.find("return static_cast<bool>(__a && __b)") == std::string::npos);
    CHECK(text.find("return static_cast<bool>(__a || __b)") == std::string::npos);
    CHECK(text.find("return static_cast<bool>(!__x)") == std::string::npos);

    for (const char* name : {"struct bit_and", "struct bit_or", "struct bit_xor"}) {
        CHECK(count_occ(code, name) == 1);
        const std::string id = std::string(name).substr(sizeof("struct ") - 1);
        CHECK(code.find("(*" + id + ")") == std::string::npos);
    }
    for (const char* hole : {"struct bit_and<void>", "struct bit_or<void>", "struct bit_xor<void>"})
        CHECK(code.find(hole) == std::string::npos);
    CHECK(text.find("return __a & __b;") != std::string::npos);
    CHECK(text.find("return __a | __b;") != std::string::npos);
    CHECK(text.find("return __a ^ __b;") != std::string::npos);
    CHECK(text.find("requires requires { __a & __b; }") != std::string::npos);
    CHECK(text.find("requires requires { __a | __b; }") != std::string::npos);
    CHECK(text.find("requires requires { __a ^ __b; }") != std::string::npos);
    CHECK(text.find("There is no stand-in integer") != std::string::npos);
    CHECK(text.find("Each returns that operator.") != std::string::npos);
    CHECK(code.find("#define bit_and _Prism_libstd_bit_and") != std::string::npos);
    CHECK(code.find("#define bit_or _Prism_libstd_bit_or") != std::string::npos);
    CHECK(code.find("#define bit_xor _Prism_libstd_bit_xor") != std::string::npos);
    CHECK(code.find("#undef bit_and") != std::string::npos);
    CHECK(code.find("#undef bit_or") != std::string::npos);
    CHECK(code.find("#undef bit_xor") != std::string::npos);
    CHECK(text.find("return static_cast<int>(__a & __b)") == std::string::npos);
    CHECK(text.find("return static_cast<int>(__a | __b)") == std::string::npos);
    CHECK(text.find("return static_cast<int>(__a ^ __b)") == std::string::npos);
    CHECK(text.find("return 0;") == std::string::npos);

    CHECK(count_occ(code, "struct bit_not") == 1);
    CHECK(code.find("(*bit_not)") == std::string::npos);
    CHECK(code.find("struct bit_not<void>") == std::string::npos);
    CHECK(code.find("bit_not<>") == std::string::npos);
    CHECK(code.find("bit_not<void>") == std::string::npos);
    CHECK(text.find("return ~__x;") != std::string::npos);
    CHECK(text.find("requires requires { ~__x; }") != std::string::npos);
    CHECK(text.find("std::bit_not<> is not declared") != std::string::npos);
    CHECK(text.find("A missing operator~ fails") != std::string::npos);
    CHECK(code.find("#define bit_not _Prism_libstd_bit_not") != std::string::npos);
    CHECK(code.find("#undef bit_not") != std::string::npos);
    CHECK(text.find("return static_cast<_Tp>(~__x)") == std::string::npos);
    CHECK(text.find("return static_cast<int>(~__x)") == std::string::npos);

    struct Arm {
        const char* check;
        const char* result;
    };
    const Arm arms[] = {
        {"__prism_check(0, \"INT-SIGNED-OVF\", \"std::plus result does not fit in T\")",
         "return static_cast<_Tp>(__a + __b);"},
        {"__prism_check(0, \"FUNC-CONTRACT\", \"std::plus result does not fit in T\")",
         "return static_cast<_Tp>(__a + __b);"},
        {"__prism_check(0, \"INT-SIGNED-OVF\", \"std::minus result does not fit in T\")",
         "return static_cast<_Tp>(__a - __b);"},
        {"__prism_check(0, \"FUNC-CONTRACT\", \"std::minus result does not fit in T\")",
         "return static_cast<_Tp>(__a - __b);"},
        {"__prism_check(0, \"INT-SIGNED-OVF\", \"std::multiplies result does not fit in T\")",
         "return static_cast<_Tp>(__a * __b);"},
        {"__prism_check(0, \"FUNC-CONTRACT\", \"std::multiplies result does not fit in T\")",
         "return static_cast<_Tp>(__a * __b);"},
        {"__prism_check(0, \"FUNC-CONTRACT\", \"std::divides divisor is zero\")",
         "return static_cast<_Tp>(__a / __b);"},
        {"__prism_check(0, \"FUNC-CONTRACT\", \"std::divides of the minimum by -1\")",
         "return static_cast<_Tp>(__a / __b);"},
        {"__prism_check(0, \"FUNC-CONTRACT\", \"std::modulus divisor is zero\")",
         "return static_cast<_Tp>(__a % __b);"},
        {"__prism_check(0, \"FUNC-CONTRACT\", \"std::modulus of the minimum by -1\")",
         "return static_cast<_Tp>(__a % __b);"},
        {"__prism_check(0, \"INT-SIGNED-OVF\", \"std::negate of the most negative value\")",
         "return static_cast<_Tp>(-__x);"},
    };
    for (const Arm& arm : arms) {
        std::size_t seen = 0;
        for (std::size_t check = 0; (check = text.find(arm.check, check)) != std::string::npos; ++seen) {
            const auto trap = text.find("__builtin_trap()", check);
            const auto ret = text.find(arm.result, trap);
            REQUIRE(trap != std::string::npos);
            REQUIRE(ret != std::string::npos);
            CHECK(check < trap);
            CHECK(trap < ret);
            const std::string between = text.substr(check, trap - check);
            CHECK(between.find("return") == std::string::npos);
            CHECK(between.find(arm.result) == std::string::npos);
            check = trap + 1;
        }
        CHECK(seen >= 1);
    }
    CHECK(count_occ(text, "__prism_check(0, \"FUNC-CONTRACT\", \"std::divides divisor is zero\")") == 2);
    CHECK(count_occ(text, "__prism_check(0, \"FUNC-CONTRACT\", \"std::modulus divisor is zero\")") == 2);
}

TEST_CASE("cxx functional model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxfunctional-doctest";
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

TEST_CASE("cxx functional model: stored pointer is called and empty operator() is a check") {
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
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "functional_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd((models / "functional").string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a.size() > 5);
    CHECK(a == b);
    bool saw_stored = false;
    bool saw_move = false;
    bool saw_held = false;
    bool saw_invoke = false;
    bool saw_invoke_ptr = false;
    bool saw_invoke_add = false;
    bool saw_invoke_ref = false;
    bool saw_invoke_rv = false;
    bool saw_invoke_void = false;
    bool saw_invoke_nx = false;
    bool saw_invoke_slot = false;
    bool saw_invoke_held = false;
    bool saw_not_zero = false;
    bool saw_not_copy = false;
    bool saw_not_flag = false;
    bool saw_not_ref = false;
    bool saw_not_rv = false;
    bool saw_not_both = false;
    bool saw_not_nx = false;
    bool saw_not_ptr = false;
    bool saw_not_obj = false;
    bool saw_not_bit = false;
    bool saw_not_hold = false;
    bool saw_id_lv = false;
    bool saw_id_same = false;
    bool saw_id_rv = false;
    bool saw_id_mo = false;
    bool saw_eq = false;
    bool saw_ne = false;
    bool saw_lt = false;
    bool saw_gt = false;
    bool saw_tr = false;
    bool saw_ptr = false;
    bool saw_obj = false;
    bool saw_add = false;
    bool saw_sub = false;
    bool saw_mul = false;
    bool saw_div = false;
    bool saw_divn = false;
    bool saw_neg = false;
    bool saw_uadd = false;
    bool saw_usub = false;
    bool saw_umul = false;
    bool saw_udiv = false;
    bool saw_uneg = false;
    bool saw_ll = false;
    bool saw_sc = false;
    bool saw_fp = false;
    bool saw_bdiv = false;
    bool saw_objadd = false;
    bool saw_quot = false;
    bool saw_bit = false;
    bool saw_land = false;
    bool saw_landc = false;
    bool saw_lor = false;
    bool saw_lnot = false;
    bool saw_lbool = false;
    bool saw_blog = false;
    bool saw_band = false;
    bool saw_bor = false;
    bool saw_bxor = false;
    bool saw_bbool = false;
    bool saw_ubit = false;
    bool saw_bobj = false;
    bool saw_bandonly = false;
    bool saw_bnot = false;
    bool saw_bnotc = false;
    bool saw_unot = false;
    bool saw_bnotobj = false;
    bool saw_bnotonly = false;
    for (const auto& line : b) {
        if (line == "call 42 calls=1") saw_stored = true;
        if (line == "move src=0 dst=1 calls=1") saw_move = true;
        if (line == "held 5") saw_held = true;
        if (line == "invoke 42 calls=5") saw_invoke = true;
        if (line == "invoke ptr 42 calls=6") saw_invoke_ptr = true;
        if (line == "invoke add 42 calls=7") saw_invoke_add = true;
        if (line == "invoke ref 11 n=11 calls=8") saw_invoke_ref = true;
        if (line == "invoke rv 10 calls=9") saw_invoke_rv = true;
        if (line == "invoke void 10") saw_invoke_void = true;
        if (line == "invoke nx 42 calls=11") saw_invoke_nx = true;
        if (line == "invoke slot 9") saw_invoke_slot = true;
        if (line == "invoke held 5") saw_invoke_held = true;
        if (line == "not zero 1 calls=12") saw_not_zero = true;
        if (line == "not copy 0 1 calls=14") saw_not_copy = true;
        if (line == "not flag 1 0 calls=16") saw_not_flag = true;
        if (line == "not ref 0 m=4 calls=17") saw_not_ref = true;
        if (line == "not rv 0 calls=18") saw_not_rv = true;
        if (line == "not both 1 calls=19") saw_not_both = true;
        if (line == "not nx 1 calls=20") saw_not_nx = true;
        if (line == "not ptr 1 0 calls=22") saw_not_ptr = true;
        if (line == "not obj 1 0 calls=24") saw_not_obj = true;
        if (line == "not bit 1 bit=0 calls=25") saw_not_bit = true;
        if (line == "not hold") saw_not_hold = true;
        if (line == "id lv 7 x=7") saw_id_lv = true;
        if (line == "id same 8") saw_id_same = true;
        if (line == "id rv 8") saw_id_rv = true;
        if (line == "id mo 3 3 moves=0") saw_id_mo = true;
        if (line == "eq 1 0") saw_eq = true;
        if (line == "ne 0 1") saw_ne = true;
        if (line == "lt 1 0") saw_lt = true;
        if (line == "gt 1 0") saw_gt = true;
        if (line == "tr 1 1 1 0") saw_tr = true;
        if (line == "ptr 1") saw_ptr = true;
        if (line == "obj 1 0 1 1") saw_obj = true;
        if (line == "add 5 -5") saw_add = true;
        if (line == "sub 1") saw_sub = true;
        if (line == "mul -12") saw_mul = true;
        if (line == "div 3 1") saw_div = true;
        if (line == "divn -3 -1") saw_divn = true;
        if (line == "neg -4 4 0") saw_neg = true;
        if (line == "uadd 5") saw_uadd = true;
        if (line == "usub 3") saw_usub = true;
        if (line == "umul 6") saw_umul = true;
        if (line == "udiv 3 1") saw_udiv = true;
        if (line == "uneg 0 4294967295") saw_uneg = true;
        if (line == "ll 42") saw_ll = true;
        if (line == "sc 5") saw_sc = true;
        if (line == "fp 4.0") saw_fp = true;
        if (line == "bdiv 1") saw_bdiv = true;
        if (line == "objadd 5") saw_objadd = true;
        if (line == "quot 3 1") saw_quot = true;
        if (line == "bit 2") saw_bit = true;
        if (line == "land 0") saw_land = true;
        if (line == "landc 0 1") saw_landc = true;
        if (line == "lor 1 0") saw_lor = true;
        if (line == "lnot 1 0") saw_lnot = true;
        if (line == "lbool 0 0 0") saw_lbool = true;
        if (line == "blog 0 1 1") saw_blog = true;
        if (line == "band 2 1") saw_band = true;
        if (line == "bor 7 3") saw_bor = true;
        if (line == "bxor 5 0") saw_bxor = true;
        if (line == "bbool 0 0 0") saw_bbool = true;
        if (line == "ubit 2 7 5") saw_ubit = true;
        if (line == "bobj 2 7 5") saw_bobj = true;
        if (line == "bandonly 2") saw_bandonly = true;
        if (line == "bnot -1") saw_bnot = true;
        if (line == "bnotc -2 0") saw_bnotc = true;
        if (line == "unot 4294967295") saw_unot = true;
        if (line == "bnotobj -7") saw_bnotobj = true;
        if (line == "bnotonly -1") saw_bnotonly = true;
        CHECK(line.find("call 0") == std::string::npos);
        CHECK(line != "held 0");
        CHECK(line != "invoke held 0");
        CHECK(line != "reassign 0 calls=3");
        CHECK(line != "copy call 0 calls=4");
    }
    CHECK(saw_stored);
    CHECK(saw_move);
    CHECK(saw_held);
    CHECK(saw_invoke);
    CHECK(saw_invoke_ptr);
    CHECK(saw_invoke_add);
    CHECK(saw_invoke_ref);
    CHECK(saw_invoke_rv);
    CHECK(saw_invoke_void);
    CHECK(saw_invoke_nx);
    CHECK(saw_invoke_slot);
    CHECK(saw_invoke_held);
    CHECK(saw_not_zero);
    CHECK(saw_not_copy);
    CHECK(saw_not_flag);
    CHECK(saw_not_ref);
    CHECK(saw_not_rv);
    CHECK(saw_not_both);
    CHECK(saw_not_nx);
    CHECK(saw_not_ptr);
    CHECK(saw_not_obj);
    CHECK(saw_not_bit);
    CHECK(saw_not_hold);
    CHECK(saw_id_lv);
    CHECK(saw_id_same);
    CHECK(saw_id_rv);
    CHECK(saw_id_mo);
    CHECK(saw_eq);
    CHECK(saw_ne);
    CHECK(saw_lt);
    CHECK(saw_gt);
    CHECK(saw_tr);
    CHECK(saw_ptr);
    CHECK(saw_obj);
    CHECK(saw_add);
    CHECK(saw_sub);
    CHECK(saw_mul);
    CHECK(saw_div);
    CHECK(saw_divn);
    CHECK(saw_neg);
    CHECK(saw_uadd);
    CHECK(saw_usub);
    CHECK(saw_umul);
    CHECK(saw_udiv);
    CHECK(saw_uneg);
    CHECK(saw_ll);
    CHECK(saw_sc);
    CHECK(saw_fp);
    CHECK(saw_bdiv);
    CHECK(saw_objadd);
    CHECK(saw_quot);
    CHECK(saw_bit);
    CHECK(saw_land);
    CHECK(saw_landc);
    CHECK(saw_lor);
    CHECK(saw_lnot);
    CHECK(saw_lbool);
    CHECK(saw_blog);
    CHECK(saw_band);
    CHECK(saw_bor);
    CHECK(saw_bxor);
    CHECK(saw_bbool);
    CHECK(saw_ubit);
    CHECK(saw_bobj);
    CHECK(saw_bandonly);
    CHECK(saw_bnot);
    CHECK(saw_bnotc);
    CHECK(saw_unot);
    CHECK(saw_bnotobj);
    CHECK(saw_bnotonly);
    CHECK(empty_call_is_check(mod));
    CHECK(null_invoke_is_check(mod));
    CHECK(null_not_fn_is_check(mod));
    CHECK(arith_trap(mod, "plus-ovf", "INT-SIGNED-OVF", "std::plus result does not fit in T"));
    CHECK(arith_trap(mod, "plus-min", "INT-SIGNED-OVF", "std::plus result does not fit in T"));
    CHECK(arith_trap(mod, "plus-uovf", "FUNC-CONTRACT", "std::plus result does not fit in T"));
    CHECK(arith_trap(mod, "plus-ll", "INT-SIGNED-OVF", "std::plus result does not fit in T"));
    CHECK(arith_trap(mod, "plus-sc", "INT-SIGNED-OVF", "std::plus result does not fit in T"));
    CHECK(arith_trap(mod, "minus-ovf", "INT-SIGNED-OVF", "std::minus result does not fit in T"));
    CHECK(arith_trap(mod, "minus-pos", "INT-SIGNED-OVF", "std::minus result does not fit in T"));
    CHECK(arith_trap(mod, "minus-uovf", "FUNC-CONTRACT", "std::minus result does not fit in T"));
    CHECK(arith_trap(mod, "mul-min", "INT-SIGNED-OVF", "std::multiplies result does not fit in T"));
    CHECK(arith_trap(mod, "mul-max", "INT-SIGNED-OVF", "std::multiplies result does not fit in T"));
    CHECK(arith_trap(mod, "mul-neg", "INT-SIGNED-OVF", "std::multiplies result does not fit in T"));
    CHECK(arith_trap(mod, "mul-uovf", "FUNC-CONTRACT", "std::multiplies result does not fit in T"));
    CHECK(arith_trap(mod, "mul-sc", "INT-SIGNED-OVF", "std::multiplies result does not fit in T"));
    CHECK(arith_trap(mod, "div-zero", "FUNC-CONTRACT", "std::divides divisor is zero"));
    CHECK(arith_trap(mod, "div-min", "FUNC-CONTRACT", "std::divides of the minimum by -1"));
    CHECK(arith_trap(mod, "div-sc", "FUNC-CONTRACT", "std::divides of the minimum by -1"));
    CHECK(arith_trap(mod, "div-uzero", "FUNC-CONTRACT", "std::divides divisor is zero"));
    CHECK(arith_trap(mod, "div-fzero", "FUNC-CONTRACT", "std::divides divisor is zero"));
    CHECK(arith_trap(mod, "div-bzero", "FUNC-CONTRACT", "std::divides divisor is zero"));
    CHECK(arith_trap(mod, "mod-zero", "FUNC-CONTRACT", "std::modulus divisor is zero"));
    CHECK(arith_trap(mod, "mod-min", "FUNC-CONTRACT", "std::modulus of the minimum by -1"));
    CHECK(arith_trap(mod, "mod-uzero", "FUNC-CONTRACT", "std::modulus divisor is zero"));
    CHECK(arith_trap(mod, "mod-bzero", "FUNC-CONTRACT", "std::modulus divisor is zero"));
    CHECK(arith_trap(mod, "neg-min", "INT-SIGNED-OVF", "std::negate of the most negative value"));
    CHECK(arith_trap(mod, "neg-sc", "INT-SIGNED-OVF", "std::negate of the most negative value"));

    const fs::path rejected = c.tmp / "rejected.cpp";
    std::ofstream(rejected) << "#include <functional>\n"
                               "int id(int x) { return x; }\n"
                               "struct F { int operator()(int x) const { return x; } };\n"
                               "struct S { int m(int x) { return x; } };\n"
                               "int use_bind() { return std::bind(id, 1)(); }\n"
                               "int use_mem(S& s) { return std::mem_fn(&S::m)(s, 1); }\n"
                               "int use_mem_invoke(S& s) { return std::invoke(&S::m, s, 1); }\n"
                               "void use_ref(int& x) { (void)std::ref(x); }\n"
                               "int use_fun() { F f; return std::invoke(f, 2); }\n"
                               "int use_fn(std::function<int(int)> f) { return std::invoke(f, 2); }\n";
    std::string rejected_err;
    CHECK(syntax_only(c.cxx, rejected, false, &rejected_err));
    CHECK_FALSE(syntax_only(c.cxx, rejected, true, &rejected_err));

    const fs::path accepted = c.tmp / "accepted.cpp";
    std::ofstream(accepted) << "#include <functional>\n"
                               "int id(int x) { return x; }\n"
                               "int idn(int x) noexcept { return x + 1; }\n"
                               "int use() { return std::invoke(id, 2) + std::invoke(idn, 2); }\n";
    std::string accepted_err;
    CHECK(syntax_only(c.cxx, accepted, false, &accepted_err));
    CHECK(syntax_only(c.cxx, accepted, true, &accepted_err));

    const fs::path not_ok = c.tmp / "not_fn_ok.cpp";
    std::ofstream(not_ok) << "#include <functional>\n"
                             "bool even(int x) { return x % 2 == 0; }\n"
                             "bool even_nx(int x) noexcept { return x % 2 == 0; }\n"
                             "int flag(int x) { return x; }\n"
                             "int* idp(int* p) { return p; }\n"
                             "bool bump(int& x) { return x > 0; }\n"
                             "bool g_acc = false;\n"
                             "bool& acc_bit() { return g_acc; }\n"
                             "int use(int x, int* p) {\n"
                             "  return static_cast<int>(std::not_fn(even)(x))\n"
                             "       + static_cast<int>(std::not_fn(even_nx)(x))\n"
                             "       + static_cast<int>(std::not_fn(flag)(x))\n"
                             "       + static_cast<int>(std::not_fn(idp)(p))\n"
                             "       + static_cast<int>(std::not_fn(bump)(x))\n"
                             "       + static_cast<int>(std::not_fn(acc_bit)());\n"
                             "}\n";
    std::string not_ok_err;
    CHECK(syntax_only(c.cxx, not_ok, false, &not_ok_err));
    CHECK(syntax_only(c.cxx, not_ok, true, &not_ok_err));

    const fs::path not_fun = c.tmp / "not_fn_fun.cpp";
    std::ofstream(not_fun) << "#include <functional>\n"
                              "struct F { bool operator()(int x) const { return x != 0; } };\n"
                              "int use(int x) { return static_cast<int>(std::not_fn(F{})(x)); }\n";
    std::string not_fun_err;
    CHECK(syntax_only(c.cxx, not_fun, false, &not_fun_err));
    CHECK_FALSE(syntax_only(c.cxx, not_fun, true, &not_fun_err));

    const fs::path not_mem = c.tmp / "not_fn_mem.cpp";
    std::ofstream(not_mem) << "#include <functional>\n"
                              "struct S { bool m(int) const { return true; } };\n"
                              "int use(S& s) { return static_cast<int>(std::not_fn(&S::m)(s, 1)); }\n";
    std::string not_mem_err;
    CHECK(syntax_only(c.cxx, not_mem, false, &not_mem_err));
    CHECK_FALSE(syntax_only(c.cxx, not_mem, true, &not_mem_err));

    const fs::path not_void = c.tmp / "not_fn_void.cpp";
    std::ofstream(not_void) << "#include <functional>\n"
                               "void mark() {}\n"
                               "auto use() { return std::not_fn(mark); }\n";
    std::string not_void_err;
    CHECK(syntax_only(c.cxx, not_void, false, &not_void_err));
    CHECK_FALSE(syntax_only(c.cxx, not_void, true, &not_void_err));

    const fs::path not_e = c.tmp / "not_fn_e.cpp";
    std::ofstream(not_e) << "#include <functional>\n"
                            "struct E { int v; explicit E(int x) : v(x) {} };\n"
                            "E make_e() { return E(1); }\n"
                            "auto use() { return std::not_fn(make_e); }\n";
    std::string not_e_err;
    CHECK(syntax_only(c.cxx, not_e, false, &not_e_err));
    CHECK_FALSE(syntax_only(c.cxx, not_e, true, &not_e_err));

    const fs::path not_ell = c.tmp / "not_fn_ell.cpp";
    std::ofstream(not_ell) << "#include <functional>\n"
                              "int print(const char*, ...) { return 0; }\n"
                              "auto use() { return std::not_fn(print); }\n";
    std::string not_ell_err;
    CHECK(syntax_only(c.cxx, not_ell, false, &not_ell_err));
    CHECK_FALSE(syntax_only(c.cxx, not_ell, true, &not_ell_err));

    const fs::path cmp_ok = c.tmp / "cmp_ok.cpp";
    std::ofstream(cmp_ok) << "#include <functional>\n"
                             "struct ND { int v; explicit ND(int x) : v(x) {} };\n"
                             "struct EqOnly {\n"
                             "  int v;\n"
                             "  friend bool operator==(const EqOnly& a, const EqOnly& b) { return a.v == b.v; }\n"
                             "};\n"
                             "struct LtOnly {\n"
                             "  int v;\n"
                             "  friend bool operator<(const LtOnly& a, const LtOnly& b) { return a.v < b.v; }\n"
                             "};\n"
                             "int use(int x, EqOnly e, LtOnly a, LtOnly b) {\n"
                             "  return std::identity{}(ND(x)).v\n"
                             "       + static_cast<int>(std::equal_to<int>{}(x, x))\n"
                             "       + static_cast<int>(std::not_equal_to<EqOnly>{}(e, e))\n"
                             "       + static_cast<int>(std::not_equal_to<>{}(e, e))\n"
                             "       + static_cast<int>(std::less<LtOnly>{}(a, b))\n"
                             "       + static_cast<int>(std::less<>{}(1, 2.5));\n"
                             "}\n";
    std::string cmp_ok_err;
    CHECK(syntax_only(c.cxx, cmp_ok, false, &cmp_ok_err));
    CHECK(syntax_only(c.cxx, cmp_ok, true, &cmp_ok_err));

    // greater is b < a, so a type with < and no > still compiles here.
    // libstdc++ greater calls operator>, so that translation unit fails there.
    const fs::path cmp_gt = c.tmp / "cmp_gt.cpp";
    std::ofstream(cmp_gt) << "#include <functional>\n"
                             "struct LtOnly {\n"
                             "  int v;\n"
                             "  friend bool operator<(const LtOnly& a, const LtOnly& b) { return a.v < b.v; }\n"
                             "};\n"
                             "int use(LtOnly a, LtOnly b) {\n"
                             "  return static_cast<int>(std::greater<LtOnly>{}(a, b))\n"
                             "       + static_cast<int>(std::greater<>{}(a, b));\n"
                             "}\n";
    std::string cmp_gt_err;
    CHECK_FALSE(syntax_only(c.cxx, cmp_gt, false, &cmp_gt_err));
    CHECK(syntax_only(c.cxx, cmp_gt, true, &cmp_gt_err));

    const fs::path no_eq = c.tmp / "no_eq.cpp";
    std::ofstream(no_eq) << "#include <functional>\n"
                            "struct NoEq { int v; };\n"
                            "int use(NoEq a, NoEq b) {\n"
                            "  return static_cast<int>(std::equal_to<NoEq>{}(a, b))\n"
                            "       + static_cast<int>(std::not_equal_to<NoEq>{}(a, b));\n"
                            "}\n";
    std::string no_eq_err;
    CHECK_FALSE(syntax_only(c.cxx, no_eq, false, &no_eq_err));
    CHECK_FALSE(syntax_only(c.cxx, no_eq, true, &no_eq_err));

    const fs::path no_lt = c.tmp / "no_lt.cpp";
    std::ofstream(no_lt) << "#include <functional>\n"
                            "struct NoLt { int v; };\n"
                            "int use(NoLt a, NoLt b) {\n"
                            "  return static_cast<int>(std::less<NoLt>{}(a, b))\n"
                            "       + static_cast<int>(std::greater<NoLt>{}(a, b));\n"
                            "}\n";
    std::string no_lt_err;
    CHECK_FALSE(syntax_only(c.cxx, no_lt, false, &no_lt_err));
    CHECK_FALSE(syntax_only(c.cxx, no_lt, true, &no_lt_err));

    const fs::path arith_ok = c.tmp / "arith_ok.cpp";
    std::ofstream(arith_ok) << "#include <functional>\n"
                               "struct Sum { int v; friend Sum operator+(Sum a, Sum b) { return Sum{a.v + b.v}; } };\n"
                               "struct Quot {\n"
                               "  int v;\n"
                               "  friend Quot operator/(Quot a, Quot b) { return Quot{a.v / b.v}; }\n"
                               "  friend Quot operator%(Quot a, Quot b) { return Quot{a.v % b.v}; }\n"
                               "};\n"
                               "int use(int a, int b, unsigned u, Sum s, Quot q) {\n"
                               "  return std::plus<int>{}(a, b) + std::minus<int>{}(a, b)\n"
                               "       + std::multiplies<int>{}(a, b) + std::divides<int>{}(a, b)\n"
                               "       + std::modulus<int>{}(a, b) + std::negate<int>{}(a)\n"
                               "       + static_cast<int>(std::plus<unsigned>{}(u, 1u))\n"
                               "       + std::plus<Sum>{}(s, s).v + std::divides<Quot>{}(q, q).v\n"
                               "       + std::modulus<Quot>{}(q, q).v + std::bit_and<int>{}(a, b);\n"
                               "}\n";
    std::string arith_ok_err;
    CHECK(syntax_only(c.cxx, arith_ok, false, &arith_ok_err));
    CHECK(syntax_only(c.cxx, arith_ok, true, &arith_ok_err));

    // The transparent call is not declared here. libstdc++ still has it.
    const fs::path arith_tr = c.tmp / "arith_tr.cpp";
    std::ofstream(arith_tr) << "#include <functional>\n"
                               "int use(int a, double b) {\n"
                               "  return std::plus<>{}(a, a) + static_cast<int>(std::negate<>{}(b));\n"
                               "}\n";
    std::string arith_tr_err;
    CHECK(syntax_only(c.cxx, arith_tr, false, &arith_tr_err));
    CHECK_FALSE(syntax_only(c.cxx, arith_tr, true, &arith_tr_err));

    const fs::path no_add = c.tmp / "no_add.cpp";
    std::ofstream(no_add) << "#include <functional>\n"
                             "struct NoAdd { int v; };\n"
                             "int use(NoAdd a, NoAdd b) { return std::plus<NoAdd>{}(a, b).v; }\n";
    std::string no_add_err;
    CHECK_FALSE(syntax_only(c.cxx, no_add, false, &no_add_err));
    CHECK_FALSE(syntax_only(c.cxx, no_add, true, &no_add_err));

    const fs::path add_only = c.tmp / "add_only.cpp";
    std::ofstream(add_only) << "#include <functional>\n"
                               "struct AddOnly {\n"
                               "  int v;\n"
                               "  friend AddOnly operator+(AddOnly a, AddOnly b) { return AddOnly{a.v + b.v}; }\n"
                               "};\n"
                               "int use(AddOnly a, AddOnly b) {\n"
                               "  return std::plus<AddOnly>{}(a, b).v + std::minus<AddOnly>{}(a, b).v;\n"
                               "}\n";
    std::string add_only_err;
    CHECK_FALSE(syntax_only(c.cxx, add_only, false, &add_only_err));
    CHECK_FALSE(syntax_only(c.cxx, add_only, true, &add_only_err));

    const fs::path log_ok = c.tmp / "log_ok.cpp";
    std::ofstream(log_ok) << "#include <functional>\n"
                             "struct AndOnly {\n"
                             "  int v;\n"
                             "  friend bool operator&&(AndOnly a, AndOnly b) { return a.v && b.v; }\n"
                             "};\n"
                             "struct BoolOp {\n"
                             "  int v;\n"
                             "  friend bool operator&&(BoolOp a, BoolOp b) { return a.v && b.v; }\n"
                             "  friend bool operator||(BoolOp a, BoolOp b) { return a.v || b.v; }\n"
                             "  friend bool operator!(BoolOp a) { return !a.v; }\n"
                             "};\n"
                             "int use(int a, bool b, AndOnly x, BoolOp p, BoolOp q) {\n"
                             "  return static_cast<int>(std::logical_and<int>{}(a, 1))\n"
                             "       + static_cast<int>(std::logical_or<int>{}(a, 0))\n"
                             "       + static_cast<int>(std::logical_not<int>{}(a))\n"
                             "       + static_cast<int>(std::logical_and<bool>{}(b, true))\n"
                             "       + static_cast<int>(std::logical_or<bool>{}(b, false))\n"
                             "       + static_cast<int>(std::logical_not<bool>{}(b))\n"
                             "       + static_cast<int>(std::logical_and<AndOnly>{}(x, x))\n"
                             "       + static_cast<int>(std::logical_and<BoolOp>{}(p, q))\n"
                             "       + static_cast<int>(std::logical_or<BoolOp>{}(p, q))\n"
                             "       + static_cast<int>(std::logical_not<BoolOp>{}(p))\n"
                             "       + std::bit_and<int>{}(a, 1);\n"
                             "}\n";
    std::string log_ok_err;
    CHECK(syntax_only(c.cxx, log_ok, false, &log_ok_err));
    CHECK(syntax_only(c.cxx, log_ok, true, &log_ok_err));

    // The operator result is Gate, not a bool. libstdc++ returns bool, so .v fails there.
    const fs::path log_gate = c.tmp / "log_gate.cpp";
    std::ofstream(log_gate) << "#include <functional>\n"
                               "struct Gate {\n"
                               "  int v;\n"
                               "  friend Gate operator&&(Gate a, Gate b) { return Gate{a.v && b.v}; }\n"
                               "  friend Gate operator||(Gate a, Gate b) { return Gate{a.v || b.v}; }\n"
                               "  friend Gate operator!(Gate a) { return Gate{!a.v}; }\n"
                               "};\n"
                               "int use(Gate a, Gate b) {\n"
                               "  return std::logical_and<Gate>{}(a, b).v\n"
                               "       + std::logical_or<Gate>{}(a, b).v\n"
                               "       + std::logical_not<Gate>{}(a).v;\n"
                               "}\n";
    std::string log_gate_err;
    CHECK_FALSE(syntax_only(c.cxx, log_gate, false, &log_gate_err));
    CHECK(syntax_only(c.cxx, log_gate, true, &log_gate_err));

    // The transparent call is not declared here. libstdc++ still has it.
    const fs::path log_tr = c.tmp / "log_tr.cpp";
    std::ofstream(log_tr) << "#include <functional>\n"
                             "int use(int a, bool b) {\n"
                             "  return static_cast<int>(std::logical_and<>{}(a, 0))\n"
                             "       + static_cast<int>(std::logical_or<>{}(a, 1))\n"
                             "       + static_cast<int>(std::logical_not<>{}(b));\n"
                             "}\n";
    std::string log_tr_err;
    CHECK(syntax_only(c.cxx, log_tr, false, &log_tr_err));
    CHECK_FALSE(syntax_only(c.cxx, log_tr, true, &log_tr_err));

    const fs::path log_none = c.tmp / "log_none.cpp";
    std::ofstream(log_none) << "#include <functional>\n"
                               "struct NoLog { int v; };\n"
                               "int use(NoLog a, NoLog b) {\n"
                               "  return static_cast<int>(std::logical_and<NoLog>{}(a, b))\n"
                               "       + static_cast<int>(std::logical_or<NoLog>{}(a, b))\n"
                               "       + static_cast<int>(std::logical_not<NoLog>{}(a));\n"
                               "}\n";
    std::string log_none_err;
    CHECK_FALSE(syntax_only(c.cxx, log_none, false, &log_none_err));
    CHECK_FALSE(syntax_only(c.cxx, log_none, true, &log_none_err));

    const fs::path log_or_missing = c.tmp / "log_or_missing.cpp";
    std::ofstream(log_or_missing) << "#include <functional>\n"
                                     "struct AndOnly {\n"
                                     "  int v;\n"
                                     "  friend bool operator&&(AndOnly a, AndOnly b) { return a.v && b.v; }\n"
                                     "};\n"
                                     "int use(AndOnly a, AndOnly b) {\n"
                                     "  return static_cast<int>(std::logical_and<AndOnly>{}(a, b))\n"
                                     "       + static_cast<int>(std::logical_or<AndOnly>{}(a, b));\n"
                                     "}\n";
    std::string log_or_missing_err;
    CHECK_FALSE(syntax_only(c.cxx, log_or_missing, false, &log_or_missing_err));
    CHECK_FALSE(syntax_only(c.cxx, log_or_missing, true, &log_or_missing_err));

    const fs::path bit_ok = c.tmp / "bit_ok.cpp";
    std::ofstream(bit_ok) << "#include <functional>\n"
                             "struct AndOnly {\n"
                             "  int v;\n"
                             "  friend AndOnly operator&(AndOnly a, AndOnly b) { return AndOnly{a.v & b.v}; }\n"
                             "};\n"
                             "struct Bits {\n"
                             "  int v;\n"
                             "  friend Bits operator&(Bits a, Bits b) { return Bits{a.v & b.v}; }\n"
                             "  friend Bits operator|(Bits a, Bits b) { return Bits{a.v | b.v}; }\n"
                             "  friend Bits operator^(Bits a, Bits b) { return Bits{a.v ^ b.v}; }\n"
                             "};\n"
                             "int use(int a, bool b, unsigned u, AndOnly x, Bits p, Bits q) {\n"
                             "  return std::bit_and<int>{}(a, 1) + std::bit_or<int>{}(a, 1)\n"
                             "       + std::bit_xor<int>{}(a, 1)\n"
                             "       + static_cast<int>(std::bit_and<bool>{}(b, true))\n"
                             "       + static_cast<int>(std::bit_or<bool>{}(b, false))\n"
                             "       + static_cast<int>(std::bit_xor<bool>{}(b, true))\n"
                             "       + static_cast<int>(std::bit_and<unsigned>{}(u, 1u))\n"
                             "       + static_cast<int>(std::bit_or<unsigned>{}(u, 1u))\n"
                             "       + static_cast<int>(std::bit_xor<unsigned>{}(u, 1u))\n"
                             "       + std::bit_and<AndOnly>{}(x, x).v\n"
                             "       + std::bit_and<Bits>{}(p, q).v\n"
                             "       + std::bit_or<Bits>{}(p, q).v\n"
                             "       + std::bit_xor<Bits>{}(p, q).v\n"
                             "       + std::bit_not<int>{}(a)\n"
                             "       + static_cast<int>(std::logical_and<int>{}(a, 1));\n"
                             "}\n";
    std::string bit_ok_err;
    CHECK(syntax_only(c.cxx, bit_ok, false, &bit_ok_err));
    CHECK(syntax_only(c.cxx, bit_ok, true, &bit_ok_err));

    // The operator result is BitWord, not BitMask. libstdc++ returns BitMask, so .v fails there.
    const fs::path bit_word = c.tmp / "bit_word.cpp";
    std::ofstream(bit_word) << "#include <functional>\n"
                               "struct BitWord { int v; };\n"
                               "struct BitMask {\n"
                               "  int v;\n"
                               "  friend BitWord operator&(BitMask a, BitMask b) { return BitWord{a.v & b.v}; }\n"
                               "  friend BitWord operator|(BitMask a, BitMask b) { return BitWord{a.v | b.v}; }\n"
                               "  friend BitWord operator^(BitMask a, BitMask b) { return BitWord{a.v ^ b.v}; }\n"
                               "};\n"
                               "int use(BitMask a, BitMask b) {\n"
                               "  return std::bit_and<BitMask>{}(a, b).v\n"
                               "       + std::bit_or<BitMask>{}(a, b).v\n"
                               "       + std::bit_xor<BitMask>{}(a, b).v;\n"
                               "}\n";
    std::string bit_word_err;
    CHECK_FALSE(syntax_only(c.cxx, bit_word, false, &bit_word_err));
    CHECK(syntax_only(c.cxx, bit_word, true, &bit_word_err));

    // The transparent call is not declared here. libstdc++ still has it.
    const fs::path bit_tr = c.tmp / "bit_tr.cpp";
    std::ofstream(bit_tr) << "#include <functional>\n"
                             "int use(int a) {\n"
                             "  return std::bit_and<>{}(a, 1) + std::bit_or<>{}(a, 1) + std::bit_xor<>{}(a, 1);\n"
                             "}\n";
    std::string bit_tr_err;
    CHECK(syntax_only(c.cxx, bit_tr, false, &bit_tr_err));
    CHECK_FALSE(syntax_only(c.cxx, bit_tr, true, &bit_tr_err));

    const fs::path bit_none = c.tmp / "bit_none.cpp";
    std::ofstream(bit_none) << "#include <functional>\n"
                               "struct NoBit { int v; };\n"
                               "int use(NoBit a, NoBit b) {\n"
                               "  return std::bit_and<NoBit>{}(a, b).v\n"
                               "       + std::bit_or<NoBit>{}(a, b).v\n"
                               "       + std::bit_xor<NoBit>{}(a, b).v;\n"
                               "}\n";
    std::string bit_none_err;
    CHECK_FALSE(syntax_only(c.cxx, bit_none, false, &bit_none_err));
    CHECK_FALSE(syntax_only(c.cxx, bit_none, true, &bit_none_err));

    const fs::path bit_or_missing = c.tmp / "bit_or_missing.cpp";
    std::ofstream(bit_or_missing) << "#include <functional>\n"
                                     "struct AndOnly {\n"
                                     "  int v;\n"
                                     "  friend AndOnly operator&(AndOnly a, AndOnly b) { return AndOnly{a.v & b.v}; }\n"
                                     "};\n"
                                     "int use(AndOnly a, AndOnly b) {\n"
                                     "  return std::bit_and<AndOnly>{}(a, b).v + std::bit_or<AndOnly>{}(a, b).v;\n"
                                     "}\n";
    std::string bit_or_missing_err;
    CHECK_FALSE(syntax_only(c.cxx, bit_or_missing, false, &bit_or_missing_err));
    CHECK_FALSE(syntax_only(c.cxx, bit_or_missing, true, &bit_or_missing_err));

    const fs::path bnot_ok = c.tmp / "bnot_ok.cpp";
    std::ofstream(bnot_ok) << "#include <functional>\n"
                              "struct NotOnly {\n"
                              "  int v;\n"
                              "  friend NotOnly operator~(NotOnly a) { return NotOnly{~a.v}; }\n"
                              "};\n"
                              "struct NotBits {\n"
                              "  int v;\n"
                              "  friend NotBits operator~(NotBits a) { return NotBits{~a.v}; }\n"
                              "};\n"
                              "int use(int a, bool b, unsigned u, NotOnly x, NotBits p) {\n"
                              "  return std::bit_not<int>{}(a)\n"
                              "       + static_cast<int>(std::bit_not<bool>{}(b))\n"
                              "       + static_cast<int>(std::bit_not<unsigned>{}(u))\n"
                              "       + std::bit_not<NotOnly>{}(x).v\n"
                              "       + std::bit_not<NotBits>{}(p).v\n"
                              "       + std::bit_and<int>{}(a, 1);\n"
                              "}\n";
    std::string bnot_ok_err;
    CHECK(syntax_only(c.cxx, bnot_ok, false, &bnot_ok_err));
    CHECK(syntax_only(c.cxx, bnot_ok, true, &bnot_ok_err));

    // The operator result is BitWord, not BitMask. libstdc++ returns BitMask, so .v fails there.
    const fs::path bnot_word = c.tmp / "bnot_word.cpp";
    std::ofstream(bnot_word) << "#include <functional>\n"
                                "struct BitWord { int v; };\n"
                                "struct BitMask {\n"
                                "  int v;\n"
                                "  friend BitWord operator~(BitMask a) { return BitWord{~a.v}; }\n"
                                "};\n"
                                "int use(BitMask a) {\n"
                                "  return std::bit_not<BitMask>{}(a).v;\n"
                                "}\n";
    std::string bnot_word_err;
    CHECK_FALSE(syntax_only(c.cxx, bnot_word, false, &bnot_word_err));
    CHECK(syntax_only(c.cxx, bnot_word, true, &bnot_word_err));

    // The transparent call is not declared here. libstdc++ still has it.
    const fs::path bnot_tr = c.tmp / "bnot_tr.cpp";
    std::ofstream(bnot_tr) << "#include <functional>\n"
                              "int use(int a) {\n"
                              "  return std::bit_not<>{}(a);\n"
                              "}\n";
    std::string bnot_tr_err;
    CHECK(syntax_only(c.cxx, bnot_tr, false, &bnot_tr_err));
    CHECK_FALSE(syntax_only(c.cxx, bnot_tr, true, &bnot_tr_err));

    const fs::path bnot_none = c.tmp / "bnot_none.cpp";
    std::ofstream(bnot_none) << "#include <functional>\n"
                                "struct NoTilde { int v; };\n"
                                "int use(NoTilde a) {\n"
                                "  return std::bit_not<NoTilde>{}(a).v;\n"
                                "}\n";
    std::string bnot_none_err;
    CHECK_FALSE(syntax_only(c.cxx, bnot_none, false, &bnot_none_err));
    CHECK_FALSE(syntax_only(c.cxx, bnot_none, true, &bnot_none_err));
}
