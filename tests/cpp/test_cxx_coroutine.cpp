// The <coroutine> model is a partial std::coroutine_handle<void>. A default
// handle stores nullptr. resume, destroy, and done on a null handle are a
// FUNC-CONTRACT check and then a trap. They are not called from this
// process: a trap would abort the test binary. Equality is address() ==
// address(). Two default handles compare equal. A default handle is not
// equal to from_address of a non-null pointer. That comparison is not a
// proof that a coroutine ran. The header is read as text. Where the compiler
// accepts the builtins, the null address and the comparisons are also checked
// by including the model by path (`#include <coroutine>` here would be the
// platform header).

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#if !defined(_MSC_VER) || defined(__clang__)
#  if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4068)  // #pragma GCC system_header in the model
#  endif
#  include "../../src/prism/pir/models/cxx/coroutine"
#  if defined(_MSC_VER)
#    pragma warning(pop)
#  endif
// The header declares this intrinsic and does not define it. The live checks
// below do not fail a contract; this body is only here if a call is emitted.
// PE/COFF has no weak definitions, so on Windows the one strong definition in
// test_cxx_complex.cpp serves the whole prism_tests binary (a second one is
// an lld-link/link.exe duplicate symbol).
#if !defined(_WIN32)
extern "C" __attribute__((weak)) void __prism_check(int ok, const char*, const char*) {
    if (!ok) std::abort();
}
#endif
static_assert(std::coroutine_handle<>{}.address() == nullptr, "default handle address is nullptr");
static_assert(std::coroutine_handle<>::from_address(nullptr).address() == nullptr,
              "from_address(nullptr) is a null handle");
static_assert(!std::coroutine_handle<>{}, "a default handle is not a live coroutine");
static_assert(std::coroutine_handle<>{} == std::coroutine_handle<>{}, "two default handles compare equal");
static_assert(!(std::coroutine_handle<>{} != std::coroutine_handle<>{}),
              "two default handles are not unequal");
static_assert(std::coroutine_handle<>{} == std::coroutine_handle<>::from_address(nullptr),
              "from_address(nullptr) equals a default handle");
static int prism_coroutine_object = 0;
static_assert(!(std::coroutine_handle<>{} ==
                std::coroutine_handle<>::from_address(static_cast<void*>(&prism_coroutine_object))),
              "a default handle is not equal to from_address of a non-null pointer");
static_assert(std::coroutine_handle<>{} !=
                  std::coroutine_handle<>::from_address(static_cast<void*>(&prism_coroutine_object)),
              "a default handle is not equal to from_address of a non-null pointer");
static_assert(std::coroutine_handle<>::from_address(static_cast<void*>(&prism_coroutine_object)) ==
                  std::coroutine_handle<>::from_address(static_cast<void*>(&prism_coroutine_object)),
              "from_address of the same pointer compares equal");
#endif

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path header_path() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "coroutine"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Body of the function whose text contains `sig`, including braces.
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

}  // namespace

TEST_CASE("cxx coroutine model: resume, destroy, done use the coroutine builtins after a null check") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    CHECK(src.find("#ifndef _GLIBCXX_COROUTINE") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <thread>") == std::string::npos);
    CHECK(src.find("struct coroutine_traits") != std::string::npos);
    CHECK(src.find("noop_coroutine") != std::string::npos);
    CHECK(src.find("struct suspend_always") != std::string::npos);
    CHECK(src.find("struct suspend_never") != std::string::npos);
    CHECK(src.find("from_promise") != std::string::npos);
    CHECK(src.find("extern \"C\" void __prism_check(int ok, const char* cls, const char* msg);") != std::string::npos);
    CHECK(src.find("constexpr coroutine_handle() noexcept : _M_fr_ptr(nullptr) {}") != std::string::npos);
    CHECK(src.find("constexpr void* address() const noexcept { return _M_fr_ptr; }") != std::string::npos);
    for (const char* sig : {"void resume() const {", "void destroy() const {", "bool done() const {"}) {
        CAPTURE(sig);
        const std::string body = body_of(src, sig);
        REQUIRE_FALSE(body.empty());
        const auto check = body.find("__prism_check(_M_fr_ptr != nullptr ? 1 : 0, \"FUNC-CONTRACT\"");
        const auto builtin = body.find("__builtin_coro_");
        REQUIRE(check != std::string::npos);
        REQUIRE(builtin != std::string::npos);
        CHECK(check < builtin);
        CHECK(body.find("__builtin_trap") == std::string::npos);
    }
    // resuming a finished coroutine is a contract failure
    const std::string res = body_of(src, "void resume() const {");
    CHECK(res.find("!__builtin_coro_done(_M_fr_ptr)") != std::string::npos);
    CHECK(res.find("__builtin_coro_resume(_M_fr_ptr)") != std::string::npos);
}

#if !defined(_MSC_VER) || defined(__clang__)
TEST_CASE("cxx coroutine model: a default handle and from_address(nullptr) are null") {
    std::coroutine_handle<> h;
    CHECK(h.address() == nullptr);
    CHECK_FALSE(static_cast<bool>(h));
    const std::coroutine_handle<> n = std::coroutine_handle<>::from_address(nullptr);
    CHECK(n.address() == nullptr);
    CHECK_FALSE(static_cast<bool>(n));
    int object = 0;
    const std::coroutine_handle<> stored = std::coroutine_handle<>::from_address(&object);
    CHECK(stored.address() == &object);
    CHECK_FALSE(h == stored);
    CHECK(h != stored);
    const std::coroutine_handle<> again = std::coroutine_handle<>::from_address(&object);
    CHECK(stored == again);
    CHECK_FALSE(stored != again);
    int other = 0;
    const std::coroutine_handle<> different = std::coroutine_handle<>::from_address(&other);
    CHECK_FALSE(stored == different);
    CHECK(stored != different);
}

TEST_CASE("cxx coroutine model: two default handles compare equal") {
    const std::coroutine_handle<> a;
    const std::coroutine_handle<> b;
    CHECK(a == b);
    CHECK_FALSE(a != b);
    const std::coroutine_handle<> n = std::coroutine_handle<>::from_address(nullptr);
    CHECK(a == n);
    CHECK_FALSE(a != n);
    CHECK(a.address() == b.address());
}
#endif
