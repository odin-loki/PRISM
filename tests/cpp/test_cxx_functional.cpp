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
    CHECK(empty_call_is_check(mod));
    CHECK(null_invoke_is_check(mod));
    CHECK(null_not_fn_is_check(mod));

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
}
