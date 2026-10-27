// The <mutex> model is a partial mutex. These checks read the header text:
// the second try_lock is false, unlock of an unlocked mutex is a check, and
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

// A token `lock(` whose name is lock, not try_lock or unlock.
bool defines_lock(std::string_view src) {
    for (std::size_t i = 0; (i = src.find("lock(", i)) != std::string_view::npos; i += 5) {
        if (i == 0) return true;
        const unsigned char prev = static_cast<unsigned char>(src[i - 1]);
        if (std::isalnum(prev) || prev == '_') continue;
        return true;
    }
    return false;
}

}  // namespace

TEST_CASE("cxx mutex model: second try_lock is false, unlock checks, lock is omitted") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "cxx" / "mutex");
    REQUIRE(!src.empty());
    CHECK(src.find("#ifndef _GLIBCXX_MUTEX") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std {") != std::string::npos);
    CHECK(src.find("#include <thread>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <shared_mutex>") == std::string::npos);

    const auto try_lock = body_of(src, "bool try_lock()");
    REQUIRE(!try_lock.empty());
    const auto locked_if = try_lock.find("if (_M_locked)");
    const auto ret_false = try_lock.find("return false");
    const auto set_locked = try_lock.find("_M_locked = true");
    const auto ret_true = try_lock.find("return true");
    REQUIRE(locked_if != std::string::npos);
    REQUIRE(ret_false != std::string::npos);
    REQUIRE(set_locked != std::string::npos);
    REQUIRE(ret_true != std::string::npos);
    // Already locked: return false and do not store. return true is only after the flag is set.
    CHECK(locked_if < ret_false);
    CHECK(ret_false < set_locked);
    CHECK(set_locked < ret_true);
    CHECK(try_lock.find("return true", ret_true + 1) == std::string::npos);
    CHECK(try_lock.find("_M_locked = false") == std::string::npos);

    const auto unlock = body_of(src, "void unlock()");
    REQUIRE(!unlock.empty());
    const auto check = unlock.find("__prism_check");
    const auto clear = unlock.find("_M_locked = false");
    REQUIRE(check != std::string::npos);
    REQUIRE(clear != std::string::npos);
    CHECK(check < clear);
    CHECK(unlock.find("_M_locked ? 1 : 0") != std::string::npos);
    CHECK(unlock.find("mutex::unlock on an unlocked mutex") != std::string::npos);
    // Does not pretend the mutex was locked.
    CHECK(unlock.find("_M_locked = true") == std::string::npos);

    CHECK_FALSE(defines_lock(src));
    CHECK(src.find("class timed_mutex") == std::string::npos);
    CHECK(src.find("class recursive_mutex") == std::string::npos);
    CHECK(src.find("class scoped_lock") == std::string::npos);
    CHECK(src.find("class lock_guard") == std::string::npos);
    CHECK(src.find("class unique_lock") == std::string::npos);
    CHECK(src.find("timed_mutex") != std::string::npos);
    CHECK(src.find("new ") == std::string::npos);
    CHECK(src.find("std::thread") == std::string::npos);
}
