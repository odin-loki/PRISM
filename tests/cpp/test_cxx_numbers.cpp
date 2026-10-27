// The partial <numbers> model is read as text. This case does not compile
// the header and does not run pir. The long double literals are not a proof.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("numbers model: literals are not a proof and pi starts with 3.14") {
    const std::string text = slurp(repo() / "src" / "prism" / "pir" / "models" / "cxx" / "numbers");
    REQUIRE(!text.empty());

    const auto proof = text.find("a comparison against these literals is not a proof of a numeric identity");
    REQUIRE(proof != std::string::npos);
    const auto line_start = text.rfind('\n', proof);
    const auto begin = line_start == std::string::npos ? 0 : line_start + 1;
    const auto line_end = text.find('\n', proof);
    const std::string line = text.substr(begin, (line_end == std::string::npos ? text.size() : line_end) - begin);
    CHECK(line.find("//") != std::string::npos);

    const std::string key = " pi = ";
    const auto at = text.find(key);
    REQUIRE(at != std::string::npos);
    CHECK(text.compare(at + key.size(), 4, "3.14") == 0);
}
