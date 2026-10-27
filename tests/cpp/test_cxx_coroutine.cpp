// The <coroutine> model is a partial std::coroutine_handle<void>. A default
// handle stores nullptr. resume, destroy, and done on a null handle are a
// FUNC-CONTRACT check and then a trap. They are not called from this
// process: a trap would abort the test binary. The header is read as text.
// Where the compiler accepts the builtins, the null address is also checked
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
extern "C" void __prism_check(int ok, const char*, const char*) {
    if (!ok) std::abort();
}
static_assert(std::coroutine_handle<>{}.address() == nullptr, "default handle address is nullptr");
static_assert(std::coroutine_handle<>::from_address(nullptr).address() == nullptr,
              "from_address(nullptr) is a null handle");
static_assert(!std::coroutine_handle<>{}, "a default handle is not a live coroutine");
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

TEST_CASE("cxx coroutine model: null resume, destroy, and done are checks then traps") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE(src.find("struct coroutine_handle<void>") != std::string::npos);
    CHECK(src.find("#ifndef _GLIBCXX_COROUTINE") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("not a proof that a coroutine runs") != std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <thread>") == std::string::npos);
    CHECK(src.find("#include <exception>") == std::string::npos);
    CHECK(src.find("#include <atomic>") == std::string::npos);
    CHECK(src.find("promise_type") == std::string::npos);
    CHECK(src.find("co_await") == std::string::npos);
    CHECK(src.find("noop_coroutine") == std::string::npos);
    CHECK(src.find("__builtin_coro_resume") == std::string::npos);
    CHECK(src.find("__builtin_coro_destroy") == std::string::npos);
    CHECK(src.find("__builtin_coro_done") == std::string::npos);
    CHECK(src.find("~coroutine_handle") == std::string::npos);
    CHECK(src.find("extern \"C\" void __prism_check(int ok, const char* cls, const char* msg);") !=
          std::string::npos);
    CHECK(src.find("__prism_check(int ok, const char* cls, const char* msg) {") == std::string::npos);

    CHECK(src.find("constexpr coroutine_handle() noexcept : _M_fr_ptr(nullptr) {}") != std::string::npos);
    CHECK(src.find("void* _M_fr_ptr = nullptr;") != std::string::npos);
    CHECK(src.find("constexpr void* address() const noexcept { return _M_fr_ptr; }") != std::string::npos);
    CHECK(src.find("__h._M_fr_ptr = __a;") != std::string::npos);

    for (const char* sig : {"void resume() const", "void destroy() const", "bool done() const"}) {
        const auto body = body_of(src, sig);
        REQUIRE(!body.empty());
        const auto check = body.find("__prism_check");
        const auto trap = body.find("__builtin_trap");
        REQUIRE(check != std::string::npos);
        REQUIRE(trap != std::string::npos);
        CHECK(check < trap);
        CHECK(body.find("FUNC-CONTRACT") != std::string::npos);
        CHECK(body.find("_M_fr_ptr != nullptr ? 1 : 0") != std::string::npos);
        CHECK(body.find("return") == std::string::npos);
        CHECK(body.find("new") == std::string::npos);
    }
    const auto done = body_of(src, "bool done() const");
    CHECK(done.find("return true") == std::string::npos);
    CHECK(done.find("true") == std::string::npos);
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
}
#endif
