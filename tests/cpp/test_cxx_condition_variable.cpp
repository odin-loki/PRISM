// The <condition_variable> model is one bool in the object. notify_one and
// notify_all set that flag and do not run a waiter. A default object has the
// flag false. wait returns at once (a spurious wakeup the standard permits);
// wait(lock, pred) assumes pred() on return.
//
// Included by path so prism_tests does not put the model directory on the
// include path for every TU. `#include <condition_variable>` here would be
// the platform header.

#include <doctest/doctest.h>

#include "prose_text.hpp"

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4068)  // #pragma GCC system_header in the model
#endif
#include "../../src/prism/pir/models/cxx/condition_variable"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path header_path() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "condition_variable"; }

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

bool is_ident(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

// `name` as its own identifier, then '('. wait_for does not define wait.
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

}  // namespace

TEST_CASE("cxx condition_variable model: notify sets a flag, wait is a spurious wakeup or assumes pred") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = prism_test::unwrap_prose(slurp(header));
    REQUIRE(!src.empty());

    CHECK(src.find("#ifndef _GLIBCXX_CONDITION_VARIABLE") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_CONDITION_VARIABLE 1") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std {") != std::string::npos);
    CHECK(src.find("not a concurrency proof") != std::string::npos);
    CHECK(src.find("spurious wakeup") != std::string::npos);
    CHECK(src.find("#include <mutex>") == std::string::npos);
    CHECK(src.find("#include <thread>") == std::string::npos);
    CHECK(src.find("#include <atomic>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <chrono>") == std::string::npos);
    CHECK(src.find("#include <functional>") == std::string::npos);
    CHECK(src.find("std::thread") == std::string::npos);
    CHECK(src.find("new ") == std::string::npos);
    CHECK(src.find("class condition_variable_any") != std::string::npos);

    CHECK(src.find("bool _M_flag;") != std::string::npos);
    CHECK(src.find("condition_variable() noexcept : _M_flag(false) {}") != std::string::npos);
    CHECK(src.find("~condition_variable() = default;") != std::string::npos);
    CHECK(src.find("condition_variable(const condition_variable&) = delete;") != std::string::npos);

    const std::string decl =
        "extern \"C\" void __prism_check(int __ok, const char* __cls, const char* __msg);";
    const auto at = src.find(decl);
    REQUIRE(at != std::string::npos);
    const auto call = src.find("__prism_check(");
    REQUIRE(call != std::string::npos);
    CHECK(call > at);
    CHECK(call < at + decl.size());
    CHECK(src.find("__prism_check(", call + std::string("__prism_check(").size()) == std::string::npos);

    const auto one = body_of(src, "void notify_one() noexcept");
    const auto all = body_of(src, "void notify_all() noexcept");
    REQUIRE(one == "{ _M_flag = true; }");
    REQUIRE(all == "{ _M_flag = true; }");
    CHECK(one.find("(") == std::string::npos);
    CHECK(all.find("(") == std::string::npos);
    CHECK(one.find("__prism_check") == std::string::npos);
    CHECK(all.find("__prism_check") == std::string::npos);
    CHECK(one.find("_M_flag = false") == std::string::npos);
    CHECK(all.find("_M_flag = false") == std::string::npos);

    CHECK(defines_call(src, "wait"));
    CHECK(defines_call(src, "wait_for"));
    CHECK(defines_call(src, "wait_until"));
    CHECK_FALSE(defines_call(src, "notify_all_at_thread_exit"));

    static_assert(sizeof(std::condition_variable) == sizeof(bool));
    static_assert(std::is_trivially_destructible_v<std::condition_variable>);
    static_assert(std::is_standard_layout_v<std::condition_variable>);
    static_assert(!std::is_copy_constructible_v<std::condition_variable>);
    static_assert(!std::is_move_constructible_v<std::condition_variable>);

    std::condition_variable cv;
    cv.notify_one();
    cv.notify_all();
}
