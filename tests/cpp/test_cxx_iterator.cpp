// The <iterator> model is partial: std::begin and std::end for T (&)[N]
// only. begin is a pointer to the first element. end is one past the last.
// N == 0 yields the same pointer from both, and neither function
// dereferences it. A pointer argument is not declared.
//
// This case reads the header as text. It does not `#include` the model:
// doctest already pulls in the platform library, and defining
// _GLIBCXX_ITERATOR here would hide that header or duplicate std::begin.
// The trace is tests/cxx_models/iterator_trace.cpp. This is not a proof
// that an iterator stays in range.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path header_path() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "iterator"; }

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

std::size_t count_occ(std::string_view src, std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = 0; (at = src.find(needle, at)) != std::string_view::npos; at += needle.size()) ++n;
    return n;
}

}  // namespace

TEST_CASE("cxx iterator model: array begin and end do not dereference and a pointer is not declared") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE(src.find("This is not a proof that an iterator stays in range.") != std::string::npos);
    CHECK(src.find("#ifndef _GLIBCXX_ITERATOR") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_ITERATOR 1") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std {") != std::string::npos);
    CHECK(src.find("#include <cstddef>") != std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <string>") == std::string::npos);
    CHECK(src.find("#include <algorithm>") == std::string::npos);
    CHECK(src.find("#include <ranges>") == std::string::npos);
    CHECK(src.find("/*") == std::string::npos);

    const std::string code = strip_line_comments(src);
    CHECK(count_occ(code, "#include <") == 1);
    CHECK(code.find("#include <cstddef>") != std::string::npos);
    const std::string begin_sig = "constexpr _Tp* begin(_Tp (&__arr)[_Nm]) noexcept";
    const std::string end_sig = "constexpr _Tp* end(_Tp (&__arr)[_Nm]) noexcept";
    const std::string zero_begin_sig = "constexpr auto begin(_Arr& __arr) noexcept";
    const std::string zero_end_sig = "constexpr auto end(_Arr& __arr) noexcept";
    CHECK(count_occ(code, begin_sig) == 1);
    CHECK(count_occ(code, end_sig) == 1);
    CHECK(count_occ(code, zero_begin_sig) == 1);
    CHECK(count_occ(code, zero_end_sig) == 1);
    CHECK(count_occ(code, "begin(") == 2);
    CHECK(count_occ(code, "end(") == 2);
    CHECK(count_occ(code, "requires(sizeof(_Arr) == 0)") == 2);
    CHECK(count_occ(code, "*") == 2);
    CHECK(code.find("reverse_iterator") == std::string::npos);
    CHECK(code.find("next") == std::string::npos);
    CHECK(code.find("prev") == std::string::npos);
    CHECK(code.find("advance") == std::string::npos);
    CHECK(code.find("__prism_check") == std::string::npos);
    CHECK(code.find("_Container") == std::string::npos);

    const auto begin_body = body_of(code, begin_sig);
    const auto end_body = body_of(code, end_sig);
    const auto zero_begin_body = body_of(code, zero_begin_sig);
    const auto zero_end_body = body_of(code, zero_end_sig);
    REQUIRE(begin_body.find("return __arr;") != std::string::npos);
    REQUIRE(end_body.find("return __arr + _Nm;") != std::string::npos);
    REQUIRE(zero_begin_body.find("return __arr;") != std::string::npos);
    REQUIRE(zero_end_body.find("return __arr + 0;") != std::string::npos);
    CHECK(begin_body.find('*') == std::string::npos);
    CHECK(begin_body.find('[') == std::string::npos);
    CHECK(begin_body.find('+') == std::string::npos);
    CHECK(end_body.find('*') == std::string::npos);
    CHECK(end_body.find('[') == std::string::npos);
    CHECK(end_body.find('&') == std::string::npos);
    CHECK(zero_begin_body.find('*') == std::string::npos);
    CHECK(zero_begin_body.find('[') == std::string::npos);
    CHECK(zero_begin_body.find('+') == std::string::npos);
    CHECK(zero_end_body.find('*') == std::string::npos);
    CHECK(zero_end_body.find('[') == std::string::npos);
}
