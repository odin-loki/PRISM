// The <spanstream> model is a partial basic_spanbuf. These checks read the
// header text: sputc at the size is a __prism_check and does not write, and
// the destructor is defaulted or does not call free.

#include <doctest/doctest.h>

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

// Body of the first function whose text contains `sig`, including braces.
// A defaulted declaration has no brace and yields an empty string.
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

}  // namespace

TEST_CASE("cxx spanstream model: write at size is a check, destructor does not free") {
    const auto header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "spanstream";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const auto src = slurp(header);
    REQUIRE(!src.empty());

    const auto sputc = body_of(src, "int sputc(");
    REQUIRE(!sputc.empty());
    CHECK(sputc.find("__prism_check") != std::string::npos);
    CHECK(sputc.find("_M_off < _M_size") != std::string::npos);
    const auto guard = sputc.find("if (_M_off < _M_size)");
    const auto store = sputc.find("_M_ptr[_M_off]");
    REQUIRE(guard != std::string::npos);
    REQUIRE(store != std::string::npos);
    CHECK(guard < store);

    const bool defaulted = src.find("~basic_spanbuf() noexcept = default") != std::string::npos ||
                           src.find("~basic_spanbuf() = default") != std::string::npos;
    const auto dtor = body_of(src, "~basic_spanbuf()");
    const bool no_free = dtor.find("free(") == std::string::npos && dtor.find("::operator delete") == std::string::npos;
    CHECK((defaulted || no_free));
    if (defaulted) CHECK(src.find("free(") == std::string::npos);
}
