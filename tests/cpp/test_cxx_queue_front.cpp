// Empty std::queue::front and back are the FUNC-CONTRACT check
// __glibcxx_assert(!this->empty()), then a load of the node. The assert
// does not return, so an empty call does not load a node and does not
// return T{}. The header is read as text. It includes libstdc++ private
// headers, so this doctest does not include it.
// CMake's prism_tests source list is explicit (not a glob): this file is one
// line in _prism_test_sources.

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

void require_assert_before_load(const std::string& src, std::string_view sig, std::string_view load) {
    const auto body = body_of(src, sig);
    REQUIRE_FALSE(body.empty());
    const auto check = body.find("__glibcxx_assert(!this->empty())");
    const auto load_at = body.find(load);
    REQUIRE(check != std::string::npos);
    REQUIRE(load_at != std::string::npos);
    CHECK(check < load_at);
    const auto before = body.substr(0, load_at);
    CHECK(before.find("_M_valptr") == std::string::npos);
    CHECK(before.find("_M_head") == std::string::npos);
    CHECK(before.find("_M_tail") == std::string::npos);
    CHECK(before.find("return") == std::string::npos);
    CHECK(body.find("T{}") == std::string::npos);
    CHECK(body.find("return {}") == std::string::npos);
    CHECK(body.find("value_type{}") == std::string::npos);
    CHECK(body.find("_Tp{}") == std::string::npos);
    CHECK(body.find("_Tp()") == std::string::npos);
}

}  // namespace

TEST_CASE("cxx queue model: empty front and back trap before a node load") {
    const fs::path header = repo() / "src" / "prism" / "pir" / "models" / "cxx" / "queue";
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(src.find("!this->empty() on front, back, and pop") != std::string::npos);

    // Non-const front() is the first "reference front()" in the header.
    // const_reference contains that substring later, so the search hits
    // the non-const overload.
    require_assert_before_load(src, "reference front() noexcept", "return *_M_head->_M_valptr();");
    require_assert_before_load(src, "const_reference front() const noexcept", "return *_M_head->_M_valptr();");
    require_assert_before_load(src, "reference back() noexcept", "return *_M_tail->_M_valptr();");
    require_assert_before_load(src, "const_reference back() const noexcept", "return *_M_tail->_M_valptr();");

    CHECK(body_of(src, "bool empty() const noexcept") == "{ return _M_head == nullptr; }");

    const fs::path note = repo() / "docs" / "finish-notes" / "queue-front.md";
    const std::string note_txt = slurp(note);
    REQUIRE_FALSE(note_txt.empty());
    CHECK(note_txt.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(note_txt.find("front") != std::string::npos);
    CHECK(note_txt.find("back") != std::string::npos);
}
