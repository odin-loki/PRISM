// Windows PIR <stdatomic.h> is a compile shim. Read it as text. Do not
// compile it: the header is C11, not a C++ translation unit.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST_CASE("stdatomic shim: comment is NEEDS-HARNESS and not a proof") {
    const auto path = std::filesystem::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" /
                      "models" / "libc" / "shim" / "stdatomic.h";
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto guard = text.find("#ifndef");
    REQUIRE(guard != std::string::npos);
    const auto comment = text.substr(0, guard);
    CHECK(comment.find("NEEDS-HARNESS") != std::string::npos);
    CHECK(comment.find("not a proof") != std::string::npos);
}
