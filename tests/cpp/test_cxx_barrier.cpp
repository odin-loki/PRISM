// The std::barrier model header is one file,
// src/prism/pir/models/cxx/barrier. arrive past the remaining count is a
// __prism_check. The header does not define wait(: a blocking wait that
// returns immediately would be a fake synchronization success.

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
}
