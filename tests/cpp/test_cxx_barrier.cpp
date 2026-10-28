// The std::barrier model header is one file,
// src/prism/pir/models/cxx/barrier. arrive past the remaining count is a
// __prism_check. A negative constructor count is not stored. max() returns
// PTRDIFF_MAX and does not wait. The header does not define wait(: a
// blocking wait that returns immediately would be a fake synchronization
// success.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path barrier_header() {
    return fs::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" / "cxx" / "barrier";
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Comments name the omitted members. A definition is what remains.
std::string strip_comments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            i += 2;
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            if (i + 1 < s.size()) i += 2;
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

}  // namespace

TEST_CASE("cxx barrier: arrive past the count is a check") {
    const fs::path header = barrier_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    const auto arrive = src.find("void arrive(");
    REQUIRE(arrive != std::string::npos);
    const auto drop = src.find("void arrive_and_drop(", arrive);
    REQUIRE(drop != std::string::npos);
    const std::string body = src.substr(arrive, drop - arrive);
    CHECK(body.find("__prism_check") != std::string::npos);
    CHECK(body.find("__n > 0") != std::string::npos);
    CHECK(body.find("__n <= _M_expected") != std::string::npos);
    CHECK(body.find("barrier::arrive past the expected count") != std::string::npos);
    CHECK(body.find("if (__ok)") != std::string::npos);
    CHECK(body.find("_M_expected -= __n") != std::string::npos);
    CHECK(src.find("#include <atomic>") == std::string::npos);
    CHECK(src.find("#include <latch>") == std::string::npos);
    CHECK(src.find("#include <semaphore>") == std::string::npos);
    CHECK(src.find("#include <thread>") == std::string::npos);
}

TEST_CASE("cxx barrier: the header does not define wait(") {
    const std::string code = strip_comments(slurp(barrier_header()));
    CHECK(code.find("wait(") == std::string::npos);
    CHECK(code.find("arrive_and_wait") == std::string::npos);
    CHECK(code.find("void arrive(") != std::string::npos);
    CHECK(code.find("void arrive_and_drop(") != std::string::npos);
}

TEST_CASE("cxx barrier: a negative count is not stored and max does not wait") {
    const std::string src = slurp(barrier_header());
    const auto ctor = src.find("explicit barrier(");
    REQUIRE(ctor != std::string::npos);
    const auto mx = src.find("static constexpr ptrdiff_t max() noexcept", ctor);
    REQUIRE(mx != std::string::npos);
    const auto arrive = src.find("void arrive(", mx);
    REQUIRE(arrive != std::string::npos);
    const std::string ctor_span = src.substr(ctor, mx - ctor);
    CHECK(ctor_span.find(": _M_expected(0)") != std::string::npos);
    CHECK(ctor_span.find("_M_expected(__expected)") == std::string::npos);
    CHECK(ctor_span.find("barrier constructed with a negative count") != std::string::npos);
    const auto check_at = ctor_span.find("__prism_check");
    const auto store_at = ctor_span.find("_M_expected = __expected");
    REQUIRE(check_at != std::string::npos);
    REQUIRE(store_at != std::string::npos);
    CHECK(check_at < store_at);
    CHECK(ctor_span.find("if (__ok)") != std::string::npos);
    CHECK(ctor_span.find("if (__ok)") < store_at);

    CHECK(mx < arrive);
    const auto mx_end = src.find('}', mx);
    REQUIRE(mx_end != std::string::npos);
    const std::string max_fn = src.substr(mx, mx_end - mx);
    CHECK(max_fn.find("return PTRDIFF_MAX;") != std::string::npos);
    CHECK(max_fn.find("_M_expected") == std::string::npos);
    CHECK(max_fn.find("-=") == std::string::npos);
    CHECK(max_fn.find("arrive") == std::string::npos);
    CHECK(max_fn.find("wait") == std::string::npos);
    CHECK(src.find("max() is modelled") != std::string::npos);
    CHECK(src.find("does not change any barrier") != std::string::npos);
    CHECK(src.find("wait and arrive_and_wait stay omitted") != std::string::npos);
    CHECK(src.find("#include <cstdint>") != std::string::npos);
    CHECK(src.find("#include <atomic>") == std::string::npos);
    CHECK(src.find("#include <latch>") == std::string::npos);
    CHECK(src.find("#include <semaphore>") == std::string::npos);
    CHECK(src.find("#include <thread>") == std::string::npos);
}
