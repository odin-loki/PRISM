// The <execution> model is partial: namespace std::execution contains
// sequenced_policy and seq, and nothing else. seq is the sequenced policy
// tag. par, par_unseq, and unseq are not declared. A parallel policy that
// ran on one thread would be a fake parallel execution. No algorithm is
// declared.
//
// This case reads the header as text. It does not `#include` the model:
// defining _GLIBCXX_EXECUTION here would hide libstdc++'s <execution>.
// The trace is tests/cxx_models/execution_trace.cpp. This header is not a
// parallelism proof.

#include <doctest/doctest.h>

#include "prose_text.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path header_path() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "execution"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Drop // comments. The header has no block comments and no string literals.
std::string strip_line_comments(std::string_view src) {
    std::string out;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
            if (i < src.size()) out.push_back('\n');
            continue;
        }
        out.push_back(src[i]);
    }
    return out;
}

std::size_t count_occ(std::string_view src, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = 0; (at = src.find(needle, at)) != std::string_view::npos; at += needle.size()) ++n;
    return n;
}

}  // namespace

TEST_CASE("cxx execution model: seq is the only policy and no algorithm is declared") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    CHECK(header.filename() == "execution");
    CHECK(header.extension().empty());

    const std::string src = slurp(header);
    const std::string prose = prism_test::unwrap_prose(src);
    REQUIRE(!src.empty());
    CHECK(prose.find("This header is not a parallelism proof.") != std::string::npos);
    CHECK(prose.find("A parallel policy that ran on one thread would be a fake parallel execution.") != std::string::npos);
    CHECK(src.find("#ifndef _GLIBCXX_EXECUTION") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_EXECUTION 1") != std::string::npos);
    CHECK(src.find("#endif  // _GLIBCXX_EXECUTION") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std::execution {") != std::string::npos);
    CHECK(src.find("#include <thread>") == std::string::npos);
    CHECK(src.find("#include <atomic>") == std::string::npos);
    CHECK(src.find("#include <algorithm>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("/*") == std::string::npos);

    const std::string code = strip_line_comments(src);
    CHECK(code.find("#include") == std::string::npos);
    CHECK(code.find("(") == std::string::npos);
    CHECK(code.find("par") == std::string::npos);
    CHECK(code.find("unseq") == std::string::npos);
    CHECK(code.find("for_each") == std::string::npos);
    CHECK(code.find("reduce") == std::string::npos);
    CHECK(code.find("sort") == std::string::npos);
    CHECK(code.find("transform") == std::string::npos);
    CHECK(code.find("thread") == std::string::npos);
    CHECK(code.find("atomic") == std::string::npos);
    CHECK(code.find("algorithm") == std::string::npos);
    CHECK(code.find("memory") == std::string::npos);
    CHECK(code.find("__prism_check") == std::string::npos);
    CHECK(count_occ(code, "class sequenced_policy {}") == 1);
    CHECK(count_occ(code, "inline constexpr sequenced_policy seq{}") == 1);
    CHECK(count_occ(code, "sequenced_policy") == 2);
}
