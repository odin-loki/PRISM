// The <shared_mutex> model stores an exclusive flag and a shared count.
// These checks read the header text: exclusive try_lock fails while a shared
// lock is held, unlock of an unlocked shared_mutex is a __prism_check, and
// the header does not define lock(.

#include <doctest/doctest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Body of the function whose text contains `sig`, including braces.
std::string body_of(std::string_view src, std::string_view sig) {
    const auto at = src.find(sig);
    if (at == std::string_view::npos) return {};
    const auto brace = src.find('{', at);
    if (brace == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = brace; i < src.size(); ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') {
            if (--depth == 0) return std::string(src.substr(brace, i - brace + 1));
        }
    }
    return {};
}

// A definition of `name` (for example "lock("). try_lock( and unlock( are
// different names. A // comment that mentions the omitted name is not one.
bool defines_name(std::string_view src, std::string_view name) {
    for (std::size_t i = 0; (i = src.find(name, i)) != std::string_view::npos;) {
        const bool boundary =
            i == 0 || (src[i - 1] != '_' && !std::isalnum(static_cast<unsigned char>(src[i - 1])));
        if (boundary) {
            const auto nl = src.rfind('\n', i);
            const auto start = nl == std::string_view::npos ? 0 : nl + 1;
            const auto comment = src.find("//", start);
            if (comment == std::string_view::npos || comment > i) return true;
        }
        i += name.size();
    }
    return false;
}

}  // namespace

TEST_CASE("cxx shared_mutex: exclusive try_lock fails under a shared hold, unlock checks, lock is omitted") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "cxx" / "shared_mutex");
    REQUIRE(!src.empty());
    CHECK(src.find("#ifndef _GLIBCXX_SHARED_MUTEX") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std") != std::string::npos);
    CHECK(src.find("#include <mutex>") == std::string::npos);
    CHECK(src.find("#include <thread>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("operator new") == std::string::npos);

    const auto try_lock = body_of(src, "bool try_lock() noexcept");
    REQUIRE(!try_lock.empty());
    const auto failed = try_lock.find("return false");
    const auto taken = try_lock.find("_M_exclusive = true");
    REQUIRE(failed != std::string::npos);
    REQUIRE(taken != std::string::npos);
    CHECK(try_lock.find("_M_exclusive") < failed);
    CHECK(try_lock.find("_M_shared != 0") < failed);
    CHECK(failed < taken);
    CHECK(try_lock.find("++_M_shared") == std::string::npos);
    CHECK(try_lock.find("--_M_shared") == std::string::npos);
    CHECK(try_lock.find("_M_shared =") == std::string::npos);

    const auto try_shared = body_of(src, "bool try_lock_shared() noexcept");
    REQUIRE(!try_shared.empty());
    const auto shared_fail = try_shared.find("return false");
    const auto shared_inc = try_shared.find("++_M_shared");
    REQUIRE(shared_fail != std::string::npos);
    REQUIRE(shared_inc != std::string::npos);
    CHECK(try_shared.find("_M_exclusive") < shared_fail);
    CHECK(shared_fail < shared_inc);

    const auto unlock = body_of(src, "void unlock() noexcept");
    REQUIRE(!unlock.empty());
    const auto unlock_check = unlock.find("__prism_check(_M_exclusive");
    REQUIRE(unlock_check != std::string::npos);
    CHECK(unlock_check < unlock.find("_M_exclusive = false"));
    CHECK(unlock.find("_M_exclusive = true") == std::string::npos);
    CHECK(unlock.find("++_M_shared") == std::string::npos);

    const auto unlock_shared = body_of(src, "void unlock_shared() noexcept");
    REQUIRE(!unlock_shared.empty());
    CHECK(unlock_shared.find("__prism_check") != std::string::npos);
    CHECK(unlock_shared.find("_M_shared > 0") != std::string::npos);
    CHECK(unlock_shared.find("__prism_check") < unlock_shared.find("--_M_shared"));
    CHECK(unlock_shared.find("++_M_shared") == std::string::npos);

    CHECK(src.find("lock() and lock_shared() are omitted") != std::string::npos);
    CHECK_MESSAGE(!defines_name(src, "lock("), "the header does not define lock(");
    CHECK_MESSAGE(!defines_name(src, "lock_shared("), "the header does not define lock_shared(");
}
