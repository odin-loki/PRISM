// The <valarray> model is a partial owning buffer. operator[] checks the
// index before it returns an element. A default valarray has size 0.
//
// tests/cpp/test_cxx_list.cpp compiles a separate program that includes
// <list>. This header has no libstdc++ private headers, so the same kind of
// include compiles in this doctest when the model file is named by path
// (`#include <valarray>` here would be the platform header). The operator[]
// check is also read from the header text.

#include <doctest/doctest.h>

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4068)  // #pragma GCC system_header in the model
#endif
#include "../../src/prism/pir/models/cxx/valarray"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path header_path() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "valarray"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Body of the nth function whose text contains `sig`, including braces.
std::string body_of(std::string_view src, std::string_view sig, std::size_t nth = 0) {
    std::size_t from = 0;
    for (std::size_t n = 0;; ++n) {
        const auto at = src.find(sig, from);
        if (at == std::string_view::npos) return {};
        from = at + sig.size();
        if (n != nth) continue;
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
}

}  // namespace

TEST_CASE("cxx valarray model: operator[] checks the index and a default valarray has size 0") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE(src.find("class valarray") != std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <initializer_list>") == std::string::npos);
    CHECK(src.find("::operator new") != std::string::npos);
    CHECK(src.find("::operator delete(") != std::string::npos);
    CHECK(src.find("delete[]") == std::string::npos);

    for (std::size_t n = 0; n < 2; ++n) {
        const auto body = body_of(src, "operator[](size_type __n)", n);
        REQUIRE(!body.empty());
        const auto check = body.find("__prism_check");
        const auto ret = body.find("return _M_data[__n];");
        REQUIRE(check != std::string::npos);
        REQUIRE(ret != std::string::npos);
        CHECK(check < ret);
        CHECK(body.find("__n < this->size()") != std::string::npos);
        CHECK(body.find("return _Tp") == std::string::npos);
        CHECK(body.find("return {}") == std::string::npos);
        CHECK(body.find("return value_type") == std::string::npos);
    }

    CHECK(src.find("size_type size() const noexcept { return _M_size; }") != std::string::npos);
    CHECK(src.find("size_type _M_size = 0;") != std::string::npos);
    CHECK(src.find("valarray() noexcept = default;") != std::string::npos);

    const auto resize = body_of(src, "void resize(size_type __n, const value_type& __c = value_type())");
    const auto release = body_of(src, "void _M_release() noexcept");
    REQUIRE(!resize.empty());
    REQUIRE(!release.empty());
    const auto drop = resize.find("_M_release()");
    const auto store = resize.find("_M_data = __fresh;");
    REQUIRE(drop != std::string::npos);
    REQUIRE(store != std::string::npos);
    CHECK(drop < store);
    CHECK(release.find("_M_data = nullptr;") != std::string::npos);
    CHECK(release.find("_M_data = nullptr;") < release.find("::operator delete(__old);"));
    CHECK(release.find("__old[__i].~_Tp();") != std::string::npos);

    std::valarray<int> empty;
    CHECK(empty.size() == 0);
    empty.resize(1, 4);
    CHECK(empty.size() == 1);
    empty.resize(3, 8);
    CHECK(empty.size() == 3);
    empty.resize(1, 2);
    CHECK(empty.size() == 1);
    empty.resize(0);
    CHECK(empty.size() == 0);
}
