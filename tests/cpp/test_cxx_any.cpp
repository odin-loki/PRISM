// std::any model (src/prism/pir/models/cxx/any). Same shape as
// tests/cpp/test_cxx_list.cpp: this driver is its own translation unit.
// CMake's prism_tests source list is explicit (not a glob).
//
// An empty any_cast is a __prism_check. A stored int any_casts back to that
// int. The model does not invent a value for a failed cast. make_any<T>
// returns an any that owns one T from the in_place constructor. type() on
// an empty any is typeid(void) and does not load a holder. type() on an
// any that holds T is that T's type_info. It does not throw and it does
// not return a default T. Without a libstdc++ clang++ that can run
// ASan/UBSan the runtime cases report NOTRUN. The source checks do not
// need that compiler.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

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

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "any"; }

struct AnyCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

AnyCtx& ctx() {
    static AnyCtx c;
    return c;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const std::vector<std::string> kFlags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                       "-fno-sanitize-recover=all", "-g", "-O1", "-w"};

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <any>\n"
                                  "int main() { std::any a(1); return std::any_cast<int>(a) - 1; }\n",
                                  kFlags);
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("any_trace.cpp"), "any_trace", kFlags, with_models);
}

}  // namespace

TEST_CASE("cxx any model: empty any_cast is a check and does not invent a value") {
    REQUIRE(fs::is_regular_file(model_header()));
    const std::string src = slurp(model_header());
    CHECK(src.find("#define _GLIBCXX_ANY") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std") != std::string::npos);
    CHECK(src.find("__prism_check(") != std::string::npos);
    CHECK(src.find("any_cast requires a contained object of this type") != std::string::npos);
    CHECK(src.find("has_value()") != std::string::npos);
    CHECK(src.find("fails to compile") != std::string::npos);
    CHECK(src.find("pir falls back") != std::string::npos);
    // A failed cast is the check, then bad_any_cast. Not a fabricated T.
    CHECK(src.find("return T{}") == std::string::npos);
    CHECK(src.find("return _Up{}") == std::string::npos);
    CHECK(src.find("return _ValueType{}") == std::string::npos);
    CHECK(src.find("reinterpret_cast") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <variant>") == std::string::npos);
    // Pointer overloads are declared. The old omission note is gone.
    CHECK(src.find("any_cast pointer overloads") == std::string::npos);
    CHECK(src.find("const _ValueType* any_cast(const any* __any)") != std::string::npos);
    CHECK(src.find("__any.template _M_target<_Up>()") != std::string::npos);
    const auto ptr_at = src.find("_ValueType* any_cast(any* __any)");
    REQUIRE(ptr_at != std::string::npos);
    const auto ptr_end = src.find("_GLIBCXX_END_NAMESPACE_VERSION", ptr_at);
    REQUIRE(ptr_end != std::string::npos);
    const std::string ptrs = src.substr(ptr_at, ptr_end - ptr_at);
    CHECK(ptrs.find("return nullptr") != std::string::npos);
    CHECK(ptrs.find("_M_target_ptr") != std::string::npos);
    CHECK(ptrs.find("_M_target<") == std::string::npos);
    CHECK(ptrs.find("__prism_check") == std::string::npos);
    CHECK(ptrs.find("throw bad_any_cast") == std::string::npos);
    CHECK(ptrs.find("return &") == std::string::npos);
    const auto helper = src.find("_M_target_ptr() const noexcept");
    REQUIRE(helper != std::string::npos);
    const auto helper_end = src.find("public:", helper);
    REQUIRE(helper_end != std::string::npos);
    const std::string body = src.substr(helper, helper_end - helper);
    const auto null_at = body.find("if (_M_ptr == nullptr) return nullptr;");
    const auto tag_at = body.find("_M_tag");
    CHECK(null_at != std::string::npos);
    CHECK(tag_at != std::string::npos);
    CHECK(null_at < tag_at);
    CHECK(body.find("_M_value") != std::string::npos);
    CHECK(body.find("__prism_check") == std::string::npos);
    CHECK(body.find("throw bad_any_cast") == std::string::npos);

    // make_any is the in_place constructor. It is not an omitted bullet, and
    // it does not invent a T or a stand-in of another type.
    const auto mk = src.find("make_any(_Args&&... __args)");
    REQUIRE(mk != std::string::npos);
    const auto mk_end = src.find("template <typename _ValueType>", mk);
    REQUIRE(mk_end != std::string::npos);
    // The enable_if constraint is the return type, written before the name.
    const auto mk_from = src.rfind("template <typename _Tp, typename... _Args>", mk);
    REQUIRE(mk_from != std::string::npos);
    const std::string made = src.substr(mk_from, mk_end - mk_from);
    CHECK(made.find("is_constructible_v<any, in_place_type_t<_Tp>, _Args...>") != std::string::npos);
    CHECK(made.find("return any(in_place_type<_Tp>, std::forward<_Args>(__args)...);") != std::string::npos);
    CHECK(made.find("return T{}") == std::string::npos);
    CHECK(made.find("return _Tp{}") == std::string::npos);
    CHECK(made.find("return any{}") == std::string::npos);
    CHECK(made.find("return any()") == std::string::npos);
    CHECK(made.find("__prism_check") == std::string::npos);
    CHECK(made.find("throw") == std::string::npos);
    CHECK(src.find("* make_any\n") == std::string::npos);
    CHECK(src.find("* type()") == std::string::npos);
    CHECK(src.find("* comparison") != std::string::npos);
}

TEST_CASE("cxx any model: type() is the stored type and empty is typeid(void)") {
    const std::string src = slurp(model_header());
    CHECK(src.find("#include <typeinfo>") != std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("* type()") == std::string::npos);
    CHECK(src.find("* comparison") != std::string::npos);
    CHECK(src.find("return T{}") == std::string::npos);

    const auto stored = src.find("&typeid(_Tp)");
    REQUIRE(stored != std::string::npos);
    CHECK(src.find("&typeid(_Tp)", stored + 1) == std::string::npos);
    CHECK(src.find("const type_info* _M_type") != std::string::npos);
    CHECK(src.find("_Prism_any_holder(&_Prism_any_tag<_Tp>::id, &typeid(_Tp))") != std::string::npos);

    const auto ty = src.find("const type_info& type() const noexcept");
    REQUIRE(ty != std::string::npos);
    const auto ty_end = src.find("// Qualified std::any_cast", ty);
    REQUIRE(ty_end != std::string::npos);
    const std::string body = src.substr(ty, ty_end - ty);
    const auto void_at = body.find("if (_M_ptr == nullptr) return typeid(void);");
    const auto load_at = body.find("_M_ptr->_M_type");
    CHECK(void_at != std::string::npos);
    CHECK(load_at != std::string::npos);
    CHECK(void_at < load_at);
    CHECK(body.find("return *_M_ptr->_M_type;") != std::string::npos);
    CHECK(body.find("throw") == std::string::npos);
    CHECK(body.find("__prism_check") == std::string::npos);
    CHECK(body.find("typeid(_Tp)") == std::string::npos);
    CHECK(body.find("typeid(int)") == std::string::npos);
    CHECK(body.find("return T{}") == std::string::npos);
    CHECK(body.find("return _Tp{}") == std::string::npos);
    CHECK(body.find("_M_value") == std::string::npos);

    // reset still only deletes the holder. That delete clears the stored type.
    const auto reset_at = src.find("void reset() noexcept");
    REQUIRE(reset_at != std::string::npos);
    const auto reset_end = src.find("void swap(", reset_at);
    REQUIRE(reset_end != std::string::npos);
    const std::string reset = src.substr(reset_at, reset_end - reset_at);
    CHECK(reset.find("delete _M_ptr;") != std::string::npos);
    CHECK(reset.find("_M_ptr = nullptr;") != std::string::npos);
    CHECK(reset.find("_M_type") == std::string::npos);
    CHECK(reset.find("typeid") == std::string::npos);
}

TEST_CASE("cxx any model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxany-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx any model: stored int round-trips and a bad any_cast is a check") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), kFlags.begin(), kFlags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "any_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(model_header().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a.size() > 10);
    CHECK(a == b);

    // Model only: libstdc++ throws bad_any_cast without calling __prism_check.
    CHECK(run_cmd({mod.string(), "e"}));
    CHECK(run_cmd({mod.string(), "w"}));
    CHECK(run_cmd({mod.string(), "i"}));
    CHECK(run_cmd({lib.string(), "t"}));
    CHECK(run_cmd({mod.string(), "t"}));
}
