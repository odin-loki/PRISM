// The <system_error> model stores the int error_code is built with.
// error_code(5, generic_category()).value() is that 5, not 0. The header
// text does not contain the word Success: message() is omitted, not stubbed.

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

TEST_CASE("cxx system_error: value 5 stays 5 and the header has no Success") {
    const fs::path header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "system_error";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const std::string src = slurp(header);
    REQUIRE(!src.empty());

    CHECK(src.find("_GLIBCXX_SYSTEM_ERROR") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std") != std::string::npos);

    // The word Success is not in the header. message() is not a stub string.
    CHECK(src.find("Success") == std::string::npos);
    CHECK(src.find("#include <string>") == std::string::npos);
    CHECK(src.find("#include <string_view>") == std::string::npos);
    CHECK(src.find("#include <stdexcept>") == std::string::npos);
    CHECK(src.find("class error_condition") == std::string::npos);
    CHECK(src.find("class system_error") == std::string::npos);

    // error_code(5, generic_category()) stores 5. value() returns that member,
    // so the 5 stays 5 and is not rewritten to 0. A default code stores 0,
    // and operator bool is true only when value() is not 0.
    CHECK(src.find("error_code(int __v, const __generic_category_tag&) noexcept : _M_value(__v), _M_cat(1)") !=
          std::string::npos);
    CHECK(src.find("int value() const noexcept { return _M_value; }") != std::string::npos);
    CHECK(src.find("return 0") == std::string::npos);
    CHECK(src.find("error_code() noexcept : _M_value(0), _M_cat(0)") != std::string::npos);
    CHECK(src.find("explicit operator bool() const noexcept { return value() != 0; }") != std::string::npos);
}
